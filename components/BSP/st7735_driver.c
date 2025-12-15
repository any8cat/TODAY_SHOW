#include "st7735_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "ST7735";

// 刷新完成回调函数
static lcd_flush_done_cb s_flush_done_cb = NULL;
static void* s_cb_param = NULL;

// SPI传输完成回调
static bool notify_flush_ready(esp_lcd_panel_io_handle_t panel_io, 
                              esp_lcd_panel_io_event_data_t *edata, 
                              void *user_ctx) {
    if (s_flush_done_cb) {
        s_flush_done_cb(s_cb_param);
    }
    return false;
}

esp_err_t st7735_init(st7735_cfg_t* cfg, st7735_dev_t** dev) {
    if (cfg == NULL || dev == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    // 分配设备内存
    st7735_dev_t* lcd_dev = malloc(sizeof(st7735_dev_t));
    if (lcd_dev == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for LCD device");
        return ESP_ERR_NO_MEM;
    }

    // 初始化设备结构
    lcd_dev->bl_pin = cfg->bl;
    lcd_dev->width = cfg->width;
    lcd_dev->height = cfg->height;
    lcd_dev->x_offset = cfg->x_offset;
    lcd_dev->y_offset = cfg->y_offset;

    // 设置回调函数
    s_flush_done_cb = cfg->done_cb;
    s_cb_param = cfg->cb_param;

    // 配置SPI总线（使用DMA）
    spi_bus_config_t buscfg = {
        .sclk_io_num = cfg->clk,
        .mosi_io_num = cfg->mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = cfg->width * cfg->height * 2 + 8,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus initialization failed");
        free(lcd_dev);
        return ret;
    }

    // 初始化背光GPIO
    if (cfg->bl >= 0) {
        gpio_config_t bl_conf = {
            .pin_bit_mask = (1ULL << cfg->bl),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = gpio_config(&bl_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Backlight GPIO config failed");
            free(lcd_dev);
            return ret;
        }
    }

    // 初始化复位GPIO
    if (cfg->rst >= 0) {
        gpio_config_t rst_conf = {
            .pin_bit_mask = (1ULL << cfg->rst),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = gpio_config(&rst_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Reset GPIO config failed");
            free(lcd_dev);
            return ret;
        }
    }

    // 创建LCD面板IO句柄（使用DMA）
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = cfg->dc,
        .cs_gpio_num = cfg->cs,
        .pclk_hz = cfg->spi_freq,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_flush_ready,
        .user_ctx = s_cb_param,
        .flags = {
            .sio_mode = 0, // 单线模式
        },
    };

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &lcd_dev->io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LCD IO handle");
        spi_bus_free(SPI2_HOST);
        free(lcd_dev);
        return ret;
    }

    // 硬件复位
    if (cfg->rst >= 0) {
        gpio_set_level(cfg->rst, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(cfg->rst, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // ST7735初始化序列
    ESP_LOGI(TAG, "Starting ST7735 initialization with DMA");
    
    // 软件复位
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    
    // 退出睡眠模式
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    
    // 帧率控制
    uint8_t framerate_data1[] = {0x01, 0x2C, 0x2D};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_FRMCTR1, framerate_data1, 3);
    
    uint8_t framerate_data2[] = {0x01, 0x2C, 0x2D};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_FRMCTR2, framerate_data2, 3);
    
    uint8_t framerate_data3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_FRMCTR3, framerate_data3, 6);
    
    // 显示反转控制
    uint8_t inv_data[] = {0x07};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_INVCTR, inv_data, 1);
    
    // 电源控制
    uint8_t pwctr1_data[] = {0xA2, 0x02, 0x84};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_PWCTR1, pwctr1_data, 3);
    
    uint8_t pwctr2_data[] = {0xC5};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_PWCTR2, pwctr2_data, 1);
    
    uint8_t pwctr3_data[] = {0x0A, 0x00};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_PWCTR3, pwctr3_data, 2);
    
    uint8_t pwctr4_data[] = {0x8A, 0x2A};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_PWCTR4, pwctr4_data, 2);
    
    uint8_t pwctr5_data[] = {0x8A, 0xEE};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_PWCTR5, pwctr5_data, 2);
    
    uint8_t vmctr_data[] = {0x0E};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_VMCTR1, vmctr_data, 1);
    
    // 内存数据访问控制
    uint8_t madctl = 0xC8; // 对于GREENTAB3使用0xC8
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_MADCTL, &madctl, 1);
    
    // 接口像素格式
    uint8_t colmod_data[] = {0x05}; // 16位像素
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_COLMOD, colmod_data, 1);
    
    // 伽马校正
    uint8_t gamma_pos[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                          0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_GMCTRP1, gamma_pos, 16);
    
    uint8_t gamma_neg[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                          0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_GMCTRN1, gamma_neg, 16);
    
    // 正常显示模式
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, 0x13, NULL, 0); // NORON
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 开启显示
    esp_lcd_panel_io_tx_param(lcd_dev->io_handle, ST7735_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    
    // 开启背光
    if (lcd_dev->bl_pin >= 0) {
        st7735_set_backlight(lcd_dev, true);
    }
    
    *dev = lcd_dev;
    ESP_LOGI(TAG, "ST7735 initialized successfully with DMA");
    return ESP_OK;
}

void st7735_flush(st7735_dev_t* dev, int x1, int x2, int y1, int y2, void* data) {
    if (dev == NULL || data == NULL) {
        if (s_flush_done_cb) {
            s_flush_done_cb(s_cb_param);
        }
        return;
    }

    // 检查坐标有效性
    if (x2 <= x1 || y2 <= y1) {
        if (s_flush_done_cb) {
            s_flush_done_cb(s_cb_param);
        }
        return;
    }

    // 应用偏移量
    x1 += dev->x_offset;
    x2 += dev->x_offset;
    y1 += dev->y_offset;
    y2 += dev->y_offset;

    // 设置显示窗口（与ST7789相同的方式）
    uint8_t col_data[] = {
        (x1 >> 8) & 0xFF,
        x1 & 0xFF,
        ((x2 - 1) >> 8) & 0xFF,
        (x2 - 1) & 0xFF,
    };
    esp_lcd_panel_io_tx_param(dev->io_handle, ST7735_CASET, col_data, 4);
    
    uint8_t row_data[] = {
        (y1 >> 8) & 0xFF,
        y1 & 0xFF,
        ((y2 - 1) >> 8) & 0xFF,
        (y2 - 1) & 0xFF,
    };
    esp_lcd_panel_io_tx_param(dev->io_handle, ST7735_RASET, row_data, 4);
    
    // 传输像素数据（使用DMA）
    size_t len = (x2 - x1) * (y2 - y1) * 2;
    esp_lcd_panel_io_tx_color(dev->io_handle, ST7735_RAMWR, data, len);
}

void st7735_set_backlight(st7735_dev_t* dev, bool enable) {
    if (dev && dev->bl_pin >= 0) {
        gpio_set_level(dev->bl_pin, enable ? 1 : 0);
    }
}

void st7735_set_rotation(st7735_dev_t* dev, uint8_t rotation) {
    if (dev == NULL) return;
    
    uint8_t madctl = 0;
    switch (rotation % 4) {
        case 0: // 0度
            madctl = 0xC8;
            break;
        case 1: // 90度
            madctl = 0xA8;
            break;
        case 2: // 180度
            madctl = 0x08;
            break;
        case 3: // 270度
            madctl = 0x68;
            break;
    }
    esp_lcd_panel_io_tx_param(dev->io_handle, ST7735_MADCTL, &madctl, 1);
}

void st7735_sleep_in(st7735_dev_t* dev) {
    if (dev) {
        esp_lcd_panel_io_tx_param(dev->io_handle, ST7735_SLPIN, NULL, 0);
    }
}

void st7735_sleep_out(st7735_dev_t* dev) {
    if (dev) {
        esp_lcd_panel_io_tx_param(dev->io_handle, ST7735_SLPOUT, NULL, 0);
    }
}

void st7735_display_off(st7735_dev_t* dev) {
    if (dev) {
        esp_lcd_panel_io_tx_param(dev->io_handle, ST7735_DISPOFF, NULL, 0);
    }
}

void st7735_display_on(st7735_dev_t* dev) {
    if (dev) {
        esp_lcd_panel_io_tx_param(dev->io_handle, ST7735_DISPON, NULL, 0);
    }
}

void st7735_deinit(st7735_dev_t* dev) {
    if (dev) {
        if (dev->io_handle) {
            esp_lcd_panel_io_del(dev->io_handle);
        }
        spi_bus_free(SPI2_HOST);
        free(dev);
    }
}