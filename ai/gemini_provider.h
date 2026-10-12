#ifndef COYOTE_AI_GEMINI_PROVIDER_H
#define COYOTE_AI_GEMINI_PROVIDER_H

#include <stddef.h>

#include "ai/sse_provider.h"

/*
 * Direct Gemini provider: the Gemini API's streaming endpoint,
 *
 *   POST https://generativelanguage.googleapis.com
 *        /v1beta/models/<model>:streamGenerateContent?alt=sse
 *
 * with the key in the "x-goog-api-key" header.  The key is never put in the
 * URL: the query string holds only "alt=sse", which asks for server-sent
 * events.
 *
 * Request
 * -------
 *   {["systemInstruction":{"parts":[{"text":S}]},]
 *    "contents":[{"role":R,"parts":[{"text":C}]},...],
 *    "generationConfig":{"maxOutputTokens":N,
 *                        "thinkingConfig":{"thinkingLevel":"LOW"}}}
 *
 * Roles are "user" and "model" (an "assistant" message is sent as "model").
 * No temperature is sent: Google asks that the current models be left at
 * their default.  They think before answering and maxOutputTokens covers the
 * thinking, so N is the configured limit plus
 * PROVIDER_REASONING_HEADROOM_TOKENS.
 *
 * Reply stream (framing in sse_provider.h)
 * ----------------------------------------
 * Every field line must be "data: {json}", one GenerateContentResponse.  The
 * text of each candidates[].content.parts[] entry becomes a CONTENT event,
 * unless the part is marked "thought": true.  A response with no candidates
 * or no parts (usage figures only) produces nothing.
 *
 * Gemini sends no end marker.  The reply is complete when a candidate carries
 * a finishReason: STOP or MAX_TOKENS is the normal end and emits DONE; any
 * other reason (SAFETY, RECITATION, ...) emits ERROR.  A promptFeedback with a
 * blockReason, or an "error" member, also emits ERROR.  Anything after the
 * end, and the end of input before a finishReason, fails the stream.
 *
 * The reply really is incremental, but in few, large records, so a line may
 * be up to GEMINI_LINE_MAX bytes; a longer one fails the stream.
 */

#define GEMINI_LINE_MAX SSE_LINE_CAPACITY
#define GEMINI_API_KEY_HEADER "x-goog-api-key"
#define GEMINI_PATH_PREFIX "/v1beta/models/"
#define GEMINI_PATH_SUFFIX ":streamGenerateContent?alt=sse"
/* Room for the path of the longest model name a configuration can hold. */
#define GEMINI_PATH_CAPACITY 112u

#define GEMINI_ERROR_SERVER "Gemini error"
#define GEMINI_ERROR_PROMPT_BLOCKED "prompt blocked by Gemini"
#define GEMINI_ERROR_REPLY_BLOCKED "reply stopped by Gemini"

/*
 * Serialise a request.  Returns 0 on success, -1 if model is NULL or empty, a
 * role is not "user" or "assistant", the message count exceeds
 * OLLAMA_MAX_MESSAGES, message_count is non-zero with a NULL messages array,
 * max_tokens is not positive, or the request does not fit.  system_prompt may
 * be NULL or empty to omit it.  temperature is accepted for symmetry with the
 * other builders and not sent.  The model is named by the path, not the body;
 * the caller checks it against its allowlist.
 */
int gemini_provider_build_request(sse_provider_t *provider,
                                  const char *model,
                                  const ollama_message_t *messages,
                                  size_t message_count,
                                  int max_tokens,
                                  const char *system_prompt,
                                  int temperature);

/* Write the request path for model.  Returns 0, or -1 if model is NULL,
 * empty, holds anything but letters, digits, "-", "." and "_", or does not
 * fit. */
int gemini_provider_path(const char *model, char *out, size_t capacity);

#endif
