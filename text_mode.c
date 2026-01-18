#include "text_mode.h"
#include "lcdspi.h"
#include "keyboard_definition.h"
#include <string.h>

#define MAX_TEXT_LEN 1024
static char text_buffer[MAX_TEXT_LEN];
static int text_len = 0;

void text_mode_init() {
    // For now, it's just a stub, so we keep the buffer as is or clear it
    // The requirement says "tabs should remain in memory", 
    // but text mode itself is a scratchpad.
}

void text_mode_redraw() {
    lcd_clear();
    set_current_x(0);
    set_current_y(0);
    lcd_set_text_color(BLACK, WHITE);
    lcd_print_string("--- TEXT MODE (SCRATCHPAD) ---\n");
    lcd_print_string(text_buffer);
}

void text_mode_handle_input(int c) {
    if (c == KEY_BACKSPACE) {
        if (text_len > 0) {
            text_len--;
            text_buffer[text_len] = '\0';
            text_mode_redraw();
        }
    } else if (c >= 32 && c < 127) {
        if (text_len < MAX_TEXT_LEN - 1) {
            text_buffer[text_len++] = (char)c;
            text_buffer[text_len] = '\0';
            text_mode_redraw();
        }
    } else if (c == KEY_ENTER) {
        if (text_len < MAX_TEXT_LEN - 1) {
            text_buffer[text_len++] = '\n';
            text_buffer[text_len] = '\0';
            text_mode_redraw();
        }
    }
}
