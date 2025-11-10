#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include <stdint.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/semphr.h"  // 添加semphr.h头文件

// 颜色定义
#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_RED       0xF800
#define COLOR_GREEN     0x07E0
#define COLOR_BLUE      0x001F
#define COLOR_CYAN      0x07FF
#define COLOR_YELLOW    0xFFE0

// 字体结构
typedef struct {
    uint8_t width;
    uint8_t height;
    const uint8_t *data;
} font_t;

// LCD配置结构
typedef struct {
    int miso_io_num;
    int mosi_io_num;
    int sclk_io_num;
    int cs_io_num;
    int dc_io_num;
    int rst_io_num;
    int spi_freq_hz;
    int width;
    int height;
    bool invert_colors;
} lcd_config_t;

// LCD显示结构
typedef struct {
    spi_device_handle_t spi;
    int width;
    int height;
    int dc_pin;
    int rst_pin;
    int cs_pin;
    font_t *current_font;
    uint16_t text_color;
    uint16_t bg_color;
    void (*custom_font_draw)(int x, int y, const char* str, uint16_t color);
    SemaphoreHandle_t spi_mutex;  // 添加互斥锁
} lcd_display_t;

// 函数声明
esp_err_t lcd_init(lcd_display_t *lcd, const lcd_config_t *config);
void lcd_send_command(lcd_display_t *lcd, uint8_t cmd);
void lcd_send_data(lcd_display_t *lcd, uint8_t data);
void lcd_send_data_buffer(lcd_display_t *lcd, const uint8_t *data, uint32_t length);
void lcd_set_window(lcd_display_t *lcd, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void lcd_draw_pixel(lcd_display_t *lcd, uint16_t x, uint16_t y, uint16_t color);
void lcd_fill_rect(lcd_display_t *lcd, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void lcd_fill_screen(lcd_display_t *lcd, uint16_t color);
void lcd_draw_char(lcd_display_t *lcd, uint16_t x, uint16_t y, char c);
void lcd_draw_string(lcd_display_t *lcd, uint16_t x, uint16_t y, const char *str);
void lcd_draw_custom_string(lcd_display_t *lcd, uint16_t x, uint16_t y, const char *str);
void lcd_set_font(lcd_display_t *lcd, font_t *font);
void lcd_set_text_color(lcd_display_t *lcd, uint16_t color);
void lcd_set_custom_font(lcd_display_t *lcd, void (*draw_func)(int x, int y, const char* str, uint16_t color));

// 预定义字体
extern font_t font_standard;
extern font_t font_large;
extern font_t font_xlarge;

#endif