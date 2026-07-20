#include "pn532_reader.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "cJSON.h"


#include "ha_client.h"
#include "status_led.h"

#define I2C_MASTER_SCL_IO 9
#define I2C_MASTER_SDA_IO 8
#define I2C_MASTER_FREQ_HZ 100000
#define PN532_I2C_ADDRESS 0x24 // PN532 7位 I2C 地址 (0x48 >> 1)

static const char *TAG = "PN532";
static i2c_master_dev_handle_t pn532_handle;

// PN532 的唤醒和配置指令 (SAMConfiguration)
static const uint8_t pn532_sam_config[] = {
    0x00, 0x00, 0xFF, 0x03, 0xFD, 0xD4, 0x14, 0x01, 0x17, 0x00};

// 寻卡指令 (InListPassiveTarget - 寻找 106 kbps type A (Mifare) 卡)
static const uint8_t pn532_inlist[] = {
    0x00, 0x00, 0xFF, 0x04, 0xFC, 0xD4, 0x4A, 0x01, 0x00, 0xE1, 0x00};

// 新增：向目标发送 APDU (SELECT AID: F0 01 02 03 04 05 06)
// 该帧由 PN532 的 InDataExchange (0x40) 指令包装
static const uint8_t pn532_select_apdu[] = {
    0x00, 0x00, 0xFF, 0x10, 0xF0, 0xD4, 0x40, 0x01,
    0x00, 0xA4, 0x04, 0x00, 0x07, 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00,
    0x37, 0x00};

// 检查 PN532 是否准备好返回数据
static bool pn532_is_ready(void)
{
    uint8_t status;
    esp_err_t err = i2c_master_receive(pn532_handle, &status, 1, 100);
    return (err == ESP_OK && (status & 0x01)); // 最低位为1表示Ready
}

// 读取完整响应包
static esp_err_t pn532_read_response(uint8_t *buf, size_t len)
{
    uint8_t status_and_data[len + 1];
    esp_err_t err = i2c_master_receive(pn532_handle, status_and_data, len + 1, 100);
    if (err == ESP_OK)
    {
        memcpy(buf, status_and_data + 1, len); // 剥离第一个状态字节
    }
    return err;
}

// 轮询任务
static void pn532_poll_task(void *pvParameters)
{
    uint8_t ack_buf[6];
    uint8_t response[64];

    // 1. 发送 SAM 配置，唤醒 PN532
    i2c_master_transmit(pn532_handle, pn532_sam_config, sizeof(pn532_sam_config), 100);
    vTaskDelay(pdMS_TO_TICKS(10));

    while (!pn532_is_ready())
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    pn532_read_response(ack_buf, 6);

    while (!pn532_is_ready())
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    pn532_read_response(response, 8);

    ESP_LOGI(TAG, "PN532 Initialized. Robust polling started...");

    while (1)
    {
        // 【关键修复 1】：在发送新指令前，清理 PN532 缓冲区里的“垃圾数据”
        // 防止上一次手机拿开太快导致残留数据阻塞总线
        int flush_count = 0;
        while (pn532_is_ready() && flush_count < 5)
        {
            pn532_read_response(response, sizeof(response));
            vTaskDelay(pdMS_TO_TICKS(5));
            flush_count++;
        }

        // 2. 发送寻卡指令
        i2c_master_transmit(pn532_handle, pn532_inlist, sizeof(pn532_inlist), 100);
        vTaskDelay(pdMS_TO_TICKS(10));

        if (pn532_is_ready())
        {
            pn532_read_response(ack_buf, 6);
        }

        // 3. 寻卡等待
        int timeout = 0;
        while (!pn532_is_ready() && timeout < 50)
        { // 约 500ms 寻卡窗口
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout++;
        }

        // 4. 读到卡片/手机
        if (pn532_is_ready())
        {
            pn532_read_response(response, 24);

            if (response[0] == 0x00 && response[1] == 0x00 && response[2] == 0xFF &&
                response[5] == 0xD5 && response[6] == 0x4B)
            {

                int tags_found = response[7];
                if (tags_found > 0)
                {
                    // 发起 APDU 交互
                    i2c_master_transmit(pn532_handle, pn532_select_apdu, sizeof(pn532_select_apdu), 100);
                    vTaskDelay(pdMS_TO_TICKS(10));

                    if (pn532_is_ready())
                    {
                        pn532_read_response(ack_buf, 6);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Warning: Failed to get APDU ACK.");
                        continue; // 提前退出本次循环，重置状态
                    }

                    // 【关键修复 2】：增加 APDU 等待时间到 2 秒，因为 Android 拉起 HCE 服务有时会有系统级延迟
                    int apdu_timeout = 0;
                    while (!pn532_is_ready() && apdu_timeout < 200)
                    {
                        vTaskDelay(pdMS_TO_TICKS(10));
                        apdu_timeout++;
                    }

                    if (pn532_is_ready())
                    {
                        pn532_read_response(response, 64);

                        if (response[0] == 0x00 && response[5] == 0xD5 && response[6] == 0x41)
                        {
                            int status = response[7];
                            if (status == 0x00)
                            {
                                int data_len = response[3] - 3;
                                if (data_len > 2)
                                {
                                    int str_len = data_len - 2;
                                    uint8_t *app_data = &response[8];

                                    char custom_payload[32] = {0};
                                    int copy_len = str_len < 31 ? str_len : 31;
                                    memcpy(custom_payload, app_data, copy_len);

                                    cJSON *root = cJSON_CreateObject();
                                    cJSON_AddStringToObject(root, "device", "esp32s3_n8r2");
                                    cJSON_AddStringToObject(root, "token", custom_payload);
                                    char *json_string = cJSON_PrintUnformatted(root);

                                    ESP_LOGI(TAG, "==== HCE Auth Success ====");
                                    ESP_LOGI(TAG, "JSON payload: %s", json_string);

                                    // 触发状态灯闪烁 (绿色)
                                    status_led_set(SYS_STATE_WIFI_OK);

                                    // 异步发送给 Home Assistant
                                    ha_client_post_json(json_string);

                                    cJSON_free(json_string);
                                    cJSON_Delete(root);

                                    vTaskDelay(pdMS_TO_TICKS(1500));

                                    // 读取完恢复默认黄灯/蓝灯，视你的定义而定
                                    status_led_set(SYS_STATE_INIT);
                                }
                                else
                                {
                                    ESP_LOGW(TAG, "APDU Data too short: %d bytes", data_len);
                                }
                            }
                            else
                            {
                                ESP_LOGW(TAG, "APDU Exchange failed with status: 0x%02X", status);
                            }
                        }
                        else
                        {
                            ESP_LOGW(TAG, "Invalid APDU response format.");
                        }
                    }
                    else
                    {
                        // 【关键修复 3】：打印出明确的超时警告
                        ESP_LOGW(TAG, "APDU Wait Timeout. Phone moved away too fast or HCE service didn't respond.");
                    }
                }
            }
        }
        // 两次寻卡间隙
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


void pn532_init_and_start(void)
{
    // 使用 ESP-IDF v5.3 的新 I2C Master API 配置
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, // 启用内部上拉
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &bus_handle));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PN532_I2C_ADDRESS,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &pn532_handle));

    // 创建后台运行的 FreeRTOS 任务，优先级设为适中的 5
    xTaskCreate(pn532_poll_task, "pn532_task", 4096, NULL, 5, NULL);
}