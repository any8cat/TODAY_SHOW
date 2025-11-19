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

#include "lcd_driver.h"
#include "weather.h"
#include "fonts.h"

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
bool firstRun = true;

// 全局LCD对象
static lcd_display_t g_lcd;

// NTP时间同步标志
static bool time_sync_notified = false;

// 连接状态相关
uint32_t lastDotUpdate = 0;
int dotCount = 0;

// 时间显示相关
uint32_t lastTimeDisplay = 0;
const uint32_t TIME_DISPLAY_INTERVAL = 30000; // 30秒显示一次时间

// 定义各文字区域的背景缓存
static text_area_bg_t *hour_area = NULL;
static text_area_bg_t *minute_area = NULL;
static text_area_bg_t *date_area = NULL;
static text_area_bg_t *weather_area = NULL;
static text_area_bg_t *address_area = NULL;
static text_area_bg_t *second_area = NULL;

void init_text_areas(lcd_display_t *lcd) {
    // 小时部分区域
    hour_area = lcd_init_text_area(lcd, 16, 80, 36, 24); 
    
    // 分钟部分区域  
    minute_area = lcd_init_text_area(lcd, 68, 80, 36, 24);
    
    // 日期区域（包含日期和星期）
    date_area = lcd_init_text_area(lcd, 16, 106, 60, 12);
    
    // 天气区域
    weather_area = lcd_init_text_area(lcd, 64, 5, 64, 32);
    
    // 地址区域
    address_area = lcd_init_text_area(lcd, 5, 5, 60, 16);
    
    // 秒数区域
    second_area = lcd_init_text_area(lcd, 84, 104, 20, 12);
    
    ESP_LOGI(TAG, "Text areas initialized successfully");
}

// 安全日志输出宏
#define SAFE_LOG_STRING(str) ((str) ? (str) : "NULL")

// 函数声明
static void obtain_time(void);
static void initialize_sntp(void);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void show_info_on_image(lcd_display_t *lcd, int hour, int minute, int second, int year, int month, int day, const char* week, const char* address, const char* weather, const char* temperature);
void check_network_connection(void);

void draw_address(lcd_display_t *lcd, const char* address, int x, int y);
void draw_weather_info(lcd_display_t *lcd, const char* weather, const char* temperature, int x, int y);
void draw_time_without_seconds(lcd_display_t *lcd, int hour, int minute, int x, int y);
void draw_seconds(lcd_display_t *lcd, int second, int x, int y);
void draw_date_and_week(lcd_display_t *lcd, int month, int day, const char* week, int x, int y);
void draw_time_info(lcd_display_t *lcd, int hour, int minute, int second, int month, int day, const char* week, int x, int y);
// 任务函数声明
static void obtain_time_task(void *arg);

// 设置时区为北京时间（UTC+8）
void set_timezone(void)
{
    // 设置时区为北京时间
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to Beijing Time (UTC+8)");
}

// 时间同步通知回调函数
void time_sync_notification_cb(struct timeval *tv)
{
    time_sync_notified = true;
    ESP_LOGI(TAG, "Time synchronization notification received");
    
    // 获取当前时间并显示
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    ESP_LOGI(TAG, "=== SYNCHRONIZED TIME: %04d-%02d-%02d %02d:%02d:%02d %s (UTC+8) ===",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
             weekDays[timeinfo.tm_wday]);
}

// 网络检查函数
void check_network_connection(void)
{
    // 检查WiFi连接状态
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi RSSI: %d dBm", ap_info.rssi);
    } else {
        ESP_LOGW(TAG, "Failed to get WiFi AP info");
    }
    
    // 尝试DNS解析检查网络连通性 - 检查NTP服务器
    struct addrinfo hints = {0};
    struct addrinfo *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    if (getaddrinfo("pool.ntp.org", NULL, &hints, &res) == 0) {
        ESP_LOGI(TAG, "DNS resolution successful for pool.ntp.org");
        freeaddrinfo(res);
    } else {
        ESP_LOGW(TAG, "DNS resolution failed for pool.ntp.org");
    }
    
    // 检查天气API服务器
    if (getaddrinfo("api.seniverse.com", NULL, &hints, &res) == 0) {
        ESP_LOGI(TAG, "DNS resolution successful for api.seniverse.com");
        freeaddrinfo(res);
    } else {
        ESP_LOGW(TAG, "DNS resolution failed for api.seniverse.com");
    }
}

void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    
    // 设置时区
    set_timezone();
    
    // 使用ESP-SNTP初始化方法
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // 设置多个备用NTP服务器 - 使用更可靠的服务器
    const char* servers[] = {
        "pool.ntp.org",           // 主服务器
        "cn.pool.ntp.org",        // 中国区的NTP服务器
        "time.apple.com",         // 苹果时间服务器
        "time.windows.com"        // Windows时间服务器
    };
    
    for (int i = 0; i < 4; i++) {
        esp_sntp_setservername(i, servers[i]);
    }
    
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    sntp_set_sync_interval(30000); // 设置同步间隔为30秒
    
    // 设置超时回调
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    
    esp_sntp_init();
    
    // 延迟一段时间让SNTP初始化完成
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    // 安全地获取服务器名称并检查有效性
    for (int i = 0; i < 4; i++) {
        const char* server = esp_sntp_getservername(i);
        if (server == NULL || strlen(server) == 0) {
            ESP_LOGW(TAG, "SNTP server %d is invalid, re-setting", i);
            esp_sntp_setservername(i, servers[i]);
        }
    }
    
    // 重新获取服务器名称并记录
    ESP_LOGI(TAG, "SNTP initialized with servers: %s, %s, %s, %s",
             SAFE_LOG_STRING(esp_sntp_getservername(0)),
             SAFE_LOG_STRING(esp_sntp_getservername(1)),
             SAFE_LOG_STRING(esp_sntp_getservername(2)),
             SAFE_LOG_STRING(esp_sntp_getservername(3)));
}

void obtain_time(void)
{
    initialize_sntp();

    // 等待时间同步（最多60秒）
    int retry = 0;
    const int retry_count = 30; // 30次尝试（60秒）
    time_sync_notified = false;
    
    TickType_t start_ticks = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(60000); // 60秒超时
    
    // 先显示当前系统时间
    time_t current_time = time(NULL);
    struct tm current_timeinfo;
    localtime_r(&current_time, &current_timeinfo);
    ESP_LOGI(TAG, "Current system time before sync: %04d-%02d-%02d %02d:%02d:%02d",
             current_timeinfo.tm_year + 1900, current_timeinfo.tm_mon + 1, current_timeinfo.tm_mday,
             current_timeinfo.tm_hour, current_timeinfo.tm_min, current_timeinfo.tm_sec);
    
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && 
           !time_sync_notified && 
           (xTaskGetTickCount() - start_ticks) < timeout_ticks) {
        retry++;
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        
        // 检查网络连接状态
        if (retry % 5 == 0) { // 每10秒检查一次网络
            check_network_connection();
        }
        
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    
    if (retry >= retry_count || (xTaskGetTickCount() - start_ticks) >= timeout_ticks) {
        ESP_LOGW(TAG, "SNTP synchronization timeout after %d seconds", 60);
        
        // 检查SNTP状态
        sntp_sync_status_t status = esp_sntp_get_sync_status();
        ESP_LOGW(TAG, "SNTP sync status: %d", status);
        
        // 设置一个默认时间，避免程序卡住
        struct timeval tv = {
            .tv_sec = 1704067200, // 2024-01-01 00:00:00
            .tv_usec = 0
        };
        if (settimeofday(&tv, NULL) == 0) {
            ESP_LOGI(TAG, "Default time set successfully");
        } else {
            ESP_LOGE(TAG, "Failed to set default time");
        }
    } else {
        ESP_LOGI(TAG, "SNTP synchronization completed successfully");
        
        // 同步成功后，立即获取天气信息
        if (get_weather_info(now_weather, now_temperature, sizeof(now_weather))) {
            ESP_LOGI(TAG, "Weather info obtained: %s, %s", now_weather, now_temperature);
        } else {
            ESP_LOGW(TAG, "Failed to get weather info");
            strcpy(now_weather, "未知");
            strcpy(now_temperature, "N/A");
        }
    }
}

// 获取时间的任务函数
static void obtain_time_task(void *arg)
{
    obtain_time();
    vTaskDelete(NULL); // 任务完成后删除自己
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
        } else {
            ESP_LOGE(TAG, "Failed to connect after %d attempts", max_retry);
            // 可以在这里添加重启或其他恢复逻辑
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        
        // 检查网络连通性
        check_network_connection();
        
        // WiFi连接成功后获取时间（使用任务函数而不是lambda）
        xTaskCreate(obtain_time_task, "obtain_time_task", 4096, NULL, 5, NULL);
        
        // 清屏并显示主界面
        lcd_fill_screen(&g_lcd, COLOR_BLACK);
    }
}

void show_connecting_dots(lcd_display_t *lcd, int dotCount)
{
    char dots[5] = {0};
    for (int i = 0; i < dotCount; i++) {
        dots[i] = '.';
    }
    
    lcd_set_font(lcd, &font_xstandard);
    lcd_set_text_color(lcd, COLOR_WHITE);
    lcd_draw_string(lcd, 10 + 8 * 14, 40, dots); // 8像素字符宽度 * 14个字符
}

// 显示当前时间到日志
void display_current_time(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    ESP_LOGI(TAG, "🕒🕒🕒 CURRENT TIME: %04d-%02d-%02d %02d:%02d:%02d %s (UTC+8)",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
             weekDays[timeinfo.tm_wday]);
}

// 确保字符显示的函数
void safe_draw_string(lcd_display_t *lcd, uint16_t x, uint16_t y, const char *str, font_t *font, uint16_t color)
{
    if (lcd == NULL || str == NULL || font == NULL) {
        ESP_LOGE(TAG, "Invalid parameters in safe_draw_string");
        return;
    }
    
    // 设置字体和颜色
    lcd_set_font(lcd, font);
    lcd_set_text_color(lcd, color);
    
    // 绘制字符串
    lcd_draw_string(lcd, x, y, str);
}

void verify_background_data(text_area_bg_t *area, const char* area_name) {
    if (area == NULL || area->buffer == NULL) {
        ESP_LOGE(TAG, "Invalid area in verify_background_data: %s", area_name);
        return;
    }
    
    uint32_t total_pixels = area->width * area->height;
    uint32_t black_pixels = 0;
    uint32_t non_black_pixels = 0;
    
    for (uint32_t i = 0; i < total_pixels; i++) {
        if (area->buffer[i] == COLOR_BLACK) {
            black_pixels++;
        } else {
            non_black_pixels++;
        }
    }
    
    ESP_LOGI(TAG, "%s background verification:", area_name);
    ESP_LOGI(TAG, "  Total pixels: %lu, Black: %lu (%.1f%%), Non-black: %lu (%.1f%%)",
             total_pixels, black_pixels, (float)black_pixels/total_pixels*100.0f,
             non_black_pixels, (float)non_black_pixels/total_pixels*100.0f);
    
    // 如果黑色像素比例过高，可能是保存失败
    if ((float)black_pixels/total_pixels > 0.8f) {
        ESP_LOGW(TAG, "WARNING: %s background may not be saved correctly!", area_name);
    }
}


// void show_info_on_image(lcd_display_t *lcd, 
//                        int hour, int minute, int second, 
//                        int year, int month, int day, 
//                        const char* week, 
//                        const char* address, const char* weather, const char* temperature)
// {
//     static int last_second = -1;
//     if (second == last_second) {
//         // 秒数没变，不重绘
//         return;
//     }
//     last_second = second;

//     if (lcd == NULL) {
//         ESP_LOGE(TAG, "LCD is NULL in show_info_on_image");
//         return;
//     }    

//     // 显示当前时间到日志（用于调试）
//     ESP_LOGI(TAG, "Displaying: %02d:%02d:%02d %04d/%02d/%02d %s - %s %s℃",
//              hour, minute, second, year, month, day, week, weather, temperature);
    
//     // 1. 首先绘制背景图片（雷神图片）
//     lcd_draw_image(lcd, 0, 0, 128, 128, thunderGod);
    
//     // 2. 左上角显示地点 - 使用自定义字体显示汉字
//     lcd_set_custom_font(lcd, show_custom_font);
//     lcd_set_text_color(lcd, COLOR_WHITE);
//     lcd_draw_custom_string(lcd, 5, 5, address);

//     // 3. 右上角显示天气和温度
//     int weatherX = 64;
//     int weatherY = 5;
    
//     // 如果天气信息为空，使用默认值
//     char display_weather[32];
//     char display_temperature[8];
    
//     if (strlen(weather) == 0 || strcmp(weather, ",") == 0) {
//         strcpy(display_weather, "未知");
//         strcpy(display_temperature, "N/A");
//     } else {
//         strcpy(display_weather, weather);
//         strcpy(display_temperature, temperature);
//     }
    
//     int weatherCharCount = strlen(display_weather) / 3; // 中文字符数
    
//     // 修复：统一使用自定义字体显示天气汉字
//     lcd_set_custom_font(lcd, show_custom_font);
//     lcd_set_text_color(lcd, COLOR_WHITE);
    
//     if (weatherCharCount <= 2) {
//         // 短天气描述
//         lcd_draw_custom_string(lcd, weatherX + 16, weatherY, display_weather);
        
//         // 修复：温度显示使用标准字体，设置正确的字体
//         lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
//         lcd_set_text_color(lcd, COLOR_CYAN);
//         char temp_str[16];
//         snprintf(temp_str, sizeof(temp_str), "%s", display_temperature);
//         lcd_draw_string(lcd, weatherX + weatherCharCount * 16 + 16, weatherY + 2, temp_str);
//     } 
//     else if (weatherCharCount <= 4) {
//         // 中等长度天气描述
//         lcd_draw_custom_string(lcd, weatherX, weatherY, display_weather);
        
//         // 温度显示在天气下方
//         lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
//         lcd_set_text_color(lcd, COLOR_CYAN);
//         char temp_str[16];
//         snprintf(temp_str, sizeof(temp_str), "%s", display_temperature);
//         int tempY = weatherY + 16;
//         lcd_draw_string(lcd, weatherX + 16, tempY + 6, temp_str);
//     }
//     else {
//         // 长天气描述，截断显示
//         char shortWeather[16] = {0};
//         strncpy(shortWeather, display_weather, 12);
//         if (strlen(display_weather) > 12) {
//             strcat(shortWeather, "...");
//         }
        
//         lcd_draw_custom_string(lcd, weatherX, weatherY, shortWeather);
        
//         // 温度显示在天气下方
//         lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
//         lcd_set_text_color(lcd, COLOR_CYAN);
//         char temp_str[16];
//         snprintf(temp_str, sizeof(temp_str), "%s", display_temperature);
//         int tempY = weatherY + 16;
//         lcd_draw_string(lcd, weatherX + 16, tempY + 6, temp_str);
//     }
    
//     // 4. 中间偏下显示时间
//     int timeX = 16;
//     int timeY = 80;
    
//     char timeHM[6];
//     snprintf(timeHM, sizeof(timeHM), "%02d:%02d", hour, minute);
    
//     // 修复：使用大字体显示时间，确保字体设置正确
//     lcd_set_font_size(lcd, FONT_SIZE_LARGE);
//     lcd_set_text_color(lcd, COLOR_WHITE);
//     lcd_draw_string(lcd, timeX, timeY, timeHM);
    
//     // 5. 显示秒数
//     char secStr[4];
//     int secondX = timeX + 68;
//     int secondY = timeY + 24;
    
//     if (secondX + 20 < 128) {
//         snprintf(secStr, sizeof(secStr), ":%02d", second);
//     } else {
//         snprintf(secStr, sizeof(secStr), "%02d", second);
//     }
    
//     // 修复：秒数使用标准字体
//     lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
//     lcd_set_text_color(lcd, COLOR_WHITE);
//     lcd_draw_string(lcd, secondX, secondY, secStr);
    
//     // 6. 显示日期和星期
//     char dateStr[12];
//     snprintf(dateStr, sizeof(dateStr), "%02d/%02d", month, day);
    
//     // 修复：日期使用标准字体
//     lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
//     lcd_set_text_color(lcd, COLOR_WHITE);
//     lcd_draw_string(lcd, timeX, timeY + 26, dateStr);
    
//     // 修复：星期使用自定义字体显示汉字
//     lcd_set_custom_font(lcd, show_custom_font);
//     lcd_set_text_color(lcd, COLOR_WHITE);
//     lcd_draw_custom_string(lcd, timeX + 6 * 6, timeY + 30, week);
// }
void show_info_on_image(lcd_display_t *lcd, 
                       int hour, int minute, int second, 
                       int year, int month, int day, 
                       const char* week, 
                       const char* address, const char* weather, const char* temperature)
{
    // 静态变量保存上一次的值
    static int last_hour = -1, last_minute = -1, last_second = -1;
    static int last_year = -1, last_month = -1, last_day = -1;
    static char last_week[10] = "";
    static char last_address[16] = "";
    static char last_weather[32] = "";
    static char last_temperature[8] = "";
    
    if (lcd == NULL) {
        ESP_LOGE(TAG, "LCD is NULL in show_info_on_image");
        return;
    }
    
    // 检查是否需要全屏刷新（首次运行）
    bool need_full_refresh = firstRun;
    
    // 记录哪些部分需要刷新
    bool refresh_address = false;
    bool refresh_weather = false;
    bool refresh_hour = false;
    bool refresh_minute = false;
    bool refresh_second = false;
    bool refresh_date = false;
    bool refresh_week = false;
    
    if (need_full_refresh) {
        // 全屏刷新
        ESP_LOGI(TAG, "Performing full screen refresh");
        lcd_draw_image(lcd, 0, 0, 128, 128, thunderGod);
        
        firstRun = false;
    } else {
        // 局部刷新
        ESP_LOGI(TAG, "Performing partial refresh with background restoration");
        
        // 检查地址变化
        if (strcmp(address, last_address) != 0) {
            refresh_address = true;
            strcpy(last_address, address);
        }
        
        // 检查天气变化
        if (strcmp(weather, last_weather) != 0 || strcmp(temperature, last_temperature) != 0) {
            refresh_weather = true;
            strcpy(last_weather, weather);
            strcpy(last_temperature, temperature);
        }
        
        // 检查时间变化
        if (hour != last_hour) {
            refresh_hour = true;
            last_hour = hour;
        }
        
        if (minute != last_minute) {
            refresh_minute = true;
            last_minute = minute;
        }
        
        if (second != last_second) {
            refresh_second = true;
            last_second = second;
        }
        
        // 检查日期变化
        if (year != last_year || month != last_month || day != last_day) {
            refresh_date = true;
            last_year = year;
            last_month = month;
            last_day = day;
        }
        
        // 检查星期变化
        if (strcmp(week, last_week) != 0) {
            refresh_week = true;
            strcpy(last_week, week);
        }
        
        // 如果没有变化且不是首次运行，直接返回
        if (!need_full_refresh && !refresh_address && !refresh_weather && 
            !refresh_hour && !refresh_minute && !refresh_second && 
            !refresh_date && !refresh_week) {
            return;
        }
        
        // 记录刷新类型
        ESP_LOGI(TAG, "Refresh: Full=%d, Addr=%d, Weather=%d, Hour=%d, Min=%d, Sec=%d, Date=%d, Week=%d",
                need_full_refresh, refresh_address, refresh_weather, 
                refresh_hour, refresh_minute, refresh_second, 
                refresh_date, refresh_week);

        // 最后只刷新变化的部分
        if (refresh_address) {
            ESP_LOGI(TAG, "Refreshing address area");
            if (address_area) lcd_restore_text_area_bg(lcd, address_area);
            draw_address(lcd, address, 5, 5);
        }
        
        if (refresh_weather) {
            ESP_LOGI(TAG, "Refreshing weather area");
            if (weather_area) lcd_restore_text_area_bg(lcd, weather_area);
            draw_weather_info(lcd, weather, temperature, 64, 5);
        }
        
        if (refresh_hour || refresh_minute) {
            ESP_LOGI(TAG, "Refreshing time area (hour=%d, minute=%d)", refresh_hour, refresh_minute);
            if (hour_area) lcd_restore_text_area_bg(lcd, hour_area);
            if (minute_area) lcd_restore_text_area_bg(lcd, minute_area);
            draw_time_without_seconds(lcd, hour, minute, 16, 80);
        }
        
        if (refresh_second) {
            ESP_LOGI(TAG, "Refreshing second area");
            if (second_area) lcd_restore_text_area_bg(lcd, second_area);
            draw_seconds(lcd, second, 16 + 68, 80 + 24);
        }
        
        if (refresh_date || refresh_week) {
            ESP_LOGI(TAG, "Refreshing date/week area (date=%d, week=%d)", refresh_date, refresh_week);
            if (date_area) lcd_restore_text_area_bg(lcd, date_area);
            draw_date_and_week(lcd, month, day, week, 16, 80 + 26);
        }
    }
}

// 辅助函数：绘制地址
void draw_address(lcd_display_t *lcd, const char* address, int x, int y)
{
    if (lcd == NULL || address == NULL) return;
    
    lcd_set_custom_font(lcd, show_custom_font);
    lcd_set_text_color(lcd, COLOR_WHITE);
    lcd_draw_custom_string(lcd, x, y, address);
}

// 辅助函数：绘制天气信息
void draw_weather_info(lcd_display_t *lcd, const char* weather, const char* temperature, int x, int y)
{
    if (lcd == NULL || weather == NULL || temperature == NULL) return;
    
    char display_weather[32];
    char display_temperature[8];
    
    // 如果天气信息为空，使用默认值
    if (strlen(weather) == 0 || strcmp(weather, ",") == 0) {
        strcpy(display_weather, "未知");
        strcpy(display_temperature, "N/A");
    } else {
        strcpy(display_weather, weather);
        strcpy(display_temperature, temperature);
    }
    
    int weatherCharCount = strlen(display_weather) / 3; // 中文字符数
    
    // 使用自定义字体显示天气汉字
    lcd_set_custom_font(lcd, show_custom_font);
    lcd_set_text_color(lcd, COLOR_WHITE);
    
    if (weatherCharCount <= 2) {
        lcd_draw_custom_string(lcd, x + 16, y, display_weather);
        
        lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
        lcd_set_text_color(lcd, COLOR_CYAN);
        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%s", display_temperature);
        lcd_draw_string(lcd, x + weatherCharCount * 16 + 16, y + 2, temp_str);
    } 
    else if (weatherCharCount <= 4) {
        lcd_draw_custom_string(lcd, x, y, display_weather);
        
        lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
        lcd_set_text_color(lcd, COLOR_CYAN);
        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%s", display_temperature);
        int tempY = y + 16;
        lcd_draw_string(lcd, x + 16, tempY + 6, temp_str);
    }
    else {
        char shortWeather[16] = {0};
        strncpy(shortWeather, display_weather, 12);
        if (strlen(display_weather) > 12) {
            strcat(shortWeather, "...");
        }
        
        lcd_draw_custom_string(lcd, x, y, shortWeather);
        
        lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
        lcd_set_text_color(lcd, COLOR_CYAN);
        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%s", display_temperature);
        int tempY = y + 16;
        lcd_draw_string(lcd, x + 16, tempY + 6, temp_str);
    }
}

// 辅助函数：绘制时间（不含秒数）
void draw_time_without_seconds(lcd_display_t *lcd, int hour, int minute, int x, int y)
{
    if (lcd == NULL) return;
    
    // 分别绘制小时和分钟，避免覆盖整个时间区域
    char hourStr[3];
    char minuteStr[3];
    snprintf(hourStr, sizeof(hourStr), "%02d", hour);
    snprintf(minuteStr, sizeof(minuteStr), "%02d", minute);
    
    lcd_set_font_size(lcd, FONT_SIZE_LARGE);
    lcd_set_text_color(lcd, COLOR_WHITE);
    
    // 绘制小时
    lcd_draw_string(lcd, x, y, hourStr);
    
    // 绘制冒号
    lcd_draw_string(lcd, x + 36, y, ":");
    
    // 绘制分钟
    lcd_draw_string(lcd, x + 36 + 16, y, minuteStr);
}

// 辅助函数：绘制秒数
void draw_seconds(lcd_display_t *lcd, int second, int x, int y)
{
    if (lcd == NULL) return;
    
    char secStr[4];
    snprintf(secStr, sizeof(secStr), ":%02d", second);
    
    lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
    lcd_set_text_color(lcd, COLOR_WHITE);
    lcd_draw_string(lcd, x, y, secStr);
}

// 辅助函数：绘制日期和星期
void draw_date_and_week(lcd_display_t *lcd, int month, int day, const char* week, int x, int y)
{
    if (lcd == NULL || week == NULL) return;
    
    char dateStr[12];
    snprintf(dateStr, sizeof(dateStr), "%02d/%02d", month, day);
    
    // 绘制日期
    lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
    lcd_set_text_color(lcd, COLOR_WHITE);
    lcd_draw_string(lcd, x, y, dateStr);
    
    // 绘制星期
    lcd_set_custom_font(lcd, show_custom_font);
    lcd_set_text_color(lcd, COLOR_WHITE);
    lcd_draw_custom_string(lcd, x + 6 * 6, y, week);
}

// 辅助函数：绘制完整时间信息（兼容旧代码）
void draw_time_info(lcd_display_t *lcd, int hour, int minute, int second, int month, int day, const char* week, int x, int y)
{
    draw_time_without_seconds(lcd, hour, minute, x, y);
    draw_seconds(lcd, second, x + 68, y + 24);
    draw_date_and_week(lcd, month, day, week, x, y + 26);
}

void test_font_display(lcd_display_t *lcd)
{
    if (lcd == NULL) return;
    
    // 清屏
    lcd_fill_screen(lcd, COLOR_BLACK);
    
    // 测试小字体
    lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
    lcd_set_text_color(lcd, COLOR_WHITE);
    lcd_draw_string(lcd, 10, 10, "Small: 012ABCabc");
    
    // 测试中字体
    lcd_set_font_size(lcd, FONT_SIZE_MEDIUM);
    lcd_set_text_color(lcd, COLOR_GREEN);
    lcd_draw_string(lcd, 10, 30, "Medium: 456DEFdef");
    
    // 测试大字体
    lcd_set_font_size(lcd, FONT_SIZE_LARGE);
    lcd_set_text_color(lcd, COLOR_BLUE);
    lcd_draw_string(lcd, 10, 0, "@");
    
    // 测试特殊字符
    lcd_set_font_size(lcd, FONT_SIZE_XSMALL);
    lcd_set_text_color(lcd, COLOR_RED);
    lcd_draw_string(lcd, 10, 80, "Special: +-*/=<>?");
    
    vTaskDelay(3000 / portTICK_PERIOD_MS);
}

// void app_main(void)
// {
//     ESP_LOGI(TAG, "Starting TFT Clock Application");
    
//     lcd_validate_fonts();
    
//     // 初始化NVS
//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         ret = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(ret);

//     // 初始化LCD
//     lcd_config_t lcd_config = {
//         .miso_io_num = 19,
//         .mosi_io_num = 23,
//         .sclk_io_num = 18,
//         .cs_io_num = 27,
//         .dc_io_num = 25,
//         .rst_io_num = 26,
//         .spi_freq_hz = 27000000,
//         .width = 128,
//         .height = 128,
//         .invert_colors = true,
//     };
    
//     // 使用全局变量g_lcd
//     if (lcd_init(&g_lcd, &lcd_config) != ESP_OK) {
//         ESP_LOGE(TAG, "LCD initialization failed!");
//         return;
//     }
    
//     // 设置全局LCD对象供字体函数使用
//     set_global_lcd(&g_lcd);
    
//     // 设置自定义字体显示函数
//     lcd_set_custom_font(&g_lcd, show_custom_font);
    
//     // 清屏
//     lcd_fill_screen(&g_lcd, COLOR_BLACK);
    
//     test_font_display(&g_lcd);
    
//     // 测试：显示纯色背景
//     ESP_LOGI(TAG, "Testing with solid colors...");
//     lcd_fill_screen(&g_lcd, COLOR_RED);
//     vTaskDelay(1000 / portTICK_PERIOD_MS);
//     lcd_fill_screen(&g_lcd, COLOR_GREEN);
//     vTaskDelay(1000 / portTICK_PERIOD_MS);
//     lcd_fill_screen(&g_lcd, COLOR_BLUE);
//     vTaskDelay(1000 / portTICK_PERIOD_MS);
//     lcd_fill_screen(&g_lcd, COLOR_BLACK);
    
//     // 测试：显示图片
//     ESP_LOGI(TAG, "Testing image display...");
//     lcd_draw_image(&g_lcd, 0, 0, 128, 128, thunderGod);
//     vTaskDelay(3000 / portTICK_PERIOD_MS);
    
//     // 显示连接中信息 - 使用安全绘制函数
//     safe_draw_string(&g_lcd, 10, 40, "WiFi Connecting", &font_xstandard, COLOR_WHITE);
    
//     // 初始化WiFi和网络事件
//     ESP_ERROR_CHECK(esp_netif_init());
//     ESP_ERROR_CHECK(esp_event_loop_create_default());
//     esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
//     assert(sta_netif);

//     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
//     ESP_ERROR_CHECK(esp_wifi_init(&cfg));

//     // 注册WiFi事件处理
//     esp_event_handler_instance_t instance_any_id;
//     esp_event_handler_instance_t instance_got_ip;
//     ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
//                                                         ESP_EVENT_ANY_ID,
//                                                         &wifi_event_handler,
//                                                         NULL,
//                                                         &instance_any_id));
//     ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
//                                                         IP_EVENT_STA_GOT_IP,
//                                                         &wifi_event_handler,
//                                                         NULL,
//                                                         &instance_got_ip));

//     wifi_config_t wifi_config = {
//         .sta = {
//             .ssid = WIFI_SSID,
//             .password = WIFI_PASS,
//             .scan_method = WIFI_ALL_CHANNEL_SCAN,
//             .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
//             .threshold.rssi = -127,
//             .threshold.authmode = WIFI_AUTH_WPA2_PSK,
//         },
//     };
    
//     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
//     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
//     ESP_ERROR_CHECK(esp_wifi_start());

//     ESP_LOGI(TAG, "Connecting to WiFi...");
    
//     // 主循环
//     time_t last_time_check = time(NULL);
//     const time_t max_stuck_time = 60; // 60秒最大卡住时间
//     bool time_initialized = false;
//     time_t lastTimeDisplay = 0;
    
//     while (1) {
//         // 获取当前时间
//         time_t now;
//         struct tm timeinfo;
//         time(&now);
        
//         // 每30秒显示一次当前时间到日志
//         if (now - lastTimeDisplay >= TIME_DISPLAY_INTERVAL) {
//             display_current_time();
//             lastTimeDisplay = now;
//         }
        
//         // 检查是否卡在时间同步
//         if (!time_initialized && now - last_time_check > max_stuck_time) {
//             ESP_LOGW(TAG, "System seems stuck, forcing time recovery");
//             struct timeval tv = {
//                 .tv_sec = now + 1, // 至少让时间前进
//                 .tv_usec = 0
//             };
//             settimeofday(&tv, NULL);
//             last_time_check = now;
//             time_initialized = true;
//         }
        
//         // 检查时间是否合理（不在1970年）
//         if (now < 1609459200) { // 2021-01-01 00:00:00之前的时间视为无效
//             ESP_LOGW(TAG, "System time is invalid, using default time");
//             struct timeval tv = {
//                 .tv_sec = 1704067200, // 2024-01-01 00:00:00
//                 .tv_usec = 0
//             };
//             settimeofday(&tv, NULL);
//             time(&now);
//         }
        
//         localtime_r(&now, &timeinfo);
        
//         // 定期更新天气信息（每10分钟，避免API限制）
//         if (now - lastWeatherUpdate >= WEATHER_UPDATE_INTERVAL) {
//             ESP_LOGI(TAG, "Attempting to update weather information...");
            
//             // 保存当前天气信息作为备份
//             char backup_weather[32];
//             char backup_temperature[8];
//             strcpy(backup_weather, now_weather);
//             strcpy(backup_temperature, now_temperature);
            
//             if (get_weather_info(now_weather, now_temperature, sizeof(now_weather))) {
//                 ESP_LOGI(TAG, "Weather info updated: %s, %s", now_weather, now_temperature);
//                 lastWeatherUpdate = now;
//             } else {
//                 ESP_LOGW(TAG, "Failed to update weather info, using previous data");
//                 // 恢复备份数据
//                 strcpy(now_weather, backup_weather);
//                 strcpy(now_temperature, backup_temperature);
//             }
//         }
        
//         // 显示信息
//         show_info_on_image(&g_lcd, 
//                           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
//                           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
//                           weekDays[timeinfo.tm_wday],
//                           now_address, now_weather, now_temperature);
        
//         vTaskDelay(100 / portTICK_PERIOD_MS);
//     }
// }

void app_main(void)
{
    ESP_LOGI(TAG, "Starting TFT Clock Application with Partial Refresh");
    
    lcd_validate_fonts();
    
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化LCD
    lcd_config_t lcd_config = {
        .miso_io_num = 11,
        .mosi_io_num = 13,
        .sclk_io_num = 12,
        .cs_io_num = 10,
        .dc_io_num = 9,
        .rst_io_num = 8,
        .spi_freq_hz = 27000000,
        .width = 128,
        .height = 128,
        .invert_colors = true,
    };
    
    // 使用全局变量g_lcd
    if (lcd_init(&g_lcd, &lcd_config) != ESP_OK) {
        ESP_LOGE(TAG, "LCD initialization failed!");
        return;
    }
    
    // 设置全局LCD对象供字体函数使用
    set_global_lcd(&g_lcd);
    
    // 设置自定义字体显示函数
    lcd_set_custom_font(&g_lcd, show_custom_font);
    
    // 清屏
    lcd_fill_screen(&g_lcd, COLOR_BLACK);
    
    // 初始化文字区域（局部刷新功能）
    ESP_LOGI(TAG, "Initializing text areas for partial refresh...");
    init_text_areas(&g_lcd);
    
    // 测试字体显示
    test_font_display(&g_lcd);
    
    // 清屏
    lcd_fill_screen(&g_lcd, COLOR_BLACK);

    // 显示连接中信息
    safe_draw_string(&g_lcd, 10, 40, "WiFi Connecting", &font_xstandard, COLOR_WHITE);
    
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
    
    // ============ 这里开始添加您的代码 ============
    // 等待WiFi连接（最多等待10秒）
    TickType_t wifi_timeout = pdMS_TO_TICKS(10000);
    TickType_t start_ticks = xTaskGetTickCount();
    
    while (firstRun && (xTaskGetTickCount() - start_ticks) < wifi_timeout) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    if (firstRun) {
        // WiFi连接成功后的首次初始化
        ESP_LOGI(TAG, "WiFi connected, initializing background for first run");
        
        // 首次运行，显示完整背景并初始化区域
        lcd_draw_image(&g_lcd, 0, 0, 128, 128, thunderGod);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        
        // 保存所有区域的背景
        if (hour_area) lcd_save_text_area_bg(&g_lcd, hour_area);
        if (minute_area) lcd_save_text_area_bg(&g_lcd, minute_area);
        if (date_area) lcd_save_text_area_bg(&g_lcd, date_area);
        if (weather_area) lcd_save_text_area_bg(&g_lcd, weather_area);
        if (address_area) lcd_save_text_area_bg(&g_lcd, address_area);
        if (second_area) lcd_save_text_area_bg(&g_lcd, second_area);
        
        // 获取初始时间并显示
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        
        // 获取初始天气信息
        if (get_weather_info(now_weather, now_temperature, sizeof(now_weather))) {
            ESP_LOGI(TAG, "Initial weather info: %s, %s", now_weather, now_temperature);
        } else {
            strcpy(now_weather, "未知");
            strcpy(now_temperature, "N/A");
        }
        
        // 显示初始信息
        show_info_on_image(&g_lcd, 
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                          weekDays[timeinfo.tm_wday],
                          now_address, now_weather, now_temperature);
        
        firstRun = false;
    }
    // ============ 这里结束添加您的代码 ============
    
    // 主循环
    time_t last_time_check = time(NULL);
    const time_t max_stuck_time = 60; // 60秒最大卡住时间
    bool time_initialized = false;
    time_t lastTimeDisplay = 0;
    
    // 首次运行标志已经在上面处理过了，这里设置为false
    // firstRun = false; // 这行不需要，因为上面已经设置了
    
    while (1) {
        // 获取当前时间
        time_t now;
        struct tm timeinfo;
        time(&now);
        
        // 每30秒显示一次当前时间到日志
        if (now - lastTimeDisplay >= TIME_DISPLAY_INTERVAL) {
            display_current_time();
            lastTimeDisplay = now;
        }
        
        // 检查是否卡在时间同步
        if (!time_initialized && now - last_time_check > max_stuck_time) {
            ESP_LOGW(TAG, "System seems stuck, forcing time recovery");
            struct timeval tv = {
                .tv_sec = now + 1, // 至少让时间前进
                .tv_usec = 0
            };
            settimeofday(&tv, NULL);
            last_time_check = now;
            time_initialized = true;
        }
        
        // 检查时间是否合理（不在1970年）
        if (now < 1609459200) { // 2021-01-01 00:00:00之前的时间视为无效
            ESP_LOGW(TAG, "System time is invalid, using default time");
            struct timeval tv = {
                .tv_sec = 1704067200, // 2024-01-01 00:00:00
                .tv_usec = 0
            };
            settimeofday(&tv, NULL);
            time(&now);
        }
        
        localtime_r(&now, &timeinfo);
        
        // 定期更新天气信息（每10分钟，避免API限制）
        if (now - lastWeatherUpdate >= WEATHER_UPDATE_INTERVAL) {
            ESP_LOGI(TAG, "Attempting to update weather information...");
            
            // 保存当前天气信息作为备份
            char backup_weather[32];
            char backup_temperature[8];
            strcpy(backup_weather, now_weather);
            strcpy(backup_temperature, now_temperature);
            
            if (get_weather_info(now_weather, now_temperature, sizeof(now_weather))) {
                ESP_LOGI(TAG, "Weather info updated: %s, %s", now_weather, now_temperature);
                lastWeatherUpdate = now;
            } else {
                ESP_LOGW(TAG, "Failed to update weather info, using previous data");
                // 恢复备份数据
                strcpy(now_weather, backup_weather);
                strcpy(now_temperature, backup_temperature);
            }
        }
        
        // 显示信息（现在使用局部刷新功能）
        show_info_on_image(&g_lcd, 
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                          weekDays[timeinfo.tm_wday],
                          now_address, now_weather, now_temperature);
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}