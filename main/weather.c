#include "weather.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "WEATHER";

// 心知天气API配置
#define WEATHER_API_KEY "SrqxKpth7Fvzao2Wi"
#define WEATHER_LOCATION "hangzhou"
#define WEATHER_LANGUAGE "zh-Hans"
#define WEATHER_UNIT "c"

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool get_weather_info(char *weather, char *temperature, int weather_len)
{
    // 构建完整的API URL
    char url[256];
    snprintf(url, sizeof(url), 
             "https://api.seniverse.com/v3/weather/now.json?key=%s&location=%s&language=%s&unit=%s",
             WEATHER_API_KEY, WEATHER_LOCATION, WEATHER_LANGUAGE, WEATHER_UNIT);
    
    ESP_LOGI(TAG, "Requesting weather from: %s", url);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    // 设置HTTP头
    esp_http_client_set_header(client, "User-Agent", "ESP32-Weather-Client");
    
    esp_err_t err = esp_http_client_perform(client);
    bool success = false;
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status: %d", status_code);
        
        if (status_code == 200) {
            int content_length = esp_http_client_get_content_length(client);
            char *buffer = malloc(content_length + 1);
            
            if (buffer) {
                int read_len = esp_http_client_read(client, buffer, content_length);
                buffer[read_len] = 0;
                
                // 解析JSON
                cJSON *root = cJSON_Parse(buffer);
                if (root) {
                    cJSON *results = cJSON_GetObjectItem(root, "results");
                    if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
                        cJSON *result0 = cJSON_GetArrayItem(results, 0);
                        cJSON *now = cJSON_GetObjectItem(result0, "now");
                        
                        if (now) {
                            cJSON *text = cJSON_GetObjectItem(now, "text");
                            cJSON *temp = cJSON_GetObjectItem(now, "temperature");
                            
                            if (text && temp) {
                                strncpy(weather, text->valuestring, weather_len - 1);
                                weather[weather_len - 1] = 0;
                                strncpy(temperature, temp->valuestring, 7);
                                strcat(temperature, "°C");
                                success = true;
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
                free(buffer);
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return success;
}