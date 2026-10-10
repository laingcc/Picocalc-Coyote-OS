#include "chat_mode.h"
#include "ui.h"
#include "lcdspi.h"
#include "keyboard_definition.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include "ai/app_services.h"
#include "ai/chat_layout.h"
#include "ai/chat_model.h"
#include "ai/chat_request.h"
#include "ai/chat_settings.h"
#include "ai/config_store.h"
#include "time/time_service.h"

#define COLS (LCD_WIDTH / 8)
#define ROW_H 12
#define STATUS_ROW 0
#define TRANSCRIPT_ROW 1
#define TRANSCRIPT_ROWS 22
#define COMPOSER_ROW (TRANSCRIPT_ROW + TRANSCRIPT_ROWS)
#define COMPOSER_ROWS 3
#define GUTTER 2
#define TEXT_COLS (COLS - GUTTER)
#define SCROLL_STEP 4
#define STREAM_REDRAW_MS 100
#define NOTE_MAX 30
#define CONFIG_DIR "/coyote"

/* All chat state is static: the transcript alone is ~36 KB. */
static chat_model_t chat;
static chat_request_measure_t measure;
static ai_config_t config;  /* the applied settings */
static ai_config_t draft;   /* settings being edited; wiped when the menu closes */
static char note[NOTE_MAX + 1];
static char status_drawn[COLS + 1];
static char setting_labels[CHAT_SETTING_COUNT + 1][32];
static char scan_labels[WIFI_SCAN_MAX_NETWORKS][32];
static size_t scroll;       /* rows scrolled back from the newest line */
static uint32_t transcript_drawn_ms;
static bool transcript_dirty, composer_dirty;

static void sync_with_transport(void);

static void set_note(const char *text) { snprintf(note, sizeof(note), "%s", text); }

static void draw_cells(int row, int col, const char *text, size_t len, int width, int fg, int bg) {
    for (int i = 0; i < width; i++) {
        char ch = (size_t)i < len ? text[i] : ' ';
        if (ch < 32 || ch > 126) ch = '?';
        lcd_print_char_at(fg, bg, ch, 0, (col + i) * 8, row * ROW_H);
    }
}

static const char *wifi_label(void) {
    switch (app_services_wifi_state()) {
        case WIFI_STATE_OFF: return "off";
        case WIFI_STATE_NEEDS_CONFIG: return "no SSID";
        case WIFI_STATE_CONNECTING: return "joining";
        case WIFI_STATE_ONLINE: return "online";
        case WIFI_STATE_BACKOFF: return "retry";
        default: return app_services_wifi_error() == WIFI_ERROR_AUTH ? "bad auth" : "error";
    }
}

/* The left of the status line: Wi-Fi state, then the model and what the chat
 * is doing.  A note (usually an error) takes the model's place so it is not
 * cut short. */
static void format_status_text(char *line, size_t capacity) {
    if (note[0]) snprintf(line, capacity, "%-8s %s", wifi_label(), note);
    else snprintf(line, capacity, "%-8s %.13s %s", wifi_label(), config.model[0] ? config.model : "(no model)",
                  chat_model_is_streaming(&chat) ? "streaming" : "F5:menu");
}

/* Pad line out to COLS and put the clock (local time) at its right edge,
 * unless the text is long enough to need the room. */
static void place_status_clock(char *line) {
    char clock[TIME_SYNC_LABEL_CAPACITY];
    size_t len = strlen(line), clock_len;

    time_service_label(config.utc_offset_minutes, clock, sizeof(clock));
    clock_len = strlen(clock);
    if (len + 1 + clock_len > COLS) return;
    memset(line + len, ' ', COLS - len);
    memcpy(line + COLS - clock_len, clock, clock_len + 1);
}

static void draw_status(bool force) {
    char line[COLS + 1];

    format_status_text(line, sizeof(line));
    place_status_clock(line);
    if (!force && strcmp(line, status_drawn) == 0) return;
    strcpy(status_drawn, line);
    draw_cells(STATUS_ROW, 0, line, strlen(line), COLS, WHITE, GRAY);
}

/* Rows a message occupies: its wrapped text, plus one marker row when the
 * reply is still empty ("...") or was cut short ("[incomplete]"). */
static size_t message_rows(const chat_message_t *m) {
    size_t rows = chat_layout_count_lines(m->text, m->length, TEXT_COLS);
    return rows + ((m->partial || m->length == 0) ? 1 : 0);
}

static void draw_transcript(void) {
    size_t count = chat_model_message_count(&chat), total = 0, row = 0, first, max_scroll;
    int drawn = 0;

    if (count == 0) {
        static const char *const hint[] = {"Coyote chat", "", "Type a message; Enter sends it.",
                                           "Esc cancels a reply.  Up/Down scroll.", "F5: model, Wi-Fi and settings."};
        for (int i = 0; i < TRANSCRIPT_ROWS; i++) {
            const char *text = i < 5 ? hint[i] : "";
            draw_cells(TRANSCRIPT_ROW + i, 0, text, strlen(text), COLS, GRAY, BLACK);
        }
        transcript_dirty = false;
        return;
    }

    for (size_t i = 0; i < count; i++) total += message_rows(chat_model_message_at(&chat, i));
    max_scroll = total > TRANSCRIPT_ROWS ? total - TRANSCRIPT_ROWS : 0;
    if (scroll > max_scroll) scroll = max_scroll;
    first = max_scroll - scroll;

    for (size_t i = 0; i < count && drawn < TRANSCRIPT_ROWS; i++) {
        const chat_message_t *m = chat_model_message_at(&chat, i);
        bool user = m->role == CHAT_ROLE_USER;
        int fg = user ? CYAN : WHITE;
        size_t offset = 0;
        bool lead = true;
        while (offset < m->length && drawn < TRANSCRIPT_ROWS) {
            size_t len, next = chat_layout_next_line(m->text, m->length, offset, TEXT_COLS, &len);
            if (row >= first) {
                draw_cells(TRANSCRIPT_ROW + drawn, 0, ">", lead && user ? 1 : 0, GUTTER, fg, BLACK);
                draw_cells(TRANSCRIPT_ROW + drawn, GUTTER, m->text + offset, len, TEXT_COLS, fg, BLACK);
                drawn++;
            }
            row++; offset = next; lead = false;
        }
        if ((m->partial || m->length == 0) && drawn < TRANSCRIPT_ROWS) {
            const char *mark = m->partial ? "[incomplete]" : "...";
            if (row >= first) {
                draw_cells(TRANSCRIPT_ROW + drawn, 0, "", 0, GUTTER, fg, BLACK);
                draw_cells(TRANSCRIPT_ROW + drawn, GUTTER, mark, strlen(mark), TEXT_COLS, m->partial ? RED : GRAY, BLACK);
                drawn++;
            }
            row++;
        }
    }
    for (; drawn < TRANSCRIPT_ROWS; drawn++) draw_cells(TRANSCRIPT_ROW + drawn, 0, "", 0, COLS, WHITE, BLACK);
    transcript_dirty = false;
    transcript_drawn_ms = to_ms_since_boot(get_absolute_time());
}

/* The composer shows "> ", the tail of the text that fits, and a cursor. */
static void draw_composer(void) {
    const char *text = chat_model_composer_text(&chat);
    size_t len = chat_model_composer_length(&chat), room = COLS * COMPOSER_ROWS - 3;
    char cells[COLS * COMPOSER_ROWS];
    size_t shown = len > room ? room : len;

    memset(cells, ' ', sizeof(cells));
    cells[0] = '>';
    memcpy(cells + 2, text + (len - shown), shown);
    cells[2 + shown] = '_';
    for (int i = 0; i < COMPOSER_ROWS; i++)
        draw_cells(COMPOSER_ROW + i, 0, cells + i * COLS, COLS, COLS, BLACK, WHITE);
    composer_dirty = false;
}

void chat_mode_redraw(void) {
    sync_with_transport();
    draw_status(true);
    draw_transcript();
    draw_composer();
    draw_rect_spi(0, (COMPOSER_ROW + COMPOSER_ROWS) * ROW_H, LCD_WIDTH - 1, LCD_HEIGHT - 1, WHITE);
}

/* Repaint what changed.  While a reply streams the transcript is repainted
 * at most every STREAM_REDRAW_MS so drawing never starves the network. */
static void refresh(void) {
    if (transcript_dirty && (!chat_model_is_streaming(&chat) ||
            to_ms_since_boot(get_absolute_time()) - transcript_drawn_ms >= STREAM_REDRAW_MS))
        draw_transcript();
    if (composer_dirty) draw_composer();
    draw_status(false);
}

static const char *stream_error_text(http_stream_error_t error) {
    switch (error) {
        case HTTP_STREAM_ERROR_NETWORK_DOWN: return "network down";
        case HTTP_STREAM_ERROR_DNS: return "host not found";
        case HTTP_STREAM_ERROR_CONNECT: return "connect failed";
        case HTTP_STREAM_ERROR_CONNECT_TIMEOUT: return "connect timeout";
        case HTTP_STREAM_ERROR_SEND: return "send failed";
        case HTTP_STREAM_ERROR_IDLE_TIMEOUT: return "idle timeout";
        case HTTP_STREAM_ERROR_TIMEOUT: return "request timeout";
        case HTTP_STREAM_ERROR_REMOTE_CLOSED: return "connection closed";
        case HTTP_STREAM_ERROR_PROTOCOL: return "bad HTTP reply";
        case HTTP_STREAM_ERROR_SINK: return "bad reply stream";
        case HTTP_STREAM_ERROR_CANCELLED: return "cancelled";
        default: return "request failed";
    }
}

/* Runs inside app_services_poll, possibly while a menu is open or another
 * mode is in front, so it only updates the transcript and flags a repaint. */
static void on_provider_event(void *context, const provider_event_t *event) {
    (void)context;
    if (event == NULL || !chat_model_is_streaming(&chat)) return;
    switch (event->type) {
        case PROVIDER_EVENT_CONTENT:
            if (chat_model_append_response(&chat, event->data, event->length) != 0) {
                app_services_chat_cancel();
                chat_model_fail_response(&chat);
                set_note("reply too long");
            }
            break;
        case PROVIDER_EVENT_DONE:
            chat_model_complete_response(&chat);
            break;
        case PROVIDER_EVENT_ERROR:
            chat_model_fail_response(&chat);
            snprintf(note, sizeof(note), "%.*s", event->data ? (int)(event->length > NOTE_MAX ? NOTE_MAX : event->length) : 0,
                     event->data ? event->data : "");
            if (!note[0]) set_note("server error");
            break;
    }
    transcript_dirty = true;
}

/* A request that dies in the transport (timeout, refused, link lost) ends
 * without a provider event; settle the transcript from the stream's outcome. */
static void sync_with_transport(void) {
    if (!chat_model_is_streaming(&chat) || app_services_chat_active()) return;
    if (app_services_chat_state() == HTTP_STREAM_DONE) {
        chat_model_complete_response(&chat);
    } else {
        chat_model_fail_response(&chat);
        if (app_services_chat_error() == HTTP_STREAM_ERROR_HTTP_STATUS)
            snprintf(note, sizeof(note), "HTTP %d", app_services_chat_status_code());
        else set_note(stream_error_text(app_services_chat_error()));
    }
    transcript_dirty = true;
}

static void cancel_reply(void) {
    if (!chat_model_is_streaming(&chat)) return;
    app_services_chat_cancel();
    chat_model_cancel_response(&chat);
    transcript_dirty = true;
}

static void send(void) {
    ollama_message_t messages[OLLAMA_MAX_MESSAGES];
    size_t count;

    if (chat_model_composer_length(&chat) == 0) return;
    if (chat_model_is_streaming(&chat)) { set_note("busy: Esc cancels"); return; }
    if (!config.model[0]) { set_note("set a model (F5)"); return; }
    if (!config.host[0]) { set_note("set host (F5)"); return; }
    if (app_services_wifi_state() != WIFI_STATE_ONLINE) { set_note("Wi-Fi not online"); return; }
    if (app_services_chat_active()) { set_note("busy"); return; }
    if (chat_model_submit(&chat) != 0) { set_note("message too long"); return; }

    /* The array points into the transcript; chat_start serialises it before
     * returning.  The empty in-flight reply is left out by the conversion. */
    count = chat_request_convert(chat_model_message_at(&chat, 0), chat_model_message_count(&chat), NULL,
                                 messages, OLLAMA_MAX_MESSAGES);
    if (count == CHAT_REQUEST_UNMEASURABLE || app_services_chat_start(messages, count, on_provider_event, NULL) != 0) {
        chat_model_fail_response(&chat);
        set_note("could not start");
    } else note[0] = '\0';
    scroll = 0;
    transcript_dirty = composer_dirty = true;
}

/* ---- Menus --------------------------------------------------------------- */

/* Menus and prompts overlay the chat screen, so each starts from a clean one. */
static int menu(const char *title, const char *const *labels, int count, int sel) {
    chat_mode_redraw();
    return ui_show_list_menu(title, labels, count, sel);
}

static void notice(const char *title, const char *text) {
    menu(title, &text, 1, 0);
}

static void apply_config(void) {
    /* Reconfiguring cancels the request in flight; keep the transcript in step. */
    if (chat_model_is_streaming(&chat)) chat_model_cancel_response(&chat);
    if (app_services_configure(&config) != 0) set_note("settings rejected");
    /* An unsaved configuration still applies for this session. */
    else if (config_store_save(&config, CONFIG_DIR) != AI_CONFIG_OK) set_note("settings not saved");
    else note[0] = '\0';
    transcript_dirty = true;
}

static void explain_rejection(chat_setting_t field, ai_config_status_t status) {
    char text[32];
    long lo = 0, hi = 0;
    switch (field) {
        case CHAT_SETTING_PORT: lo = 1; hi = 65535; break;
        case CHAT_SETTING_CONNECT_TIMEOUT: lo = AI_CONFIG_CONNECT_TIMEOUT_MS_MIN; hi = AI_CONFIG_CONNECT_TIMEOUT_MS_MAX; break;
        case CHAT_SETTING_REQUEST_TIMEOUT: lo = AI_CONFIG_REQUEST_TIMEOUT_MS_MIN; hi = AI_CONFIG_REQUEST_TIMEOUT_MS_MAX; break;
        case CHAT_SETTING_IDLE_TIMEOUT: lo = AI_CONFIG_IDLE_TIMEOUT_MS_MIN; hi = AI_CONFIG_IDLE_TIMEOUT_MS_MAX; break;
        case CHAT_SETTING_MAX_PREDICT: lo = AI_CONFIG_MAX_PREDICT_MIN; hi = AI_CONFIG_MAX_PREDICT_MAX; break;
        case CHAT_SETTING_UTC_OFFSET: lo = AI_CONFIG_UTC_OFFSET_MINUTES_MIN; hi = AI_CONFIG_UTC_OFFSET_MINUTES_MAX; break;
        default: break;
    }
    if (hi) snprintf(text, sizeof(text), " Enter %ld to %ld ", lo, hi);
    else snprintf(text, sizeof(text), "%s", status == AI_CONFIG_VALUE_TOO_LONG ? " Too long " : " Not a valid value ");
    notice(" NOT CHANGED ", text);
}

/* Prompt for one field and store it in target if it validates.  Every field,
 * the Wi-Fi password included, is shown as it is typed; the typed value is
 * wiped afterwards because it may be that password. */
static bool edit_setting(ai_config_t *target, chat_setting_t field) {
    char value[UI_INPUT_MAX + 1], title[24];
    unsigned flags = UI_INPUT_ANY_CHAR;
    ai_config_status_t status;

    if (chat_settings_allows_empty(field)) flags |= UI_INPUT_ALLOW_EMPTY;
    snprintf(title, sizeof(title), " %s ", chat_settings_label(field));
    if (!ui_show_input_dialog(title, value, sizeof(value), flags)) return false;
    status = chat_settings_set(target, field, value);
    memset(value, 0, sizeof(value));
    if (status != AI_CONFIG_OK) { explain_rejection(field, status); return false; }
    return true;
}

static void setting_label(const ai_config_t *from, chat_setting_t field, char *label, size_t capacity) {
    char value[20];
    chat_settings_format(from, field, value, sizeof(value));
    snprintf(label, capacity, " %s: %s ", chat_settings_label(field), value[0] ? value : "(none)");
}

/* Run a network scan to its end, keeping the transport serviced.  The scan is
 * bounded by WIFI_SCAN_TIMEOUT_MS; keys pressed meanwhile are discarded so
 * they cannot pick from the list that follows. */
static bool scan_networks(void) {
    if (app_services_wifi_scan_start() != 0) return false;
    set_note("Scanning...");
    chat_mode_redraw();
    while (app_services_wifi_scan_state() == WIFI_SCAN_SCANNING) {
        app_services_poll();
        lcd_getc(0);
        sleep_ms(20);
    }
    note[0] = '\0';
    return app_services_wifi_scan_state() == WIFI_SCAN_DONE;
}

/* Scan, pick a network into target's SSID, then ask for its password. */
static void scan_and_pick(ai_config_t *target) {
    const char *labels[WIFI_SCAN_MAX_NETWORKS];
    char ssid[WIFI_SCAN_SSID_CAPACITY];
    ai_config_status_t status;
    int count, rssi, sel;

    if (!scan_networks()) { notice(" WI-FI SCAN ", " Scan failed "); return; }
    count = (int)app_services_wifi_scan_count();
    if (count == 0) { notice(" WI-FI SCAN ", " No networks found "); return; }
    for (int i = 0; i < count; i++) {
        app_services_wifi_scan_at((size_t)i, ssid, &rssi);
        snprintf(scan_labels[i], sizeof(scan_labels[i]), " %-21.21s %4d dBm", ssid, rssi);
        labels[i] = scan_labels[i];
    }
    sel = menu(" NETWORKS ", labels, count, 0);
    if (sel < 0 || app_services_wifi_scan_at((size_t)sel, ssid, NULL) != 0) return;
    status = chat_settings_set(target, CHAT_SETTING_SSID, ssid);
    if (status != AI_CONFIG_OK) { explain_rejection(CHAT_SETTING_SSID, status); return; }
    edit_setting(target, CHAT_SETTING_PASSWORD);
}

static void ssid_menu(ai_config_t *target) {
    static const char *const choices[] = {" Scan networks ", " Type manually "};
    int sel = menu(" SSID ", choices, 2, 0);
    if (sel == 0) scan_and_pick(target);
    else if (sel == 1) edit_setting(target, CHAT_SETTING_SSID);
}

static void settings_menu(void) {
    const char *labels[CHAT_SETTING_COUNT + 1];
    int sel = 0;

    draft = config;
    while (1) {
        for (int i = 0; i < CHAT_SETTING_COUNT; i++) {
            setting_label(&draft, (chat_setting_t)i, setting_labels[i], sizeof(setting_labels[i]));
            labels[i] = setting_labels[i];
        }
        labels[CHAT_SETTING_COUNT] = " Apply & connect ";
        sel = menu(" CHAT SETTINGS ", labels, CHAT_SETTING_COUNT + 1, sel);
        if (sel < 0) break;
        if (sel == CHAT_SETTING_COUNT) { config = draft; apply_config(); break; }
        if (sel == CHAT_SETTING_PROVIDER) continue; /* chosen from the chat menu; only ollama exists */
        if (sel == CHAT_SETTING_SSID) ssid_menu(&draft);
        else edit_setting(&draft, (chat_setting_t)sel);
    }
    memset(&draft, 0, sizeof(draft));
}

static void provider_menu(void) {
    static const char *const providers[] = {" ollama "};
    if (menu(" PROVIDER ", providers, 1, 0) != 0) return;
    if (strcmp(config.provider, "ollama") == 0) return;
    if (chat_settings_set(&config, CHAT_SETTING_PROVIDER, "ollama") == AI_CONFIG_OK) apply_config();
}

static void chat_menu(void) {
    const char *labels[4];
    int sel = 0;

    while (1) {
        setting_label(&config, CHAT_SETTING_MODEL, setting_labels[0], sizeof(setting_labels[0]));
        setting_label(&config, CHAT_SETTING_PROVIDER, setting_labels[1], sizeof(setting_labels[1]));
        labels[0] = setting_labels[0]; labels[1] = setting_labels[1];
        labels[2] = " Connection settings "; labels[3] = " New chat ";
        sel = menu(" CHAT ", labels, 4, sel);
        if (sel == 0) { if (edit_setting(&config, CHAT_SETTING_MODEL)) apply_config(); }
        else if (sel == 1) provider_menu();
        else if (sel == 2) settings_menu();
        else if (sel == 3) { cancel_reply(); chat_model_reset(&chat); note[0] = '\0'; scroll = 0; break; }
        else break;
    }
    chat_mode_redraw();
}

/* ---- Entry points -------------------------------------------------------- */

void chat_mode_init(void) {
    ai_config_status_t loaded;

    memset(&chat, 0, sizeof(chat));
    ai_config_init(&config);
    loaded = config_store_load(&config, CONFIG_DIR);
    chat_request_measure_init(&measure, &config);
    chat_model_init(&chat, chat_request_measure, &measure, OLLAMA_REQUEST_MAX);
    note[0] = '\0'; status_drawn[0] = '\0';
    /* Saved credentials rejoin Wi-Fi at boot; a bad file leaves the defaults. */
    if (loaded != AI_CONFIG_OK) set_note("ai.ini unreadable");
    else if (config.ssid[0]) app_services_configure(&config);
    scroll = 0;
    transcript_dirty = composer_dirty = false;
}

void chat_mode_show_connection_settings(void) { settings_menu(); }

void chat_mode_handle_input(int c) {
    sync_with_transport();
    switch (c) {
        case KEY_F5: chat_menu(); return;
        case KEY_ENTER: send(); break;
        case KEY_ESC:
            if (chat_model_is_streaming(&chat)) { cancel_reply(); set_note("cancelled"); }
            else { chat_model_composer_clear(&chat); composer_dirty = true; }
            break;
        case KEY_BACKSPACE:
            if (chat_model_composer_backspace(&chat) == 0) composer_dirty = true;
            break;
        case KEY_UP: scroll += SCROLL_STEP; transcript_dirty = true; break;
        case KEY_DOWN: scroll = scroll > SCROLL_STEP ? scroll - SCROLL_STEP : 0; transcript_dirty = true; break;
        default:
            if (c >= 32 && c < 127) {
                char ch = (char)c;
                if (chat_model_composer_append(&chat, &ch, 1) == 0) composer_dirty = true;
            }
    }
    refresh();
}
