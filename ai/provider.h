#ifndef COYOTE_AI_PROVIDER_H
#define COYOTE_AI_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Provider boundary for the Coyote AI chat path.
 *
 * A provider turns a bounded, offset-readable request body into a stream of
 * events as response bytes arrive.  The request is exposed through
 * provider_request_t so the transport can write it to a socket a window at a
 * time without ever needing a second copy of the payload.  No provider may
 * allocate heap memory.
 */

typedef enum {
    PROVIDER_EVENT_CONTENT = 0,
    PROVIDER_EVENT_DONE,
    PROVIDER_EVENT_ERROR
} provider_event_type_t;

typedef struct {
    provider_event_type_t type;
    const char *data; /* CONTENT/ERROR payload; NULL for DONE */
    size_t length;    /* payload length in bytes */
} provider_event_t;

typedef void (*provider_event_callback_t)(void *context, const provider_event_t *event);

/* Offset-readable view over a provider-owned, fixed-capacity request buffer. */
typedef struct {
    size_t (*length)(void *context);
    /* Copy up to capacity bytes starting at offset into destination.  Returns
     * the number of bytes copied; 0 once offset >= length. */
    size_t (*read)(void *context, size_t offset, char *destination, size_t capacity);
    void *context;
} provider_request_t;

#endif
