/**
 * PicoCalc Hello World
 * https://www.clockworkpi.com/
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "i2ckbd.h"
#include "lcdspi.h"
#include "pico/bootrom.h"
#include "UI/ui.h"

const uint LEDPIN = 25;


void test_battery(){
    char buf[64];
    int bat_pcnt = read_battery();
    bat_pcnt = bat_pcnt>>8;
    int bat_charging = bitRead(bat_pcnt,7);
    bitClear(bat_pcnt,7);
    sprintf(buf,"battery percent is %d\n",bat_pcnt);
    printf("%s", buf);
    lcd_print_string(buf);
    if(bat_charging ==0 ){
        sprintf(buf,"battery is not charging\n");
    }else{
        sprintf(buf,"battery is charging\n");
    }
    printf("%s", buf);
    lcd_print_string(buf);
}

void test_kbd_backlight(uint8_t val) {
    char buf[64];
    int kbd_backlight = set_kbd_backlight(val);
    kbd_backlight = kbd_backlight>>8;

    sprintf(buf, "kbd backlight %d\n", kbd_backlight);
    printf("%s", buf);
    lcd_print_string(buf);
}

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

int main() {
    set_sys_clock_khz(133000, true);
    stdio_init_all();

    uart_init(uart0, 115200);

    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);  // 8-N-1
    uart_set_fifo_enabled(uart0, false);

    init_i2c_kbd();

    ui_init();
    while (1) {
        int c = lcd_getc(0);
        if(c != -1 && c > 0) {
            lcd_putc(0,c);
        }
        if (c == 0x85) {
            lcd_clear();
            lcd_print_string("Rebooting to bootsel...\n");
            sleep_ms(1000);
            reset_usb_boot(1,0);
        }
        sleep_ms(20);
    }
}

