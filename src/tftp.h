/*
 * tftp.h - TFTP (RFC 1350) packet helpers and transfer engine.
 */
#ifndef TFTP_H
#define TFTP_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define TFTP_BLOCK_SIZE   512
#define TFTP_MAX_PACKET   (4 + TFTP_BLOCK_SIZE)
#define TFTP_TIMEOUT_SEC  2
#define TFTP_MAX_RETRIES  5

enum tftp_opcode {
    OP_RRQ   = 1, /* read request  */
    OP_WRQ   = 2, /* write request */
    OP_DATA  = 3,
    OP_ACK   = 4,
    OP_ERROR = 5
};

enum tftp_error {
    ERR_UNDEFINED   = 0,
    ERR_NOT_FOUND   = 1,
    ERR_ACCESS      = 2,
    ERR_DISK_FULL   = 3,
    ERR_ILLEGAL_OP  = 4,
    ERR_UNKNOWN_TID = 5,
    ERR_FILE_EXISTS = 6,
    ERR_NO_USER     = 7
};

/* Packet builders. They return the packet length, or 0 if it does not fit. */
size_t tftp_build_request(uint8_t *buf, size_t cap, uint16_t opcode,
                          const char *filename, const char *mode);
size_t tftp_build_data(uint8_t *buf, uint16_t block, const uint8_t *data, size_t len);
size_t tftp_build_ack(uint8_t *buf, uint16_t block);
size_t tftp_build_error(uint8_t *buf, size_t cap, uint16_t code, const char *msg);

/* Packet parsers. tftp_opcode() returns 0 for an invalid packet. */
uint16_t tftp_opcode(const uint8_t *buf, size_t len);
uint16_t tftp_block(const uint8_t *buf, size_t len);
int tftp_parse_request(const uint8_t *buf, size_t len,
                       char *filename, size_t fcap, char *mode, size_t mcap);

/* Socket helpers. */
int  tftp_set_timeout(int sock, int seconds);
void tftp_send_error(int sock, const struct sockaddr_in *peer, uint16_t code, const char *msg);

/*
 * Transfer engine (lock-step, one block in flight, retransmission on timeout).
 *
 * `peer` is where packets are sent. When `learn_tid` is set, the first reply
 * coming from the same IP address fixes the peer's transfer ID (port), as a
 * client must do after sending its request to port 69.
 *
 * `initial` is an optional first packet (RRQ/WRQ from a client, ACK 0 from a
 * server) that is sent and retransmitted until the peer answers.
 *
 * Both functions return the number of bytes transferred, or -1 on failure.
 */
long long tftp_send_file(int sock, struct sockaddr_in *peer, int learn_tid, FILE *fp,
                         const uint8_t *initial, size_t initial_len);
long long tftp_recv_file(int sock, struct sockaddr_in *peer, int learn_tid, FILE *fp,
                         const uint8_t *initial, size_t initial_len);

#endif /* TFTP_H */
