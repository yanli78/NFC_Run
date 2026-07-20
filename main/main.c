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

// 分离出来的 OTA 组件头文件
#include "web_ota.h"
#include "udp_logger.h"
#include "status_led.h"
#include "pn532_reader.h"

#define WIFI_SSID "Smart"
#define WIFI_PASS "88888888"

static const char *TAG = "MAIN";
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

/* ==============================================================================
 * 1. Wi-Fi 连接模块 (保持不变)
 * ============================================================================== */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // 获取断开原因，方便排查
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi断开，错误码：%d，500ms后重连", disconn->reason);

        // 延时再重连，防止高频刷屏、射频过载
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "联网成功 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        status_led_set(SYS_STATE_WIFI_OK);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    status_led_set(SYS_STATE_WIFI_CONN);

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_err_t err = esp_netif_set_hostname(netif, "ESP32-NFC");
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .bssid_set = 0},
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialization complete. Waiting for connection...");
    // 最多等待10秒，超时直接往下执行，后台仍会自动重试WiFi
    EventBits_t wifi_bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(10000));

    if (wifi_bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "WiFi 初次连接成功");
        status_led_set(SYS_STATE_WIFI_OK);
    }
    else
    {
        ESP_LOGE(TAG, "10秒未连上WiFi，设备后台持续重试WiFi");
        status_led_set(SYS_STATE_ERROR);
    }
    // 无论WiFi有没有连上，都启动UDP日志和OTA服务
    init_udp_logging();
}

/* ==============================================================================
 * 2. 主函数入口
 * ============================================================================== */
void app_main(void)
{
    status_led_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 连接 Wi-Fi
    wifi_init_sta();

    // 调用 web_ota 组件中的函数启动 OTA 服务
    start_webserver();

    pn532_init_and_start();
}