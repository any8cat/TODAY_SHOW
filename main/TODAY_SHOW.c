#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "esp_sntp.h"

#include "lvgl.h"
#include "lvgl_port.h"
#include "st7735_driver.h"
#include "weather.h"
#include "thunderGod.c"

// 字体声明
LV_FONT_DECLARE(myFont_16);
LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_24);
//图片声明
LV_IMG_DECLARE(thunderGod);

static const char *TAG = "TFT_CLOCK";

// WiFi配置
#define WIFI_SSID      "ZYUX"
#define WIFI_PASS      "3085129162"

// 星期名称
const char* weekDays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

// 全局变量
char now_address[16] = "杭州";
char now_temperature[8] = "";
char now_weather[32] = "";
uint32_t lastWeatherUpdate = 0;
const uint32_t WEATHER_UPDATE_INTERVAL = 300000; // 5分钟

// LVGL对象
static lv_obj_t *bg_img;           // 背景图片
static lv_obj_t *label_addr;       // 地址标签
static lv_obj_t *label_weather;    // 天气标签
static lv_obj_t *label_temp;       // 温度标签
static lv_obj_t *label_time;       // 时间标签
static lv_obj_t *label_sec;        // 秒数标签
static lv_obj_t *label_date;       // 日期标签
static lv_obj_t *label_week;       // 星期标签

// 时间显示相关
uint32_t lastTimeDisplay = 0;
const uint32_t TIME_DISPLAY_INTERVAL = 30000; // 30秒显示一次时间

// 函数声明
static void obtain_time(void);
static void initialize_sntp(void);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void create_lvgl_ui(void);
void update_time_display(void);
void update_weather_display(void);
void check_network_connection(void);

// 设置时区为北京时间（UTC+8）
void set_timezone(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to Beijing Time (UTC+8)");
}

// 时间同步通知回调函数
void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronization notification received");
}

// 网络检查函数
void check_network_connection(void)
{
    // 检查WiFi连接状态
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi RSSI: %d dBm", ap_info.rssi);
    }
}

void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    
    // 设置时区
    set_timezone();
    
    // 使用ESP-SNTP初始化方法
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // 设置多个备用NTP服务器
    const char* servers[] = {
        "pool.ntp.org",
        "cn.pool.ntp.org",
        "time.apple.com",
        "time.windows.com"
    };
    
    for (int i = 0; i < 4; i++) {
        esp_sntp_setservername(i, servers[i]);
    }
    
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    sntp_set_sync_interval(30000);
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
}

void obtain_time(void)
{
    initialize_sntp();

    // 等待时间同步（最多60秒）
    int retry = 0;
    const int retry_count = 30;
    
    TickType_t start_ticks = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(60000);
    
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && 
           (xTaskGetTickCount() - start_ticks) < timeout_ticks) {
        retry++;
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        
        if (retry % 5 == 0) {
            check_network_connection();
        }
        
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    
    if (retry >= retry_count || (xTaskGetTickCount() - start_ticks) >= timeout_ticks) {
        ESP_LOGW(TAG, "SNTP synchronization timeout");
        
        // 设置默认时间
        struct timeval tv = {
            .tv_sec = 1704067200, // 2024-01-01 00:00:00
            .tv_usec = 0
        };
        if (settimeofday(&tv, NULL) == 0) {
            ESP_LOGI(TAG, "Default time set successfully");
        }
    } else {
        ESP_LOGI(TAG, "SNTP synchronization completed successfully");
        
        // 同步成功后获取天气信息
        if (get_weather_info(now_weather, now_temperature, sizeof(now_weather))) {
            ESP_LOGI(TAG, "Weather info obtained: %s, %s", now_weather, now_temperature);
            update_weather_display();
        }
    }
}

// 创建LVGL用户界面
void create_lvgl_ui(void)
{
    bg_img = lv_img_create(lv_scr_act());
    lv_img_set_src(bg_img, &thunderGod);  // 使用图片
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
    
    // 创建地址标签（左上角）
    label_addr = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_addr, &myFont_16, 0);
    lv_obj_set_style_text_color(label_addr, lv_color_white(), 0);
    lv_label_set_text(label_addr, now_address);
    lv_obj_align(label_addr, LV_ALIGN_TOP_LEFT, 5, 5);
    
    // 创建天气标签（右上角）
    label_weather = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_weather, &myFont_16, 0);
    lv_obj_set_style_text_color(label_weather, lv_color_white(), 0);
    lv_label_set_text(label_weather, "天气");
    lv_obj_align(label_weather, LV_ALIGN_TOP_RIGHT, -5, 5);
    
    // 创建温度标签
    label_temp = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_temp, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label_temp, lv_color_hex(0x00FFFF), 0); // 青色
    lv_label_set_text(label_temp, "N/A");
    lv_obj_align(label_temp, LV_ALIGN_TOP_RIGHT, -5, 25);
    
    // 创建时间标签（中间偏下）
    label_time = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_time, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_time, lv_color_white(), 0);
    lv_label_set_text(label_time, "00:00");
    lv_obj_align(label_time, LV_ALIGN_CENTER, 0, 20);
    
    // 创建秒数标签
    label_sec = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_sec, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label_sec, lv_color_white(), 0);
    lv_label_set_text(label_sec, "00");
    lv_obj_align(label_sec, LV_ALIGN_CENTER, 40, 35);
    
    // 创建日期标签
    label_date = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_date, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label_date, lv_color_white(), 0);
    lv_label_set_text(label_date, "01/01");
    lv_obj_align(label_date, LV_ALIGN_CENTER, -20, 50);
    
    // 创建星期标签
    label_week = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_week, &myFont_16, 0);
    lv_obj_set_style_text_color(label_week, lv_color_white(), 0);
    lv_label_set_text(label_week, "周一");
    lv_obj_align(label_week, LV_ALIGN_CENTER, 20, 50);
}

// 更新时间显示
void update_time_display(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // 更新时间
    char time_str[6];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    lv_label_set_text(label_time, time_str);
    
    // 更新秒数
    char sec_str[3];
    snprintf(sec_str, sizeof(sec_str), "%02d", timeinfo.tm_sec);
    lv_label_set_text(label_sec, sec_str);
    
    // 更新日期
    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%02d/%02d", timeinfo.tm_mon + 1, timeinfo.tm_mday);
    lv_label_set_text(label_date, date_str);
    
    // 更新星期
    lv_label_set_text(label_week, weekDays[timeinfo.tm_wday]);
    
    // 每30秒显示一次当前时间到日志
    static time_t last_log_time = 0;
    if (now - last_log_time >= TIME_DISPLAY_INTERVAL) {
        ESP_LOGI(TAG, "当前时间: %04d-%02d-%02d %02d:%02d:%02d %s",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                 weekDays[timeinfo.tm_wday]);
        last_log_time = now;
    }
}

// 更新天气显示
void update_weather_display(void)
{
    lv_label_set_text(label_weather, now_weather);
    lv_label_set_text(label_temp, now_temperature);
}

// WiFi事件处理函数
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    static int retry_count = 0;
    const int max_retry = 5;
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        retry_count = 0;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < max_retry) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retry to connect to the AP (%d/%d)", retry_count, max_retry);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        
        check_network_connection();
        obtain_time();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting TFT Clock Application with LVGL");
    
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化LVGL端口
    lv_port_init();
    
    // 创建LVGL用户界面
    create_lvgl_ui();
    
    // 显示连接中信息
    lv_label_set_text(label_addr, "连接中...");
    
    // 初始化WiFi和网络事件
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册WiFi事件处理
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.rssi = -127,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi...");
    
    // 主循环
    time_t last_weather_update = 0;
    
    while (1) {
        // LVGL任务处理
        lv_timer_handler();
        
        // 更新时间显示
        update_time_display();
        
        // 获取当前时间
        time_t now = time(NULL);
        
        // 定期更新天气信息
        if (now - last_weather_update >= WEATHER_UPDATE_INTERVAL) {
            ESP_LOGI(TAG, "Updating weather information...");
            
            char backup_weather[32];
            char backup_temperature[8];
            strcpy(backup_weather, now_weather);
            strcpy(backup_temperature, now_temperature);
            
            if (get_weather_info(now_weather, now_temperature, sizeof(now_weather))) {
                ESP_LOGI(TAG, "Weather info updated: %s, %s", now_weather, now_temperature);
                update_weather_display();
                last_weather_update = now;
            } else {
                ESP_LOGW(TAG, "Failed to update weather info");
                strcpy(now_weather, backup_weather);
                strcpy(now_temperature, backup_temperature);
            }
        }
        
        // 恢复地址显示
        lv_label_set_text(label_addr, now_address);
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}