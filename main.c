#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "i2ckbd.h"
#include "keyboard_definition.h"
#include "lcdspi.h"
#include "tinyexpr/tinyexpr.h"
#include "UI/ui.h"
#include "pwm_sound/pwm_sound.h"
#include "config.h"
#include "utils.h"

#include "blockdevice/sd.h"
#include "filesystem/fat.h"
#include "filesystem/vfs.h"
#include "dirent.h"

char active_directory[50] = "/coyote";

void handle_keyboard() {
    int active_idx = ui_get_active_tab_idx();
    TabContext* ctx = ui_get_tab_context(active_idx);
    int c = lcd_getc(0);
    double a;
    switch (c) {
        case 19: // Ctrl+S
            draw_rect_spi(0, 0, 319, 319, WHITE);
            sound_play(SND_BEEP);
            sleep_ms(50);
            draw();
            ui_redraw_tab_content();
            take_screenshot(active_directory);
            break;
        case KEY_F1:
            update_active_tab(0);
            break;
        case KEY_F2:
            update_active_tab(1);
            break;
        case KEY_F3:
            update_active_tab(2);
            break;
        case KEY_F4:
            update_active_tab(3);
            break;
        case KEY_F5:
            ui_show_menu();
            break;
        case KEY_ENTER:
            if (active_idx == 3) {
                // Graphing mode: we don't need the result, just add to history to trigger redraw
                ui_add_to_history(active_idx, ctx->current_input, 0);
                sound_play(SND_BEEP);
            } else {
                a = te_interp(ctx->current_input, 0);
                if (isnan(a)) {
                    sound_play(SND_ERROR);
                } else {
                    sound_play(SND_BEEP);
                }
                ui_add_to_history(active_idx, ctx->current_input, a);
            }
            memset(ctx->current_input, 0, sizeof(ctx->current_input));
            ctx->input_index = 0;
            ui_redraw_tab_content();
            break;
        case KEY_MOD_SHL:
        case KEY_MOD_SHR:
            break;
        case KEY_BACKSPACE:
            if (ctx->input_index > 0) {
                ctx->input_index--;
                ctx->current_input[ctx->input_index] = '\0';
                ui_redraw_input_only();
            }
            break;
        default:
            if(c != -1 && c > 0 && c < 128) {
                if (ctx->input_index < INPUT_BUFFER_SIZE - 1) {
                    ctx->current_input[ctx->input_index++] = c;
                    ctx->current_input[ctx->input_index] = '\0';
                    ui_redraw_input_only();
                }
            };
    }
}

bool fs_init(void) {
    blockdevice_t *sd = blockdevice_sd_create(
        spi0,
        SD_MOSI_PIN,
        SD_MISO_PIN,
        SD_SCLK_PIN,
        SD_CS_PIN,
        125000000 /2 /4,
        true
        );

    filesystem_t *fat = filesystem_fat_create();
    int err = fs_mount("/", fat, sd);
    if (err == -1)
    {
        err = fs_format(fat, sd);
        if (err == -1)
        {
            return false;
        }
        err = fs_mount("/", fat, sd);
        if (err == -1)
        {
            return false;
        }
    }
    return true;
}

int main() {
    set_sys_clock_khz(133000, true);
    stdio_init_all();

    uart_init(uart0, 115200);

    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);  // 8-N-1
    uart_set_fifo_enabled(uart0, false);

    init_i2c_kbd();

    sound_init();
    ui_init();

    bool test = fs_init();
    if (test) {
        // lcd_print_string("Filesystem initialized\n");
        DIR *dir = opendir(active_directory);
        if (dir == NULL) {
            lcd_print_string("Main Coyote Directory Not Found, Creating...\n");
            int err = mkdir(active_directory,0755);
            if (err == -1) {
                lcd_print_string("Error Creating Main Coyote Directory\n");
            }else {
                lcd_print_string("Main Coyote Directory Created\n");
            }
        }
    }

    while (1) {
        handle_keyboard();
        sleep_ms(20);
    }
}

