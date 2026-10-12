#ifndef COYOTE_AI_CLOUD_PROVIDER_H
#define COYOTE_AI_CLOUD_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

#include "ai/ai_config.h"
#include "ai/http_stream.h"
#include "ai/sse_provider.h"

/*
 * The cloud providers as the transport sees them.
 *
 * ai_config.h fixes each provider's host and models.  This module adds what
 * the request needs: the path, how the API key is presented, the body format
 * and the reply parser.  cloud_provider_prepare is the one place a cloud
 * request is put together, and it always produces the same shape:
 *
 *   - the host is the provider's own, taken from the table, never from the
 *     configured host (which the start check has confirmed is the same);
 *   - the port is 443 and tls is set, so the stream connects through the
 *     verified TLS adapter or not at all;
 *   - the key travels in the provider's header and nowhere else.
 *
 *   provider  path                                           key header
 *   deepseek  /chat/completions                              Authorization: Bearer
 *   openai    /v1/chat/completions                           Authorization: Bearer
 *   mistral   /v1/chat/completions                           Authorization: Bearer
 *   gemini    /v1beta/models/<model>:streamGenerateContent   x-goog-api-key
 *             ?alt=sse
 *   claude    /v1/messages                                   x-api-key, with
 *                                                            anthropic-version: 2023-06-01
 *
 * Whichever provider built the request, its reply is read with
 * sse_provider_feed and sse_provider_finish.
 */

/*
 * Whether a cloud request may be started, decided before anything touches
 * the network.  clock_synced must be true only once SNTP has set the clock
 * since boot (time_service_trusted_now): certificate validity cannot be
 * judged without the real date, so a request made earlier is refused here
 * rather than attempted.
 */
typedef enum {
    CLOUD_START_OK = 0,
    CLOUD_START_NOT_CLOUD,    /* the configuration names no cloud provider */
    CLOUD_START_BAD_ENDPOINT, /* host or port is not the provider's */
    CLOUD_START_BAD_MODEL,
    CLOUD_START_NO_KEY,
    CLOUD_START_BAD_KEY,      /* not a valid bearer token */
    CLOUD_START_CLOCK_NOT_SYNCED
} cloud_start_t;

cloud_start_t cloud_provider_start_check(const ai_config_t *config, bool clock_synced);

/* A short message for a status line; "" for CLOUD_START_OK. */
const char *cloud_provider_start_text(cloud_start_t status);

/* Room for any provider's request path. */
#define CLOUD_PROVIDER_PATH_CAPACITY 112u

/*
 * Build the request body into provider and fill request with everything but
 * the sink and the timeouts.  path is storage for the request path; it and
 * config must stay valid until http_stream_start has returned, and provider
 * until the request has ended.  *status (which may be NULL) receives the
 * start check's verdict.  Unless the result is CLOUD_PREPARE_OK, request is
 * left zeroed and names no host.
 */
typedef enum {
    CLOUD_PREPARE_OK = 0,
    CLOUD_PREPARE_NOT_READY, /* the start check refused; see *status */
    CLOUD_PREPARE_NO_BODY    /* the messages cannot form a request */
} cloud_prepare_t;

cloud_prepare_t cloud_provider_prepare(const ai_config_t *config,
                                       bool clock_synced,
                                       const ollama_message_t *messages,
                                       size_t message_count,
                                       sse_provider_t *provider,
                                       char path[CLOUD_PROVIDER_PATH_CAPACITY],
                                       http_stream_request_t *request,
                                       cloud_start_t *status);

/*
 * What an HTTP error status from a provider means, as a short fixed message,
 * or NULL when there is nothing better to say than the number.  The error
 * body is never read: it can quote the request, and nothing in it is needed.
 * provider is a "provider" configuration value.
 */
const char *cloud_provider_status_text(const char *provider, int status);

#endif
