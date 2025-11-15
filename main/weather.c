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

// 用于存储HTTP响应数据的结构
typedef struct {
    char *buffer;
    size_t size;
    size_t length;
} http_response_t;

// 全局响应数据
static http_response_t response_data = {0};

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            
            // 分配或扩展缓冲区
            if (response_data.buffer == NULL) {
                response_data.size = evt->data_len + 1;
                response_data.buffer = malloc(response_data.size);
                if (response_data.buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate response buffer");
                    return ESP_FAIL;
                }
                response_data.length = 0;
            } else if (response_data.length + evt->data_len >= response_data.size) {
                size_t new_size = response_data.size + evt->data_len + 1;
                char *new_buffer = realloc(response_data.buffer, new_size);
                if (new_buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to reallocate response buffer");
                    free(response_data.buffer);
                    response_data.buffer = NULL;
                    return ESP_FAIL;
                }
                response_data.buffer = new_buffer;
                response_data.size = new_size;
            }
            
            // 复制数据到缓冲区
            memcpy(response_data.buffer + response_data.length, evt->data, evt->data_len);
            response_data.length += evt->data_len;
            response_data.buffer[response_data.length] = '\0';
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
    // 重置响应数据
    if (response_data.buffer) {
        free(response_data.buffer);
        response_data.buffer = NULL;
    }
    response_data.size = 0;
    response_data.length = 0;
    
    // 构建完整的API URL - 使用HTTP而不是HTTPS
    char url[256];
    snprintf(url, sizeof(url), 
             "http://api.seniverse.com/v3/weather/now.json?key=%s&location=%s&language=%s&unit=%s",
             WEATHER_API_KEY, WEATHER_LOCATION, WEATHER_LANGUAGE, WEATHER_UNIT);
    
    ESP_LOGI(TAG, "Requesting weather from: %s", url);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .timeout_ms = 15000,  // 增加到15秒
        .disable_auto_redirect = false,  // 允许重定向
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }
    
    // 设置HTTP头
    esp_http_client_set_header(client, "User-Agent", "ESP32-Weather-Client");
    esp_http_client_set_header(client, "Accept", "application/json");
    
    esp_err_t err = esp_http_client_perform(client);
    bool success = false;
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status: %d", status_code);
        
        if (status_code == 200) {
            if (response_data.buffer != NULL && response_data.length > 0) {
                ESP_LOGI(TAG, "Received JSON: %s", response_data.buffer);
                
                // 解析JSON
                cJSON *root = cJSON_Parse(response_data.buffer);
                if (root != NULL) {
                    cJSON *results = cJSON_GetObjectItem(root, "results");
                    if (results != NULL && cJSON_IsArray(results)) {
                        int array_size = cJSON_GetArraySize(results);
                        if (array_size > 0) {
                            cJSON *result0 = cJSON_GetArrayItem(results, 0);
                            if (result0 != NULL) {
                                cJSON *now = cJSON_GetObjectItem(result0, "now");
                                if (now != NULL) {
                                    cJSON *text = cJSON_GetObjectItem(now, "text");
                                    cJSON *temp = cJSON_GetObjectItem(now, "temperature");
                                    
                                    if (text != NULL && temp != NULL) {
                                        // 安全复制天气信息
                                        const char *weather_text = cJSON_GetStringValue(text);
                                        const char *temp_text = cJSON_GetStringValue(temp);
                                        
                                        if (weather_text && temp_text) {
                                            strncpy(weather, weather_text, weather_len - 1);
                                            weather[weather_len - 1] = '\0';
                                            
                                            strncpy(temperature, temp_text, 6);
                                            temperature[6] = '\0';
                                            //strcat(temperature, "°C");
                                            
                                            success = true;
                                            ESP_LOGI(TAG, "Weather parsed successfully: %s, %s", weather, temperature);
                                        }
                                    } else {
                                        ESP_LOGE(TAG, "Text or temperature field is NULL");
                                    }
                                } else {
                                    ESP_LOGE(TAG, "No 'now' object in response");
                                }
                            } else {
                                ESP_LOGE(TAG, "First result is NULL");
                            }
                        } else {
                            ESP_LOGE(TAG, "Results array is empty");
                        }
                    } else {
                        ESP_LOGE(TAG, "No results field or not an array");
                        
                        // 检查错误信息
                        cJSON *error_msg = cJSON_GetObjectItem(root, "status");
                        if (error_msg != NULL) {
                            ESP_LOGE(TAG, "API Error: %s", cJSON_GetStringValue(error_msg));
                        }
                    }
                    cJSON_Delete(root);
                } else {
                    ESP_LOGE(TAG, "Failed to parse JSON response");
                    const char *error_ptr = cJSON_GetErrorPtr();
                    if (error_ptr != NULL) {
                        ESP_LOGE(TAG, "JSON error before: %s", error_ptr);
                    }
                }
            } else {
                ESP_LOGE(TAG, "No response data received");
            }
        } else {
            ESP_LOGE(TAG, "HTTP request failed with status: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        
        // 获取HTTP客户端错误代码
        int http_errno = esp_http_client_get_errno(client);
        if (http_errno != 0) {
            ESP_LOGE(TAG, "HTTP client error code: %d", http_errno);
        }
    }
    
    esp_http_client_cleanup(client);
    
    // 清理响应数据
    if (response_data.buffer) {
        free(response_data.buffer);
        response_data.buffer = NULL;
    }
    response_data.size = 0;
    response_data.length = 0;
    
    return success;
}