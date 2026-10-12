#ifndef COYOTE_AI_CLAUDE_PROVIDER_H
#define COYOTE_AI_CLAUDE_PROVIDER_H

#include <stddef.h>

#include "ai/sse_provider.h"

/*
 * Direct Claude provider: Anthropic's Messages API with streaming,
 * POST https://api.anthropic.com/v1/messages, sent with the "x-api-key"
 * header and "anthropic-version: 2023-06-01".
 *
 * Request
 * -------
 *   {"model":M,"max_tokens":N,"stream":true,"output_config":{"effort":"low"},
 *    ["system":S,]"messages":[{"role":R,"content":C},...]}
 *
 * The system prompt is the top-level "system" member; messages carry only the
 * "user" and "assistant" roles.  No temperature is sent: the current models
 * refuse it.  They think before answering, and max_tokens covers the thinking
 * as well as the reply, so N is the configured limit plus
 * PROVIDER_REASONING_HEADROOM_TOKENS; "effort":"low" keeps the thinking short.
 *
 * Reply stream (framing in sse_provider.h)
 * ----------------------------------------
 * Each event is an "event: NAME" line, a "data: {json}" line and a blank
 * line.  The JSON must carry "type" equal to NAME.  The events:
 *
 *   message_start         must come before any content
 *   content_block_delta   a delta of type "text_delta" becomes a CONTENT event
 *                         with its text; any other delta type (thinking,
 *                         signature, tool input, ...) produces nothing
 *   message_delta         its stop_reason is noted
 *   message_stop          the end: DONE, or ERROR if stop_reason was "refusal"
 *   error                 ERROR with a fixed message chosen by the error type
 *   ping, content_block_start, content_block_stop and any event not listed
 *                         are checked for form and otherwise ignored
 *
 * Text is taken from nothing but a content_block_delta event whose data is a
 * content_block_delta with a text_delta.  A data line with no event line, two
 * in one event, an event with no data, a type that does not match its event,
 * content before message_start, any other field line, and anything after
 * message_stop all fail the stream, as does the end of input before
 * message_stop.
 *
 * A thinking block ends with a signature that can be longer than a line may
 * be.  An over-long data line is let through unread only when it belongs to
 * an event that never carries text, or is a content_block_delta whose delta
 * type, read from what was kept of the line, is not "text_delta".
 */

#define CLAUDE_LINE_MAX 2048u
#define CLAUDE_API_VERSION_HEADER "anthropic-version: 2023-06-01"
#define CLAUDE_API_KEY_HEADER "x-api-key"
#define CLAUDE_PATH "/v1/messages"

#define CLAUDE_ERROR_SERVER "Claude error"
#define CLAUDE_ERROR_OVERLOADED "Claude busy: try later"
#define CLAUDE_ERROR_RATE_LIMITED "rate limited: try later"
#define CLAUDE_ERROR_REFUSED "Claude declined the request"

/*
 * Serialise a request.  Returns 0 on success, -1 if model is NULL or empty, a
 * role is not "user" or "assistant", the message count exceeds
 * OLLAMA_MAX_MESSAGES, message_count is non-zero with a NULL messages array,
 * max_tokens is not positive, or the request does not fit.  system_prompt may
 * be NULL or empty to omit it.  temperature is accepted for symmetry with the
 * other builders and not sent.  The caller checks the model against its
 * allowlist.
 */
int claude_provider_build_request(sse_provider_t *provider,
                                  const char *model,
                                  const ollama_message_t *messages,
                                  size_t message_count,
                                  int max_tokens,
                                  const char *system_prompt,
                                  int temperature);

#endif
