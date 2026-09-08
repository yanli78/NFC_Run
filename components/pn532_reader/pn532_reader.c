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
#define PN532_I2C_ADDRESS 0x24

static const char *TAG = "PN532";
static i2c_master_dev_handle_t pn532_handle;

// PN532 唤醒与 SAM 配置
static const uint8_t pn532_sam_config[] = {
    0x00, 0x00, 0xFF, 0x03, 0xFD, 0xD4, 0x14, 0x01, 0x17, 0x00};

// 寻卡指令 (InListPassiveTarget - Type A)
static const uint8_t pn532_inlist[] = {
    0x00, 0x00, 0xFF, 0x04, 0xFC, 0xD4, 0x4A, 0x01, 0x00, 0xE1, 0x00};

// Android HCE APDU 指令
static const uint8_t pn532_select_apdu[] = {
    0x00, 0x00, 0xFF, 0x10, 0xF0, 0xD4, 0x40, 0x01,
    0x00, 0xA4, 0x04, 0x00, 0x07, 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00,
    0x37, 0x00};

// 从转储图确认：Sector 1 的 Key A 为 D3 F7 D3 F7 D3 F7
static const uint8_t m1_key_a[6] = {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7};

// 检查 PN532 是否就绪
static bool pn532_is_ready(void)
{
    uint8_t status;
    esp_err_t err = i2c_master_receive(pn532_handle, &status, 1, 50);
    return (err == ESP_OK && (status & 0x01));
}

// 等待就绪超时函数
static esp_err_t pn532_wait_ready(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!pn532_is_ready() && waited < timeout_ms)
    {
        vTaskDelay(pdMS_TO_TICKS(5));
        waited += 5;
    }
    return pn532_is_ready() ? ESP_OK : ESP_ERR_TIMEOUT;
}

// 读取响应包
static esp_err_t pn532_read_response(uint8_t *buf, size_t len)
{
    uint8_t status_and_data[len + 1];
    esp_err_t err = i2c_master_receive(pn532_handle, status_and_data, len + 1, 100);
    if (err == ESP_OK)
    {
        memcpy(buf, status_and_data + 1, len);
    }
    return err;
}

// 检查并读取 ACK
static esp_err_t pn532_check_ack(void)
{
    if (pn532_wait_ready(100) != ESP_OK)
        return ESP_ERR_TIMEOUT;
    uint8_t ack[6];
    if (pn532_read_response(ack, 6) != ESP_OK)
        return ESP_FAIL;

    const uint8_t expected_ack[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    return (memcmp(ack, expected_ack, 6) == 0) ? ESP_OK : ESP_FAIL;
}

// 通用发送指令帧函数（自动计算包长、LCS、DCS 与终止符）
static esp_err_t pn532_send_cmd(const uint8_t *cmd, size_t cmd_len)
{
    uint8_t payload_len = 1 + cmd_len; // 1(TFI: 0xD4) + cmd_len
    size_t frame_len = 7 + payload_len;
    uint8_t frame[64];

    frame[0] = 0x00;
    frame[1] = 0x00;
    frame[2] = 0xFF;
    frame[3] = payload_len;
    frame[4] = (uint8_t)(0x100 - payload_len);
    frame[5] = 0xD4; // TFI: 主机到 PN532
    memcpy(&frame[6], cmd, cmd_len);

    uint8_t dcs = 0xD4;
    for (size_t i = 0; i < cmd_len; i++)
    {
        dcs += cmd[i];
    }
    frame[6 + cmd_len] = (uint8_t)(~dcs + 1);
    frame[7 + cmd_len] = 0x00;

    return i2c_master_transmit(pn532_handle, frame, frame_len, 100);
}

// M1 扇区认证与读取
static esp_err_t m1_auth_and_read_block(uint8_t block_num, const uint8_t *key, const uint8_t *uid, uint8_t *out_data)
{
    uint8_t response[32];

    // 1. 构建认证载荷: 0x40(InDataExchange), 0x01(Target), 0x60(Auth A), Block, Key(6B), UID(4B)
    uint8_t auth_payload[14];
    auth_payload[0] = 0x40;
    auth_payload[1] = 0x01;
    auth_payload[2] = 0x60;
    auth_payload[3] = block_num;
    memcpy(&auth_payload[4], key, 6);
    memcpy(&auth_payload[10], uid, 4);

    if (pn532_send_cmd(auth_payload, sizeof(auth_payload)) != ESP_OK)
        return ESP_FAIL;
    if (pn532_check_ack() != ESP_OK)
    {
        ESP_LOGE(TAG, "Auth Command ACK Failed");
        return ESP_FAIL;
    }

    if (pn532_wait_ready(100) != ESP_OK)
    {
        ESP_LOGE(TAG, "Auth Response Timeout");
        return ESP_ERR_TIMEOUT;
    }

    pn532_read_response(response, 10);
    // 校验返回: 0xD5, 0x41, 状态码 (0x00 表示认证成功)
    if (response[5] != 0xD5 || response[6] != 0x41 || response[7] != 0x00)
    {
        ESP_LOGE(TAG, "Auth Failed! Status Code: 0x%02X", response[7]);
        return ESP_FAIL;
    }

    // 2. 构建读块载荷: 0x40(InDataExchange), 0x01(Target), 0x30(Read), Block
    uint8_t read_payload[4] = {0x40, 0x01, 0x30, block_num};
    if (pn532_send_cmd(read_payload, sizeof(read_payload)) != ESP_OK)
        return ESP_FAIL;
    if (pn532_check_ack() != ESP_OK)
    {
        ESP_LOGE(TAG, "Read Command ACK Failed");
        return ESP_FAIL;
    }

    if (pn532_wait_ready(100) != ESP_OK)
    {
        ESP_LOGE(TAG, "Read Response Timeout");
        return ESP_ERR_TIMEOUT;
    }

    pn532_read_response(response, 26);
    if (response[5] == 0xD5 && response[6] == 0x41 && response[7] == 0x00)
    {
        memcpy(out_data, &response[8], 16);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Read Block Failed! Status Code: 0x%02X", response[7]);
    return ESP_FAIL;
}

// 统一数据上报函数（保证 HCE 与 M1 发送格式完全一致）
static void report_token_to_ha(const char *token)
{
    if (token == NULL || strlen(token) == 0)
    {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", "esp32s3_n8r2");
    cJSON_AddStringToObject(root, "token", token);
    char *json_string = cJSON_PrintUnformatted(root);

    ESP_LOGI(TAG, "==== Auth Success ====");
    ESP_LOGI(TAG, "JSON: %s", json_string);

    status_led_set(SYS_STATE_WIFI_OK);
    ha_client_post_json(json_string);

    cJSON_free(json_string);
    cJSON_Delete(root);

    vTaskDelay(pdMS_TO_TICKS(1500));
    status_led_set(SYS_STATE_INIT);
}

// 轮询任务
static void pn532_poll_task(void *pvParameters)
{
    uint8_t ack_buf[6];
    uint8_t response[64];

    // 初始化 PN532
    i2c_master_transmit(pn532_handle, pn532_sam_config, sizeof(pn532_sam_config), 100);
    vTaskDelay(pdMS_TO_TICKS(10));
    while (!pn532_is_ready())
        vTaskDelay(pdMS_TO_TICKS(10));
    pn532_read_response(ack_buf, 6);
    while (!pn532_is_ready())
        vTaskDelay(pdMS_TO_TICKS(10));
    pn532_read_response(response, 8);

    ESP_LOGI(TAG, "PN532 Initialized. Polling started...");

    while (1)
    {
        int flush_count = 0;
        while (pn532_is_ready() && flush_count < 5)
        {
            pn532_read_response(response, sizeof(response));
            vTaskDelay(pdMS_TO_TICKS(5));
            flush_count++;
        }

        // 发起寻卡
        i2c_master_transmit(pn532_handle, pn532_inlist, sizeof(pn532_inlist), 100);
        vTaskDelay(pdMS_TO_TICKS(10));
        if (pn532_is_ready())
        {
            pn532_read_response(ack_buf, 6);
        }

        int timeout = 0;
        while (!pn532_is_ready() && timeout < 50)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout++;
        }

        if (pn532_is_ready())
        {
            pn532_read_response(response, 24);

            if (response[0] == 0x00 && response[1] == 0x00 && response[2] == 0xFF &&
                response[5] == 0xD5 && response[6] == 0x4B)
            {
                int tags_found = response[7];
                if (tags_found > 0)
                {
                    uint8_t sak = response[11];
                    uint8_t *uid = &response[13];

                    // 分支 A: Android HCE 设备
                    if (sak & 0x20)
                    {
                        ESP_LOGI(TAG, "Target supports ISO 14443-4 (Android HCE)");
                        i2c_master_transmit(pn532_handle, pn532_select_apdu, sizeof(pn532_select_apdu), 100);
                        vTaskDelay(pdMS_TO_TICKS(10));

                        if (pn532_is_ready())
                        {
                            pn532_read_response(ack_buf, 6);
                        }
                        else
                        {
                            continue;
                        }

                        int apdu_timeout = 0;
                        while (!pn532_is_ready() && apdu_timeout < 200)
                        {
                            vTaskDelay(pdMS_TO_TICKS(10));
                            apdu_timeout++;
                        }

                        if (pn532_is_ready())
                        {
                            pn532_read_response(response, 64);
                            if (response[0] == 0x00 && response[5] == 0xD5 && response[6] == 0x41 && response[7] == 0x00)
                            {
                                int data_len = response[3] - 3;
                                if (data_len > 2)
                                {
                                    int str_len = data_len - 2;
                                    char custom_payload[32] = {0};
                                    int copy_len = str_len < 31 ? str_len : 31;
                                    memcpy(custom_payload, &response[8], copy_len);

                                    // 统一上报
                                    report_token_to_ha(custom_payload);
                                }
                            }
                        }
                    }
                    // 分支 B: Mifare Classic 物理卡
                    else
                    {
                        ESP_LOGI(TAG, "Mifare Classic Detected (UID: %02X%02X%02X%02X)",
                                 uid[0], uid[1], uid[2], uid[3]);

                        uint8_t block_target = 4; // 扇区 1 的第 0 块
                        uint8_t block_data[16] = {0};

                        esp_err_t ret = m1_auth_and_read_block(block_target, m1_key_a, uid, block_data);
                        if (ret == ESP_OK)
                        {
                            char payload_text[17] = {0};
                            memcpy(payload_text, block_data, 16);

                            // 统一上报
                            report_token_to_ha(payload_text);
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void pn532_init_and_start(void)
{
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &bus_handle));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PN532_I2C_ADDRESS,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &pn532_handle));

    xTaskCreate(pn532_poll_task, "pn532_task", 4096, NULL, 5, NULL);
}

