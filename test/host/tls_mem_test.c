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

/* Up to 8 roots (comma-separated DER files), searched like ca_roots.h is. */
static unsigned char roots[8][4096];
static size_t root_lens[8];
static int n_roots;

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

#include "mbedtls/ecp.h"

/* One session: connect, handshake (restartable, as on the device), GET,
 * read to the end, free. Returns 0 when it read more than 100 kB. */
/* The roots as httpc.c holds them: a chain parsed in place for the length
 * of a handshake (roots_ref/roots_unref there). */
static mbedtls_x509_crt chain;
static int roots_ref(mbedtls_ssl_config *conf) {
    mbedtls_x509_crt_init(&chain);
    for (int i = 0; i < n_roots; i++) {
        int rc = mbedtls_x509_crt_parse_der_nocopy(&chain, roots[i], root_lens[i]);
        if (rc) { printf("root %d parse -0x%04x\n", i, (unsigned)-rc); mbedtls_x509_crt_free(&chain); return rc; }
    }
    mbedtls_ssl_conf_ca_chain(conf, &chain, NULL);
    return 0;
}
static void roots_unref(mbedtls_ssl_config *conf) {
    mbedtls_ssl_conf_ca_chain(conf, NULL, NULL);
    mbedtls_x509_crt_free(&chain);
}

static int session(mbedtls_ssl_config *conf, int port, const char *path, size_t base, int k) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { perror("connect"); return 2; }
    mbedtls_ssl_context *ssl = mbedtls_calloc(1, sizeof *ssl);
    mbedtls_ssl_init(ssl);
    if (mbedtls_ssl_setup(ssl, conf) || mbedtls_ssl_set_hostname(ssl, "localhost")) { puts("FAIL setup"); return 1; }
    mbedtls_ssl_set_verify(ssl, verify_cb, NULL);
    mbedtls_ssl_set_bio(ssl, NULL, b_send, b_recv, NULL);
    peak_b = now_b;
    size_t after_setup = now_b - base, roots_peak = 0;
    /* As httpc.c's tls_step: one state at a time, the roots held only for
     * the state that verifies the server's chain. */
    int r = 0, steps = 0;
    bool held = false;
    while (!mbedtls_ssl_is_handshake_over(ssl)) {
        int st = ssl->MBEDTLS_PRIVATE(state);
        if (st == MBEDTLS_SSL_SERVER_CERTIFICATE && !held && !insecure) {
            if (roots_ref(conf)) { puts("FAIL roots"); return 1; }
            held = true;
        } else if (st > MBEDTLS_SSL_SERVER_CERTIFICATE && held) {
            roots_unref(conf);
            held = false;
        }
        r = mbedtls_ssl_handshake_step(ssl);
        if (held && now_b - base > roots_peak) { roots_peak = now_b - base; }
        if (r == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS) { steps++; r = 0; continue; }
        if (r) { break; }
    }
    if (held) { roots_unref(conf); }
    if (r != 0) {
        printf("FAIL session %d: handshake -0x%04x verify 0x%x\n", k, (unsigned)-r,
               (unsigned)mbedtls_ssl_get_verify_result(ssl));
        mbedtls_ssl_free(ssl); mbedtls_free(ssl); close(fd);
        printf("session %d: %zu B left after free\n", k, now_b - base);
        return 1;
    }
    size_t hs_peak = peak_b - base, after_hs = now_b - base;
    char req[256];
    int n = snprintf(req, sizeof req, "GET /%s HTTP/1.0\r\nHost: localhost\r\n\r\n", path);
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
    printf("session %d: %s, %d restart steps, received %zu B; %zu B after setup, %zu B with the roots, "
           "%zu B steady after the handshake, %zu B handshake peak, %zu B overall peak, "
           "%zu B left after free\n", k, "ok", steps, got,
           after_setup, roots_peak, after_hs, hs_peak, total_peak, now_b - base);
    return got > 100000 && now_b == base ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: tls_mem_test port root.der[,root2.der...] path [insecure]\n"); return 2; }
    insecure = argc > 4 && strcmp(argv[4], "insecure") == 0;
    char list[1024];
    snprintf(list, sizeof list, "%s", argv[2]);
    for (char *t = strtok(list, ","); t && n_roots < 8; t = strtok(NULL, ",")) {
        FILE *f = fopen(t, "rb");
        if (!f) { perror(t); return 2; }
        root_lens[n_roots] = fread(roots[n_roots], 1, sizeof roots[0], f);
        n_roots++;
        fclose(f);
    }
    int sessions = getenv("SESSIONS") ? atoi(getenv("SESSIONS")) : 1;
    mbedtls_platform_set_calloc_free(cnt_calloc, cnt_free);
    /* As the device: restartable ECC in bounded steps (httpc.c). */
    mbedtls_ecp_set_max_ops(600);

    static mbedtls_ssl_config conf;
    static mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&drbg);
    if (mbedtls_ctr_drbg_seed(&drbg, entropy, NULL, (const unsigned char *)"t", 1) != 0) { puts("FAIL seed"); return 1; }
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) { puts("FAIL defaults"); return 1; }
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_authmode(&conf, insecure ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);
    size_t base = now_b;
    int rc = 0;
    for (int k = 1; k <= sessions; k++) {
        int r = session(&conf, atoi(argv[1]), argv[3], base, k);
        if (r) { rc = r; }
    }
    return rc;
}
