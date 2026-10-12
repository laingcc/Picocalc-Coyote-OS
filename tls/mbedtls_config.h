#ifndef COYOTE_MBEDTLS_CONFIG_H
#define COYOTE_MBEDTLS_CONFIG_H

/*
 * mbedTLS build configuration for the firmware (found by the Pico SDK's
 * pico_mbedtls through the include path).  It builds a TLS 1.2 client and
 * nothing else: no server, no TLS 1.0/1.1, no static-RSA or CBC cipher
 * suites, no session tickets, no renegotiation.
 */

/* Some mbedTLS sources use INT_MAX without including limits.h. */
#include <limits.h>

/* ---- Platform ----------------------------------------------------------- */

/* Entropy comes from the SDK: pico_mbedtls implements mbedtls_hardware_poll
 * on pico_rand (the RP2350 TRNG and ring oscillator). */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* lwIP's altcp_tls reads fields of the SSL context. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/* With MBEDTLS_PLATFORM_MEMORY and no standard calloc named, mbedTLS has no
 * allocator until lwIP's altcp_tls installs one that draws on lwIP's fixed
 * heap (MEM_SIZE in lwipopts.h).  TLS therefore never touches the C heap. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

/*
 * Time.  Certificate validity periods are checked (MBEDTLS_HAVE_TIME_DATE),
 * so mbedTLS must be given a real wall clock.  The C library's time() is not
 * one here: nothing sets the SDK's always-on timer, so it would report a date
 * in 1970 or whatever it was last left at.  Instead every clock mbedTLS uses
 * is supplied by tls/tls_clock.c:
 *
 *   mbedtls_time              -> coyote_tls_time, the SNTP-set time from
 *                                time_service, or 0 until SNTP has answered
 *                                since boot.  At 0 every certificate is "not
 *                                yet valid" and verification fails.
 *   mbedtls_platform_gmtime_r -> a calendar conversion with no hidden state
 *   mbedtls_ms_time           -> the monotonic clock
 */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_MACRO coyote_tls_time
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#ifndef __ASSEMBLER__
#include <time.h>
time_t coyote_tls_time(time_t *now);
#endif

/* ---- Protocol ----------------------------------------------------------- */

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET

/* A server may send records of up to 16384 bytes whatever the client asks
 * for, so the receive buffer has to be full size.  Requests are small. */
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048

/* Forward-secret key exchange with AEAD ciphers only. */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C

/* ---- Public key and certificates ---------------------------------------- */

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ---- Hashes and random numbers ------------------------------------------ */

#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

#endif
