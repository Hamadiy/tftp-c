/*
 * tftp_client.c - Command-line TFTP client (RFC 1350, octet mode).
 */
#include "tftp.h"

#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [-p port] <server> get <remote_file> [local_file]\n"
            "  %s [-p port] <server> put <local_file> [remote_file]\n",
            prog, prog);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

static double elapsed(const struct timespec *t0)
{
    struct timespec t1;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) + (double)(t1.tv_nsec - t0->tv_nsec) / 1e9;
}

static long long do_get(int sock, struct sockaddr_in *server, const char *remote, const char *local)
{
    uint8_t req[TFTP_MAX_PACKET];
    size_t len = tftp_build_request(req, sizeof req, OP_RRQ, remote, "octet");
    long long bytes;
    FILE *fp;

    if (len == 0) {
        fprintf(stderr, "tftp: file name too long\n");
        return -1;
    }
    fp = fopen(local, "wb");
    if (fp == NULL) {
        perror(local);
        return -1;
    }
    bytes = tftp_recv_file(sock, server, 1, fp, req, len);
    if (fclose(fp) != 0)
        bytes = -1;
    if (bytes < 0)
        remove(local);
    return bytes;
}

static long long do_put(int sock, struct sockaddr_in *server, const char *local, const char *remote)
{
    uint8_t req[TFTP_MAX_PACKET];
    size_t len = tftp_build_request(req, sizeof req, OP_WRQ, remote, "octet");
    long long bytes;
    FILE *fp;

    if (len == 0) {
        fprintf(stderr, "tftp: file name too long\n");
        return -1;
    }
    fp = fopen(local, "rb");
    if (fp == NULL) {
        perror(local);
        return -1;
    }
    bytes = tftp_send_file(sock, server, 1, fp, req, len);
    fclose(fp);
    return bytes;
}

int main(int argc, char **argv)
{
    const char *port = "69";
    const char *host, *cmd, *file1, *file2;
    struct addrinfo hints, *res;
    struct sockaddr_in server;
    struct timespec t0;
    long long bytes;
    int opt, rc, sock;

    while ((opt = getopt(argc, argv, "p:h")) != -1) {
        switch (opt) {
        case 'p':
            port = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (argc - optind < 3 || argc - optind > 4) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    host = argv[optind];
    cmd = argv[optind + 1];
    file1 = argv[optind + 2];
    file2 = (argc - optind == 4) ? argv[optind + 3] : base_name(file1);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "tftp: %s: %s\n", host, gai_strerror(rc));
        return EXIT_FAILURE;
    }
    memcpy(&server, res->ai_addr, sizeof server);
    freeaddrinfo(res);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    tftp_set_timeout(sock, TFTP_TIMEOUT_SEC);
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (strcmp(cmd, "get") == 0) {
        bytes = do_get(sock, &server, file1, file2);
    } else if (strcmp(cmd, "put") == 0) {
        bytes = do_put(sock, &server, file1, file2);
    } else {
        usage(argv[0]);
        close(sock);
        return EXIT_FAILURE;
    }
    close(sock);

    if (bytes < 0)
        return EXIT_FAILURE;
    printf("%s: %lld bytes transferred in %.3f s\n", cmd, bytes, elapsed(&t0));
    return EXIT_SUCCESS;
}
