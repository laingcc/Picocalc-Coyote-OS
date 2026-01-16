//
// Created by chuck on 2025-12-19.
//

#include "ui.h"
#include "lcdspi.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"

int tab_count = 5;
int active_tab = 0;

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
    active_tab = new_tab;
    draw_tabs(tab_count, active_tab);
}

void update_tab_count(int new_count) {
    tab_count = new_count;
    draw_tabs(tab_count, active_tab);
}

void ui_init() {
    lcd_init();
    draw();
}

void reboot_to_bootloader() {
    lcd_clear();
    lcd_print_string("Rebooting to bootsel...\n");
    sleep_ms(1000);
    reset_usb_boot(1, 0);
}
