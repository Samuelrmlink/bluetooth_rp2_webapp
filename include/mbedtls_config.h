/*
 * mbedTLS 3.6 for the HTTP client (src/net/httpc.c): a TLS 1.2 CLIENT and
 * nothing else, cut down for an 864 KiB firmware slot.
 *
 *   key exchange  ECDHE-ECDSA and ECDHE-RSA (no static RSA, no DHE, no PSK)
 *   ciphers       AES-128/256-GCM (SHA-256 / SHA-384 PRF)
 *   curves        P-256, P-384 (every root here and Google's chains)
 *   certificates  RSA (PKCS#1 v1.5, up to 4096 bits: GTS R1/R2, GlobalSign
 *                 R1, ISRG X1) and ECDSA (GTS R3/R4, ISRG X2)
 *
 * Not built: TLS 1.3, servers, DTLS, PSA, CBC/CCM/ChaCha, session tickets,
 * renegotiation, PEM, the error-string table. TLS 1.3 alone would be ~25 KB
 * more; every server that matters here still speaks 1.2.
 *
 * Memory goes through mbedtls_platform_set_calloc_free (httpc_tls.c), which
 * hands out the TOP of the Lua arena -- see docs/lua-net-fs.md for why there.
 * IN_CONTENT_LEN must be the full 16 KiB: max_fragment_length is only a
 * request, and Google's front ends ignore it and send 16 KiB records.
 */
#ifndef BTGW_MBEDTLS_CONFIG_H
#define BTGW_MBEDTLS_CONFIG_H

/* --- platform ---------------------------------------------------------------- */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_MS_TIME_ALT      /* mbedtls_ms_time(): httpc_tls.c */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_DEPRECATED_REMOVED

/* --- symmetric --------------------------------------------------------------- */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES            /* tables in flash, not 8 KB of RAM */
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_MD_C
/* SHA-1 only so GlobalSign Root CA (R1), self-signed sha1WithRSA, parses at
 * all -- without it the root was unusable. A trust anchor's own signature is
 * never checked, and the default verification profile still refuses SHA-1
 * signatures anywhere in a chain. */
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA512_SMALLER

/* --- public key -------------------------------------------------------------- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_MPI_MAX_SIZE 512          /* RSA-4096 roots */
#define MBEDTLS_MPI_WINDOW_SIZE 2
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
/* No X25519: mbedTLS's Montgomery ladder is not restartable (below), so an
 * X25519 key exchange is one long stall; every server that matters offers P-256. */
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_WINDOW_SIZE 2
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0
/* Restartable ECC: with an ECDHE-ECDSA suite the handshake's curve arithmetic
 * (key generation, ECDH, ECDSA verification) runs in bounded steps
 * (mbedtls_ecp_set_max_ops, httpc.c) and the handshake returns "in progress"
 * between them, so the main loop keeps turning. Without it one handshake
 * call held the loop for ~1 s (measured in mcusim) -- USB, the radio and
 * Bluetooth with it. ECDHE-RSA suites do not get this (mbedTLS restarts only
 * the ECDSA flow), which is one reason ECDSA suites are offered first. */
#define MBEDTLS_ECP_RESTARTABLE
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* --- X.509 ------------------------------------------------------------------- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_MAX_INTERMEDIATE_CA 4

/* --- TLS --------------------------------------------------------------------- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
/* A client sends a ClientHello, its key exchange and small requests; larger
 * writes go out as several records. 2 KiB saves 2 KiB per session. */
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048
#define MBEDTLS_SSL_CIPHERSUITES \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,   \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384

#endif
