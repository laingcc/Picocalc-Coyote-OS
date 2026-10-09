#include <stdio.h>

#include "test_util.h"

int g_tests_run = 0;
int g_tests_failed = 0;

void test_json_stream(void);
void test_http_parser(void);
void test_ollama_provider(void);
void test_chat_model(void);
void test_chat_request(void);
void test_chat_settings(void);
void test_chat_layout(void);
void test_ai_config(void);
void test_config_store(void);
void test_http_stream(void);
void test_wifi_scan(void);
void test_time_sync(void);

int main(void) {
    test_json_stream();
    test_http_parser();
    test_ollama_provider();
    test_chat_model();
    test_chat_request();
    test_chat_settings();
    test_chat_layout();
    test_ai_config();
    test_config_store();
    test_http_stream();
    test_wifi_scan();
    test_time_sync();

    printf("%d checks, %d failures\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
