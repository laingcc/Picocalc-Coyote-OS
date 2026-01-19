//
// Created by chuck on 2025-12-19.
//

#include "ui.h"
#include "lcdspi.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "tinyexpr/tinyexpr.h"
#include "pwm_sound/pwm_sound.h"
#include "keyboard_definition.h"
#include "text_mode.h"
#include "dirent.h"

#define MENU_WIDTH 22
#define MENU_HEIGHT 6
#define MAX_MENU_ITEMS 16
#define MAX_GRAPH_FUNCTIONS 4

// Graph function storage
typedef struct {
    char expression[INPUT_BUFFER_SIZE];
    int color;
    bool active;
} GraphFunction;

static GraphFunction graph_functions[MAX_GRAPH_FUNCTIONS];
static const int graph_colors[MAX_GRAPH_FUNCTIONS] = {RED, BLUE, GREEN, MAGENTA};

// Generic menu item structure
typedef struct {
    char label[32];
} MenuItem;

// Generic menu drawing - draws the box frame
static void draw_menu_frame(int start_x, int start_y, int width, int height, const char* title) {
    // Background
    draw_rect_spi(start_x, start_y, start_x + width * 8, start_y + height * 12, BLACK);

    // ASCII Border - top
    lcd_set_text_color(WHITE, BLACK);
    lcd_print_char_at(WHITE, BLACK, '+', 0, start_x, start_y);
    for (int i = 1; i < width - 1; i++)
        lcd_print_char_at(WHITE, BLACK, '-', 0, start_x + i * 8, start_y);
    lcd_print_char_at(WHITE, BLACK, '+', 0, start_x + (width - 1) * 8, start_y);

    // Sides
    for (int i = 1; i < height - 1; i++) {
        lcd_print_char_at(WHITE, BLACK, '|', 0, start_x, start_y + i * 12);
        lcd_print_char_at(WHITE, BLACK, '|', 0, start_x + (width - 1) * 8, start_y + i * 12);
    }

    // Bottom
    lcd_print_char_at(WHITE, BLACK, '+', 0, start_x, start_y + (height - 1) * 12);
    for (int i = 1; i < width - 1; i++)
        lcd_print_char_at(WHITE, BLACK, '-', 0, start_x + i * 8, start_y + (height - 1) * 12);
    lcd_print_char_at(WHITE, BLACK, '+', 0, start_x + (width - 1) * 8, start_y + (height - 1) * 12);

    // Title
    int title_len = strlen(title);
    int title_x = start_x + (width * 8 - title_len * 8) / 2;
    lcd_print_char_at(WHITE, BLACK, ' ', 0, title_x - 8, start_y);
    for (int i = 0; i < title_len; i++)
        lcd_print_char_at(WHITE, BLACK, title[i], 0, title_x + i * 8, start_y);
    lcd_print_char_at(WHITE, BLACK, ' ', 0, title_x + title_len * 8, start_y);
}

// Draw a menu option with optional selection highlight
static void draw_menu_option(int start_x, int start_y, int width, int row, const char* text, bool selected) {
    int text_len = strlen(text);
    int opt_x = start_x + (width * 8 - text_len * 8) / 2;
    int opt_y = start_y + row * 12;

    int fg = selected ? BLACK : WHITE;
    int bg = selected ? WHITE : BLACK;

    for (int i = 0; i < text_len; i++)
        lcd_print_char_at(fg, bg, text[i], 0, opt_x + i * 8, opt_y);
}

// Draw text input field
static void draw_input_field(int start_x, int start_y, int width, int row,
                             const char* text, int max_display) {
    int field_y = start_y + row * 12;
    int field_x = start_x + 8;  // 1 char padding from border
    int field_width = (width - 2) * 8;  // width minus borders

    // Clear field area
    draw_rect_spi(field_x, field_y, field_x + field_width, field_y + 12, BLACK);

    // Draw text (inverted colors for input field)
    int text_len = strlen(text);
    int display_len = text_len > max_display ? max_display : text_len;
    int display_start = text_len > max_display ? text_len - max_display : 0;

    for (int i = 0; i < display_len; i++) {
        lcd_print_char_at(BLACK, WHITE, text[display_start + i], 0, field_x + i * 8, field_y);
    }
    // Draw cursor
    lcd_print_char_at(BLACK, WHITE, '_', 0, field_x + display_len * 8, field_y);
}

// Text input dialog - returns true if confirmed, false if cancelled
// Result is stored in out_buffer (must be at least max_len bytes)
static bool run_input_dialog(const char* title, char* out_buffer, int max_len) {
    int width = MENU_WIDTH;
    int height = 5;
    int start_x = (LCD_WIDTH - width * 8) / 2;
    int start_y = (LCD_HEIGHT - height * 12) / 2;
    int max_display = width - 3;  // Leave room for borders and cursor

    char input[64];
    int input_len = 0;
    input[0] = '\0';

    draw_menu_frame(start_x, start_y, width, height, title);
    draw_input_field(start_x, start_y, width, 2, input, max_display);

    while (true) {
        int c = lcd_getc(0);
        if (c == KEY_ENTER) {
            if (input_len > 0) {
                strncpy(out_buffer, input, max_len - 1);
                out_buffer[max_len - 1] = '\0';
                return true;
            }
        } else if (c == KEY_ESC || (c == KEY_BACKSPACE && input_len == 0)) {
            return false;
        } else if (c == KEY_BACKSPACE) {
            if (input_len > 0) {
                input_len--;
                input[input_len] = '\0';
                draw_input_field(start_x, start_y, width, 2, input, max_display);
            }
        } else if (c >= 32 && c < 127 && input_len < max_len - 1 && input_len < 63) {
            // Filter out invalid filename characters
            if (c != '/' && c != '\\' && c != ':' && c != '*' &&
                c != '?' && c != '"' && c != '<' && c != '>' && c != '|') {
                input[input_len++] = (char)c;
                input[input_len] = '\0';
                draw_input_field(start_x, start_y, width, 2, input, max_display);
            }
        }
        sleep_ms(20);
    }
}

// Generic menu loop - returns selected item index, or -1 if cancelled
static int run_menu_loop(int start_x, int start_y, int width, int height,
                         const char* title, MenuItem* items, int item_count,
                         int initial_selection) {
    int selected = initial_selection;
    bool exit_menu = false;

    // Initial draw
    draw_menu_frame(start_x, start_y, width, height, title);
    for (int i = 0; i < item_count; i++) {
        draw_menu_option(start_x, start_y, width, 2 + i, items[i].label, i == selected);
    }

    while (!exit_menu) {
        int c = lcd_getc(0);
        switch (c) {
            case KEY_UP:
                if (selected > 0) {
                    selected--;
                    for (int i = 0; i < item_count; i++) {
                        draw_menu_option(start_x, start_y, width, 2 + i, items[i].label, i == selected);
                    }
                }
                break;
            case KEY_DOWN:
                if (selected < item_count - 1) {
                    selected++;
                    for (int i = 0; i < item_count; i++) {
                        draw_menu_option(start_x, start_y, width, 2 + i, items[i].label, i == selected);
                    }
                }
                break;
            case KEY_ENTER:
                return selected;
            case KEY_ESC:
            case KEY_BACKSPACE:
                return -1;
            default:
                break;
        }
        sleep_ms(20);
    }
    return -1;
}

int tab_count = MAX_TABS;
int active_tab = 0;
TabContext tab_contexts[MAX_TABS];
static app_mode_t current_mode = MODE_CALCULATOR;

static void draw();

app_mode_t ui_get_current_mode() {
    return current_mode;
}

void ui_set_current_mode(app_mode_t mode) {
    current_mode = mode;
    if (current_mode == MODE_CALCULATOR) {
        draw();
        ui_redraw_tab_content();
    } else if (current_mode == MODE_TEXT) {
        text_mode_redraw();
    }
}

void clear_tabs() {
    draw_rect_spi(0, 295, 320, 320, WHITE);
}

void draw_tabs(int count, int active) {
    clear_tabs();
    for (int i = 0; i < count; ++i) {
        int x = i * 40 + 5;
        int x2 = x + 20;
        int y = 300;
        if (i == active) {
            y = 295;
        }
        int y2 = 320;
        draw_rect_spi(x+5, y, x2, y2, GRAY);
        lcd_print_char_at(WHITE, GRAY, '1'+i, ORIENT_NORMAL, x + 10, y + 5);
    }
}

static void draw() {
    lcd_clear();
    draw_tabs(tab_count, active_tab);
    set_current_y(12);
    set_current_x(0);
}

void update_active_tab(int new_tab) {
    if (new_tab >= 0 && new_tab < tab_count) {
        if (active_tab != new_tab) {
            sound_play(SND_TAB_SWITCH);
        }
        active_tab = new_tab;
        draw();
        ui_redraw_tab_content();
    }
}

void update_tab_count(int new_count) {
    tab_count = new_count;
    draw_tabs(tab_count, active_tab);
}

void ui_init() {
    memset(tab_contexts, 0, sizeof(tab_contexts));
    lcd_init();
    draw();
    ui_redraw_tab_content();
}

TabContext* ui_get_tab_context(int tab_idx) {
    if (tab_idx >= 0 && tab_idx < MAX_TABS) {
        return &tab_contexts[tab_idx];
    }
    return NULL;
}

int ui_get_active_tab_idx() {
    return active_tab;
}

void ui_add_to_history(int tab_idx, const char* expression, double result) {
    if (tab_idx < 0 || tab_idx >= MAX_TABS) return;
    TabContext* ctx = &tab_contexts[tab_idx];

    if (ctx->history_count < MAX_HISTORY) {
        strncpy(ctx->history[ctx->history_count].expression, expression, INPUT_BUFFER_SIZE - 1);
        ctx->history[ctx->history_count].expression[INPUT_BUFFER_SIZE - 1] = '\0';
        ctx->history[ctx->history_count].result = result;
        ctx->history[ctx->history_count].has_result = true;
        ctx->history_count++;
    } else {
        // Shift history
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            ctx->history[i] = ctx->history[i+1];
        }
        strncpy(ctx->history[MAX_HISTORY-1].expression, expression, INPUT_BUFFER_SIZE - 1);
        ctx->history[MAX_HISTORY-1].expression[INPUT_BUFFER_SIZE - 1] = '\0';
        ctx->history[MAX_HISTORY-1].result = result;
        ctx->history[MAX_HISTORY-1].has_result = true;
    }
}

// Draw axes for graph
static void draw_graph_axes() {
    int mid_y = 154; // (14 + 294) / 2
    int mid_x = 160;
    draw_rect_spi(0, mid_y, 319, mid_y, GRAY);
    draw_rect_spi(mid_x, 14, mid_x, 294, GRAY);
}

// Draw a single graph function with specified color
static void draw_graph_function(const char* expression, int color) {
    if (!expression || expression[0] == '\0') return;

    double x_val;
    te_variable vars[] = {{"x", &x_val}};
    int err;
    te_expr *expr = te_compile(expression, vars, 1, &err);

    if (!expr) {
        return;  // Silently fail for invalid expressions
    }

    int mid_y = 154;
    int mid_x = 160;
    double scale = 16.0;

    int last_sy = -1;
    for (int sx = 0; sx < 320; sx++) {
        x_val = ((double)sx - mid_x) / scale;
        double y_val = te_eval(expr);

        if (isnan(y_val) || isinf(y_val)) {
            last_sy = -1;
            continue;
        }

        int sy = mid_y - (int)(y_val * scale);

        if (sy >= 14 && sy <= 294) {
            if (last_sy != -1) {
                int y_start = last_sy < sy ? last_sy : sy;
                int y_end = last_sy < sy ? sy : last_sy;
                for (int y = y_start; y <= y_end; y++) {
                    if (y >= 14 && y <= 294) spi_draw_pixel(sx, y, color);
                }
            } else {
                spi_draw_pixel(sx, sy, color);
            }
            last_sy = sy;
        } else {
            last_sy = -1;
        }
    }

    te_free(expr);
}

void ui_draw_graph(const char* expression) {
    if (!expression || expression[0] == '\0') return;

    double x_val;
    te_variable vars[] = {{"x", &x_val}};
    int err;
    te_expr *expr = te_compile(expression, vars, 1, &err);

    if (!expr) {
        set_current_x(0);
        set_current_y(20);
        char err_msg[64];
        sprintf(err_msg, "Graph Error at pos %d", err);
        lcd_print_string(err_msg);
        return;
    }
    te_free(expr);

    // Draw axes
    draw_graph_axes();

    // Draw all active graph functions first
    for (int i = 0; i < MAX_GRAPH_FUNCTIONS; i++) {
        if (graph_functions[i].active) {
            draw_graph_function(graph_functions[i].expression, graph_functions[i].color);
        }
    }

    // Draw the current/main expression in red
    draw_graph_function(expression, RED);
}

// Add a function to the graph
bool ui_graph_add_function(const char* expression) {
    // Find first empty slot
    for (int i = 0; i < MAX_GRAPH_FUNCTIONS; i++) {
        if (!graph_functions[i].active) {
            strncpy(graph_functions[i].expression, expression, INPUT_BUFFER_SIZE - 1);
            graph_functions[i].expression[INPUT_BUFFER_SIZE - 1] = '\0';
            graph_functions[i].color = graph_colors[i];
            graph_functions[i].active = true;
            return true;
        }
    }
    return false;  // No empty slots
}

// Clear all graph functions
void ui_graph_clear_all() {
    for (int i = 0; i < MAX_GRAPH_FUNCTIONS; i++) {
        graph_functions[i].active = false;
        graph_functions[i].expression[0] = '\0';
    }
}

// Get count of active graph functions
int ui_graph_get_count() {
    int count = 0;
    for (int i = 0; i < MAX_GRAPH_FUNCTIONS; i++) {
        if (graph_functions[i].active) count++;
    }
    return count;
}

void ui_redraw_input_only() {
    TabContext* ctx = &tab_contexts[active_tab];
    if (active_tab == 3) {
        // Redraw input bar at the bottom
        draw_rect_spi(0, 280, 320, 294, WHITE);
        set_current_x(0);
        set_current_y(281);
        lcd_set_text_color(BLACK, WHITE);
        lcd_print_string("f(x)=");
        lcd_print_string(ctx->current_input);
    } else {
        // Determine input line position. We can use history_count to estimate.
        // history_count * 2 lines (expr + result) + some spacing.
        // Each line is 8 pixels (MainFont height is 8 from font1.h)
        int y_pos = (ctx->history_count * 3 * 8);
        // 3 lines per history item: expr, result, blank line

        draw_rect_spi(0, y_pos, 320, y_pos + 8, WHITE);
        set_current_x(0);
        set_current_y(y_pos);
        lcd_set_text_color(BLACK, WHITE);
        lcd_print_string("> ");
        lcd_print_string(ctx->current_input);
    }
}

void ui_redraw_tab_content() {
    TabContext* ctx = &tab_contexts[active_tab];

    if (active_tab == 3) { // Graphing mode on Tab 4
        draw_rect_spi(0, 0, 320, 294, BLACK);
        if (ctx->history_count > 0) {
            ui_draw_graph(ctx->history[ctx->history_count - 1].expression);
        }
        ui_redraw_input_only();
    } else {
        draw_rect_spi(0, 0, 320, 294, WHITE);
        set_current_x(0);
        set_current_y(0);
        lcd_set_text_color(BLACK, WHITE);

        for (int i = 0; i < ctx->history_count; i++) {
            char buf[128];
            lcd_print_string(ctx->history[i].expression);
            sprintf(buf, "\n = %f\n", ctx->history[i].result);
            lcd_print_string(buf);
        }

        // Print current input
        lcd_print_string("> ");
        lcd_print_string(ctx->current_input);
    }
}

void reboot_to_bootloader() {
    lcd_clear();
    lcd_print_string("Rebooting to bootsel...\n");
    sleep_ms(1000);
    reset_usb_boot(1, 0);
}

void ui_show_mode_menu() {
    int start_x = (LCD_WIDTH - MENU_WIDTH * 8) / 2;
    int start_y = (LCD_HEIGHT - MENU_HEIGHT * 12) / 2;

    MenuItem items[2];
    strcpy(items[0].label, " Text ");
    strcpy(items[1].label, " Calculator ");

    int initial = (current_mode == MODE_TEXT) ? 0 : 1;
    int selected = run_menu_loop(start_x, start_y, MENU_WIDTH, MENU_HEIGHT,
                                  " MODE ", items, 2, initial);

    if (selected == 0) {
        ui_set_current_mode(MODE_TEXT);
    } else if (selected == 1) {
        ui_set_current_mode(MODE_CALCULATOR);
    }

    if (current_mode == MODE_CALCULATOR) {
        ui_redraw_tab_content();
    } else {
        text_mode_redraw();
    }
}

void ui_show_menu() {
    int start_x = (LCD_WIDTH - MENU_WIDTH * 8) / 2;
    int start_y = (LCD_HEIGHT - MENU_HEIGHT * 12) / 2;

    MenuItem items[2];
    bool done = false;

    while (!done) {
        snprintf(items[0].label, sizeof(items[0].label), " %s Beeps ",
                 sound_is_enabled() ? "Disable" : "Enable ");
        strcpy(items[1].label, " Reboot to Bootloader ");

        int selected = run_menu_loop(start_x, start_y, MENU_WIDTH, MENU_HEIGHT,
                                      " SETTINGS ", items, 2, 0);

        if (selected == 0) {
            sound_set_enabled(!sound_is_enabled());
            // Stay in menu to show updated label
        } else if (selected == 1) {
            reboot_to_bootloader();
        } else {
            done = true;
        }
    }
    ui_redraw_tab_content();
}

bool ui_show_file_menu(const char* directory, char* out_filename, int max_len) {
    MenuItem items[MAX_MENU_ITEMS];
    char filenames[MAX_MENU_ITEMS][32];  // Store actual filenames without padding
    int item_count = 0;
    bool has_files = false;

    DIR *dir = opendir(directory);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && item_count < MAX_MENU_ITEMS) {
            // Skip . and ..
            if (entry->d_name[0] == '.' &&
                (entry->d_name[1] == '\0' ||
                 (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
                continue;
            }
            // Store actual filename
            strncpy(filenames[item_count], entry->d_name, 31);
            filenames[item_count][31] = '\0';
            // Format for display with padding
            snprintf(items[item_count].label, sizeof(items[item_count].label),
                     " %.28s ", entry->d_name);
            item_count++;
            has_files = true;
        }
        closedir(dir);
    }

    if (item_count == 0) {
        strcpy(items[0].label, " (empty) ");
        item_count = 1;
    }

    // Calculate menu dimensions - height adjusts to content
    int menu_height = item_count + 3;  // 2 for border + 1 for padding
    if (menu_height > 20) menu_height = 20;

    int start_x = (LCD_WIDTH - MENU_WIDTH * 8) / 2;
    int start_y = (LCD_HEIGHT - menu_height * 12) / 2;

    int selected = run_menu_loop(start_x, start_y, MENU_WIDTH, menu_height,
                                  " FILES ", items, item_count, 0);

    if (selected >= 0 && has_files && out_filename != NULL) {
        strncpy(out_filename, filenames[selected], max_len - 1);
        out_filename[max_len - 1] = '\0';
        return true;
    }

    return false;
}

bool ui_show_save_prompt(char* out_filename, int max_len) {
    bool result = run_input_dialog(" SAVE AS ", out_filename, max_len);
    return result;
}

void ui_show_graph_menu() {
    int start_x = (LCD_WIDTH - MENU_WIDTH * 8) / 2;
    int start_y = (LCD_HEIGHT - MENU_HEIGHT * 12) / 2;

    MenuItem items[3];
    strcpy(items[0].label, " Add Current Function ");
    strcpy(items[1].label, " Clear All Functions ");
    strcpy(items[2].label, " Cancel ");

    int selected = run_menu_loop(start_x, start_y, MENU_WIDTH, MENU_HEIGHT,
                                  " GRAPH ", items, 3, 0);

    TabContext* ctx = ui_get_tab_context(3);  // Graph tab

    if (selected == 0) {
        // Add current function from last history entry
        if (ctx->history_count > 0) {
            if (!ui_graph_add_function(ctx->history[ctx->history_count - 1].expression)) {
                // Could show "slots full" message, but just silently fail for now
            }
        }
    } else if (selected == 1) {
        // Clear all functions
        ui_graph_clear_all();
    }
    // selected == 2 or -1 means cancel, do nothing

    ui_redraw_tab_content();
}
