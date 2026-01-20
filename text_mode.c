#include "text_mode.h"
#include "lcdspi.h"
#include "keyboard_definition.h"
#include "UI/ui.h"
#include <string.h>
#include <stdio.h>

#define MAX_TEXT_LEN 1024
#define COYOTE_DIR "/coyote"

static char text_buffer[MAX_TEXT_LEN];
static int text_len = 0;

void text_mode_redraw() {
    lcd_clear();
    set_current_x(0); set_current_y(0);
    lcd_set_text_color(BLACK, WHITE);
    lcd_print_string("- F1:Open F5:Save F6:New -\n");
    lcd_print_string(text_buffer);
}

static bool save_file(const char* name) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.txt", COYOTE_DIR, name);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(text_buffer, f);
    fclose(f);
    return true;
}

static bool load_file(const char* name) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", COYOTE_DIR, name);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    text_len = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF && text_len < MAX_TEXT_LEN - 1)
        text_buffer[text_len++] = ch;
    text_buffer[text_len] = '\0';
    fclose(f);
    return true;
}

void text_mode_handle_input(int c) {
    char fname[32];
    switch (c) {
        case KEY_F1:
            if (ui_show_file_menu(COYOTE_DIR, fname, sizeof(fname))) load_file(fname);
            text_mode_redraw();
            break;
        case KEY_F5:
            if (ui_show_save_prompt(fname, sizeof(fname))) save_file(fname);
            text_mode_redraw();
            break;
        case KEY_F6:
            text_buffer[0] = '\0'; text_len = 0;
            text_mode_redraw();
            break;
        case KEY_BACKSPACE:
            if (text_len > 0) { text_buffer[--text_len] = '\0'; text_mode_redraw(); }
            break;
        case KEY_ENTER:
            if (text_len < MAX_TEXT_LEN - 1) { text_buffer[text_len++] = '\n'; text_buffer[text_len] = '\0'; text_mode_redraw(); }
            break;
        default:
            if (c >= 32 && c < 127 && text_len < MAX_TEXT_LEN - 1) {
                text_buffer[text_len++] = c; text_buffer[text_len] = '\0'; text_mode_redraw();
            }
    }
}
