#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_log.h"
#include "st7735_driver.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"  // 添加heap_caps_malloc所需的头文件

#include "lvgl_port.h"  // 添加自己的头文件

#define TAG   "lv_port"
#define LCD_WIDTH  128
#define LCD_HEIGHT  128

static lv_disp_drv_t   disp_drv;
static st7735_dev_t* st7735_dev = NULL;
static bool flush_ready = true;

// 刷新完成回调函数
static void flush_ready_callback(void* param) {
    flush_ready = true;
    lv_disp_flush_ready(&disp_drv);
}

static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    if (st7735_dev == NULL) {
        lv_disp_flush_ready(disp_drv);
        return;
    }
    
    // 等待上一次传输完成
    while (!flush_ready) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    flush_ready = false;
    
    // 调用ST7735的DMA刷新函数
    st7735_flush(st7735_dev, area->x1, area->x2 + 1, area->y1, area->y2 + 1, (void*)color_p);
    
    // 注意：flush_ready_callback会在DMA传输完成后被调用
}

void lv_disp_init(void) {
    static lv_disp_draw_buf_t disp_buf;
    
    // 分配双缓冲区（使用DMA capable内存）
    const size_t buf_size = LCD_WIDTH * 40; // 40行缓冲区
    lv_color_t* buf1 = heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t* buf2 = heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_DMA);
    
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Display buffer allocation failed!");
        return;
    }
    
    // 初始化显示缓冲区
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_size);
    
    // 初始化显示驱动
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.full_refresh = 0;
    
    // 注册显示驱动
    lv_disp_t* disp = lv_disp_drv_register(&disp_drv);
    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to register display driver");
        return;
    }
    
    ESP_LOGI(TAG, "LVGL display driver initialized with DMA");
}

void st7735_hw_init(void) {
    st7735_cfg_t cfg = {
        .mosi = GPIO_NUM_13,
        .clk = GPIO_NUM_12,
        .cs = GPIO_NUM_10,
        .dc = GPIO_NUM_9,
        .rst = GPIO_NUM_8,
        .bl = GPIO_NUM_15,
        .spi_freq = 27000000,
        .width = LCD_WIDTH,
        .height = LCD_HEIGHT,
        .x_offset = 0,
        .y_offset = 0,
        .rotation = 0,
        .done_cb = flush_ready_callback,
        .cb_param = NULL
    };
    
    esp_err_t ret = st7735_init(&cfg, &st7735_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7735 initialization failed");
    } else {
        ESP_LOGI(TAG, "ST7735 hardware initialized with DMA");
    }
}

static void lv_timer_cb(void* arg)
{
    uint32_t tick_interval = *(uint32_t*)arg;
    lv_tick_inc(tick_interval);
}

void lv_tick_init(void)
{
    static uint32_t tick_interval = 5;
    const esp_timer_create_args_t lv_timer_args = {
        .arg = &tick_interval,
        .callback = lv_timer_cb,
        .name = "lvgl_timer",
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    
    esp_timer_handle_t lv_timer;
    ESP_ERROR_CHECK(esp_timer_create(&lv_timer_args, &lv_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lv_timer, tick_interval * 1000));
    
    ESP_LOGI(TAG, "LVGL tick timer initialized with %d ms interval", tick_interval);
}

void lv_port_init(void)  // 重命名避免与lv_init冲突
{
    lv_init();  // 调用LVGL库的初始化函数
    st7735_hw_init();
    lv_disp_init();
    lv_tick_init();
    
    ESP_LOGI(TAG, "LVGL port initialization completed");
}

void lv_port_set_backlight(bool enable) {
    if (st7735_dev) {
        st7735_set_backlight(st7735_dev, enable);
    }
}