#include "button_reader.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "ha_client.h" // 调用 report_token_to_ha

static const char *TAG = "BTN";

// 按键结构体定义
typedef struct
{
    gpio_num_t pin;
    const char *token; // 触发后上报的字符
    int last_level;    // 上次稳定电平 (默认 1 为未按下)
    int counter;       // 消抖计数器
} button_dev_t;

// 配置 6 个按键引脚及对应的字符
static button_dev_t btn_list[] = {
    {.pin = GPIO_NUM_4, .token = "K1", .last_level = 1, .counter = 0},
    {.pin = GPIO_NUM_13, .token = "K2", .last_level = 1, .counter = 0},
    {.pin = GPIO_NUM_14, .token = "K3", .last_level = 1, .counter = 0},
    {.pin = GPIO_NUM_16, .token = "K4", .last_level = 1, .counter = 0},
    {.pin = GPIO_NUM_17, .token = "K5", .last_level = 1, .counter = 0},
    {.pin = GPIO_NUM_18, .token = "K6", .last_level = 1, .counter = 0},
};

#define BTN_COUNT (sizeof(btn_list) / sizeof(btn_list[0]))
#define DEBOUNCE_THRESHOLD 3 // 连续 3 次采样（30ms）稳定视为有效

static void button_task(void *pvParameters)
{
    while (1)
    {
        for (size_t i = 0; i < BTN_COUNT; i++)
        {
            int current_level = gpio_get_level(btn_list[i].pin);

            // 电平发生改变时增加计数
            if (current_level != btn_list[i].last_level)
            {
                btn_list[i].counter++;
                if (btn_list[i].counter >= DEBOUNCE_THRESHOLD)
                {
                    btn_list[i].last_level = current_level;
                    btn_list[i].counter = 0;

                    // 检测到按下（下降沿：高电平 -> 低电平）
                    if (current_level == 0)
                    {
                        ESP_LOGI(TAG, "Button pressed on GPIO %d -> Token: %s",
                                 btn_list[i].pin, btn_list[i].token);

                        // 统一上报给 Home Assistant
                        report_token_to_ha(btn_list[i].token);
                    }
                }
            }
            else
            {
                btn_list[i].counter = 0; // 电平抖动复位
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // 10ms 轮询周期
    }
}

void button_reader_init_and_start(void)
{
    // 配置 GPIO 输入、内部上拉
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_4) |
                        (1ULL << GPIO_NUM_13) |
                        (1ULL << GPIO_NUM_14) |
                        (1ULL << GPIO_NUM_16) |
                        (1ULL << GPIO_NUM_17) |
                        (1ULL << GPIO_NUM_18),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // 创建按键监控任务
    xTaskCreate(button_task, "button_task", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "Button reader initialized for 6 pins.");
}