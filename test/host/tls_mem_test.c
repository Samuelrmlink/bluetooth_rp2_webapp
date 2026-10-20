/*
 * The firmware's mbedTLS configuration (include/mbedtls_config.h) on a host,
 * against a local TLS server: does the cut-down build complete a real
 * handshake and verify a real chain through the trusted-CA callback, and how
 * much memory does a session take? Every allocation goes through a counting
 * calloc, the same hook httpc_tls uses on the device, so the peak printed is
 * the arena a session needs there (modulo pointer width: the host's 64-bit
 * pointers make the structures a little larger than on the Cortex-M33).
 *
 *   tls_mem_test <port> <root.der> <path> [insecure]
 *
 * tools/run_tests.py (t_tls_mem) builds the chain with openssl (an RSA-4096
 * root signed with SHA-384 like GTS Root R1, an RSA-2048 intermediate like
 * WR2, then an ECDSA and an RSA leaf), runs `openssl s_server` on 127.0.0.1
 * and this against it. Nothing leaves the machine.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

static size_t now_b, peak_b, allocs;
typedef struct { size_t n, pad; } hdr_t;
static void *cnt_calloc(size_t n, size_t s) {
    hdr_t *h = calloc(1, sizeof *h + n * s);
    if (!h) { return NULL; }
    h->n = n * s;
    now_b += h->n;
    allocs++;
    if (now_b > peak_b) { peak_b = now_b; }
    return h + 1;
}
static void cnt_free(void *p) {
    if (!p) { return; }
    hdr_t *h = (hdr_t *)p - 1;
    now_b -= h->n;
    free(h);
}

int mbedtls_hardware_poll(void *d, unsigned char *out, size_t len, size_t *olen) {
    (void)d;
    *olen = (size_t)getrandom(out, len, 0);
    return 0;
}
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)(clock() / (CLOCKS_PER_SEC / 1000)); }

static unsigned char root[4096];
static size_t root_len;
static int ca_calls;

static int ca_cb(void *ctx, mbedtls_x509_crt const *child, mbedtls_x509_crt **cand) {
    (void)ctx;
    ca_calls++;
    *cand = NULL;
    mbedtls_x509_crt *c = mbedtls_calloc(1, sizeof *c);
    if (!c) { return MBEDTLS_ERR_X509_ALLOC_FAILED; }
    mbedtls_x509_crt_init(c);
    if (mbedtls_x509_crt_parse_der_nocopy(c, root, root_len) == 0 &&
        child->issuer_raw.len == c->subject_raw.len &&
        memcmp(child->issuer_raw.p, c->subject_raw.p, c->subject_raw.len) == 0) {
        *cand = c;
        return 0;
    }
    mbedtls_x509_crt_free(c);
    mbedtls_free(c);
    return 0;
}

static int insecure;
static int verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)ctx; (void)crt; (void)depth;
    if (insecure) { *flags = 0; }
    return 0;
}

static int fd;
static int b_send(void *c, const unsigned char *b, size_t n) { (void)c; return (int)send(fd, b, n, 0); }
static int b_recv(void *c, unsigned char *b, size_t n) { (void)c; return (int)recv(fd, b, n, 0); }

static int entropy(void *ctx, unsigned char *out, size_t len) {
    size_t ol = 0;
    (void)ctx;
    return mbedtls_hardware_poll(NULL, out, len, &ol);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: tls_mem_test port root.der path [insecure]\n"); return 2; }
    insecure = argc > 4;
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror("root"); return 2; }
    root_len = fread(root, 1, sizeof root, f);
    fclose(f);
    mbedtls_platform_set_calloc_free(cnt_calloc, cnt_free);

    static mbedtls_ssl_config conf;
    static mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&drbg);
    if (mbedtls_ctr_drbg_seed(&drbg, entropy, NULL, (const unsigned char *)"t", 1) != 0) { puts("FAIL seed"); return 1; }
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) { puts("FAIL defaults"); return 1; }
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_cb(&conf, ca_cb, NULL);
    size_t base = now_b;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { perror("connect"); return 2; }

    mbedtls_ssl_context *ssl = mbedtls_calloc(1, sizeof *ssl);
    mbedtls_ssl_init(ssl);
    if (mbedtls_ssl_setup(ssl, &conf) || mbedtls_ssl_set_hostname(ssl, "localhost")) { puts("FAIL setup"); return 1; }
    mbedtls_ssl_set_verify(ssl, verify_cb, NULL);
    mbedtls_ssl_set_bio(ssl, NULL, b_send, b_recv, NULL);
    size_t after_setup = now_b - base;
    peak_b = now_b;
    int r = mbedtls_ssl_handshake(ssl);
    if (r != 0) {
        printf("FAIL handshake -0x%04x verify 0x%x\n", (unsigned)-r, (unsigned)mbedtls_ssl_get_verify_result(ssl));
        return 1;
    }
    size_t hs_peak = peak_b - base, after_hs = now_b - base;
    char req[256];
    int n = snprintf(req, sizeof req, "GET /%s HTTP/1.0\r\nHost: localhost\r\n\r\n", argv[3]);
    mbedtls_ssl_write(ssl, (const unsigned char *)req, (size_t)n);
    size_t got = 0;
    unsigned char buf[1024];
    for (;;) {
        r = mbedtls_ssl_read(ssl, buf, sizeof buf);
        if (r <= 0) { break; }
        got += (size_t)r;
    }
    size_t total_peak = peak_b - base;
    mbedtls_ssl_free(ssl);
    mbedtls_free(ssl);
    close(fd);
    printf("suite %s, ca callbacks %d, received %zu B\n", "?", ca_calls, got);
    printf("session: %zu B after setup, %zu B steady after the handshake, %zu B handshake peak, "
           "%zu B overall peak, %zu B left after free\n", after_setup, after_hs, hs_peak, total_peak, now_b - base);
    return got > 100000 && now_b == base ? 0 : 1;
}
