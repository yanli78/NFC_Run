#ifndef HA_CLIENT_H
#define HA_CLIENT_H

#ifdef __cplusplus
extern "C"
{
#endif

    // 将生成的 JSON 异步发送到 HA
    void ha_client_post_json(const char *json_payload);

#ifdef __cplusplus
}
#endif

#endif // HA_CLIENT_H