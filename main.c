#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "i2ckbd.h"
#include "keyboard_definition.h"
#include "lcdspi.h"
#include "tinyexpr/tinyexpr.h"
#include "UI/ui.h"

void handle_keyboard() {
    int active_idx = ui_get_active_tab_idx();
    TabContext* ctx = ui_get_tab_context(active_idx);
    int c = lcd_getc(0);
    double a;
    switch (c) {
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
            reboot_to_bootloader();
            break;
        case KEY_ENTER:
            a = te_interp(ctx->current_input, 0);
            ui_add_to_history(active_idx, ctx->current_input, a);
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
                ui_redraw_tab_content();
            }
            break;
        default:
            if(c != -1 && c > 0 && c < 128) {
                if (ctx->input_index < INPUT_BUFFER_SIZE - 1) {
                    ctx->current_input[ctx->input_index++] = c;
                    ctx->current_input[ctx->input_index] = '\0';
                    ui_redraw_tab_content();
                }
            };
    }
}

int main() {
    set_sys_clock_khz(133000, true);
    stdio_init_all();

    uart_init(uart0, 115200);

    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);  // 8-N-1
    uart_set_fifo_enabled(uart0, false);

    init_i2c_kbd();

    ui_init();

    while (1) {
        handle_keyboard();
        sleep_ms(20);
    }
}

