#ifndef COYOTE_TEST_FAKE_LCDSPI_H
#define COYOTE_TEST_FAKE_LCDSPI_H

/* Host stand-in for lcdspi/lcdspi.h: the parts of the display and keyboard
 * interface the UI uses, implemented by test_mode_menu.c. */

#include <stdint.h>

#define LCD_WIDTH 320
#define LCD_HEIGHT 320

#define WHITE   0xFFFFFF
#define BLACK   0x000000
#define GRAY    0x808080
#define RED     0xFF0000
#define GREEN   0x00FF00
#define BLUE    0x0000FF
#define CYAN    0x00FFFF
#define MAGENTA 0xFF00FF

void lcd_init();
void lcd_clear();
int lcd_getc(uint8_t devn);
void lcd_print_char_at(int fc, int bc, char c, int orientation, int x, int y);
void lcd_print_string(char *s);
void lcd_set_text_color(int fc, int bc);
void draw_rect_spi(int x1, int y1, int x2, int y2, int c);
void spi_draw_pixel(uint16_t x, uint16_t y, uint32_t color);
void set_current_x(int x);
void set_current_y(int y);

#endif
