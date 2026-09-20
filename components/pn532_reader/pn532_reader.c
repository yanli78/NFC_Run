#include "pn532_reader.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

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

// Android HCE APDU 指令 (SELECT AID: F0010203040506)
static const uint8_t pn532_select_apdu[] = {
    0x00, 0x00, 0xFF, 0x10, 0xF0, 0xD4, 0x40, 0x01,
    0x00, 0xA4, 0x04, 0x00, 0x07, 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00,
    0x37, 0x00};

// Sector 1 的 Key A
static const uint8_t m1_key_a[6] = {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7};

// 检查 PN532 是否就绪
static bool pn532_is_ready(void)
{
    uint8_t status = 0;
    esp_err_t err = i2c_master_receive(pn532_handle, &status, 1, 50);
    return (err == ESP_OK && (status & 0x01));
}

// 等待就绪函数（每次至少休眠 10ms，防止 FreeRTOS tick 截断）
static esp_err_t pn532_wait_ready(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!pn532_is_ready() && waited < timeout_ms)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
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

// 通用发送指令帧函数
static esp_err_t pn532_send_cmd(const uint8_t *cmd, size_t cmd_len)
{
    uint8_t payload_len = 1 + cmd_len;
    size_t frame_len = 7 + payload_len;
    uint8_t frame[64];

    frame[0] = 0x00;
    frame[1] = 0x00;
    frame[2] = 0xFF;
    frame[3] = payload_len;
    frame[4] = (uint8_t)(0x100 - payload_len);
    frame[5] = 0xD4;
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
        ESP_LOGE(TAG, "[M1] Auth Command ACK Failed");
        return ESP_FAIL;
    }

    if (pn532_wait_ready(100) != ESP_OK)
    {
        ESP_LOGE(TAG, "[M1] Auth Response Timeout");
        return ESP_ERR_TIMEOUT;
    }

    pn532_read_response(response, 10);
    if (response[5] != 0xD5 || response[6] != 0x41 || response[7] != 0x00)
    {
        ESP_LOGE(TAG, "[M1] Auth Failed! Status Code: 0x%02X", response[7]);
        return ESP_FAIL;
    }

    uint8_t read_payload[4] = {0x40, 0x01, 0x30, block_num};
    if (pn532_send_cmd(read_payload, sizeof(read_payload)) != ESP_OK)
        return ESP_FAIL;
    if (pn532_check_ack() != ESP_OK)
    {
        ESP_LOGE(TAG, "[M1] Read Command ACK Failed");
        return ESP_FAIL;
    }

    if (pn532_wait_ready(100) != ESP_OK)
    {
        ESP_LOGE(TAG, "[M1] Read Response Timeout");
        return ESP_ERR_TIMEOUT;
    }

    pn532_read_response(response, 26);
    if (response[5] == 0xD5 && response[6] == 0x41 && response[7] == 0x00)
    {
        memcpy(out_data, &response[8], 16);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "[M1] Read Block Failed! Status Code: 0x%02X", response[7]);
    return ESP_FAIL;
}



// 轮询任务
static void pn532_poll_task(void *pvParameters)
{
    uint8_t ack_buf[6];
    uint8_t response[64];

    // 初始化 SAM
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
        // 清理残余
        int flush_count = 0;
        while (pn532_is_ready() && flush_count < 5)
        {
            pn532_read_response(response, sizeof(response));
            vTaskDelay(pdMS_TO_TICKS(10));
            flush_count++;
        }

        // 发起寻卡 (InListPassiveTarget)
        i2c_master_transmit(pn532_handle, pn532_inlist, sizeof(pn532_inlist), 100);
        vTaskDelay(pdMS_TO_TICKS(10));
        if (pn532_is_ready())
        {
            pn532_read_response(ack_buf, 6);
        }

        // 等待卡片进入射频场（恢复原版的 500ms 宽限期）
        int timeout = 0;
        while (!pn532_is_ready() && timeout < 50)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout++;
        }

        if (pn532_is_ready())
        {
            memset(response, 0, sizeof(response));
            pn532_read_response(response, 24);

            if (response[0] == 0x00 && response[1] == 0x00 && response[2] == 0xFF &&
                response[5] == 0xD5 && response[6] == 0x4B)
            {
                int tags_found = response[7];
                if (tags_found > 0)
                {
                    uint8_t sak = response[11];
                    uint8_t *uid = &response[13];

                    ESP_LOGI(TAG, "---------------------------------------------");
                    ESP_LOGI(TAG, "Target Detected! SAK=0x%02X, UID=%02X%02X%02X%02X",
                             sak, uid[0], uid[1], uid[2], uid[3]);

                    // ==========================================
                    // 分支 A: Android HCE 设备 (ISO 14443-4)
                    // ==========================================
                    if (sak & 0x20)
                    {
                        ESP_LOGI(TAG, "-> Target is ISO 14443-4 (Android HCE). Sending APDU...");
                        i2c_master_transmit(pn532_handle, pn532_select_apdu, sizeof(pn532_select_apdu), 100);
                        vTaskDelay(pdMS_TO_TICKS(10));

                        if (pn532_is_ready())
                        {
                            pn532_read_response(ack_buf, 6);
                        }
                        else
                        {
                            ESP_LOGW(TAG, "[HCE] APDU ACK not ready");
                            continue;
                        }

                        // 等待手机应答（最长等待 2 秒）
                        int apdu_timeout = 0;
                        while (!pn532_is_ready() && apdu_timeout < 200)
                        {
                            vTaskDelay(pdMS_TO_TICKS(10));
                            apdu_timeout++;
                        }

                        if (pn532_is_ready())
                        {
                            memset(response, 0, sizeof(response));
                            pn532_read_response(response, 64);

                            ESP_LOGI(TAG, "[HCE] Raw response:");
                            ESP_LOG_BUFFER_HEX_LEVEL(TAG, response, 24, ESP_LOG_INFO);

                            if (response[5] == 0xD5 && response[6] == 0x41 && response[7] == 0x00)
                            {
                                int data_len = response[3] - 3;
                                if (data_len >= 2)
                                {
                                    uint8_t sw1 = response[8 + data_len - 2];
                                    uint8_t sw2 = response[8 + data_len - 1];

                                    ESP_LOGI(TAG, "[HCE] APDU result: SW=%02X%02X", sw1, sw2);

                                    if (sw1 == 0x90 && sw2 == 0x00)
                                    {
                                        int str_len = data_len - 2;
                                        char custom_payload[32] = {0};
                                        int copy_len = str_len < 31 ? str_len : 31;
                                        memcpy(custom_payload, &response[8], copy_len);

                                        ESP_LOGI(TAG, "[HCE] Token string: [%s]", custom_payload);
                                        report_token_to_ha(custom_payload);
                                    }
                                }
                            }
                            else
                            {
                                ESP_LOGE(TAG, "[HCE] InDataExchange Error status: 0x%02X", response[7]);
                            }
                        }
                        else
                        {
                            ESP_LOGW(TAG, "[HCE] Mobile APDU response timeout");
                        }
                    }
                    // ==========================================
                    // 分支 B: Mifare Classic 物理卡
                    // ==========================================
                    else
                    {
                        ESP_LOGI(TAG, "-> Target is Mifare Classic. Reading Sector 1 Block 0...");
                        uint8_t block_target = 4;
                        uint8_t block_data[16] = {0};

                        esp_err_t ret = m1_auth_and_read_block(block_target, m1_key_a, uid, block_data);
                        if (ret == ESP_OK)
                        {
                            char payload_text[17] = {0};
                            memcpy(payload_text, block_data, 16);
                            ESP_LOGI(TAG, "[M1] Read Success: [%s]", payload_text);
                            report_token_to_ha(payload_text);
                        }
                        else
                        {
                            ESP_LOGE(TAG, "[M1] Read Failed: %d", ret);
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