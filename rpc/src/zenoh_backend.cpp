#include "zenoh_backend.h"

#ifdef HAS_ZENOH
#include <zenoh.h>
#include <memory>
#include <mutex>
#include <iostream>
#include <stdexcept>

namespace zenoh_global {

static z_owned_session_t g_session;
static std::once_flag g_init_flag;
static std::mutex g_mutex;

void init() {
    std::call_once(g_init_flag, []() {
        z_owned_config_t config;
        z_config_default(&config);

        // 可在此处设置额外的 Zenoh 配置
        // 例如: zc_config_insert_json(z_config_loan(&config),
        //        Z_CONFIG_MODE_KEY, "peer");

        if (z_open(&g_session, z_move(config)) != Z_OK) {
            throw std::runtime_error("[Zenoh] 无法打开 session，请确认 zenohd 已启动或网络可达");
        }

        LOG_INFO("[Zenoh] session 已建立");
    });
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (z_session_check(&g_session)) {
        z_close(z_move(g_session));
        LOG_INFO("[Zenoh] session 已关闭");
    }
}

z_session_t getSession() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return z_session_loan(&g_session);
}

}  // namespace zenoh_global

#else
// 非 Zenoh 构建时，cpp 文件为空（存根已在 header 中定义）
#endif  // HAS_ZENOH
