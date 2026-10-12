#ifndef COYOTE_TLS_TRUST_H
#define COYOTE_TLS_TRUST_H

#include <stddef.h>

/* Number of root certificates in tls_trust_pem. */
#define TLS_TRUST_ROOT_COUNT 7u

/* The PEM root certificates TLS connections are verified against, and their
 * length including the trailing NUL.  See tls_trust.c. */
extern const unsigned char tls_trust_pem[];
extern const size_t tls_trust_pem_length;

#endif
