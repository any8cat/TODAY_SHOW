#ifndef LV_PORT_H
#define LV_PORT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化LVGL显示端口
 * 
 * 该函数初始化LVGL库、ST7735硬件和显示驱动
 */
void lv_port_init(void);

/**
 * @brief 初始化ST7735硬件
 * 
 * 配置SPI引脚、复位引脚、背光引脚等硬件参数
 */
void st7735_hw_init(void);

/**
 * @brief 初始化LVGL显示驱动
 * 
 * 设置显示缓冲区、分辨率、刷新回调等
 */
void lv_disp_init(void);

/**
 * @brief 初始化LVGL定时器
 * 
 * 创建周期性定时器用于LVGL的tick计数
 */
void lv_tick_init(void);

void lv_port_set_backlight(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_H */