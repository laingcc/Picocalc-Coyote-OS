#ifndef COYOTE_TEST_SSE_H
#define COYOTE_TEST_SSE_H

#include <stdio.h>
#include <string.h>

#include "ai/sse_provider.h"
#include "test_util.h"

/*
 * What the cloud provider tests share: an event recorder, and a runner that
 * replays one reply at several fragment sizes, since a reply may be split at
 * any byte.  Each test file supplies sse_test_build, which builds a request
 * into sse_test_provider with its own provider's builder.
 */

typedef struct {
    char content[SSE_LINE_CAPACITY + 1u];
    size_t content_length;
    int content_events;
    int done_events;
    int error_events;
    char error[128];
} sse_recorder_t;

/* Static: too large for stack locals. */
static sse_provider_t sse_test_provider;
static sse_recorder_t sse_seen;

static void sse_test_build(void);

static void sse_record(void *context, const provider_event_t *event) {
    sse_recorder_t *r = (sse_recorder_t *)context;
    switch (event->type) {
        case PROVIDER_EVENT_CONTENT:
            r->content_events++;
            if (event->length >= sizeof(r->content) - r->content_length) {
                break; /* counted but not kept; the length check then fails */
            }
            memcpy(r->content + r->content_length, event->data, event->length);
            r->content_length += event->length;
            r->content[r->content_length] = '\0';
            break;
        case PROVIDER_EVENT_DONE:
            r->done_events++;
            break;
        case PROVIDER_EVENT_ERROR:
            r->error_events++;
            snprintf(r->error, sizeof(r->error), "%.*s", (int)event->length, event->data);
            break;
    }
}

static void sse_begin(void) {
    memset(&sse_seen, 0, sizeof(sse_seen));
    sse_provider_init(&sse_test_provider, sse_record, &sse_seen);
    sse_test_build();
}

/* Feed text in pieces of chunk bytes, then finish.  Returns finish's result,
 * or -1 as soon as a feed fails. */
static int sse_run(const char *text, size_t length, size_t chunk) {
    size_t offset = 0u;
    sse_begin();
    while (offset < length) {
        size_t take = length - offset < chunk ? length - offset : chunk;
        if (sse_provider_feed(&sse_test_provider, text + offset, take) != 0) {
            return -1;
        }
        offset += take;
    }
    return sse_provider_finish(&sse_test_provider);
}

static const size_t sse_fragments[] = {1u, 3u, 7u, 64u, 100000u};
#define SSE_FRAGMENT_COUNT (sizeof(sse_fragments) / sizeof(sse_fragments[0]))

/* The reply is accepted at every fragment size, ends in exactly one DONE, and
 * delivers exactly content. */
static void sse_check_accepted(const char *text, const char *content) {
    size_t c;
    for (c = 0u; c < SSE_FRAGMENT_COUNT; c++) {
        CHECK(sse_run(text, strlen(text), sse_fragments[c]) == 0);
        CHECK_STR_EQ(sse_seen.content, content);
        CHECK(sse_seen.done_events == 1);
        CHECK(sse_seen.error_events == 0);
        CHECK(sse_provider_is_done(&sse_test_provider));
        CHECK(!sse_provider_failed(&sse_test_provider));
    }
}

/* The reply is refused at every fragment size with exactly one ERROR carrying
 * error and latches failure.  A terminal marker may have delivered DONE before
 * later trailing input invalidates the stream; the provider must nevertheless
 * no longer report itself done.  Content delivered before the fault is checked
 * against content when it is not NULL. */
static void sse_check_rejected(const char *text, const char *error, const char *content) {
    size_t c;
    for (c = 0u; c < SSE_FRAGMENT_COUNT; c++) {
        CHECK(sse_run(text, strlen(text), sse_fragments[c]) == -1);
        CHECK(sse_seen.error_events == 1);
        CHECK_STR_EQ(sse_seen.error, error);
        if (content != NULL) {
            CHECK_STR_EQ(sse_seen.content, content);
        }
        CHECK(sse_provider_failed(&sse_test_provider));
        CHECK(!sse_provider_is_done(&sse_test_provider));
        /* Latched: nothing more is accepted and nothing more is reported. */
        CHECK(sse_provider_feed(&sse_test_provider, "\n", 1u) == -1);
        CHECK(sse_provider_finish(&sse_test_provider) == -1);
        CHECK(sse_seen.error_events == 1);
    }
}

#endif
