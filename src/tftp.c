/*
 * tftp.c - TFTP (RFC 1350) packet helpers and transfer engine.
 */
#include "tftp.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

/* ---------- Packet encoding ---------- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

size_t tftp_build_request(uint8_t *buf, size_t cap, uint16_t opcode,
                          const char *filename, const char *mode)
{
    size_t flen = strlen(filename);
    size_t mlen = strlen(mode);
    size_t total = 2 + flen + 1 + mlen + 1;

    if (total > cap)
        return 0;
    put_u16(buf, opcode);
    memcpy(buf + 2, filename, flen + 1);
    memcpy(buf + 2 + flen + 1, mode, mlen + 1);
    return total;
}

size_t tftp_build_data(uint8_t *buf, uint16_t block, const uint8_t *data, size_t len)
{
    put_u16(buf, OP_DATA);
    put_u16(buf + 2, block);
    if (len > 0)
        memcpy(buf + 4, data, len);
    return 4 + len;
}

size_t tftp_build_ack(uint8_t *buf, uint16_t block)
{
    put_u16(buf, OP_ACK);
    put_u16(buf + 2, block);
    return 4;
}

size_t tftp_build_error(uint8_t *buf, size_t cap, uint16_t code, const char *msg)
{
    size_t mlen = strlen(msg);

    if (cap < 5)
        return 0;
    if (mlen + 5 > cap)
        mlen = cap - 5;
    put_u16(buf, OP_ERROR);
    put_u16(buf + 2, code);
    memcpy(buf + 4, msg, mlen);
    buf[4 + mlen] = '\0';
    return 5 + mlen;
}

/* ---------- Packet decoding ---------- */

uint16_t tftp_opcode(const uint8_t *buf, size_t len)
{
    uint16_t op;

    if (len < 2)
        return 0;
    op = get_u16(buf);
    return (op >= OP_RRQ && op <= OP_ERROR) ? op : 0;
}

uint16_t tftp_block(const uint8_t *buf, size_t len)
{
    return len < 4 ? 0 : get_u16(buf + 2);
}

int tftp_parse_request(const uint8_t *buf, size_t len,
                       char *filename, size_t fcap, char *mode, size_t mcap)
{
    const char *p, *end, *z;

    if (len < 6) /* opcode + 1-char name + NUL + 1-char mode + NUL */
        return -1;
    p = (const char *)buf + 2;
    end = (const char *)buf + len;

    z = memchr(p, '\0', (size_t)(end - p));
    if (z == NULL || z == p || (size_t)(z - p) >= fcap)
        return -1;
    memcpy(filename, p, (size_t)(z - p) + 1);

    p = z + 1;
    if (p >= end)
        return -1;
    z = memchr(p, '\0', (size_t)(end - p));
    if (z == NULL || z == p || (size_t)(z - p) >= mcap)
        return -1;
    memcpy(mode, p, (size_t)(z - p) + 1);
    return 0;
}

/* ---------- Socket helpers ---------- */

int tftp_set_timeout(int sock, int seconds)
{
    struct timeval tv;

    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int send_packet(int sock, const struct sockaddr_in *peer, const uint8_t *pkt, size_t len)
{
    if (sendto(sock, pkt, len, 0, (const struct sockaddr *)peer, sizeof *peer) < 0) {
        perror("sendto");
        return -1;
    }
    return 0;
}

void tftp_send_error(int sock, const struct sockaddr_in *peer, uint16_t code, const char *msg)
{
    uint8_t pkt[128];
    size_t len = tftp_build_error(pkt, sizeof pkt, code, msg);

    (void)sendto(sock, pkt, len, 0, (const struct sockaddr *)peer, sizeof *peer);
}

static void report_peer_error(const uint8_t *buf)
{
    /* buf is always NUL-terminated by wait_packet(). */
    fprintf(stderr, "tftp: remote error %u: %s\n",
            (unsigned)get_u16(buf + 2), (const char *)buf + 4);
}

/*
 * Waits for the next valid packet from the peer.
 * Returns its length (>= 4), 0 on timeout, -1 on a socket error.
 * Packets coming from an unknown transfer ID are answered with an
 * ERROR 5 and ignored, as required by RFC 1350.
 */
static ssize_t wait_packet(int sock, struct sockaddr_in *peer, int *learn_tid,
                           uint8_t *buf, size_t cap)
{
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof from;
        ssize_t n = recvfrom(sock, buf, cap - 1, 0, (struct sockaddr *)&from, &fromlen);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            if (errno == EINTR)
                continue;
            perror("recvfrom");
            return -1;
        }
        buf[n] = '\0';

        if (*learn_tid) {
            if (from.sin_addr.s_addr != peer->sin_addr.s_addr)
                continue;
            *peer = from;
            *learn_tid = 0;
        } else if (!same_peer(&from, peer)) {
            tftp_send_error(sock, &from, ERR_UNKNOWN_TID, "Unknown transfer ID");
            continue;
        }
        if (n < 4)
            continue; /* too short to be meaningful */
        return n;
    }
}

/* ---------- Transfer engine ---------- */

long long tftp_send_file(int sock, struct sockaddr_in *peer, int learn_tid, FILE *fp,
                         const uint8_t *initial, size_t initial_len)
{
    uint8_t pkt[TFTP_MAX_PACKET];
    uint8_t buf[TFTP_MAX_PACKET + 1];
    uint8_t data[TFTP_BLOCK_SIZE];
    size_t pkt_len, len;
    uint16_t awaited;
    int final = 0;
    int retries = 0;
    long long total = 0;

    if (initial != NULL) {
        memcpy(pkt, initial, initial_len);
        pkt_len = initial_len;
        awaited = 0; /* a WRQ is acknowledged with ACK 0 */
    } else {
        len = fread(data, 1, sizeof data, fp);
        if (ferror(fp)) {
            tftp_send_error(sock, peer, ERR_UNDEFINED, "Read error");
            return -1;
        }
        pkt_len = tftp_build_data(pkt, 1, data, len);
        awaited = 1;
        final = len < TFTP_BLOCK_SIZE;
        total = (long long)len;
    }

    if (send_packet(sock, peer, pkt, pkt_len) < 0)
        return -1;

    for (;;) {
        ssize_t n = wait_packet(sock, peer, &learn_tid, buf, sizeof buf);
        uint16_t op;

        if (n < 0)
            return -1;
        if (n == 0) {
            if (++retries > TFTP_MAX_RETRIES) {
                fprintf(stderr, "tftp: timeout, no answer from peer\n");
                return -1;
            }
            if (send_packet(sock, peer, pkt, pkt_len) < 0)
                return -1;
            continue;
        }

        op = tftp_opcode(buf, (size_t)n);
        if (op == OP_ERROR) {
            report_peer_error(buf);
            return -1;
        }
        if (op != OP_ACK) {
            tftp_send_error(sock, peer, ERR_ILLEGAL_OP, "Expected ACK");
            return -1;
        }
        /* Duplicate ACKs are ignored to avoid the Sorcerer's Apprentice bug. */
        if (tftp_block(buf, (size_t)n) != awaited)
            continue;

        retries = 0;
        if (final)
            return total;

        len = fread(data, 1, sizeof data, fp);
        if (ferror(fp)) {
            tftp_send_error(sock, peer, ERR_UNDEFINED, "Read error");
            return -1;
        }
        awaited++;
        pkt_len = tftp_build_data(pkt, awaited, data, len);
        final = len < TFTP_BLOCK_SIZE; /* a short (possibly empty) block ends the transfer */
        total += (long long)len;
        if (send_packet(sock, peer, pkt, pkt_len) < 0)
            return -1;
    }
}

long long tftp_recv_file(int sock, struct sockaddr_in *peer, int learn_tid, FILE *fp,
                         const uint8_t *initial, size_t initial_len)
{
    uint8_t pkt[TFTP_MAX_PACKET];
    uint8_t buf[TFTP_MAX_PACKET + 1];
    size_t pkt_len = initial_len;
    uint16_t expected = 1;
    int retries = 0;
    long long total = 0;

    memcpy(pkt, initial, initial_len);
    if (send_packet(sock, peer, pkt, pkt_len) < 0)
        return -1;

    for (;;) {
        ssize_t n = wait_packet(sock, peer, &learn_tid, buf, sizeof buf);
        uint16_t op, block;

        if (n < 0)
            return -1;
        if (n == 0) {
            if (++retries > TFTP_MAX_RETRIES) {
                fprintf(stderr, "tftp: timeout, no answer from peer\n");
                return -1;
            }
            if (send_packet(sock, peer, pkt, pkt_len) < 0)
                return -1;
            continue;
        }

        op = tftp_opcode(buf, (size_t)n);
        if (op == OP_ERROR) {
            report_peer_error(buf);
            return -1;
        }
        if (op != OP_DATA) {
            tftp_send_error(sock, peer, ERR_ILLEGAL_OP, "Expected DATA");
            return -1;
        }

        block = tftp_block(buf, (size_t)n);
        if (block == expected) {
            size_t len = (size_t)n - 4;

            if (len > 0 && fwrite(buf + 4, 1, len, fp) != len) {
                tftp_send_error(sock, peer, ERR_DISK_FULL, "Write error");
                return -1;
            }
            total += (long long)len;
            retries = 0;
            pkt_len = tftp_build_ack(pkt, block);
            if (send_packet(sock, peer, pkt, pkt_len) < 0)
                return -1;
            if (len < TFTP_BLOCK_SIZE)
                return total;
            expected++;
        } else if (block == (uint16_t)(expected - 1)) {
            /* Our ACK was lost: acknowledge the duplicate again. */
            if (send_packet(sock, peer, pkt, pkt_len) < 0)
                return -1;
        }
    }
}
