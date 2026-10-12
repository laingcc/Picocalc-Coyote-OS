#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tls/tls_trust.h"
#include "test_util.h"

/*
 * The embedded trust anchors, checked for what the firmware's PEM parser
 * needs of them: real certificates, a known number of them, and the trailing
 * NUL counted in the length.  An empty or missing list would mean a TLS
 * configuration with nothing to verify against.
 */

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode the base64 between begin and end (line breaks skipped) into out.
 * Returns the decoded length, or 0 if it is not well-formed base64. */
static size_t decode(const char *begin, const char *end, unsigned char *out, size_t capacity) {
    uint32_t bits = 0u;
    int count = 0, padding = 0;
    size_t length = 0u;
    const char *p;

    for (p = begin; p < end; p++) {
        int value;
        if (*p == '\n') continue;
        if (*p == '=') { padding++; continue; }
        value = base64_value(*p);
        if (value < 0 || padding > 0) return 0u;
        bits = (bits << 6) | (uint32_t)value;
        if (++count == 4) {
            if (length + 3u > capacity) return 0u;
            out[length++] = (unsigned char)(bits >> 16);
            out[length++] = (unsigned char)(bits >> 8);
            out[length++] = (unsigned char)bits;
            bits = 0u;
            count = 0;
        }
    }
    if (padding > 2 || (count + padding) % 4 != 0 || count == 1) return 0u;
    if (count == 3) {
        if (length + 2u > capacity) return 0u;
        out[length++] = (unsigned char)(bits >> 10);
        out[length++] = (unsigned char)(bits >> 2);
    } else if (count == 2) {
        if (length + 1u > capacity) return 0u;
        out[length++] = (unsigned char)(bits >> 4);
    }
    return length;
}

static bool contains(const unsigned char *der, size_t length, const char *text) {
    size_t n = strlen(text), i;
    for (i = 0u; i + n <= length; i++) {
        if (memcmp(der + i, text, n) == 0) return true;
    }
    return false;
}

void test_tls_trust(void);
void test_tls_trust(void) {
    static const char begin_mark[] = "-----BEGIN CERTIFICATE-----\n";
    static const char end_mark[] = "-----END CERTIFICATE-----\n";
    static unsigned char der[2048];
    const char *pem = (const char *)tls_trust_pem;
    const char *cursor = pem;
    unsigned roots = 0u;
    bool amazon_root_1 = false;

    /* The PEM parser needs the NUL, and needs the length to count it. */
    CHECK(tls_trust_pem_length > 1u);
    CHECK(tls_trust_pem[tls_trust_pem_length - 1u] == '\0');
    CHECK(strlen(pem) == tls_trust_pem_length - 1u);

    while (strncmp(cursor, begin_mark, sizeof(begin_mark) - 1u) == 0) {
        const char *body = cursor + sizeof(begin_mark) - 1u;
        const char *end = strstr(body, end_mark);
        size_t length;

        CHECK(end != NULL);
        if (end == NULL) break;
        length = decode(body, end, der, sizeof(der));
        /* A DER certificate: SEQUENCE, two length bytes, and exactly that long. */
        CHECK(length > 4u);
        CHECK(der[0] == 0x30u && der[1] == 0x82u);
        CHECK(((size_t)der[2] << 8 | der[3]) + 4u == length);
        if (contains(der, length, "Amazon Root CA 1")) amazon_root_1 = true;
        roots++;
        cursor = end + sizeof(end_mark) - 1u;
    }
    /* Nothing but certificates, and all of them. */
    CHECK(*cursor == '\0');
    CHECK(roots == TLS_TRUST_ROOT_COUNT);
    /* Keep this independent check aligned with the explicit root list. */
    CHECK(roots == 7u);
    /* The root api.deepseek.com's chain ends in today. */
    CHECK(amazon_root_1);
}
