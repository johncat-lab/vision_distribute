#pragma once
#include "rpc/publisher.h"
#include "rpc/subscriber.h"
#include "rpc/service.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include <string>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>

#ifdef HAS_ZENOH
#include <zenoh.h>

// ========== Zenoh 全局会话 (定义在 zenoh_backend.cpp) ==========
namespace zenoh_global {
    void init();
    void shutdown();
    z_session_t getSession();
}

// ========== Zenoh Publisher ==========
// 使用 z_put 直接发布到 key expression
template<typename T>
class ZenohPublisher : public IPublisher<T> {
public:
    explicit ZenohPublisher(const std::string& topic)
        : topic_(topic) {
        z_session_t s = zenoh_global::getSession();
        // 声明 key expression
        z_view_keyexpr_t ke_view;
        if (z_view_keyexpr_from_string(&ke_view, topic_.c_str()) != Z_OK) {
            throw std::runtime_error("ZenohPublisher: invalid key expression '" + topic_ + "'");
        }
        // 持久化 keyexpr（可选，但可避免每次 publish 都重新解析）
        z_owned_keyexpr_t ke;
        z_declare_keyexpr(s, &ke, z_view_keyexpr_loan(&ke_view));
        if (!z_keyexpr_check(&ke)) {
            throw std::runtime_error("ZenohPublisher: failed to declare keyexpr '" + topic_ + "'");
        }
        keyexpr_ = ke;
    }

    ~ZenohPublisher() {
        z_drop(z_move(keyexpr_));
    }

    bool publish(const T& msg) override {
        try {
            std::string payload = vision::rpc::serialize(msg);
            z_session_t s = zenoh_global::getSession();

            z_put_options_t opts = z_put_options_default();
            opts.encoding = z_encoding(Z_ENCODING_PREFIX_EMPTY, nullptr);

            if (z_put(s, z_keyexpr_loan(&keyexpr_),
                      reinterpret_cast<const uint8_t*>(payload.data()),
                      payload.size(), &opts) != Z_OK) {
                LOG_ERROR("Zenoh publish error on '%s'", topic_.c_str());
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("Zenoh publish exception on '%s': %s", topic_.c_str(), e.what());
            return false;
        }
    }

    std::string getTopic() const override { return topic_; }

private:
    std::string topic_;
    z_owned_keyexpr_t keyexpr_;
};

// ========== Zenoh Subscriber ==========
template<typename T>
class ZenohSubscriber : public ISubscriber<T> {
public:
    explicit ZenohSubscriber(const std::string& topic)
        : topic_(topic), callback_(nullptr) {
        z_session_t s = zenoh_global::getSession();

        // 解析 key expression
        z_view_keyexpr_t ke_view;
        if (z_view_keyexpr_from_string(&ke_view, topic_.c_str()) != Z_OK) {
            throw std::runtime_error("ZenohSubscriber: invalid key expression '" + topic_ + "'");
        }
        z_declare_keyexpr(s, &keyexpr_,
                          z_view_keyexpr_loan(&ke_view));

        // 声明 subscriber
        z_owned_closure_sample_t closure;
        z_closure(&closure, onSample, nullptr, this);

        z_subscriber_options_t sub_opts = z_subscriber_options_default();
        if (z_declare_subscriber(s, &sub_,
                                 z_keyexpr_loan(&keyexpr_),
                                 z_move(closure), &sub_opts) != Z_OK) {
            throw std::runtime_error("ZenohSubscriber: failed to declare subscriber on '" + topic_ + "'");
        }
    }

    ~ZenohSubscriber() {
        z_undeclare_subscriber(z_move(sub_));
        z_drop(z_move(keyexpr_));
    }

    bool subscribe(typename ISubscriber<T>::Callback cb) override {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        callback_ = std::move(cb);
        return true;
    }

    std::string getTopic() const override { return topic_; }

private:
    // C 回调: z_closure_sample_callback_t
    static void onSample(const z_sample_t* sample, void* context) {
        auto* self = static_cast<ZenohSubscriber*>(context);
        if (!self || !sample) return;

        std::lock_guard<std::mutex> lock(self->cb_mutex_);
        if (!self->callback_) return;

        try {
            std::string payload(
                reinterpret_cast<const char*>(sample->payload.start),
                sample->payload.len);
            T msg = vision::rpc::deserialize<T>(payload);
            self->callback_(msg);
        } catch (const std::exception& e) {
            LOG_ERROR("Zenoh subscriber deserialize error on '%s': %s", self->topic_.c_str(), e.what());
        }
    }

    std::string topic_;
    z_owned_keyexpr_t keyexpr_;
    z_owned_subscriber_t sub_;
    typename ISubscriber<T>::Callback callback_;
    std::mutex cb_mutex_;
};

// ========== Zenoh Service ==========
// 使用 Zenoh Queryable (服务端) + z_get (客户端) 实现
//
// 服务端: 声明 queryable，接收查询并返回响应
// 客户端: 使用 z_get 发送查询，通过 reply channel 同步等待响应

template<typename Request, typename Response>
class ZenohService : public IService<Request, Response> {
    using Handler = typename IService<Request, Response>::Handler;

public:
    explicit ZenohService(const std::string& name)
        : name_(name) {}

    ~ZenohService() {
        stop_serve();
    }

    void preconnect() override {
        // Zenoh 不需要预连接
    }

    // ---- 服务端 ----
    bool serve(const std::string& endpoint,
               Handler handler) override {
        (void)endpoint;
        stop_serve();

        handler_ = std::move(handler);
        z_session_t s = zenoh_global::getSession();

        // 构建 key expression: {service_name}/**
        std::string ke_str = name_;
        z_view_keyexpr_t ke_view;
        if (z_view_keyexpr_from_string(&ke_view, ke_str.c_str()) != Z_OK) {
            LOG_ERROR("ZenohService: invalid keyexpr '%s'", ke_str.c_str());
            return false;
        }
        z_declare_keyexpr(s, &serve_keyexpr_,
                          z_view_keyexpr_loan(&ke_view));

        // 声明 queryable
        z_owned_closure_query_t closure;
        z_closure(&closure, onQuery, nullptr, this);

        z_queryable_options_t q_opts = z_queryable_options_default();
        q_opts.complete = true;  // 每次查询完成后自动标记 complete

        if (z_declare_queryable(s, &queryable_,
                                z_keyexpr_loan(&serve_keyexpr_),
                                z_move(closure), &q_opts) != Z_OK) {
            LOG_ERROR("ZenohService: failed to declare queryable on '%s'", ke_str.c_str());
            return false;
        }

        return true;
    }

    // ---- 客户端 ----
    Response call(const std::string& endpoint, const Request& req) override {
        z_session_t s = zenoh_global::getSession();

        // 构建查询的 key expression
        std::string ke_str = name_ + "/" + endpoint;
        z_view_keyexpr_t ke_view;
        if (z_view_keyexpr_from_string(&ke_view, ke_str.c_str()) != Z_OK) {
            throw std::runtime_error("ZenohService call: invalid keyexpr '" + ke_str + "'");
        }
        z_owned_keyexpr_t ke;
        z_declare_keyexpr(s, &ke,
                          z_view_keyexpr_loan(&ke_view));

        // 序列化请求作为查询 payload
        std::string req_payload = vision::rpc::serialize(req);

        // 创建 reply channel 用于同步接收响应
        z_owned_reply_channel_t channel;
        z_reply_channel_new(&channel);

        z_get_options_t opts = z_get_options_default();
        opts.target = Z_QUERY_TARGET_ALL;
        opts.timeout_ms = 5000;
        // 将序列化的请求作为查询 payload 发送
        z_owned_bytes_t payload_bytes;
        z_bytes_from_buf(&payload_bytes,
                         reinterpret_cast<const uint8_t*>(req_payload.data()),
                         req_payload.size());
        opts.payload = z_bytes_move(&payload_bytes);

        z_get(s, z_keyexpr_loan(&ke),
              "", z_move(channel.send), &opts);

        // 阻塞接收第一个响应
        z_owned_reply_t reply;
        if (!z_reply_channel_recv(&channel, &reply)) {
            z_drop(z_move(channel));
            z_drop(z_move(ke));
            throw std::runtime_error("ZenohService call timeout: " + ke_str);
        }
        z_drop(z_move(channel));

        Response resp;
        if (z_reply_is_ok(&reply)) {
            const z_sample_t* sample = z_reply_ok(&reply);
            if (sample && sample->payload.len > 0) {
                std::string payload(
                    reinterpret_cast<const char*>(sample->payload.start),
                    sample->payload.len);
                resp = vision::rpc::deserialize<Response>(payload);
            }
        } else {
            z_drop(z_move(reply));
            z_drop(z_move(ke));
            throw std::runtime_error("ZenohService call error: " + ke_str);
        }

        z_drop(z_move(reply));
        z_drop(z_move(ke));
        return resp;
    }

private:
    void stop_serve() {
        handler_ = nullptr;
        if (z_queryable_check(&queryable_)) {
            z_undeclare_queryable(z_move(queryable_));
        }
        if (z_keyexpr_check(&serve_keyexpr_)) {
            z_drop(z_move(serve_keyexpr_));
        }
    }

    // C 回调: z_closure_query_callback_t
    static void onQuery(const z_query_t* query, void* context) {
        auto* self = static_cast<ZenohService*>(context);
        if (!self || !query || !self->handler_) {
            // 无 handler 时返回空响应
            if (query) {
                z_query_reply_options_t opts = z_query_reply_options_default();
                z_query_reply(query, z_query_keyexpr(query),
                              nullptr, 0, &opts);
            }
            return;
        }

        Response resp;
        try {
            // 获取查询 payload
            z_bytes_t payload = z_query_payload(query);
            std::string req_str(
                reinterpret_cast<const char*>(payload.start),
                payload.len);
            Request req = vision::rpc::deserialize<Request>(req_str);

            // 调用业务 handler
            resp = self->handler_(req);
        } catch (const std::exception& e) {
            LOG_ERROR("ZenohService query handler error: %s", e.what());
            resp = Response();
        }

        // 序列化并发送响应
        std::string resp_str = vision::rpc::serialize(resp);
        z_query_reply_options_t opts = z_query_reply_options_default();
        z_query_reply(query, z_query_keyexpr(query),
                      reinterpret_cast<const uint8_t*>(resp_str.data()),
                      resp_str.size(), &opts);
    }

    std::string name_;
    Handler handler_;

    // 服务端
    z_owned_keyexpr_t serve_keyexpr_;
    z_owned_queryable_t queryable_;
};

#else  // !HAS_ZENOH — 存根实现

template<typename T>
class ZenohPublisher : public IPublisher<T> {
public:
    ZenohPublisher(const std::string& topic) : topic_(topic) {}
    bool publish(const T&) override {
        throw std::runtime_error("Zenoh backend not available (compile with -DHAS_ZENOH)");
    }
    std::string getTopic() const override { return topic_; }
private:
    std::string topic_;
};

template<typename T>
class ZenohSubscriber : public ISubscriber<T> {
public:
    ZenohSubscriber(const std::string& topic) : topic_(topic) {}
    bool subscribe(typename ISubscriber<T>::Callback) override {
        throw std::runtime_error("Zenoh backend not available (compile with -DHAS_ZENOH)");
    }
    std::string getTopic() const override { return topic_; }
private:
    std::string topic_;
};

template<typename Request, typename Response>
class ZenohService : public IService<Request, Response> {
public:
    ZenohService(const std::string& name) : name_(name) {}
    void preconnect() override {
        // Zenoh 不需要预连接
    }
    bool serve(const std::string&,
               typename IService<Request, Response>::Handler) override {
        throw std::runtime_error("Zenoh backend not available (compile with -DHAS_ZENOH)");
    }
    Response call(const std::string&, const Request&) override {
        throw std::runtime_error("Zenoh backend not available (compile with -DHAS_ZENOH)");
    }
private:
    std::string name_;
};

#endif  // HAS_ZENOH
