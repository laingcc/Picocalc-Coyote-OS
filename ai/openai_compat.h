#ifndef COYOTE_AI_OPENAI_COMPAT_H
#define COYOTE_AI_OPENAI_COMPAT_H

#include <stdbool.h>
#include <stddef.h>

#include "ai/sse_provider.h"

/*
 * The OpenAI-style streaming chat completion protocol, shared by the
 * providers that speak it: DeepSeek, OpenAI and Mistral.  They differ only in
 * a few request fields, which an openai_compat_dialect_t names; the reply
 * parser is one piece of code for all three.
 *
 * Request
 * -------
 *   {"model":M,"messages":[{"role":R,"content":C},...],"stream":true,
 *    <limit_key>:N[,"temperature":T][<tail>]}
 *
 * Reply stream (framing in sse_provider.h)
 * ----------------------------------------
 *   data: {json}        one chat.completion.chunk object
 *   data: [DONE]        the terminal marker
 *
 * Any other field line is refused.  A chunk's choices[].delta.content becomes
 * a CONTENT event when it is a non-empty string.  Mistral alone may instead
 * send an array of content chunks: each {"type":"text","text":...} entry
 * becomes a CONTENT event and entries of any other type, such as "thinking",
 * are skipped.  For the others a content that is not a string is malformed.  A delta
 * carrying only a role, a null or empty content, a refusal or
 * reasoning_content produces no event, and neither does a chunk whose choices
 * array is empty (the usage record).  A chunk with an "error" member becomes
 * an ERROR event.
 *
 * Each chunk is validated as a whole before anything is taken from it, so no
 * content is ever delivered out of a malformed record.
 */

#define OPENAI_COMPAT_LINE_MAX 2048u

/* Reasoning models spend output tokens thinking before they answer, and the
 * limit sent covers both.  For those providers the configured reply limit is
 * raised by this much so that a small limit still leaves room for the reply. */
#define PROVIDER_REASONING_HEADROOM_TOKENS 1024u

typedef struct {
    const char *system_role;  /* role of the leading system prompt message */
    const char *limit_key;    /* the output token limit's member name */
    unsigned limit_headroom;  /* added to the limit */
    int temperature_max;      /* hundredths; higher values are capped.  Negative: not sent */
    const char *tail;         /* further members, each led by a comma; "" for none */
    const char *error_text;   /* reported for an error chunk */
    bool show_error_message;  /* report the server's own message instead, when it has one */
    bool content_parts;       /* a delta's content may be an array of content chunks */
    size_t request_max;       /* body size cap */
} openai_compat_dialect_t;

extern const openai_compat_dialect_t openai_compat_deepseek;
extern const openai_compat_dialect_t openai_compat_openai;
extern const openai_compat_dialect_t openai_compat_mistral;

/*
 * Serialise a request.  Returns 0 on success, -1 if model is NULL or empty, a
 * role is not "system", "user" or "assistant", the message count exceeds
 * OLLAMA_MAX_MESSAGES, message_count is non-zero with a NULL messages array,
 * max_tokens is not positive, temperature is outside 0..200 (hundredths), or
 * the request does not fit.  system_prompt may be NULL or empty to omit the
 * leading system message.  The caller checks the model against its allowlist.
 */
int openai_compat_build_request(sse_provider_t *provider,
                                const openai_compat_dialect_t *dialect,
                                const char *model,
                                const ollama_message_t *messages,
                                size_t message_count,
                                int max_tokens,
                                const char *system_prompt,
                                int temperature);

#endif
