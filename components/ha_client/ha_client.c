#include "ha_client.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_log.h"

// TODO: 替换为你的 HA 服务器实际地址和长期访问令牌 (Long-Lived Access Token)
#define HA_URL "http://192.168.1.4:8123/api/events/nfc_scanned"
#define HA_TOKEN "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJkMGMzZmUyN2I1ZDI0ZWMxYTQxYTc2ZWQzNzJlNzQ1NSIsImlhdCI6MTc4NDU2MDAyNiwiZXhwIjoyMDk5OTIwMDI2fQ.kk2AFqSpfhb7kt1fN0ahouT_5D0cPcx1B2AlN5VqVxY"

static const char *TAG = "HA_CLIENT";

// 独立的 HTTP POST 任务
static void http_post_task(void *pvParameters)
{
    char *json_payload = (char *)pvParameters;

    esp_http_client_config_t config = {
        .url = HA_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(json_payload);
        vTaskDelete(NULL);
        return;
    }

    // 设置请求头
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", HA_TOKEN);

    // 设置请求体
    esp_http_client_set_post_field(client, json_payload, strlen(json_payload));

    // 执行请求
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP POST Status = %d, length = %" PRId64,
                 status_code,
                 esp_http_client_get_content_length(client));

        if (status_code == 200 || status_code == 201)
        {
            ESP_LOGI(TAG, "Successfully pushed to HA!");
        }
        else
        {
            ESP_LOGW(TAG, "HA rejected the request.");
        }
    }
    else
    {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(json_payload); // 释放堆内存
    vTaskDelete(NULL);  // 任务完成，自杀销毁
}

void ha_client_post_json(const char *json_payload)
{
    if (json_payload == NULL)
        return;

    // 因为 HTTP 任务是异步的，必须在堆上复制一份 JSON 数据，防止原函数局部变量销毁
    char *payload_copy = strdup(json_payload);
    if (payload_copy == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for JSON copy");
        return;
    }

    // 创建后台任务执行网络请求，分配 4KB 栈空间
    xTaskCreate(http_post_task, "ha_post_task", 4096, payload_copy, 4, NULL);
}