#ifndef PN532_READER_H
#define PN532_READER_H

#ifdef __cplusplus
extern "C"
{
#endif

    // 初始化 PN532 并创建一个后台轮询任务
    void pn532_init_and_start(void);

#ifdef __cplusplus
}
#endif

#endif // PN532_READER_H