#include "status_led.h"
#include "led_strip.h"
#include "esp_log.h"

#define WS2812_GPIO 48    // 绝大多数 ESP32-S3 板载 LED 的引脚
#define LED_BRIGHTNESS 20 // 亮度限制 (0-255)，建议不要太高，否则刺眼且发热

static const char *TAG = "STATUS_LED";
static led_strip_handle_t led_strip;

void status_led_init(void)
{
    // 1. 配置 LED 灯带的硬件参数
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_GPIO,
        .max_leds = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    // 2. 配置底层的 RMT 驱动参数 (ESP-IDF v5 推荐方式)
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz 分辨率
        .flags.with_dma = false,
    };

    // 3. 初始化并创建设备句柄
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

    // 初始化后先关闭 LED
    led_strip_clear(led_strip);
    ESP_LOGI(TAG, "WS2812 Status LED Initialized.");

    // 默认进入初始化状态
    status_led_set(SYS_STATE_INIT);
}

void status_led_set(sys_state_t state)
{
    if (!led_strip)
        return;

    uint8_t r = 0, g = 0, b = 0;

    switch (state)
    {
    case SYS_STATE_INIT:
        r = LED_BRIGHTNESS;
        g = LED_BRIGHTNESS;
        b = 0;
        break; // 黄色
    case SYS_STATE_WIFI_CONN:
        r = 0;
        g = 0;
        b = LED_BRIGHTNESS;
        break; // 蓝色
    case SYS_STATE_WIFI_OK:
        r = 0;
        g = LED_BRIGHTNESS;
        b = 0;
        break; // 绿色
    case SYS_STATE_OTA_UPDATING:
        r = LED_BRIGHTNESS;
        g = 0;
        b = LED_BRIGHTNESS;
        break; // 紫色
    case SYS_STATE_ERROR:
        r = LED_BRIGHTNESS;
        g = 0;
        b = 0;
        break; // 红色
    default:
        break;
    }

    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
}