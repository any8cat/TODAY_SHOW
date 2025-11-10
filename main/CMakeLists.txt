idf_component_register(SRCS "TODAY_SHOW.c" "lcd_driver.c" "weather.c" "fonts.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_timer esp_wifi nvs_flash lwip freertos esp_driver_spi driver esp_http_client esp_netif esp_event json esp-tls)
                 