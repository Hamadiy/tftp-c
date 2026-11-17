/*
 * tftpd.c - Minimal concurrent TFTP server (RFC 1350).
 *
 * Each request is served by a forked child on a fresh UDP port (new TID),
 * so several clients can transfer at the same time.
 */
#include "tftp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define FILENAME_MAX_LEN 256
#define MODE_MAX_LEN     16

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-p port] [-d root_dir]\n", prog);
}

/* Only plain file names inside the root directory are served. */
static int filename_is_safe(const char *name)
{
    return name[0] != '\0'
        && strchr(name, '/') == NULL
        && strchr(name, '\\') == NULL
        && strcmp(name, ".") != 0
        && strcmp(name, "..") != 0;
}

static int mode_is_supported(const char *mode)
{
    return strcasecmp(mode, "octet") == 0 || strcasecmp(mode, "netascii") == 0;
}

static uint16_t errno_to_tftp(int err)
{
    switch (err) {
    case ENOENT: return ERR_NOT_FOUND;
    case EACCES:
    case EPERM:  return ERR_ACCESS;
    case EEXIST: return ERR_FILE_EXISTS;
    case ENOSPC: return ERR_DISK_FULL;
    default:     return ERR_UNDEFINED;
    }
}

static void handle_rrq(int sock, struct sockaddr_in *peer, const char *filename)
{
    FILE *fp = fopen(filename, "rb");
    long long n;

    if (fp == NULL) {
        int err = errno;

        tftp_send_error(sock, peer, errno_to_tftp(err), strerror(err));
        printf("  -> refused: %s\n", strerror(err));
        return;
    }
    n = tftp_send_file(sock, peer, 0, fp, NULL, 0);
    fclose(fp);
    if (n >= 0)
        printf("  -> sent \"%s\" (%lld bytes)\n", filename, n);
    else
        printf("  -> transfer of \"%s\" failed\n", filename);
}

static void handle_wrq(int sock, struct sockaddr_in *peer, const char *filename)
{
    uint8_t ack[4];
    size_t ack_len;
    long long n;
    FILE *fp;
    int fd = open(filename, O_WRONLY | O_CREAT | O_EXCL, 0644);

    if (fd < 0) {
        int err = errno;

        tftp_send_error(sock, peer, errno_to_tftp(err), strerror(err));
        printf("  -> refused: %s\n", strerror(err));
        return;
    }
    fp = fdopen(fd, "wb");
    if (fp == NULL) {
        close(fd);
        unlink(filename);
        tftp_send_error(sock, peer, ERR_UNDEFINED, "Internal error");
        return;
    }

    ack_len = tftp_build_ack(ack, 0);
    n = tftp_recv_file(sock, peer, 0, fp, ack, ack_len);
    if (fclose(fp) != 0)
        n = -1;

    if (n >= 0) {
        printf("  -> received \"%s\" (%lld bytes)\n", filename, n);
    } else {
        unlink(filename); /* never keep a partial upload */
        printf("  -> transfer of \"%s\" failed\n", filename);
    }
}

static void handle_request(const uint8_t *req, size_t len, struct sockaddr_in *peer)
{
    char filename[FILENAME_MAX_LEN];
    char mode[MODE_MAX_LEN];
    char ip[INET_ADDRSTRLEN];
    struct sockaddr_in local;
    uint16_t op = tftp_opcode(req, len);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket");
        return;
    }
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0; /* ephemeral port = server transfer ID */
    if (bind(sock, (struct sockaddr *)&local, sizeof local) < 0) {
        perror("bind");
        close(sock);
        return;
    }
    tftp_set_timeout(sock, TFTP_TIMEOUT_SEC);
    inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof ip);

    if ((op != OP_RRQ && op != OP_WRQ)
        || tftp_parse_request(req, len, filename, sizeof filename, mode, sizeof mode) < 0) {
        printf("[%s:%u] malformed request\n", ip, (unsigned)ntohs(peer->sin_port));
        tftp_send_error(sock, peer, ERR_ILLEGAL_OP, "Malformed request");
        close(sock);
        return;
    }

    printf("[%s:%u] %s \"%s\" (%s)\n", ip, (unsigned)ntohs(peer->sin_port),
           op == OP_RRQ ? "RRQ" : "WRQ", filename, mode);

    if (!mode_is_supported(mode)) {
        tftp_send_error(sock, peer, ERR_ILLEGAL_OP, "Unsupported transfer mode");
        printf("  -> refused: unsupported mode\n");
    } else if (!filename_is_safe(filename)) {
        tftp_send_error(sock, peer, ERR_ACCESS, "Access violation");
        printf("  -> refused: path outside of the root directory\n");
    } else if (op == OP_RRQ) {
        handle_rrq(sock, peer, filename);
    } else {
        handle_wrq(sock, peer, filename);
    }
    close(sock);
}

int main(int argc, char **argv)
{
    unsigned long port = 69;
    const char *root = ".";
    struct sockaddr_in addr;
    int opt, sock, one = 1;

    while ((opt = getopt(argc, argv, "p:d:h")) != -1) {
        char *end;

        switch (opt) {
        case 'p':
            port = strtoul(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || port == 0 || port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'd':
            root = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (chdir(root) < 0) {
        perror(root);
        return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGCHLD, SIG_IGN); /* children are reaped automatically */

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return EXIT_FAILURE;
    }
    printf("tftpd listening on UDP port %lu, serving \"%s\"\n", port, root);

    for (;;) {
        uint8_t req[TFTP_MAX_PACKET];
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        ssize_t n = recvfrom(sock, req, sizeof req, 0, (struct sockaddr *)&peer, &peer_len);
        pid_t pid;

        if (n < 0) {
            if (errno != EINTR)
                perror("recvfrom");
            continue;
        }
        pid = fork();
        if (pid < 0) {
            perror("fork");
            continue;
        }
        if (pid == 0) {
            close(sock);
            handle_request(req, (size_t)n, &peer);
            fflush(stdout);
            _exit(EXIT_SUCCESS);
        }
    }
}
