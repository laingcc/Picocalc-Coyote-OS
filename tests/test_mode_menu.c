#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include "UI/chat_mode.h"
#include "UI/ui.h"
#include "ai/app_services.h"
#include "ai/config_store.h"
#include "keyboard_definition.h"
#include "lcdspi.h"
#include "pico/bootrom.h"
#include "pwm_sound/pwm_sound.h"
#include "test_util.h"
#include "text_mode.h"
#include "time/time_service.h"

/*
 * The HOME mode menu and the Connection settings it opens, run for real:
 * UI/ui.c and UI/chat_mode.c are the firmware sources, driven by scripted
 * key presses.  Only the hardware and the services under them are faked: the
 * display is a grid of character cells, and app_services, the configuration
 * store and text mode record what they were asked to do.
 */

/* ---- Fake display and keyboard ------------------------------------------- */

#define SCREEN_COLS (LCD_WIDTH / 8)
#define SCREEN_ROWS (LCD_HEIGHT / 12 + 1)
#define MAX_KEYS 256
#define MAX_SNAPSHOTS 8
/* Not a key: copies the screen as it stands when the UI next waits for input. */
#define SNAP (-2)

typedef char screen_t[SCREEN_ROWS][SCREEN_COLS + 1];

static screen_t screen;
static screen_t snapshots[MAX_SNAPSHOTS];
static int snapshot_count;
static int keys[MAX_KEYS];
static int key_count, key_next, key_underruns;
static char printed[512];

static void screen_reset(void) {
    for (int r = 0; r < SCREEN_ROWS; r++) {
        memset(screen[r], ' ', SCREEN_COLS);
        screen[r][SCREEN_COLS] = '\0';
    }
}

void lcd_init() { screen_reset(); }
void lcd_clear() { screen_reset(); printed[0] = '\0'; }

void lcd_print_char_at(int fc, int bc, char c, int orientation, int x, int y) {
    (void)fc; (void)bc; (void)orientation;
    if (x < 0 || y < 0 || x / 8 >= SCREEN_COLS || y / 12 >= SCREEN_ROWS) return;
    screen[y / 12][x / 8] = c;
}

/* Blank the cells whose top-left corner the rectangle covers. */
void draw_rect_spi(int x1, int y1, int x2, int y2, int c) {
    (void)c;
    for (int r = 0; r < SCREEN_ROWS; r++)
        for (int col = 0; col < SCREEN_COLS; col++)
            if (col * 8 >= x1 && col * 8 <= x2 && r * 12 >= y1 && r * 12 <= y2) screen[r][col] = ' ';
}

/* Cursor text (the calculator's) is kept as a log rather than placed. */
void lcd_print_string(char *s) {
    size_t used = strlen(printed);
    snprintf(printed + used, sizeof(printed) - used, "%s", s);
}

void lcd_set_text_color(int fc, int bc) { (void)fc; (void)bc; }
void spi_draw_pixel(uint16_t x, uint16_t y, uint32_t color) { (void)x; (void)y; (void)color; }
void set_current_x(int x) { (void)x; }
void set_current_y(int y) { (void)y; }

/* The next scripted key.  A script that runs dry answers Esc, so a flow that
 * went somewhere unexpected backs out of every menu instead of hanging, and
 * is counted so the test fails. */
int lcd_getc(uint8_t devn) {
    (void)devn;
    while (key_next < key_count && keys[key_next] == SNAP) {
        if (snapshot_count < MAX_SNAPSHOTS) memcpy(snapshots[snapshot_count++], screen, sizeof(screen_t));
        key_next++;
    }
    if (key_next < key_count) return keys[key_next++];
    key_underruns++;
    return KEY_ESC;
}

static void script_begin(void) {
    key_count = key_next = snapshot_count = 0;
}

static void press(int key) {
    if (key_count < MAX_KEYS) keys[key_count++] = key;
}

static void press_times(int key, int times) {
    for (int i = 0; i < times; i++) press(key);
}

/* Type text into a prompt and confirm it. */
static void type_line(const char *text) {
    for (; *text; text++) press((unsigned char)*text);
    press(KEY_ENTER);
}

/* True once every scripted key was read and none had to be invented. */
static bool script_consumed(void) {
    return key_next == key_count && key_underruns == 0;
}

static bool shows(const screen_t s, const char *text) {
    for (int r = 0; r < SCREEN_ROWS; r++)
        if (strstr(s[r], text) != NULL) return true;
    return false;
}

/* ---- Fake services ------------------------------------------------------- */

static ai_config_t configured, saved;
static int configure_calls, save_calls, chat_start_calls, text_redraws;
static wifi_state_t wifi_state;
static bool chat_active;
static deepseek_start_t deepseek_ready;

void app_services_poll(void) {}

int app_services_configure(const ai_config_t *config) {
    configured = *config;
    configure_calls++;
    chat_active = false;
    return 0;
}

wifi_state_t app_services_wifi_state(void) { return wifi_state; }
wifi_error_t app_services_wifi_error(void) { return WIFI_ERROR_NONE; }
int app_services_wifi_scan_start(void) { return -1; }
wifi_scan_state_t app_services_wifi_scan_state(void) { return WIFI_SCAN_ERROR; }
size_t app_services_wifi_scan_count(void) { return 0u; }
int app_services_wifi_scan_at(size_t i, char *ssid_buf, int *rssi) {
    (void)i; (void)ssid_buf; (void)rssi;
    return -1;
}

int app_services_chat_start(const ollama_message_t *messages, size_t message_count,
                            provider_event_callback_t callback, void *context) {
    (void)messages; (void)message_count; (void)callback; (void)context;
    chat_start_calls++;
    chat_active = true;
    return 0;
}

deepseek_start_t app_services_deepseek_ready(void) { return deepseek_ready; }
cloud_start_t app_services_cloud_ready(void) { return deepseek_ready; }

void app_services_chat_cancel(void) { chat_active = false; }
bool app_services_chat_active(void) { return chat_active; }
http_stream_state_t app_services_chat_state(void) { return HTTP_STREAM_ERROR; }
http_stream_error_t app_services_chat_error(void) { return HTTP_STREAM_ERROR_CANCELLED; }
int app_services_chat_status_code(void) { return 0; }

/* chat_mode.c is built with config_store_load/save renamed to these. */
ai_config_status_t chat_mode_test_load(ai_config_t *config, const char *dir) {
    (void)config; (void)dir;
    return AI_CONFIG_OK;
}

ai_config_status_t chat_mode_test_save(const ai_config_t *config, const char *dir) {
    (void)dir;
    saved = *config;
    save_calls++;
    return AI_CONFIG_OK;
}

int time_service_label(int utc_offset_minutes, char *buffer, size_t capacity) {
    (void)utc_offset_minutes;
    if (capacity) buffer[0] = '\0';
    return 0;
}

void text_mode_redraw() { text_redraws++; }
void sound_play(sound_type_t snd) { (void)snd; }
void sound_set_enabled(bool enabled) { (void)enabled; }
bool sound_is_enabled() { return false; }
void reset_usb_boot(uint32_t gpio_activity_pin_mask, uint32_t disable_interface_mask) {
    (void)gpio_activity_pin_mask; (void)disable_interface_mask;
}

/* ---- Tests --------------------------------------------------------------- */

/* Rows of the Connection settings list, top to bottom. */
enum { ROW_HOST = 2, ROW_PORT = 3, ROW_API_TOKEN = 4, ROW_PASSWORD = 6, ROW_SYSTEM_PROMPT = 11,
       ROW_TEMPERATURE = 12, ROW_UTC_OFFSET = 13, ROW_API_KEY = 14, ROW_APPLY = 15 };

/* HOME, then down to Connection settings from wherever the menu starts
 * (it starts on the current mode: Text, Calculator, Chat). */
static void open_settings_from(app_mode_t mode) {
    press_times(KEY_DOWN, mode == MODE_TEXT ? 3 : mode == MODE_CALCULATOR ? 2 : 1);
    press(KEY_ENTER);
}

static void check_calculator_is_in_front(void) {
    CHECK(ui_get_current_mode() == MODE_CALCULATOR);
    CHECK(!shows(screen, "CHAT SETTINGS"));
    CHECK(!shows(screen, " MODE "));
    CHECK(!shows(screen, "Coyote chat"));
    /* The tab bar the chat screen painted over is back, and so is the prompt. */
    CHECK(screen[25][1] == '1');
    CHECK(screen[25][16] == '4');
    CHECK(strstr(printed, "> ") != NULL);
}

static void check_apply_from_calculator(void) {
    script_begin();
    press(SNAP);
    open_settings_from(MODE_CALCULATOR);
    press(SNAP);
    press_times(KEY_DOWN, ROW_HOST); press(KEY_ENTER); type_line("10.0.0.5");
    press_times(KEY_DOWN, ROW_PASSWORD - ROW_HOST); press(KEY_ENTER); type_line("hunter2");
    press_times(KEY_DOWN, ROW_SYSTEM_PROMPT - ROW_PASSWORD); press(KEY_ENTER); type_line("Be brief.");
    press(KEY_DOWN); press(KEY_ENTER); type_line("1.5");
    press(KEY_DOWN); press(KEY_ENTER); type_line("-300");
    press(SNAP);
    press_times(KEY_DOWN, ROW_APPLY - ROW_UTC_OFFSET); press(KEY_ENTER);
    ui_show_mode_menu();

    CHECK(script_consumed());
    CHECK(snapshot_count == 3);

    /* The HOME menu offers the settings next to the three modes. */
    CHECK(shows(snapshots[0], " MODE "));
    CHECK(shows(snapshots[0], " Text "));
    CHECK(shows(snapshots[0], " Calculator "));
    CHECK(shows(snapshots[0], " Chat "));
    /* The label sits inside the frame (centred on a half cell, hence the
     * second blank) rather than over its border. */
    CHECK(shows(snapshots[0], "| Connection settings  |"));

    /* Every setting is listed, with the defaults of an unconfigured device. */
    CHECK(shows(snapshots[1], " CHAT SETTINGS "));
    CHECK(shows(snapshots[1], " Provider: ollama "));
    CHECK(shows(snapshots[1], " Model: (none) "));
    CHECK(shows(snapshots[1], " Host: (none) "));
    CHECK(shows(snapshots[1], " Port: 11434 "));
    CHECK(shows(snapshots[1], " API token: (none) "));
    CHECK(shows(snapshots[1], " SSID: (none) "));
    CHECK(shows(snapshots[1], " Password: (none) "));
    CHECK(shows(snapshots[1], " Connect ms: 15000 "));
    CHECK(shows(snapshots[1], " Request ms: 120000 "));
    CHECK(shows(snapshots[1], " Idle ms: 15000 "));
    CHECK(shows(snapshots[1], " Max predict: 384 "));
    CHECK(shows(snapshots[1], " System prompt: "));
    CHECK(shows(snapshots[1], " Temperature: 0.80 "));
    CHECK(shows(snapshots[1], " UTC offset min: 0 "));
    CHECK(shows(snapshots[1], " API key: (none) "));
    CHECK(shows(snapshots[1], " Apply & connect "));

    /* Edits show in the list; the stored password does not. */
    CHECK(shows(snapshots[2], " Host: 10.0.0.5 "));
    CHECK(shows(snapshots[2], " Password: ******** "));
    CHECK(!shows(snapshots[2], "hunter2"));
    CHECK(shows(snapshots[2], " System prompt: Be brief. "));
    CHECK(shows(snapshots[2], " Temperature: 1.50 "));
    CHECK(shows(snapshots[2], " UTC offset min: -300 "));

    /* Apply & connect configures the services and saves, once each. */
    CHECK(configure_calls == 1);
    CHECK(save_calls == 1);
    CHECK_STR_EQ(configured.host, "10.0.0.5");
    CHECK_STR_EQ(configured.password, "hunter2");
    CHECK_STR_EQ(configured.system_prompt, "Be brief.");
    CHECK(configured.temperature == 150u);
    CHECK(configured.utc_offset_minutes == -300);
    CHECK(configured.port == 11434u);
    CHECK_STR_EQ(saved.host, "10.0.0.5");
    CHECK_STR_EQ(saved.password, "hunter2");
    CHECK(saved.utc_offset_minutes == -300);

    check_calculator_is_in_front();
    CHECK(text_redraws == 0);
}

static void check_cancel_discards_the_draft(void) {
    script_begin();
    open_settings_from(MODE_CALCULATOR);
    press_times(KEY_DOWN, ROW_HOST); press(KEY_ENTER); type_line("9.9.9.9");
    press(SNAP);
    press(KEY_ESC);
    ui_show_mode_menu();

    CHECK(script_consumed());
    CHECK(snapshot_count == 1);
    CHECK(shows(snapshots[0], " Host: 9.9.9.9 "));
    CHECK(configure_calls == 1);
    CHECK(save_calls == 1);
    check_calculator_is_in_front();

    /* Reopened, the menu starts again from the applied settings. */
    script_begin();
    open_settings_from(MODE_CALCULATOR);
    press(SNAP);
    press(KEY_ESC);
    ui_show_mode_menu();

    CHECK(script_consumed());
    CHECK(snapshot_count == 1);
    CHECK(shows(snapshots[0], " Host: 10.0.0.5 "));
    CHECK(!shows(snapshots[0], "9.9.9.9"));
    CHECK(shows(snapshots[0], " UTC offset min: -300 "));
}

static void check_rejected_value_is_explained(void) {
    script_begin();
    open_settings_from(MODE_CALCULATOR);
    press_times(KEY_DOWN, ROW_PORT); press(KEY_ENTER); type_line("0");
    press(SNAP);
    press(KEY_ENTER); /* dismiss the notice */
    press(SNAP);
    press_times(KEY_DOWN, ROW_APPLY - ROW_PORT); press(KEY_ENTER);
    ui_show_mode_menu();

    CHECK(script_consumed());
    CHECK(snapshot_count == 2);
    CHECK(shows(snapshots[0], " NOT CHANGED "));
    CHECK(shows(snapshots[0], " Enter 1 to 65535 "));
    CHECK(shows(snapshots[1], " Port: 11434 "));
    CHECK(configure_calls == 2);
    CHECK(save_calls == 2);
    CHECK(configured.port == 11434u);
    check_calculator_is_in_front();
}

static void check_text_mode_stays_in_front(void) {
    script_begin();
    press(KEY_UP); press(KEY_ENTER); /* Calculator -> Text */
    ui_show_mode_menu();
    CHECK(script_consumed());
    CHECK(ui_get_current_mode() == MODE_TEXT);
    CHECK(text_redraws == 1);

    script_begin();
    open_settings_from(MODE_TEXT);
    press(SNAP);
    press(KEY_ESC);
    ui_show_mode_menu();

    CHECK(script_consumed());
    CHECK(snapshot_count == 1);
    CHECK(shows(snapshots[0], " CHAT SETTINGS "));
    CHECK(ui_get_current_mode() == MODE_TEXT);
    CHECK(text_redraws == 2);
    CHECK(configure_calls == 2);
}

static void check_chat_mode_stays_in_front(void) {
    script_begin();
    press_times(KEY_DOWN, 2); press(KEY_ENTER); /* Text -> Chat */
    ui_show_mode_menu();
    CHECK(script_consumed());
    CHECK(ui_get_current_mode() == MODE_CHAT);

    script_begin();
    open_settings_from(MODE_CHAT);
    press(SNAP);
    press(KEY_ESC);
    ui_show_mode_menu();

    CHECK(script_consumed());
    CHECK(snapshot_count == 1);
    CHECK(shows(snapshots[0], " CHAT SETTINGS "));
    CHECK(ui_get_current_mode() == MODE_CHAT);
    CHECK(shows(screen, "Coyote chat"));
    CHECK(shows(screen, "F5:menu"));
    CHECK(!shows(screen, "CHAT SETTINGS"));
    CHECK(!shows(screen, " MODE "));
    CHECK(text_redraws == 2);
    CHECK(configure_calls == 2);
}

/* Chat's own F5 menu still leads to the same settings. */
static void check_chat_menu_is_unchanged(void) {
    script_begin();
    press(SNAP);
    press_times(KEY_DOWN, 2); press(KEY_ENTER);
    press(SNAP);
    press(KEY_ESC); press(KEY_ESC);
    chat_mode_handle_input(KEY_F5);

    CHECK(script_consumed());
    CHECK(snapshot_count == 2);
    CHECK(shows(snapshots[0], "- CHAT -"));
    CHECK(shows(snapshots[0], " Model: (none) "));
    CHECK(shows(snapshots[0], " Provider: ollama "));
    CHECK(shows(snapshots[0], " Connection settings "));
    CHECK(shows(snapshots[0], " New chat "));
    CHECK(shows(snapshots[1], " CHAT SETTINGS "));
    CHECK(shows(snapshots[1], " Host: 10.0.0.5 "));
    CHECK(ui_get_current_mode() == MODE_CHAT);
    CHECK(configure_calls == 2);
}

/* Applying from the HOME menu while a reply streams ends that reply, as it
 * does from the chat menu, rather than leaving the transcript waiting. */
static void check_apply_cancels_a_streaming_reply(void) {
    script_begin();
    press(KEY_ENTER); type_line("m"); /* F5 > Model */
    press(KEY_ESC);
    chat_mode_handle_input(KEY_F5);
    CHECK(script_consumed());
    CHECK(configure_calls == 3);
    CHECK_STR_EQ(configured.model, "m");

    wifi_state = WIFI_STATE_ONLINE;
    chat_mode_handle_input('h');
    chat_mode_handle_input('i');
    chat_mode_handle_input(KEY_ENTER);
    CHECK(chat_start_calls == 1);
    CHECK(shows(screen, "streaming"));

    script_begin();
    open_settings_from(MODE_CHAT);
    press_times(KEY_DOWN, ROW_APPLY); press(KEY_ENTER);
    ui_show_mode_menu();

    CHECK(script_consumed());
    CHECK(configure_calls == 4);
    CHECK(save_calls == 4);
    CHECK(ui_get_current_mode() == MODE_CHAT);
    CHECK(!shows(screen, "streaming"));
    CHECK(shows(screen, "online   m F5:menu"));
    CHECK(shows(screen, "> hi"));
}

/* Not a real key: a made-up value in the shape of one. */
#define TEST_KEY "sk-0000000000000000000000000000test"

/* Choosing DeepSeek from the chat menu, giving it a key, and what the chat
 * does with it. */
static void check_deepseek_setup(void) {
    int before_configure = configure_calls, before_start = chat_start_calls;

    script_begin();
    press(KEY_DOWN); press(KEY_ENTER);  /* F5 > Provider */
    press(SNAP);
    press(KEY_DOWN); press(KEY_ENTER);  /* deepseek */
    press(SNAP);
    press(KEY_DOWN); press(KEY_ENTER);  /* Connection settings */
    press_times(KEY_DOWN, ROW_API_KEY); press(KEY_ENTER);
    for (const char *c = TEST_KEY; *c; c++) press((unsigned char)*c);
    press(SNAP);                        /* the prompt, fully typed */
    press(KEY_ENTER);
    press(SNAP);                        /* the plain-text warning */
    press(KEY_ENTER);
    press(SNAP);                        /* back in the list */
    press(KEY_DOWN); press(KEY_ENTER);  /* Apply & connect */
    press(KEY_ESC);
    chat_mode_handle_input(KEY_F5);

    CHECK(script_consumed());
    CHECK(snapshot_count == 5);
    CHECK(shows(snapshots[0], " PROVIDER "));
    CHECK(shows(snapshots[0], " ollama "));
    CHECK(shows(snapshots[0], " deepseek "));
    /* The provider brought DeepSeek's endpoint and default model with it. */
    CHECK(shows(snapshots[1], " Provider: deepseek "));
    CHECK(shows(snapshots[1], " Model: deepseek-chat "));
    /* The key is never on screen: not while typed, not in the list. */
    CHECK(shows(snapshots[2], " API key "));
    CHECK(shows(snapshots[2], "**********_"));
    CHECK(shows(snapshots[3], " Kept unencrypted on SD "));
    CHECK(shows(snapshots[4], " API key: ******** "));
    CHECK(shows(snapshots[4], " Host: api.deepseek.com "));
    CHECK(shows(snapshots[4], " Port: 443 "));
    for (int i = 0; i < snapshot_count; i++) {
        CHECK(!shows(snapshots[i], "sk-"));
        CHECK(!shows(snapshots[i], "00000000"));
        CHECK(!shows(snapshots[i], "test"));
    }
    CHECK(!shows(screen, "sk-"));

    CHECK(configure_calls == before_configure + 2);
    CHECK_STR_EQ(configured.provider, "deepseek");
    CHECK_STR_EQ(configured.host, "api.deepseek.com");
    CHECK(configured.port == 443u);
    CHECK_STR_EQ(configured.model, "deepseek-chat");
    CHECK_STR_EQ(configured.bearer_token, TEST_KEY);
    CHECK_STR_EQ(saved.bearer_token, TEST_KEY);

    /* The host cannot be pointed elsewhere while DeepSeek is the provider. */
    script_begin();
    open_settings_from(MODE_CHAT);
    press_times(KEY_DOWN, ROW_HOST); press(KEY_ENTER); type_line("evil.example");
    press(SNAP);
    press(KEY_ENTER); /* dismiss the notice */
    press(SNAP);
    press(KEY_ESC);
    ui_show_mode_menu();
    CHECK(script_consumed());
    CHECK(shows(snapshots[0], " NOT CHANGED "));
    CHECK(shows(snapshots[1], " Host: api.deepseek.com "));

    /* The model is picked from the known two, not typed. */
    script_begin();
    press(KEY_ENTER);                   /* F5 > Model */
    press(SNAP);
    press(KEY_DOWN); press(KEY_ENTER);  /* deepseek-reasoner */
    press(KEY_ESC);
    chat_mode_handle_input(KEY_F5);
    CHECK(script_consumed());
    CHECK(shows(snapshots[0], " MODEL "));
    CHECK(shows(snapshots[0], " deepseek-chat "));
    CHECK(shows(snapshots[0], " deepseek-reasoner "));
    CHECK_STR_EQ(configured.model, "deepseek-reasoner");

    /* Until the clock has been set over the network nothing is sent, and the
     * status line says why. */
    deepseek_ready = DEEPSEEK_START_CLOCK_NOT_SYNCED;
    chat_mode_handle_input('h');
    chat_mode_handle_input(KEY_ENTER);
    CHECK(chat_start_calls == before_start);
    CHECK(shows(screen, "clock not synced: wait"));
    deepseek_ready = DEEPSEEK_START_NO_KEY;
    chat_mode_handle_input(KEY_ENTER);
    CHECK(chat_start_calls == before_start);
    CHECK(shows(screen, "set the API key (F5)"));

    deepseek_ready = DEEPSEEK_START_OK;
    chat_mode_handle_input(KEY_ENTER);
    CHECK(chat_start_calls == before_start + 1);
    CHECK(shows(screen, "streaming"));
    chat_mode_handle_input(KEY_ESC);
    CHECK(!shows(screen, "sk-"));
}

static void check_usb_disk_row(void) {
    script_begin();
    ui_set_current_mode(MODE_CALCULATOR);
    CHECK(!ui_take_usb_disk_request());

    press(SNAP);
    press_times(KEY_DOWN, 3); press(KEY_ENTER); /* Calculator -> USB disk */
    ui_show_mode_menu();

    CHECK(script_consumed());
    CHECK(snapshot_count == 1);
    CHECK(shows(snapshots[0], " MODE "));
    CHECK(shows(snapshots[0], " Text "));
    CHECK(shows(snapshots[0], " USB disk "));

    /* Choosing it asks for the card rather than switching modes: only main.c
     * can unmount the filesystem and hand it over. */
    CHECK(ui_take_usb_disk_request());
    CHECK(!ui_take_usb_disk_request());
    CHECK(ui_get_current_mode() == MODE_CALCULATOR);
}

/* The API token is warned about before it is typed, masked in the list
 * afterwards, and reaches the services and the saved file unchanged. */
static void check_api_token_is_warned_and_masked(void) {
    int configured_before = configure_calls, saved_before = save_calls;

    script_begin();
    open_settings_from(MODE_CHAT);
    press_times(KEY_DOWN, ROW_API_TOKEN); press(KEY_ENTER);
    press(SNAP);
    press(KEY_ENTER); /* dismiss the notice */
    type_line("0f1e2d3c4b5a69788796a5b4c3d2e1f0");
    press(SNAP);
    press_times(KEY_DOWN, ROW_APPLY - ROW_API_TOKEN); press(KEY_ENTER);
    ui_show_mode_menu();

    CHECK(script_consumed());
    CHECK(snapshot_count == 2);
    CHECK(shows(snapshots[0], " API TOKEN "));
    CHECK(shows(snapshots[0], " Saved as plain text on SD "));
    CHECK(shows(snapshots[1], " API token: ******** "));
    CHECK(!shows(snapshots[1], "0f1e2d3c"));
    CHECK(!shows(screen, "0f1e2d3c"));
    CHECK(configure_calls == configured_before + 1);
    CHECK(save_calls == saved_before + 1);
    CHECK(strcmp(configured.api_token, "0f1e2d3c4b5a69788796a5b4c3d2e1f0") == 0);
    CHECK(strcmp(saved.api_token, "0f1e2d3c4b5a69788796a5b4c3d2e1f0") == 0);

    /* A token with a blank in it is refused and the old one is kept. */
    script_begin();
    open_settings_from(MODE_CHAT);
    press_times(KEY_DOWN, ROW_API_TOKEN); press(KEY_ENTER);
    press(KEY_ENTER);
    type_line("two words");
    press(SNAP);
    press(KEY_ENTER); /* dismiss the rejection */
    press(KEY_ESC);
    ui_show_mode_menu();

    CHECK(script_consumed());
    CHECK(snapshot_count == 1);
    CHECK(shows(snapshots[0], " NOT CHANGED "));
    CHECK(shows(snapshots[0], " Not a valid value "));
    CHECK(configure_calls == configured_before + 1);
    CHECK(strcmp(configured.api_token, "0f1e2d3c4b5a69788796a5b4c3d2e1f0") == 0);
}

void test_mode_menu(void) {
    ui_init();
    chat_mode_init();
    CHECK(ui_get_current_mode() == MODE_CALCULATOR);
    CHECK(configure_calls == 0);

    check_apply_from_calculator();
    check_cancel_discards_the_draft();
    check_rejected_value_is_explained();
    check_text_mode_stays_in_front();
    check_chat_mode_stays_in_front();
    check_chat_menu_is_unchanged();
    check_apply_cancels_a_streaming_reply();
    check_deepseek_setup();
    check_usb_disk_row();
    check_api_token_is_warned_and_masked();
}
