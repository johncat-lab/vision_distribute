/**
 * @brief ROS2 Service API 接口调用测试
 * 通过 NodeFactory → Ros2Service 路径调用远端服务，
 * 验证 preconnect() + call() 路径是否正常工作。
 */
#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "rpc/service.h"
#include "logger/logger.h"
#include <iostream>
#include <thread>
#include <chrono>

#ifdef HAS_ROS2
// ros2_backend.h 已通过 rpc/node_factory.h 间接包含
#include "vision_interfaces/srv/comm_get_status.hpp"
#include "vision_interfaces/srv/detector_get_config.hpp"
#include "vision_interfaces/srv/detector_on_off.hpp"
#endif

static int passed = 0, failed = 0;
#define TEST(name) std::cout << "\n=== " << name << " ===" << std::endl
#define PASS(msg)  { std::cout << "  PASS: " << msg << std::endl; passed++; }
#define FAIL(msg)  { std::cout << "  FAIL: " << msg << std::endl; failed++; }

int main() {
#ifdef HAS_ROS2
    std::cout << "========== ROS2 Service API 接口调用测试 ==========" << std::endl;

    // 创建客户端 NodeFactory (模拟 inspector 的行为)
    NodeConfig config;
    config.transport = TransportType::ROS2;
    config.node_name = "test_api_client_" + std::to_string(getpid());
    config.service_timeout_ms = 10000;  // 10s
    config.service_wait_ms = 5000;
    config.service_max_retries = 3;

    auto factory = std::make_unique<NodeFactory>(config);

    // === Test 1: comm/get_status ===
    TEST("comm/get_status (via API)");
    {
        auto svc = factory->createService<ServiceRequest, ServiceResponse>("comm");
        if (!svc) {
            FAIL("createService returned null");
        } else {
            // 注册原生端点 (模拟 inspector 的做法)
            auto* rs = dynamic_cast<Ros2Service<ServiceRequest, ServiceResponse>*>(svc.get());
            if (rs) {
                rs->registerNativeEndpoint<vision_interfaces::srv::CommGetStatus>(
                    "get_status",
                    [](auto) -> ServiceRequest { return ServiceRequest{}; },
                    [](const ServiceResponse&, auto) {},
                    [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CommGetStatus::Request> {
                        return std::make_shared<vision_interfaces::srv::CommGetStatus::Request>();
                    },
                    [](auto resp) -> ServiceResponse {
                        ServiceResponse sr;
                        sr.set_success(resp->success);
                        sr.set_data(resp->status_data);
                        return sr;
                    });
            }

            // 关键：调用 preconnect()（修复后的行为）
            svc->preconnect();

            // 发起调用
            ServiceRequest req;
            req.set_endpoint("get_status");
            try {
                ServiceResponse resp = svc->call("get_status", req);
                PASS("success=" << resp.success() << " data=" << resp.data().substr(0, 60));
            } catch (const std::exception& e) {
                FAIL("exception: " << e.what());
            }
        }
    }

    // === Test 2: detector/get_config ===
    TEST("detector/get_config (via API)");
    {
        auto svc = factory->createService<ServiceRequest, ServiceResponse>("detector");
        if (!svc) {
            FAIL("createService returned null");
        } else {
            auto* rs = dynamic_cast<Ros2Service<ServiceRequest, ServiceResponse>*>(svc.get());
            if (rs) {
                rs->registerNativeEndpoint<vision_interfaces::srv::DetectorGetConfig>(
                    "get_config",
                    [](auto) -> ServiceRequest { return ServiceRequest{}; },
                    [](const ServiceResponse&, auto) {},
                    [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::DetectorGetConfig::Request> {
                        return std::make_shared<vision_interfaces::srv::DetectorGetConfig::Request>();
                    },
                    [](auto resp) -> ServiceResponse {
                        ServiceResponse sr;
                        sr.set_success(resp->ready);
                        sr.set_data(resp->config_data);
                        return sr;
                    });
            }
            svc->preconnect();

            ServiceRequest req;
            req.set_endpoint("get_config");
            try {
                ServiceResponse resp = svc->call("get_config", req);
                PASS("ready=" << resp.success() << " config=" << resp.data().substr(0, 60));
            } catch (const std::exception& e) {
                FAIL("exception: " << e.what());
            }
        }
    }

    // === Test 3: detector/onoff ===
    TEST("detector/onoff (via API)");
    {
        auto svc = factory->createService<ServiceRequest, ServiceResponse>("detector");
        if (!svc) {
            FAIL("createService returned null");
        } else {
            auto* rs = dynamic_cast<Ros2Service<ServiceRequest, ServiceResponse>*>(svc.get());
            if (rs) {
                rs->registerNativeEndpoint<vision_interfaces::srv::DetectorOnOff>(
                    "onoff",
                    [](auto) -> ServiceRequest { return ServiceRequest{}; },
                    [](const ServiceResponse&, auto) {},
                    [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorOnOff::Request> {
                        auto req = std::make_shared<vision_interfaces::srv::DetectorOnOff::Request>();
                        req->command = sr.payload();
                        return req;
                    },
                    [](auto resp) -> ServiceResponse {
                        ServiceResponse sr;
                        sr.set_success(resp->success);
                        sr.set_data(resp->message);
                        return sr;
                    });
            }

            // 重复调用 2 次验证无死锁
            for (int i = 0; i < 2; i++) {
                ServiceRequest req;
                req.set_endpoint("onoff");
                req.set_payload(i == 0 ? "on" : "off");
                try {
                    ServiceResponse resp = svc->call("onoff", req);
                    PASS("cmd=" << (i == 0 ? "on" : "off")
                         << " success=" << resp.success()
                         << " msg=" << resp.data());
                } catch (const std::exception& e) {
                    FAIL("cmd=" << (i == 0 ? "on" : "off")
                         << " exception: " << e.what());
                }
            }
        }
    }

    // === 结果汇总 ===
    std::cout << "\n========================================" << std::endl;
    std::cout << "  通过: " << passed << "  失败: " << failed << std::endl;
    std::cout << "========================================" << std::endl;

#else
    std::cout << "ROS2 未启用，跳过测试" << std::endl;
#endif
    return failed > 0 ? 1 : 0;
}
