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
    lcd_print_string("- F1:Open F5:Save F6:New -\n");
    lcd_print_string(text_buffer);
}

const char* text_mode_get_buffer() {
    return text_buffer;
}

void text_mode_set_buffer(const char* content) {
    if (content == NULL) {
        text_buffer[0] = '\0';
        text_len = 0;
    } else {
        strncpy(text_buffer, content, MAX_TEXT_LEN - 1);
        text_buffer[MAX_TEXT_LEN - 1] = '\0';
        text_len = strlen(text_buffer);
    }
}

static bool save_file(const char* filename) {
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "%s/%s.txt", COYOTE_DIR, filename);

    FILE *f = fopen(filepath, "w");
    if (f == NULL) {
        return false;
    }
    fputs(text_buffer, f);
    fclose(f);
    return true;
}

static bool load_file(const char* filename) {
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "%s/%s", COYOTE_DIR, filename);

    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        return false;
    }

    text_len = 0;
    text_buffer[0] = '\0';

    int ch;
    while ((ch = fgetc(f)) != EOF && text_len < MAX_TEXT_LEN - 1) {
        text_buffer[text_len++] = (char)ch;
    }
    text_buffer[text_len] = '\0';
    fclose(f);
    return true;
}

void text_mode_handle_input(int c) {
    if (c == KEY_F1) {
        char filename[32];
        if (ui_show_file_menu(COYOTE_DIR, filename, sizeof(filename))) {
            if (load_file(filename)) {
                text_mode_redraw();
            }
        } else {
            text_mode_redraw();
        }
    } else if (c == KEY_F5) {
        char filename[32];
        if (ui_show_save_prompt(filename, sizeof(filename))) {
            save_file(filename);
        }
        text_mode_redraw();
    } else if (c == KEY_F6) {
        text_buffer[0] = '\0';
        text_len = 0;
        text_mode_redraw();
    } else if (c == KEY_BACKSPACE) {
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
