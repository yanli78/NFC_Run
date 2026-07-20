#ifndef STATUS_LED_H
#define STATUS_LED_H

#ifdef __cplusplus
extern "C"
{
#endif

    // 定义系统运行状态
    typedef enum
    {
        SYS_STATE_INIT,         // 初始化中 (黄色)
        SYS_STATE_WIFI_CONN,    // WiFi 连接中 (蓝色)
        SYS_STATE_WIFI_OK,      // WiFi 已连接/空闲 (绿色)
        SYS_STATE_OTA_UPDATING, // OTA 升级中 (紫色)
        SYS_STATE_ERROR         // 发生错误 (红色)
    } sys_state_t;

    // 初始化 LED
    void status_led_init(void);

    // 设置系统状态，LED 自动改变颜色
    void status_led_set(sys_state_t state);

#ifdef __cplusplus
}
#endif

#endif // STATUS_LED_H