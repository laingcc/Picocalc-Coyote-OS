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
    static char lcd_buffer[320 * 3] = {0};
    static int index = 0;
    int c = lcd_getc(0);
    double a;
    char buf[64];
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
            a = te_interp(lcd_buffer, 0);
            sprintf(buf, "\n = %f\n", a);
            lcd_print_string(buf);
            memset(lcd_buffer, 0, sizeof(lcd_buffer));
            index = 0;
            break;
        case KEY_MOD_SHL:
        case KEY_MOD_SHR:
            break;
        default:
            if(c != -1 && c > 0) {
                if (index < sizeof(lcd_buffer) - 1) {
                    lcd_buffer[index++] = c;
                    lcd_buffer[index] = '\0';
                    lcd_putc(0,c);
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

