#include "lcd_driver.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "LCD_DRIVER";

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
#define ST7735_MADCTL  0x36
#define ST7735_COLMOD  0x3A

// 简单的字体数据（8x16）
const uint8_t font_8x16_data[] = {
    // 这里需要包含完整的8x16字体数据
    // 为简洁起见，只显示结构
};

font_t font_standard = {8, 16, font_8x16_data};
font_t font_large = {16, 24, NULL}; // 需要实际字体数据
font_t font_xlarge = {24, 32, NULL}; // 需要实际字体数据

static void lcd_spi_pre_transfer_callback(spi_transaction_t *t)
{
    lcd_display_t *lcd = (lcd_display_t *)t->user;
    if (lcd && lcd->dc_pin >= 0) {
        gpio_set_level(lcd->dc_pin, (int)t->cmd);
    }
}

esp_err_t lcd_init(lcd_display_t *lcd, const lcd_config_t *config)
{
    esp_err_t ret;
    
    // 检查参数有效性
    if (lcd == NULL || config == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 初始化GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->dc_io_num) | (1ULL << config->rst_io_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 配置SPI总线
    spi_bus_config_t buscfg = {
        .miso_io_num = config->miso_io_num,
        .mosi_io_num = config->mosi_io_num,
        .sclk_io_num = config->sclk_io_num,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 配置SPI设备
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = config->spi_freq_hz,
        .mode = 0,
        .spics_io_num = config->cs_io_num,
        .queue_size = 7,
        .pre_cb = lcd_spi_pre_transfer_callback,
    };
    
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &lcd->spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device addition failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return ret;
    }
    
    // 存储配置
    lcd->width = config->width;
    lcd->height = config->height;
    lcd->dc_pin = config->dc_io_num;
    lcd->rst_pin = config->rst_io_num;
    lcd->cs_pin = config->cs_io_num;
    lcd->current_font = &font_standard;
    lcd->text_color = COLOR_WHITE;
    lcd->bg_color = COLOR_BLACK;
    lcd->custom_font_draw = NULL;
    
    // 创建互斥锁
    lcd->spi_mutex = xSemaphoreCreateMutex();
    if (lcd->spi_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create SPI mutex");
        spi_bus_remove_device(lcd->spi);
        spi_bus_free(SPI2_HOST);
        return ESP_FAIL;
    }
    
    // 硬件复位
    gpio_set_level(lcd->rst_pin, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    gpio_set_level(lcd->rst_pin, 1);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    // 初始化序列
    lcd_send_command(lcd, ST7735_SWRESET);
    vTaskDelay(150 / portTICK_PERIOD_MS);
    
    lcd_send_command(lcd, ST7735_SLPOUT);
    vTaskDelay(150 / portTICK_PERIOD_MS);
    
    // 颜色模式设置
    lcd_send_command(lcd, ST7735_COLMOD);
    lcd_send_data(lcd, 0x05); // 16位像素
    
    // 内存数据访问控制
    lcd_send_command(lcd, ST7735_MADCTL);
    lcd_send_data(lcd, 0xC0); // 调整方向
    
    lcd_send_command(lcd, ST7735_INVON); // 反色（BGR）
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    lcd_send_command(lcd, ST7735_DISPON);
    vTaskDelay(150 / portTICK_PERIOD_MS);
    
    // 清屏
    lcd_fill_screen(lcd, COLOR_BLACK);
    
    ESP_LOGI(TAG, "LCD initialized successfully");
    return ESP_OK;
}

void lcd_send_command(lcd_display_t *lcd, uint8_t cmd)
{
    if (lcd == NULL || lcd->spi == NULL) {
        ESP_LOGE(TAG, "Invalid LCD or SPI handle");
        return;
    }

    if (xSemaphoreTake(lcd->spi_mutex, portMAX_DELAY) == pdTRUE) {
        spi_transaction_t t = {
            .length = 8,
            .tx_buffer = &cmd,
            .user = (void *)lcd,
            .cmd = 0, // DC线为0表示命令
        };
        spi_device_polling_transmit(lcd->spi, &t);
        xSemaphoreGive(lcd->spi_mutex);
    }
}

void lcd_send_data(lcd_display_t *lcd, uint8_t data)
{
    if (lcd == NULL || lcd->spi == NULL) {
        ESP_LOGE(TAG, "Invalid LCD or SPI handle");
        return;
    }

    if (xSemaphoreTake(lcd->spi_mutex, portMAX_DELAY) == pdTRUE) {
        spi_transaction_t t = {
            .length = 8,
            .tx_buffer = &data,
            .user = (void *)lcd,
            .cmd = 1, // DC线为1表示数据
        };
        spi_device_polling_transmit(lcd->spi, &t);
        xSemaphoreGive(lcd->spi_mutex);
    }
}

void lcd_set_window(lcd_display_t *lcd, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    lcd_send_command(lcd, ST7735_CASET);
    lcd_send_data(lcd, x0 >> 8);
    lcd_send_data(lcd, x0 & 0xFF);
    lcd_send_data(lcd, x1 >> 8);
    lcd_send_data(lcd, x1 & 0xFF);
    
    lcd_send_command(lcd, ST7735_RASET);
    lcd_send_data(lcd, y0 >> 8);
    lcd_send_data(lcd, y0 & 0xFF);
    lcd_send_data(lcd, y1 >> 8);
    lcd_send_data(lcd, y1 & 0xFF);
    
    lcd_send_command(lcd, ST7735_RAMWR);
}

void lcd_draw_pixel(lcd_display_t *lcd, uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= lcd->width || y >= lcd->height) return;
    
    lcd_set_window(lcd, x, y, x, y);
    lcd_send_data(lcd, color >> 8);
    lcd_send_data(lcd, color & 0xFF);
}

void lcd_fill_rect(lcd_display_t *lcd, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (lcd == NULL || lcd->spi == NULL) return;

    if (x + w > lcd->width) w = lcd->width - x;
    if (y + h > lcd->height) h = lcd->height - y;

    // 设置窗口（不加锁，因为内部函数已加锁）
    lcd_set_window(lcd, x, y, x + w - 1, y + h - 1);

    uint32_t pixels = w * h;
    uint8_t color_buffer[2] = { color >> 8, color & 0xFF };

    if (xSemaphoreTake(lcd->spi_mutex, portMAX_DELAY) == pdTRUE) {
        for (uint32_t i = 0; i < pixels; i++) {
            spi_transaction_t t = {
                .length = 16,
                .tx_buffer = color_buffer,
                .user = (void *)lcd,
                .cmd = 1, // DC线为1表示数据
            };
            spi_device_polling_transmit(lcd->spi, &t);
        }
        xSemaphoreGive(lcd->spi_mutex);
    }
}

void lcd_fill_screen(lcd_display_t *lcd, uint16_t color)
{
    lcd_fill_rect(lcd, 0, 0, lcd->width, lcd->height, color);
}

void lcd_draw_char(lcd_display_t *lcd, uint16_t x, uint16_t y, char c)
{
    if (lcd->current_font == NULL || lcd->current_font->data == NULL) return;
    
    // 简单的字符绘制实现
    // 需要根据实际字体数据实现
}

void lcd_draw_string(lcd_display_t *lcd, uint16_t x, uint16_t y, const char *str)
{
    if (lcd->current_font == NULL || lcd->current_font->data == NULL) return;
    
    uint16_t current_x = x;
    while (*str) {
        lcd_draw_char(lcd, current_x, y, *str);
        current_x += lcd->current_font->width;
        str++;
    }
}

void lcd_draw_custom_string(lcd_display_t *lcd, uint16_t x, uint16_t y, const char *str)
{
    if (lcd->custom_font_draw) {
        lcd->custom_font_draw(x, y, str, lcd->text_color);
    }
}

void lcd_set_font(lcd_display_t *lcd, font_t *font)
{
    lcd->current_font = font;
}

void lcd_set_text_color(lcd_display_t *lcd, uint16_t color)
{
    lcd->text_color = color;
}

void lcd_set_custom_font(lcd_display_t *lcd, void (*draw_func)(int x, int y, const char* str, uint16_t color))
{
    lcd->custom_font_draw = draw_func;
}