cmake_minimum_required(VERSION 3.13)

# Standalone host-native test project for the AI protocol core.  It does not
# require the Pico SDK; run it directly:
#
#   cmake -S tests -B build-tests
#   cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure

project(coyote_ai_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(COYOTE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(coyote_ai STATIC
    "${COYOTE_ROOT}/ai/ai_config.c"
    "${COYOTE_ROOT}/ai/json_stream.c"
    "${COYOTE_ROOT}/ai/http_parser.c"
    "${COYOTE_ROOT}/ai/ollama_provider.c"
    "${COYOTE_ROOT}/ai/chat_model.c"
    "${COYOTE_ROOT}/ai/http_stream.c"
    "${COYOTE_ROOT}/ai/wifi_manager.c"
)
target_include_directories(coyote_ai PUBLIC "${COYOTE_ROOT}")
target_compile_options(coyote_ai PRIVATE -Wall -Wextra -Werror)

enable_testing()

add_executable(coyote_ai_tests
    test_main.c
    test_json_stream.c
    test_http_parser.c
    test_ollama_provider.c
    test_chat_model.c
    test_ai_config.c
    test_http_stream.c
)
target_link_libraries(coyote_ai_tests PRIVATE coyote_ai)
target_compile_options(coyote_ai_tests PRIVATE -Wall -Wextra -Werror)

add_test(NAME coyote_ai_tests COMMAND coyote_ai_tests)
