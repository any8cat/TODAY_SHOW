#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include <stdint.h>
#include "driver/spi_master.h"
#include "freertos/semphr.h"

// 颜色定义
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_CYAN    0x07FF
#define COLOR_YELLOW  0xFFE0
#define COLOR_MAGENTA 0xF81F

// ST7735_GREENTAB3 偏移量定义
#define ST7735_GREENTAB3_X_OFFSET 2
#define ST7735_GREENTAB3_Y_OFFSET 3

// 字体结构体定义
typedef struct {
    uint8_t width;
    uint8_t height;
    const uint8_t *data;
} font_t;

// 字体大小枚举
typedef enum {
    FONT_SIZE_SMALL = 0,
    FONT_SIZE_MEDIUM,
    FONT_SIZE_LARGE,
    FONT_SIZE_XLARGE
} font_size_t;

// LCD配置结构体
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

// LCD显示结构体
typedef struct {
    spi_device_handle_t spi;
    int dc_pin;
    int rst_pin;
    int cs_pin;
    int width;
    int height;
    font_t *current_font;
    uint16_t text_color;
    uint16_t bg_color;
    void (*custom_font_draw)(int x, int y, const char* str, uint16_t color);
    SemaphoreHandle_t spi_mutex;
    uint8_t x_offset;
    uint8_t y_offset;
} lcd_display_t;

// 字体变量声明
extern font_t font_standard;
extern font_t font_medium;
extern font_t font_large;
extern font_t font_xlarge;

// 函数声明
esp_err_t lcd_init(lcd_display_t *lcd, const lcd_config_t *config);
void lcd_send_command(lcd_display_t *lcd, uint8_t cmd);
void lcd_send_data(lcd_display_t *lcd, uint8_t data);
void lcd_set_window(lcd_display_t *lcd, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void lcd_draw_pixel(lcd_display_t *lcd, uint16_t x, uint16_t y, uint16_t color);
void lcd_fill_rect(lcd_display_t *lcd, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void lcd_fill_screen(lcd_display_t *lcd, uint16_t color);
void lcd_draw_char(lcd_display_t *lcd, uint16_t x, uint16_t y, char c);
void lcd_draw_string(lcd_display_t *lcd, uint16_t x, uint16_t y, const char *str);
void lcd_draw_custom_string(lcd_display_t *lcd, uint16_t x, uint16_t y, const char *str);
void lcd_set_font(lcd_display_t *lcd, font_t *font);
void lcd_set_font_size(lcd_display_t *lcd, font_size_t size); // 新增函数
void lcd_set_text_color(lcd_display_t *lcd, uint16_t color);
void lcd_set_bg_color(lcd_display_t *lcd, uint16_t color); // 新增函数
void lcd_set_custom_font(lcd_display_t *lcd, void (*draw_func)(int x, int y, const char* str, uint16_t color));
void lcd_draw_image(lcd_display_t *lcd, int x, int y, int width, int height, const uint16_t *image);
void lcd_validate_fonts(void);

// 新增：获取字符串宽度（用于布局计算）
uint16_t lcd_get_string_width(lcd_display_t *lcd, const char *str);

#endif // LCD_DRIVER_H