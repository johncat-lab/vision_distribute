/**
 * ROS2 通信验证测试
 *
 * 测试内容:
 *   1. Pub/Sub 模式 — 通过 ROS2 topic 发布/订阅 DetectionMsg
 *   2. Service 单进程模式 — server 与 client 在同一节点内互调
 *   3. Service 跨进程模式 — 仅作 client，连接外部运行的 camera_node
 *      （需先启动 camera_node，再运行此测试；若 camera 未运行则跳过）
 *
 * 编译: build.sh 自动 source /opt/ros/humble/setup.bash
 * 运行: ./test_ros2_comm
 */

#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <cassert>

#ifndef HAS_ROS2
int main() {
    std::cerr << "ROS2 不可用，请使用 HAS_ROS2 编译。" << std::endl;
    return 1;
}
#else

static int passed = 0;
static int failed = 0;

#define TEST_ASSERT(expr, msg) \
    do { \
        if (!(expr)) { \
            std::cerr << "  FAIL: " << msg << " (at line " << __LINE__ << ")" << std::endl; \
            ++failed; \
            return; \
        } \
    } while(0)

#define TEST_PASS(msg) \
    do { \
        std::cout << "  PASS: " << msg << std::endl; \
        ++passed; \
    } while(0)

// ========== 测试1: Pub/Sub 通信 ==========
void test_ros2_pubsub() {
    std::cout << "\n=== 测试1: ROS2 Pub/Sub 通信 ===" << std::endl;

    NodeConfig config;
    config.transport = TransportType::ROS2;
    config.node_name = "test_pubsub_node";
    NodeFactory factory(config);

    auto pub = factory.createPublisher<DetectionMsg>("test/detection_ros2");
    auto sub = factory.createSubscriber<DetectionMsg>("test/detection_ros2");

    std::atomic<bool> received{false};
    DetectionMsg received_msg;

    sub->subscribe([&](const DetectionMsg& msg) {
        received_msg = msg;
        received.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    DetectionMsg send_msg;
    send_msg.frame_num = 88;
    send_msg.timestamp = 1700000000;
    send_msg.object_count = 2;
    send_msg.protocol_string = "TA,150,250,35,45,0.92;NG";

    bool pub_ok = pub->publish(send_msg);
    TEST_ASSERT(pub_ok, "发布消息失败");

    int retry = 0;
    while (!received.load() && retry < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ++retry;
    }

    TEST_ASSERT(received.load(), "未收到订阅消息 (超时5秒)");
    TEST_ASSERT(received_msg.frame_num == 88, "frame_num 不匹配");
    TEST_ASSERT(received_msg.protocol_string == "TA,150,250,35,45,0.92;NG",
                "protocol_string 不匹配");

    TEST_PASS("ROS2 Pub/Sub 通信验证通过");
    // factory 析构时调用 shutdown()，g_initialized 重置为 false，下一个测试可重新 init
}

// ========== 测试2: Service 单进程模式 ==========
void test_ros2_service_inproc() {
    std::cout << "\n=== 测试2: ROS2 Service 单进程模式 ===" << std::endl;

    NodeConfig config;
    config.transport = TransportType::ROS2;
    config.node_name = "test_service_node";
    NodeFactory factory(config);

    auto svc = factory.createService<ServiceRequest, ServiceResponse>("test_inproc_svc");

    svc->serve("echo", [](const ServiceRequest& req) -> ServiceResponse {
        ServiceResponse resp;
        resp.success = true;
        resp.data = "ACK|" + req.endpoint + "|" + req.payload;
        return resp;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ServiceRequest req;
    req.endpoint = "echo";
    req.payload = "hello";

    ServiceResponse resp = svc->call("echo", req);
    TEST_ASSERT(resp.success, "服务响应 success=false");
    TEST_ASSERT(resp.data == "ACK|echo|hello",
                "服务响应数据不匹配: " + resp.data);

    TEST_PASS("ROS2 Service 单进程模式验证通过");
}

// ========== 测试3: Service 跨进程 client-only 模式 ==========
// 连接外部运行的 camera_node（service 名 "camera"，endpoint "get_config"）
void test_ros2_service_crossproc() {
    std::cout << "\n=== 测试3: ROS2 Service 跨进程 client 模式 ===" << std::endl;

    NodeConfig config;
    config.transport = TransportType::ROS2;
    config.node_name = "test_client_node";
    NodeFactory factory(config);

    auto svc = factory.createService<ServiceRequest, ServiceResponse>("camera");

    // 直接调用 call()，若 camera_node 未运行则超时抛异常，catch 住跳过
    ServiceRequest req;
    req.endpoint = "get_config";
    req.payload  = "";

    try {
        ServiceResponse resp = svc->call("get_config", req);
        std::cerr << "  [跨进程] 收到响应: success=" << resp.success
                  << " data.size()=" << resp.data.size() << std::endl;
        TEST_ASSERT(resp.data.size() > 0, "跨进程响应数据为空");
        TEST_PASS("ROS2 Service 跨进程 client 模式验证通过");
    } catch (const std::exception& e) {
        std::cout << "  SKIP: camera_node 未运行，跳过跨进程测试 (" << e.what() << ")" << std::endl;
    }
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "  ROS2 通信验证测试" << std::endl;
    std::cout << "==========================================" << std::endl;

    test_ros2_pubsub();
    test_ros2_service_inproc();
    test_ros2_service_crossproc();

    std::cout << "\n==========================================" << std::endl;
    std::cout << "  测试结果: " << passed << " 通过, " << failed << " 失败" << std::endl;
    std::cout << "==========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}

#endif  // HAS_ROS2
