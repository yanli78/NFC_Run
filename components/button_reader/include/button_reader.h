#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化 GPIO 4, 13, 14, 16, 17, 18 并创建按键轮询任务
     */
    void button_reader_init_and_start(void);

#ifdef __cplusplus
}
#endif