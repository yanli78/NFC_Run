idf_component_register(SRCS "web_ota.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server app_update esp_wifi status_led)