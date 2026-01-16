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

int tab_count = MAX_TABS;
int active_tab = 0;
TabContext tab_contexts[MAX_TABS];

void clear_tabs() {
    draw_rect_spi(0, 295, 320, 320, BLACK);
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
        draw_rect_spi(x+5, y, x2, y2, BLUE);
        lcd_print_char_at(WHITE, BLUE, 'A' + i, ORIENT_NORMAL, x + 10, y + 5);
    }
}

void draw_header() {
    lcd_print_string("Coyote OS v0.0.0");
    draw_rect_spi(0, 12, 320, 13, BLUE);
}

void draw() {
    lcd_clear();
    draw_header();
    draw_tabs(tab_count, active_tab);
    set_current_y(13+12);
    set_current_x(0);
}

void update_active_tab(int new_tab) {
    if (new_tab >= 0 && new_tab < tab_count) {
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

    // Graph area: x [0, 319], y [14, 294]
    int mid_y = 154; // (14 + 294) / 2
    int mid_x = 160;
    double scale = 16.0; // 10 units = 160 pixels

    // Draw axes
    draw_rect_spi(0, mid_y, 319, mid_y, GRAY);
    draw_rect_spi(mid_x, 14, mid_x, 294, GRAY);

    for (int sx = 0; sx < 320; sx++) {
        x_val = ((double)sx - mid_x) / scale;
        double y_val = te_eval(expr);

        if (isnan(y_val) || isinf(y_val)) continue;

        int sy = mid_y - (int)(y_val * scale);

        if (sy >= 14 && sy <= 294) {
            spi_draw_pixel(sx, sy, RED);
        }
    }

    te_free(expr);
}

void ui_redraw_tab_content() {
    TabContext* ctx = &tab_contexts[active_tab];

    if (active_tab == 3) { // Graphing mode on Tab 4
        draw_rect_spi(0, 14, 320, 294, BLACK);
        if (ctx->history_count > 0) {
            ui_draw_graph(ctx->history[ctx->history_count - 1].expression);
        }
        // Overlay input bar
        draw_rect_spi(0, 280, 320, 294, WHITE);
        set_current_x(0);
        set_current_y(281);
        lcd_set_text_color(BLACK, WHITE);
        lcd_print_string("f(x)=");
        lcd_print_string(ctx->current_input);
    } else {
        // Clear work area (y=14 to 294)
        draw_rect_spi(0, 14, 320, 294, WHITE);
        set_current_x(0);
        set_current_y(15);
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
