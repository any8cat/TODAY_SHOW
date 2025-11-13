#ifndef FONTS_H
#define FONTS_H

#include <stdint.h>
#include "lcd_driver.h"

// 汉字字模结构
typedef struct {
    char index[4];           // 汉字GB2312编码（3字节+结束符）
    const uint8_t *bitmap;    // 点阵数据指针
    uint8_t width;           // 汉字宽度（通常为16）
} chinese_char_t;

// 函数声明
void set_global_lcd(lcd_display_t *lcd);
void show_custom_font(int x, int y, const char* str, uint16_t color);
void show_single_char(int x, int y, const char* ch, uint16_t color);
void lcd_draw_rect(lcd_display_t *lcd, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

// 外部字模声明
extern const chinese_char_t chinese_chars[];
extern const uint16_t thunderGod[];
#endif