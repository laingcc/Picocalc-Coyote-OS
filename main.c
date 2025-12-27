#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "i2ckbd.h"
#include "keyboard_definition.h"
#include "lcdspi.h"
#include "pico/bootrom.h"
#include "tinyexpr/tinyexpr.h"
#include "UI/ui.h"

const uint LEDPIN = 25;

void updateOverlay() {
    char buf[64];
    int bat = read_battery();
    bat = bat>>8;
    int charging = bitRead(bat, 7);
    if (charging == 1) {
        lcd_print_battery(6);
    }
    if (charging == 0) {
        int val = (int) (((float) bat / 100) * 5);
        lcd_print_battery(val);
    }
}

void reboot_to_bootloader() {
    lcd_clear();
    lcd_print_string("Rebooting to bootsel...\n");
    sleep_ms(1000);
    reset_usb_boot(1, 0);
}

int main() {
    set_sys_clock_khz(133000, true);
    stdio_init_all();

    uart_init(uart0, 115200);

    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);  // 8-N-1
    uart_set_fifo_enabled(uart0, false);

    init_i2c_kbd();

    ui_init();
    unsigned char lcd_buffer[320 * 3] = {0};
    int index = 0;
    while (1) {
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
            case KEY_ENTER:
                a = te_interp(lcd_buffer, 0);
                sprintf(buf, "\n = %f\n", a);
                lcd_print_string(buf);
                memset(lcd_buffer, 0, sizeof(lcd_buffer));
                index = 0;

            case KEY_MOD_SHL:
            case KEY_MOD_SHR:
                break;
            default:
                if(c != -1 && c > 0) {
                    lcd_buffer[index++] = c;
                    lcd_putc(0,c);
                };
        }
        sleep_ms(20);
    }
}

