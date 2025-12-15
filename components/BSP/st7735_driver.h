#ifndef _ST7735_DRIVER_H_
#define _ST7735_DRIVER_H_

#include "driver/gpio.h"
#include "esp_err.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

// ST7735命令定义
#define ST7735_NOP     0x00
#define ST7735_SWRESET 0x01
#define ST7735_SLPIN   0x10
#define ST7735_SLPOUT  0x11
#define ST7735_INVOFF  0x20
#define ST7735_INVON   0x21
#define ST7735_DISPOFF 0x28
#define ST7735_DISPON  0x29
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_RAMRD   0x2E
#define ST7735_MADCTL  0x36
#define ST7735_COLMOD  0x3A
#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR  0xB4
#define ST7735_PWCTR1  0xC0
#define ST7735_PWCTR2  0xC1
#define ST7735_PWCTR3  0xC2
#define ST7735_PWCTR4  0xC3
#define ST7735_PWCTR5  0xC4
#define ST7735_VMCTR1  0xC5
#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1

// 颜色定义
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_CYAN    0x07FF
#define COLOR_YELLOW  0xFFE0
#define COLOR_MAGENTA 0xF81F

typedef void(*lcd_flush_done_cb)(void* param);

// ST7735配置结构体（参考ST7789）
typedef struct {
    gpio_num_t  mosi;       // MOSI引脚
    gpio_num_t  clk;        // SCLK引脚  
    gpio_num_t  cs;         // CS引脚
    gpio_num_t  dc;         // DC引脚
    gpio_num_t  rst;        // RST引脚
    gpio_num_t  bl;         // 背光引脚（可选）
    uint32_t    spi_freq;   // SPI频率
    uint16_t    width;      // 屏幕宽度
    uint16_t    height;     // 屏幕高度
    uint8_t     x_offset;   // X轴偏移（用于不同型号）
    uint8_t     y_offset;   // Y轴偏移（用于不同型号）
    uint8_t     rotation;   // 旋转方向 (0-3)
    lcd_flush_done_cb done_cb;    // 刷新完成回调函数
    void*       cb_param;   // 回调函数参数
} st7735_cfg_t;

// LCD设备句柄（使用esp_lcd接口）
typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    gpio_num_t bl_pin;
    uint16_t width;
    uint16_t height;
    uint8_t x_offset;
    uint8_t y_offset;
} st7735_dev_t;

/**
 * @brief 初始化ST7735显示屏（使用DMA）
 * @param cfg 配置参数
 * @param dev 设备句柄指针
 * @return esp_err_t 错误代码
 */
esp_err_t st7735_init(st7735_cfg_t* cfg, st7735_dev_t** dev);

/**
 * @brief ST7735刷新显示区域（使用DMA传输）
 * @param dev ST7735设备句柄
 * @param x1 起始x坐标
 * @param x2 结束x坐标（不包含）
 * @param y1 起始y坐标
 * @param y2 结束y坐标（不包含）
 * @param data 像素数据（RGB565格式）
 */
void st7735_flush(st7735_dev_t* dev, int x1, int x2, int y1, int y2, void* data);

/**
 * @brief 控制背光
 * @param dev 设备句柄
 * @param enable 是否开启背光
 */
void st7735_set_backlight(st7735_dev_t* dev, bool enable);

/**
 * @brief 设置显示方向
 * @param dev 设备句柄
 * @param rotation 旋转方向 (0-3)
 */
void st7735_set_rotation(st7735_dev_t* dev, uint8_t rotation);

/**
 * @brief 进入睡眠模式
 * @param dev 设备句柄
 */
void st7735_sleep_in(st7735_dev_t* dev);

/**
 * @brief 退出睡眠模式
 * @param dev 设备句柄
 */
void st7735_sleep_out(st7735_dev_t* dev);

/**
 * @brief 关闭显示
 * @param dev 设备句柄
 */
void st7735_display_off(st7735_dev_t* dev);

/**
 * @brief 开启显示
 * @param dev 设备句柄
 */
void st7735_display_on(st7735_dev_t* dev);

/**
 * @brief 释放设备资源
 * @param dev 设备句柄
 */
void st7735_deinit(st7735_dev_t* dev);

#endif // _ST7735_DRIVER_H_