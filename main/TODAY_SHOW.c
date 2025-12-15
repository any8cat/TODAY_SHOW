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
// 图片声明
LV_IMG_DECLARE(thunderGod);

static const char *TAG = "TFT_CLOCK_LVGL";

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
bool timeSynced = false; // 新增：时间同步状态标志

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

// NTP时间同步标志
static bool time_sync_notified = false;

// 文本区域缓存结构
typedef struct {
    lv_obj_t *label;
    char last_text[64];
} lvgl_text_area_t;

static lvgl_text_area_t second_area = {0};
static lvgl_text_area_t date_area = {0};
static lvgl_text_area_t week_area = {0};
static lvgl_text_area_t weather_area = {0};
static lvgl_text_area_t address_area = {0};
static lvgl_text_area_t temp_area = {0};

// 函数声明
static void obtain_time(void);
static void initialize_sntp(void);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void create_lvgl_ui(void);
void update_time_display(void);
void update_weather_display(void);
void check_network_connection(void);
static void obtain_time_task(void *arg);
void display_current_time(void);
void safe_update_label(lv_obj_t *label, const char *text);
void init_lvgl_text_areas(void);
bool need_text_update(lvgl_text_area_t *area, const char *new_text);
void update_lvgl_text_area(lvgl_text_area_t *area, const char *text);
void refresh_all_displays(void); // 新增：强制刷新所有显示

// 安全日志输出宏
#define SAFE_LOG_STRING(str) ((str) ? (str) : "NULL")

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
    time_sync_notified = true;
    timeSynced = true; // 设置时间同步标志
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
    
    // 尝试DNS解析检查网络连通性
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
    
    ESP_LOGI(TAG, "SNTP initialized");
}

void obtain_time(void)
{
    initialize_sntp();

    // 等待时间同步（最多60秒）
    int retry = 0;
    const int retry_count = 30;
    time_sync_notified = false;
    
    TickType_t start_ticks = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(60000);
    
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
        
        if (retry % 5 == 0) {
            check_network_connection();
        }
        
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    
    if (retry >= retry_count || (xTaskGetTickCount() - start_ticks) >= timeout_ticks) {
        ESP_LOGW(TAG, "SNTP synchronization timeout after %d seconds", 60);
        
        // 设置默认时间
        struct timeval tv = {
            .tv_sec = 1704067200, // 2024-01-01 00:00:00
            .tv_usec = 0
        };
        if (settimeofday(&tv, NULL) == 0) {
            ESP_LOGI(TAG, "Default time set successfully");
            timeSynced = true; // 即使超时也标记为已同步
        } else {
            ESP_LOGE(TAG, "Failed to set default time");
        }
    } else {
        ESP_LOGI(TAG, "SNTP synchronization completed successfully");
        timeSynced = true;
    }
    
    // 无论是否同步成功，都尝试获取天气信息
    if (get_weather_info(now_weather, now_temperature, sizeof(now_weather))) {
        ESP_LOGI(TAG, "Weather info obtained: %s, %s", now_weather, now_temperature);
    } else {
        ESP_LOGW(TAG, "Failed to get weather info");
        strcpy(now_weather, "未知");
        strcpy(now_temperature, "N/A");
    }
    
    // 强制刷新显示
    refresh_all_displays();
}

// 获取时间的任务函数
static void obtain_time_task(void *arg)
{
    obtain_time();
    vTaskDelete(NULL);
}

// 安全更新标签文本
void safe_update_label(lv_obj_t *label, const char *text)
{
    if (label != NULL && text != NULL) {
        lv_label_set_text(label, text);
        // 立即处理LVGL刷新
        lv_refr_now(NULL);
    }
}

// 初始化LVGL文本区域缓存
void init_lvgl_text_areas(void)
{
    // 初始化各个文本区域
    memset(&second_area, 0, sizeof(second_area));
    memset(&date_area, 0, sizeof(date_area));
    memset(&week_area, 0, sizeof(week_area));
    memset(&weather_area, 0, sizeof(weather_area));
    memset(&address_area, 0, sizeof(address_area));
    memset(&temp_area, 0, sizeof(temp_area));
    
    ESP_LOGI(TAG, "LVGL text areas initialized");
}

// 检查文本是否需要更新
bool need_text_update(lvgl_text_area_t *area, const char *new_text)
{
    if (area == NULL || new_text == NULL) return true;
    
    if (strcmp(area->last_text, new_text) != 0) {
        strncpy(area->last_text, new_text, sizeof(area->last_text) - 1);
        area->last_text[sizeof(area->last_text) - 1] = '\0';
        return true;
    }
    return false;
}

// 更新LVGL文本区域
void update_lvgl_text_area(lvgl_text_area_t *area, const char *text)
{
    if (area == NULL || text == NULL) return;
    
    if (need_text_update(area, text)) {
        ESP_LOGI(TAG, "Updating display: %s", text);
        safe_update_label(area->label, text);
    }
}

// 强制刷新所有显示
void refresh_all_displays(void)
{
    ESP_LOGI(TAG, "Forcing refresh of all displays");
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // 更新时间显示
    char time_str[6];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    safe_update_label(label_time, time_str);
    
    // 更新秒数
    char sec_str[3];
    snprintf(sec_str, sizeof(sec_str), "%02d", timeinfo.tm_sec);
    safe_update_label(label_sec, sec_str);
    
    // 更新日期
    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%02d/%02d", timeinfo.tm_mon + 1, timeinfo.tm_mday);
    safe_update_label(label_date, date_str);
    
    // 更新星期
    safe_update_label(label_week, weekDays[timeinfo.tm_wday]);
    
    // 更新地址
    safe_update_label(label_addr, now_address);
    
    // 更新天气和温度
    safe_update_label(label_weather, now_weather);
    
    char temp_display[16];
    snprintf(temp_display, sizeof(temp_display), "%s°C", now_temperature);
    safe_update_label(label_temp, temp_display);
    
    ESP_LOGI(TAG, "Display refresh completed");
}

// 创建LVGL用户界面
void create_lvgl_ui(void)
{
    // 创建背景图片
    bg_img = lv_img_create(lv_scr_act());
    lv_img_set_src(bg_img, &thunderGod);
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
    
    // 创建地址标签（左上角）
    label_addr = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_addr, &myFont_16, 0);
    lv_obj_set_style_text_color(label_addr, lv_color_white(), 0);
    lv_label_set_text(label_addr, "连接中...");
    lv_obj_align(label_addr, LV_ALIGN_TOP_LEFT, 5, 5);
    address_area.label = label_addr;
    
    // 创建天气标签（右上角）
    label_weather = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_weather, &myFont_16, 0);
    lv_obj_set_style_text_color(label_weather, lv_color_white(), 0);
    lv_label_set_text(label_weather, "天气");
    lv_obj_align(label_weather, LV_ALIGN_TOP_RIGHT, -5, 5);
    weather_area.label = label_weather;
    
    // 创建温度标签
    label_temp = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_temp, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label_temp, lv_color_hex(0x00FFFF), 0);
    lv_label_set_text(label_temp, "N/A");
    lv_obj_align(label_temp, LV_ALIGN_TOP_RIGHT, -5, 25);
    temp_area.label = label_temp;
    
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
    second_area.label = label_sec;
    
    // 创建日期标签
    label_date = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_date, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label_date, lv_color_white(), 0);
    lv_label_set_text(label_date, "01/01");
    lv_obj_align(label_date, LV_ALIGN_CENTER, -20, 50);
    date_area.label = label_date;
    
    // 创建星期标签
    label_week = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label_week, &myFont_16, 0);
    lv_obj_set_style_text_color(label_week, lv_color_white(), 0);
    lv_label_set_text(label_week, "周一");
    lv_obj_align(label_week, LV_ALIGN_CENTER, 20, 50);
    week_area.label = label_week;
    
    // 初始化文本区域缓存
    init_lvgl_text_areas();
    
    ESP_LOGI(TAG, "LVGL UI created successfully");
}

// 更新时间显示
void update_time_display(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // 静态变量记录上次更新的值
    static int last_second = -1;
    static int last_minute = -1;
    static int last_hour = -1;
    static int last_day = -1;
    static int last_month = -1;
    static int last_weekday = -1;
    
    // 每秒都更新秒数
    char sec_str[3];
    snprintf(sec_str, sizeof(sec_str), "%02d", timeinfo.tm_sec);
    safe_update_label(label_sec, sec_str);
    
    // 只有分钟变化时才更新时间
    if (last_minute != timeinfo.tm_min) {
        char time_str[6];
        snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        safe_update_label(label_time, time_str);
        last_minute = timeinfo.tm_min;
        last_hour = timeinfo.tm_hour; // 同时记录小时，确保小时变化也会更新
    }
    
    // 只有日期变化时才更新日期和星期
    if (last_day != timeinfo.tm_mday || last_month != timeinfo.tm_mon) {
        char date_str[16];
        snprintf(date_str, sizeof(date_str), "%02d/%02d", timeinfo.tm_mon + 1, timeinfo.tm_mday);
        safe_update_label(label_date, date_str);
        last_day = timeinfo.tm_mday;
        last_month = timeinfo.tm_mon;
    }
    
    // 只有星期变化时才更新星期
    if (last_weekday != timeinfo.tm_wday) {
        safe_update_label(label_week, weekDays[timeinfo.tm_wday]);
        last_weekday = timeinfo.tm_wday;
    }
    
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
    update_lvgl_text_area(&weather_area, now_weather);
    
    char temp_display[16];
    snprintf(temp_display, sizeof(temp_display), "%s°C", now_temperature);
    update_lvgl_text_area(&temp_area, temp_display);
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

// WiFi事件处理函数
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    static int retry_count = 0;
    const int max_retry = 5;
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        retry_count = 0;
        ESP_LOGI(TAG, "WiFi station started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < max_retry) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retry to connect to the AP (%d/%d)", retry_count, max_retry);
            
            // 更新连接状态显示
            char connecting_text[32];
            snprintf(connecting_text, sizeof(connecting_text), "连接中%s", 
                     retry_count == 1 ? "." : retry_count == 2 ? ".." : "...");
            safe_update_label(label_addr, connecting_text);
        } else {
            ESP_LOGE(TAG, "Failed to connect after %d attempts", max_retry);
            safe_update_label(label_addr, "连接失败");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        
        // 更新地址显示
        safe_update_label(label_addr, now_address);
        
        check_network_connection();
        
        // WiFi连接成功后获取时间（使用任务）
        xTaskCreate(obtain_time_task, "obtain_time_task", 4096, NULL, 5, NULL);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting TFT Clock Application with LVGL (Real-time Update Version)");
    
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
    
    // 主循环变量
    time_t lastTimeDisplay = 0;
    uint32_t lastSecondTick = 0;
    uint32_t lastWeatherCheck = 0;
    
    while (1) {
        uint32_t current_tick = xTaskGetTickCount();
        
        // LVGL任务处理 - 必须定期调用
        lv_timer_handler();
        
        // 每秒更新一次时间显示
        if (current_tick - lastSecondTick >= pdMS_TO_TICKS(1000)) {
            lastSecondTick = current_tick;
            
            if (timeSynced) {
                // 正常时间显示更新 - 每秒都更新
                update_time_display();
            } else {
                // 时间未同步时显示闪烁效果
                static bool blink_state = false;
                blink_state = !blink_state;
                
                if (blink_state) {
                    safe_update_label(label_time, "--:--");
                    safe_update_label(label_sec, "--");
                    safe_update_label(label_date, "--/--");
                    safe_update_label(label_week, "--");
                } else {
                    safe_update_label(label_time, "00:00");
                    safe_update_label(label_sec, "00");
                    safe_update_label(label_date, "01/01");
                    safe_update_label(label_week, "周一");
                }
            }
        }
        
        // 每30秒显示一次当前时间到日志
        time_t now = time(NULL);
        if (now - lastTimeDisplay >= TIME_DISPLAY_INTERVAL) {
            display_current_time();
            lastTimeDisplay = now;
        }
        
        // 定期更新天气信息（每5分钟）
        if (now - lastWeatherUpdate >= WEATHER_UPDATE_INTERVAL) {
            ESP_LOGI(TAG, "Updating weather information...");
            
            // 保存当前天气信息作为备份
            char backup_weather[32];
            char backup_temperature[8];
            strcpy(backup_weather, now_weather);
            strcpy(backup_temperature, now_temperature);
            
            if (get_weather_info(now_weather, now_temperature, sizeof(now_weather))) {
                ESP_LOGI(TAG, "Weather info updated: %s, %s", now_weather, now_temperature);
                update_weather_display();
                lastWeatherUpdate = now;
            } else {
                ESP_LOGW(TAG, "Failed to update weather info, using previous data");
                // 恢复备份数据
                strcpy(now_weather, backup_weather);
                strcpy(now_temperature, backup_temperature);
            }
        }
        
        // 确保LVGL任务得到处理
        lv_timer_handler();
        
        // 短延时，确保系统响应
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
