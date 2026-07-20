#include "udp_logger.h"
#include <stdio.h>
#include <stdarg.h>
#include "lwip/sockets.h"
#include "esp_log.h"

#define UDP_LOG_PC_IP "192.168.1.100" // 修改为你的目标 IP
#define UDP_LOG_PORT 5140

static int udp_log_socket = -1;
static struct sockaddr_in dest_addr;
static vprintf_like_t default_vprintf;

static int udp_vprintf(const char *fmt, va_list args)
{
    char log_buffer[256];
    int len = vsnprintf(log_buffer, sizeof(log_buffer), fmt, args);

    if (udp_log_socket >= 0 && len > 0)
    {
        sendto(udp_log_socket, log_buffer, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    }
    return default_vprintf(fmt, args);
}

void init_udp_logging(void)
{
    dest_addr.sin_addr.s_addr = inet_addr(UDP_LOG_PC_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_LOG_PORT);

    udp_log_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_log_socket < 0)
    {
        ESP_LOGE("UDP_LOG", "Unable to create socket");
        return;
    }

    default_vprintf = esp_log_set_vprintf(udp_vprintf);
    ESP_LOGI("UDP_LOG", "UDP Logging initialized.");
}