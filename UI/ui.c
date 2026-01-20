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

#define MENU_W 22
#define MENU_H 6
#define MAX_MENU_ITEMS 16
#define MAX_GRAPH_FN 4
#define MENU_X ((LCD_WIDTH - MENU_W * 8) / 2)
#define MENU_Y ((LCD_HEIGHT - MENU_H * 12) / 2)

typedef struct { char expression[INPUT_BUFFER_SIZE]; int color; bool active; } GraphFn;
typedef struct { char label[32]; } MenuItem;

static GraphFn graph_fns[MAX_GRAPH_FN];
static const int graph_colors[] = {RED, BLUE, GREEN, MAGENTA};

int tab_count = MAX_TABS;
int active_tab = 0;
TabContext tab_contexts[MAX_TABS];
static app_mode_t current_mode = MODE_CALCULATOR;

static void draw_menu_frame(int x, int y, int w, int h, const char* title) {
    draw_rect_spi(x, y, x + w * 8, y + h * 12, BLACK);
    lcd_set_text_color(WHITE, BLACK);
    for (int i = 0; i < w; i++) {
        char c = (i == 0 || i == w-1) ? '+' : '-';
        lcd_print_char_at(WHITE, BLACK, c, 0, x + i*8, y);
        lcd_print_char_at(WHITE, BLACK, c, 0, x + i*8, y + (h-1)*12);
    }
    for (int i = 1; i < h-1; i++) {
        lcd_print_char_at(WHITE, BLACK, '|', 0, x, y + i*12);
        lcd_print_char_at(WHITE, BLACK, '|', 0, x + (w-1)*8, y + i*12);
    }
    int tlen = strlen(title);
    int tx = x + (w*8 - tlen*8) / 2;
    for (int i = 0; i < tlen; i++)
        lcd_print_char_at(WHITE, BLACK, title[i], 0, tx + i*8, y);
}

static void draw_menu_opt(int x, int y, int w, int row, const char* text, bool sel) {
    int len = strlen(text);
    int ox = x + (w*8 - len*8) / 2;
    int fg = sel ? BLACK : WHITE, bg = sel ? WHITE : BLACK;
    for (int i = 0; i < len; i++)
        lcd_print_char_at(fg, bg, text[i], 0, ox + i*8, y + row*12);
}

static void draw_input_field(int x, int y, int w, int row, const char* text, int maxd) {
    int fy = y + row*12, fx = x + 8;
    draw_rect_spi(fx, fy, fx + (w-2)*8, fy + 12, BLACK);
    int len = strlen(text);
    int dlen = len > maxd ? maxd : len;
    int dstart = len > maxd ? len - maxd : 0;
    for (int i = 0; i < dlen; i++)
        lcd_print_char_at(BLACK, WHITE, text[dstart + i], 0, fx + i*8, fy);
    lcd_print_char_at(BLACK, WHITE, '_', 0, fx + dlen*8, fy);
}

static bool run_input_dialog(const char* title, char* out, int max_len) {
    int x = MENU_X, y = (LCD_HEIGHT - 5*12)/2, maxd = MENU_W - 3;
    char input[64] = {0};
    int len = 0;
    draw_menu_frame(x, y, MENU_W, 5, title);
    draw_input_field(x, y, MENU_W, 2, input, maxd);
    while (1) {
        int c = lcd_getc(0);
        if (c == KEY_ENTER && len > 0) {
            strncpy(out, input, max_len-1); out[max_len-1] = '\0';
            return true;
        } else if (c == KEY_ESC || (c == KEY_BACKSPACE && len == 0)) {
            return false;
        } else if (c == KEY_BACKSPACE && len > 0) {
            input[--len] = '\0';
            draw_input_field(x, y, MENU_W, 2, input, maxd);
        } else if (c >= 32 && c < 127 && len < max_len-1 && len < 63) {
            if (c != '/' && c != '\\' && c != ':' && c != '*' && c != '?' && c != '"' && c != '<' && c != '>' && c != '|') {
                input[len++] = c; input[len] = '\0';
                draw_input_field(x, y, MENU_W, 2, input, maxd);
            }
        }
        sleep_ms(20);
    }
}

static int run_menu(int x, int y, int w, int h, const char* title, MenuItem* items, int cnt, int sel) {
    draw_menu_frame(x, y, w, h, title);
    for (int i = 0; i < cnt; i++) draw_menu_opt(x, y, w, 2+i, items[i].label, i == sel);
    while (1) {
        int c = lcd_getc(0);
        if (c == KEY_UP && sel > 0) sel--;
        else if (c == KEY_DOWN && sel < cnt-1) sel++;
        else if (c == KEY_ENTER) return sel;
        else if (c == KEY_ESC || c == KEY_BACKSPACE) return -1;
        else { sleep_ms(20); continue; }
        for (int i = 0; i < cnt; i++) draw_menu_opt(x, y, w, 2+i, items[i].label, i == sel);
        sleep_ms(20);
    }
}

void draw() {
    lcd_clear();
    draw_rect_spi(0, 295, 320, 320, WHITE);
    for (int i = 0; i < tab_count; i++) {
        int x = i*40 + 10, y = (i == active_tab) ? 295 : 300;
        draw_rect_spi(x, y, x+20, 320, GRAY);
        lcd_print_char_at(WHITE, GRAY, '1'+i, 0, x+5, y+5);
    }
    set_current_y(12); set_current_x(0);
}

app_mode_t ui_get_current_mode() { return current_mode; }

void ui_set_current_mode(app_mode_t mode) {
    current_mode = mode;
    if (mode == MODE_CALCULATOR) { draw(); ui_redraw_tab_content(); }
    else text_mode_redraw();
}

void update_active_tab(int t) {
    if (t >= 0 && t < tab_count) {
        if (active_tab != t) sound_play(SND_TAB_SWITCH);
        active_tab = t; draw(); ui_redraw_tab_content();
    }
}

void ui_init() {
    memset(tab_contexts, 0, sizeof(tab_contexts));
    lcd_init(); draw(); ui_redraw_tab_content();
}

TabContext* ui_get_tab_context(int i) { return (i >= 0 && i < MAX_TABS) ? &tab_contexts[i] : NULL; }
int ui_get_active_tab_idx() { return active_tab; }

void ui_add_to_history(int idx, const char* expr, double result) {
    if (idx < 0 || idx >= MAX_TABS) return;
    TabContext* ctx = &tab_contexts[idx];
    if (ctx->history_count >= MAX_HISTORY) {
        memmove(&ctx->history[0], &ctx->history[1], sizeof(HistoryItem)*(MAX_HISTORY-1));
        ctx->history_count = MAX_HISTORY - 1;
    }
    strncpy(ctx->history[ctx->history_count].expression, expr, INPUT_BUFFER_SIZE-1);
    ctx->history[ctx->history_count].result = result;
    ctx->history[ctx->history_count].has_result = true;
    ctx->history_count++;
}

static void draw_graph_fn(const char* expr, int color) {
    if (!expr || !expr[0]) return;
    double x_val;
    te_variable vars[] = {{"x", &x_val}};
    te_expr *e = te_compile(expr, vars, 1, 0);
    if (!e) return;
    int last_sy = -1;
    for (int sx = 0; sx < 320; sx++) {
        x_val = (sx - 160) / 16.0;
        double yv = te_eval(e);
        if (isnan(yv) || isinf(yv)) { last_sy = -1; continue; }
        int sy = 154 - (int)(yv * 16);
        if (sy >= 14 && sy <= 294) {
            if (last_sy != -1) {
                int ys = last_sy < sy ? last_sy : sy, ye = last_sy < sy ? sy : last_sy;
                for (int y = ys; y <= ye; y++) if (y >= 14 && y <= 294) spi_draw_pixel(sx, y, color);
            } else spi_draw_pixel(sx, sy, color);
            last_sy = sy;
        } else last_sy = -1;
    }
    te_free(e);
}

void ui_draw_graph(const char* expr) {
    if (!expr || !expr[0]) return;
    double x_val;
    te_variable vars[] = {{"x", &x_val}};
    te_expr *e = te_compile(expr, vars, 1, 0);
    if (!e) {
        set_current_x(0); set_current_y(20);
        lcd_print_string("Graph Error");
        return;
    }
    te_free(e);
    draw_rect_spi(0, 154, 319, 154, GRAY);
    draw_rect_spi(160, 14, 160, 294, GRAY);
    for (int i = 0; i < MAX_GRAPH_FN; i++)
        if (graph_fns[i].active) draw_graph_fn(graph_fns[i].expression, graph_fns[i].color);
    draw_graph_fn(expr, RED);
}

bool ui_graph_add_function(const char* expr) {
    for (int i = 0; i < MAX_GRAPH_FN; i++) {
        if (!graph_fns[i].active) {
            strncpy(graph_fns[i].expression, expr, INPUT_BUFFER_SIZE-1);
            graph_fns[i].color = graph_colors[i];
            graph_fns[i].active = true;
            return true;
        }
    }
    return false;
}

void ui_graph_clear_all() {
    for (int i = 0; i < MAX_GRAPH_FN; i++) { graph_fns[i].active = false; graph_fns[i].expression[0] = '\0'; }
}

void ui_redraw_input_only() {
    TabContext* ctx = &tab_contexts[active_tab];
    if (active_tab == 3) {
        draw_rect_spi(0, 280, 320, 294, WHITE);
        set_current_x(0); set_current_y(281);
        lcd_set_text_color(BLACK, WHITE);
        lcd_print_string("f(x)="); lcd_print_string(ctx->current_input);
    } else {
        int y = ctx->history_count * 24;
        draw_rect_spi(0, y, 320, y+8, WHITE);
        set_current_x(0); set_current_y(y);
        lcd_set_text_color(BLACK, WHITE);
        lcd_print_string("> "); lcd_print_string(ctx->current_input);
    }
}

void ui_redraw_tab_content() {
    TabContext* ctx = &tab_contexts[active_tab];
    if (active_tab == 3) {
        draw_rect_spi(0, 0, 320, 294, BLACK);
        if (ctx->history_count > 0) ui_draw_graph(ctx->history[ctx->history_count-1].expression);
        ui_redraw_input_only();
    } else {
        draw_rect_spi(0, 0, 320, 294, WHITE);
        set_current_x(0); set_current_y(0);
        lcd_set_text_color(BLACK, WHITE);
        for (int i = 0; i < ctx->history_count; i++) {
            char buf[64];
            lcd_print_string(ctx->history[i].expression);
            sprintf(buf, "\n = %f\n", ctx->history[i].result);
            lcd_print_string(buf);
        }
        lcd_print_string("> "); lcd_print_string(ctx->current_input);
    }
}

void ui_show_mode_menu() {
    MenuItem items[] = {{" Text "}, {" Calculator "}};
    int sel = run_menu(MENU_X, MENU_Y, MENU_W, MENU_H, " MODE ", items, 2, current_mode == MODE_TEXT ? 0 : 1);
    if (sel == 0) ui_set_current_mode(MODE_TEXT);
    else if (sel == 1) ui_set_current_mode(MODE_CALCULATOR);
    else { if (current_mode == MODE_CALCULATOR) ui_redraw_tab_content(); else text_mode_redraw(); }
}

void ui_show_menu() {
    MenuItem items[2];
    while (1) {
        snprintf(items[0].label, 32, " %s Beeps ", sound_is_enabled() ? "Disable" : "Enable ");
        strcpy(items[1].label, " Reboot ");
        int sel = run_menu(MENU_X, MENU_Y, MENU_W, MENU_H, " SETTINGS ", items, 2, 0);
        if (sel == 0) sound_set_enabled(!sound_is_enabled());
        else if (sel == 1) { lcd_clear(); lcd_print_string("Rebooting...\n"); sleep_ms(500); reset_usb_boot(1,0); }
        else break;
    }
    ui_redraw_tab_content();
}

bool ui_show_file_menu(const char* dir, char* out, int max_len) {
    MenuItem items[MAX_MENU_ITEMS];
    char fnames[MAX_MENU_ITEMS][32];
    int cnt = 0;
    bool has = false;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && cnt < MAX_MENU_ITEMS) {
            if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;
            strncpy(fnames[cnt], e->d_name, 31); fnames[cnt][31] = '\0';
            snprintf(items[cnt].label, 32, " %.28s ", e->d_name);
            cnt++; has = true;
        }
        closedir(d);
    }
    if (!cnt) { strcpy(items[0].label, " (empty) "); cnt = 1; }
    int h = cnt + 3; if (h > 20) h = 20;
    int sel = run_menu(MENU_X, (LCD_HEIGHT - h*12)/2, MENU_W, h, " FILES ", items, cnt, 0);
    if (sel >= 0 && has && out) { strncpy(out, fnames[sel], max_len-1); out[max_len-1] = '\0'; return true; }
    return false;
}

bool ui_show_save_prompt(char* out, int max_len) { return run_input_dialog(" SAVE AS ", out, max_len); }

void ui_show_graph_menu() {
    MenuItem items[] = {{" Add Function "}, {" Clear All "}, {" Cancel "}};
    int sel = run_menu(MENU_X, MENU_Y, MENU_W, MENU_H, " GRAPH ", items, 3, 0);
    TabContext* ctx = ui_get_tab_context(3);
    if (sel == 0 && ctx->history_count > 0) ui_graph_add_function(ctx->history[ctx->history_count-1].expression);
    else if (sel == 1) ui_graph_clear_all();
    ui_redraw_tab_content();
}
