#include "lcd_driver.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"

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

// 简单的字体数据
const uint8_t font_8x16_data[] = {
    // 这里需要包含完整的8x16字体数据
    // 为简洁起见，只显示结构
};

// 字体变量定义
font_t font_standard = {8, 16, font_8x16_data};
font_t font_large = {16, 24, NULL};
font_t font_xlarge = {24, 32, NULL};

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
        .max_transfer_sz = 128 * 128 * 2 + 8,
    };
    
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 配置SPI设备
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 27000000,
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
    
    // 设置GREENTAB3偏移量
    lcd->x_offset = ST7735_GREENTAB3_X_OFFSET;
    lcd->y_offset = ST7735_GREENTAB3_Y_OFFSET;
    
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
    
    // 完整的ST7735初始化序列（解决对比度问题）
    ESP_LOGI(TAG, "Starting complete ST7735 initialization");
    
    // 软件复位
    lcd_send_command(lcd, ST7735_SWRESET);
    vTaskDelay(150 / portTICK_PERIOD_MS);
    
    // 退出睡眠模式
    lcd_send_command(lcd, ST7735_SLPOUT);
    vTaskDelay(150 / portTICK_PERIOD_MS);
    
    // 帧率控制 - 正常模式
    lcd_send_command(lcd, ST7735_FRMCTR1);
    lcd_send_data(lcd, 0x01);
    lcd_send_data(lcd, 0x2C);
    lcd_send_data(lcd, 0x2D);
    
    lcd_send_command(lcd, ST7735_FRMCTR2);
    lcd_send_data(lcd, 0x01);
    lcd_send_data(lcd, 0x2C);
    lcd_send_data(lcd, 0x2D);
    
    // 帧率控制 - 空闲模式
    lcd_send_command(lcd, ST7735_FRMCTR3);
    lcd_send_data(lcd, 0x01);
    lcd_send_data(lcd, 0x2C);
    lcd_send_data(lcd, 0x2D);
    lcd_send_data(lcd, 0x01);
    lcd_send_data(lcd, 0x2C);
    lcd_send_data(lcd, 0x2D);
    
    // 显示反转控制
    lcd_send_command(lcd, ST7735_INVCTR);
    lcd_send_data(lcd, 0x07);  // 无反转
    
    // 电源控制 - 这是提高对比度的关键
    lcd_send_command(lcd, ST7735_PWCTR1);
    lcd_send_data(lcd, 0xA2);
    lcd_send_data(lcd, 0x02);
    lcd_send_data(lcd, 0x84);
    
    lcd_send_command(lcd, ST7735_PWCTR2);
    lcd_send_data(lcd, 0xC5);
    
    lcd_send_command(lcd, ST7735_PWCTR3);
    lcd_send_data(lcd, 0x0A);
    lcd_send_data(lcd, 0x00);
    
    lcd_send_command(lcd, ST7735_PWCTR4);
    lcd_send_data(lcd, 0x8A);
    lcd_send_data(lcd, 0x2A);
    
    lcd_send_command(lcd, ST7735_PWCTR5);
    lcd_send_data(lcd, 0x8A);
    lcd_send_data(lcd, 0xEE);
    
    lcd_send_command(lcd, ST7735_VMCTR1);
    lcd_send_data(lcd, 0x0E);  // VCOM控制，影响对比度
    
    // 内存数据访问控制
    lcd_send_command(lcd, ST7735_MADCTL);
    lcd_send_data(lcd, 0xC8);  // 对于GREENTAB3使用0xC8
    
    // 接口像素格式
    lcd_send_command(lcd, ST7735_COLMOD);
    lcd_send_data(lcd, 0x05);  // 16位像素
    
    // 伽马校正 - 这是解决颜色问题的关键
    lcd_send_command(lcd, ST7735_GMCTRP1);
    lcd_send_data(lcd, 0x02); lcd_send_data(lcd, 0x1C); lcd_send_data(lcd, 0x07); lcd_send_data(lcd, 0x12);
    lcd_send_data(lcd, 0x37); lcd_send_data(lcd, 0x32); lcd_send_data(lcd, 0x29); lcd_send_data(lcd, 0x2D);
    lcd_send_data(lcd, 0x29); lcd_send_data(lcd, 0x25); lcd_send_data(lcd, 0x2B); lcd_send_data(lcd, 0x39);
    lcd_send_data(lcd, 0x00); lcd_send_data(lcd, 0x01); lcd_send_data(lcd, 0x03); lcd_send_data(lcd, 0x10);
    
    lcd_send_command(lcd, ST7735_GMCTRN1);
    lcd_send_data(lcd, 0x03); lcd_send_data(lcd, 0x1D); lcd_send_data(lcd, 0x07); lcd_send_data(lcd, 0x06);
    lcd_send_data(lcd, 0x2E); lcd_send_data(lcd, 0x2C); lcd_send_data(lcd, 0x29); lcd_send_data(lcd, 0x2D);
    lcd_send_data(lcd, 0x2E); lcd_send_data(lcd, 0x2E); lcd_send_data(lcd, 0x37); lcd_send_data(lcd, 0x3F);
    lcd_send_data(lcd, 0x00); lcd_send_data(lcd, 0x00); lcd_send_data(lcd, 0x02); lcd_send_data(lcd, 0x10);
    
    // 设置显示窗口
    lcd_set_window(lcd, 0, 0, lcd->width - 1, lcd->height - 1);
    
    // 正常显示模式
    lcd_send_command(lcd, 0x13);  // NORON
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    // 开启显示
    lcd_send_command(lcd, ST7735_DISPON);
    vTaskDelay(150 / portTICK_PERIOD_MS);
    
    // 清屏
    lcd_fill_screen(lcd, COLOR_BLACK);
    
    ESP_LOGI(TAG, "LCD initialized successfully with enhanced contrast settings");
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
    // 应用GREENTAB3偏移量
    x0 += lcd->x_offset;
    x1 += lcd->x_offset;
    y0 += lcd->y_offset;
    y1 += lcd->y_offset;
    
    // 确保坐标在有效范围内
    if (x1 >= lcd->width + lcd->x_offset) x1 = lcd->width + lcd->x_offset - 1;
    if (y1 >= lcd->height + lcd->y_offset) y1 = lcd->height + lcd->y_offset - 1;
    
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

// 其他函数保持不变...
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

    lcd_set_window(lcd, x, y, x + w - 1, y + h - 1);

    uint32_t pixels = w * h;
    uint8_t color_buffer[2] = { color >> 8, color & 0xFF };

    if (xSemaphoreTake(lcd->spi_mutex, portMAX_DELAY) == pdTRUE) {
        for (uint32_t i = 0; i < pixels; i++) {
            spi_transaction_t t = {
                .length = 16,
                .tx_buffer = color_buffer,
                .user = (void *)lcd,
                .cmd = 1,
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

void lcd_draw_image(lcd_display_t *lcd, int x, int y, int width, int height, const uint16_t *image) 
{
    if (lcd == NULL || image == NULL) {
        ESP_LOGE(TAG, "Invalid parameters in lcd_draw_image");
        return;
    }
    
    ESP_LOGI(TAG, "Drawing image at (%d,%d) size %dx%d with offsets x=%d, y=%d", 
             x, y, width, height, lcd->x_offset, lcd->y_offset);
    
    // 设置显示窗口（应用偏移）
    lcd_set_window(lcd, x, y, x + width - 1, y + height - 1);
    
    if (xSemaphoreTake(lcd->spi_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < width * height; i++) {
            uint16_t color = image[i];
            // 颜色转换：RGB565 -> BGR565
            uint16_t bgr_color = ((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8);
            
            spi_transaction_t t = {
                .length = 16,
                .tx_buffer = &bgr_color,
                .user = (void *)lcd,
                .cmd = 1,
            };
            spi_device_polling_transmit(lcd->spi, &t);
        }
        xSemaphoreGive(lcd->spi_mutex);
    }
    
    ESP_LOGI(TAG, "Image display completed");
}