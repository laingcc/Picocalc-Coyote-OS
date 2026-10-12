#include <string.h>

#include "ai/cloud_provider.h"
#include "test_util.h"

static const ollama_message_t hello[] = {{"user", "hi"}};

static void select(ai_config_t *config, const char *name) {
    CHECK(ai_config_init(config) == AI_CONFIG_OK);
    CHECK(ai_config_set_provider(config, name) == AI_CONFIG_OK);
    strcpy(config->bearer_token, "key-123");
}

static void check_prepare(const char *name, const char *host, const char *path,
                          const char *key_header, const char *extra_header) {
    ai_config_t config;
    sse_provider_t provider;
    char actual_path[CLOUD_PROVIDER_PATH_CAPACITY];
    http_stream_request_t request;
    cloud_start_t status = CLOUD_START_NOT_CLOUD;

    select(&config, name);
    sse_provider_init(&provider, NULL, NULL);
    CHECK(cloud_provider_prepare(&config, true, hello, 1u, &provider, actual_path, &request, &status) == CLOUD_PREPARE_OK);
    CHECK(status == CLOUD_START_OK);
    CHECK_STR_EQ(request.host, host);
    CHECK(request.port == 443u && request.tls);
    CHECK_STR_EQ(request.path, path);
    CHECK_STR_EQ(request.method, "POST");
    CHECK_STR_EQ(request.content_type, "application/json");
    if (key_header == NULL) {
        CHECK_STR_EQ(request.bearer_token, "key-123");
        CHECK(request.api_key == NULL && request.api_key_header == NULL);
    } else {
        CHECK(request.bearer_token == NULL);
        CHECK_STR_EQ(request.api_key_header, key_header);
        CHECK_STR_EQ(request.api_key, "key-123");
    }
    if (extra_header == NULL) CHECK(request.extra_header == NULL);
    else CHECK_STR_EQ(request.extra_header, extra_header);
}

static void check_all_providers(void) {
    check_prepare("deepseek", "api.deepseek.com", "/chat/completions", NULL, NULL);
    check_prepare("openai", "api.openai.com", "/v1/chat/completions", NULL, NULL);
    check_prepare("mistral", "api.mistral.ai", "/v1/chat/completions", NULL, NULL);
    check_prepare("gemini", "generativelanguage.googleapis.com",
                  "/v1beta/models/gemini-3.8-flash:streamGenerateContent?alt=sse", "x-goog-api-key", NULL);
    check_prepare("claude", "api.anthropic.com", "/v1/messages", "x-api-key", "anthropic-version: 2023-06-01");
}

static void check_rejections(void) {
    ai_config_t config;
    sse_provider_t provider;
    char path[CLOUD_PROVIDER_PATH_CAPACITY];
    http_stream_request_t request;
    cloud_start_t status;

    CHECK(ai_config_init(&config) == AI_CONFIG_OK);
    CHECK(cloud_provider_start_check(&config, true) == CLOUD_START_NOT_CLOUD);
    select(&config, "openai");
    config.bearer_token[0] = '\0';
    CHECK(cloud_provider_start_check(&config, true) == CLOUD_START_NO_KEY);
    strcpy(config.bearer_token, "bad key");
    CHECK(cloud_provider_start_check(&config, true) == CLOUD_START_BAD_KEY);
    strcpy(config.bearer_token, "key");
    strcpy(config.host, "evil.example");
    CHECK(cloud_provider_start_check(&config, true) == CLOUD_START_BAD_ENDPOINT);
    strcpy(config.host, AI_CONFIG_OPENAI_HOST);
    CHECK(cloud_provider_start_check(&config, false) == CLOUD_START_CLOCK_NOT_SYNCED);
    sse_provider_init(&provider, NULL, NULL);
    CHECK(cloud_provider_prepare(&config, false, hello, 1u, &provider, path, &request, &status) == CLOUD_PREPARE_NOT_READY);
    CHECK(status == CLOUD_START_CLOCK_NOT_SYNCED);
    CHECK(request.host == NULL);
    CHECK_STR_EQ(cloud_provider_status_text("openai", 401), "API key rejected");
    CHECK_STR_EQ(cloud_provider_status_text("gemini", 400), "API key or request refused");
    CHECK(cloud_provider_status_text("ollama", 401) == NULL);
}

void test_cloud_provider(void);
void test_cloud_provider(void) {
    check_all_providers();
    check_rejections();
}
