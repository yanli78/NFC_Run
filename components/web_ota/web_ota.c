#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "sys/param.h"
#include "web_ota.h"

#include "status_led.h" 

static const char *TAG = "WEB_OTA";

// HTML 页面内容保留原样
static esp_err_t index_get_handler(httpd_req_t *req)
{
    // 为了避免 C 语言转义字符的混乱，HTML 内部的属性建议全部使用单引号
    const char *html =
        "<!DOCTYPE html>"
        "<html lang='zh-CN'>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>ESP32-S3 OTA Update</title>"
        "<style>"
        "body{font-family:'Segoe UI',Tahoma,sans-serif;background:#f4f7f6;color:#333;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}"
        ".card{background:#fff;padding:30px;border-radius:12px;box-shadow:0 8px 16px rgba(0,0,0,0.1);width:100%;max-width:400px;text-align:center;}"
        "h2{margin-top:0;color:#2c3e50;font-weight:600;}"
        ".file-area{margin:25px 0;position:relative;overflow:hidden;}"
        ".btn{background:#3498db;color:#fff;padding:12px 24px;border:none;border-radius:6px;cursor:pointer;font-size:16px;transition:0.3s;width:100%;box-sizing:border-box;font-weight:500;}"
        ".btn:hover{background:#2980b9;}"
        ".btn:disabled{background:#bdc3c7;cursor:not-allowed;}"
        ".btn-select{background:#ecf0f1;color:#2c3e50;border:2px dashed #bdc3c7;}"
        ".btn-select:hover{background:#e2e6e7;border-color:#95a5a6;}"
        "input[type='file']{position:absolute;left:0;top:0;opacity:0;cursor:pointer;width:100%;height:100%;}"
        "#file-name{margin:15px 0;font-size:14px;color:#7f8c8d;word-break:break-all;}"
        ".progress-bg{width:100%;background:#ecf0f1;border-radius:6px;overflow:hidden;margin-bottom:15px;display:none;height:12px;}"
        ".progress-bar{height:100%;background:#2ecc71;width:0%;transition:width 0.2s ease;}"
        "#status{font-size:14px;font-weight:600;min-height:20px;margin-bottom:15px;}"
        ".success{color:#2ecc71;} .error{color:#e74c3c;} .info{color:#f39c12;}"
        "</style>"
        "</head>"
        "<body>"
        "<div class='card'>"
        "<h2>设备固件升级 (OTA)</h2>"
        "<div class='file-area'>"
        "<button class='btn btn-select'>点击选择 .bin 固件</button>"
        "<input type='file' id='file' accept='.bin'>"
        "</div>"
        "<div id='file-name'>未选择任何文件</div>"
        "<div class='progress-bg' id='p-bg'>"
        "<div class='progress-bar' id='p-bar'></div>"
        "</div>"
        "<div id='status'></div>"
        "<button class='btn' id='upload-btn' disabled>开始烧录</button>"
        "</div>"
        "<script>"
        "const fileIn = document.getElementById('file');"
        "const fileName = document.getElementById('file-name');"
        "const upBtn = document.getElementById('upload-btn');"
        "const pBg = document.getElementById('p-bg');"
        "const pBar = document.getElementById('p-bar');"
        "const status = document.getElementById('status');"

        "fileIn.addEventListener('change', () => {"
        "if (fileIn.files.length > 0) {"
        "const f = fileIn.files[0];"
        "if (!f.name.endsWith('.bin')) {"
        "status.className = 'error'; status.innerText = '错误: 请选择 .bin 后缀的固件文件';"
        "upBtn.disabled = true; fileName.innerText = '未选择任何文件'; return;"
        "}"
        "fileName.innerText = f.name + ' (' + (f.size / 1024).toFixed(1) + ' KB)';"
        "upBtn.disabled = false; status.innerText = '';"
        "}"
        "});"

        "upBtn.addEventListener('click', () => {"
        "if (fileIn.files.length === 0) return;"
        "const f = fileIn.files[0];"
        "upBtn.disabled = true; fileIn.disabled = true;"
        "pBg.style.display = 'block'; status.className = 'info';"
        "status.innerText = '正在上传数据，请勿断开设备电源...';"

        "const xhr = new XMLHttpRequest();"
        "xhr.upload.addEventListener('progress', (e) => {"
        "if (e.lengthComputable) {"
        "const percent = Math.round((e.loaded / e.total) * 100);"
        "pBar.style.width = percent + '%';"
        "if(percent === 100) status.innerText = '上传完成，正在写入 Flash 并重启...';"
        "}"
        "});"

        "xhr.onreadystatechange = () => {"
        "if (xhr.readyState === XMLHttpRequest.DONE) {"
        "if (xhr.status === 200) {"
        "status.className = 'success'; status.innerText = '升级成功！设备正在重启...';"
        "pBar.style.backgroundColor = '#27ae60';"
        "} else {"
        "status.className = 'error'; status.innerText = '升级失败，HTTP 状态码: ' + xhr.status;"
        "upBtn.disabled = false; fileIn.disabled = false;"
        "pBar.style.backgroundColor = '#e74c3c';"
        "}"
        "}"
        "};"

        "xhr.open('POST', '/update', true);"
        "xhr.setRequestHeader('Content-Type', 'application/octet-stream');"
        "xhr.send(f);"
        "});"
        "</script>"
        "</body>"
        "</html>";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    status_led_set(SYS_STATE_OTA_UPDATING);

    esp_err_t err;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(TAG, "Starting OTA update...");

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL)
    {
        ESP_LOGE(TAG, "Failed to get OTA partition");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int received;
    char buf[1024];

    while (remaining > 0)
    {
        if ((received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            ESP_LOGE(TAG, "File receive failed");
            esp_ota_abort(ota_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, (const void *)buf, received);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA Success. Rebooting...");
    httpd_resp_sendstr(req, "Update Success! Rebooting...");

    // 注意：重启前建议给 HTTP 响应留出发送时间
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // 1. 增大任务栈空间到 8192 字节，防止 OTA 写入时爆栈崩溃
    config.stack_size = 8192;

    // 2. 增加接收和发送的超时时间 (默认通常较短)
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t uri_get = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &uri_get);

        httpd_uri_t uri_post = {.uri = "/update", .method = HTTP_POST, .handler = update_post_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &uri_post);
        ESP_LOGI(TAG, "Web server started.");
    }
}