#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "i2ckbd.h"
#include "keyboard_definition.h"
#include "lcdspi.h"
#include "tinyexpr/tinyexpr.h"
#include "UI/ui.h"
#include "pwm_sound/pwm_sound.h"
#include "config.h"
#include "text_mode.h"
#include "blockdevice/sd.h"
#include "filesystem/fat.h"
#include "filesystem/vfs.h"
#include "dirent.h"

#define COYOTE_DIR "/coyote"

void handle_keyboard() {
    int c = lcd_getc(0);
    if (c == KEY_HOME) { ui_show_mode_menu(); return; }
    if (ui_get_current_mode() == MODE_TEXT) { text_mode_handle_input(c); return; }

    int idx = ui_get_active_tab_idx();
    TabContext* ctx = ui_get_tab_context(idx);

    if (c >= KEY_F1 && c <= KEY_F4) { update_active_tab(c - KEY_F1); return; }

    switch (c) {
        case KEY_F5: ui_show_menu(); break;
        case KEY_F6: if (idx == 3) ui_show_graph_menu(); break;
        case KEY_ENTER: {
            double a = (idx == 3) ? 0 : te_interp(ctx->current_input, 0);
            sound_play((idx == 3 || !isnan(a)) ? SND_BEEP : SND_ERROR);
            ui_add_to_history(idx, ctx->current_input, a);
            memset(ctx->current_input, 0, sizeof(ctx->current_input));
            ctx->input_index = 0;
            ui_redraw_tab_content();
            break;
        }
        case KEY_BACKSPACE:
            if (ctx->input_index > 0) {
                ctx->current_input[--ctx->input_index] = '\0';
                ui_redraw_input_only();
            }
            break;
        default:
            if (c > 0 && c < 128 && ctx->input_index < INPUT_BUFFER_SIZE - 1) {
                ctx->current_input[ctx->input_index++] = c;
                ctx->current_input[ctx->input_index] = '\0';
                ui_redraw_input_only();
            }
    }
}

bool fs_init(void) {
    blockdevice_t *sd = blockdevice_sd_create(spi0, SD_MOSI_PIN, SD_MISO_PIN, SD_SCLK_PIN, SD_CS_PIN, 125000000/8, true);
    filesystem_t *fat = filesystem_fat_create();
    if (fs_mount("/", fat, sd) == -1) {
        if (fs_format(fat, sd) == -1 || fs_mount("/", fat, sd) == -1) return false;
    }
    return true;
}

int main() {
    set_sys_clock_khz(133000, true);
    stdio_init_all();
    uart_init(uart0, 115200);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart0, false);
    init_i2c_kbd();
    sound_init();
    ui_init();

    if (fs_init()) {
        DIR *dir = opendir(COYOTE_DIR);
        if (!dir) mkdir(COYOTE_DIR, 0755);
        else closedir(dir);
    }

    while (1) { handle_keyboard(); sleep_ms(20); }
}
