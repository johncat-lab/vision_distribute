/**
 * ZeroMQ 通信验证测试
 *
 * 测试内容:
 *   1. Pub/Sub 模式 — 通过 ZMQ 发布/订阅 DetectionMsg
 *   2. Service 模式 — 通过 ZMQ REQ/REP 调用 ServiceRequest/ServiceResponse
 *
 * 编译: build.sh 后通过 cmake --build 构建 test_zmq_comm 目标
 * 运行: ./test_zmq_comm
 */

#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <cassert>
#include <cstdlib>

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
void test_zmq_pubsub() {
    std::cout << "\n=== 测试1: ZeroMQ Pub/Sub 通信 ===" << std::endl;

    NodeConfig config;
    config.transport = TransportType::ZEROMQ;
    config.base_port = 6550;  // 使用不同端口避免冲突
    NodeFactory factory(config);

    // 创建 Publisher 和 Subscriber
    auto pub = factory.createPublisher<DetectionMsg>("test/detection");
    auto sub = factory.createSubscriber<DetectionMsg>("test/detection");

    std::atomic<bool> received{false};
    DetectionMsg received_msg;

    // 订阅消息
    sub->subscribe([&](const DetectionMsg& msg) {
        received_msg = msg;
        received.store(true);
    });

    // 等待订阅者连接就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 发布消息
    DetectionMsg send_msg;
    send_msg.set_frame_num(42);
    send_msg.set_timestamp(1234567890);
    send_msg.set_object_count(3);
    send_msg.set_protocol_string("TA,100,200,30,40,0.95;TA,300,400,50,60,0.88;NG");

    bool pub_ok = pub->publish(send_msg);
    TEST_ASSERT(pub_ok, "发布消息失败");

    // 等待接收
    int retry = 0;
    while (!received.load() && retry < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ++retry;
    }

    TEST_ASSERT(received.load(), "未收到订阅消息 (超时5秒)");
    TEST_ASSERT(received_msg.frame_num() == 42, "frame_num 不匹配");
    TEST_ASSERT(received_msg.timestamp() == 1234567890, "timestamp 不匹配");
    TEST_ASSERT(received_msg.object_count() == 3, "object_count 不匹配");
    TEST_ASSERT(received_msg.protocol_string() == "TA,100,200,30,40,0.95;TA,300,400,50,60,0.88;NG",
                "protocol_string 不匹配");

    TEST_PASS("ZeroMQ Pub/Sub 通信验证通过");
}

// ========== 测试2: Service 请求/响应 ==========
void test_zmq_service() {
    std::cout << "\n=== 测试2: ZeroMQ Service 请求/响应 ===" << std::endl;

    NodeConfig config;
    config.transport = TransportType::ZEROMQ;
    config.base_port = 6650;  // 使用不同端口避免冲突
    NodeFactory factory(config);

    auto svc = factory.createService<ServiceRequest, ServiceResponse>("test_service");

    // 注册服务处理函数
    bool serve_ok = svc->serve("get_status", [](const ServiceRequest& req) -> ServiceResponse {
        ServiceResponse resp;
        resp.set_success(true);
        resp.set_data("OK|" + req.endpoint() + "|" + req.payload());
        return resp;
    });
    TEST_ASSERT(serve_ok, "注册服务失败");

    // 等待服务端绑定就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 调用服务
    ServiceRequest req;
    req.set_endpoint("get_status");
    req.set_payload("camera_0");

    ServiceResponse resp = svc->call("get_status", req);
    TEST_ASSERT(resp.success(), "服务响应 success=false");
    TEST_ASSERT(resp.data() == "OK|get_status|camera_0",
                "服务响应数据不匹配: " + resp.data());

    TEST_PASS("ZeroMQ Service 请求/响应验证通过");
}

// ========== 测试3: 序列化/反序列化一致性 ==========
void test_serialization() {
    std::cout << "\n=== 测试3: 消息序列化/反序列化一致性 ===" << std::endl;

    // DetectionMsg
    {
        DetectionMsg original;
        original.set_frame_num(999);
        original.set_timestamp(9876543210);
        original.set_object_count(5);
        original.set_protocol_string("TA,10,20,5,5,0.99");

        std::string data;
        original.SerializeToString(&data);
        DetectionMsg restored;
        restored.ParseFromString(data);

        TEST_ASSERT(restored.frame_num() == 999, "DetectionMsg frame_num 序列化不匹配");
        TEST_ASSERT(restored.timestamp() == 9876543210, "DetectionMsg timestamp 序列化不匹配");
        TEST_ASSERT(restored.object_count() == 5, "DetectionMsg object_count 序列化不匹配");
        TEST_ASSERT(restored.protocol_string() == "TA,10,20,5,5,0.99",
                    "DetectionMsg protocol_string 序列化不匹配");
    }

    // ServiceRequest / ServiceResponse
    {
        ServiceRequest req;
        req.set_endpoint("capture");
        req.set_payload("cam_id=1&exposure=100");

        std::string data;
        req.SerializeToString(&data);
        ServiceRequest restored;
        restored.ParseFromString(data);

        TEST_ASSERT(restored.endpoint() == "capture", "ServiceRequest endpoint 序列化不匹配");
        TEST_ASSERT(restored.payload() == "cam_id=1&exposure=100",
                    "ServiceRequest payload 序列化不匹配");
    }

    {
        ServiceResponse resp;
        resp.set_success(true);
        resp.set_data("result_ok");

        std::string data;
        resp.SerializeToString(&data);
        ServiceResponse restored;
        restored.ParseFromString(data);

        TEST_ASSERT(restored.success() == true, "ServiceResponse success 序列化不匹配");
        TEST_ASSERT(restored.data() == "result_ok", "ServiceResponse data 序列化不匹配");
    }

    // FrameMsg
    {
        FrameMsg frame;
        frame.set_camera_id(7);
        frame.set_timestamp(111222333);
        frame.set_width(640);
        frame.set_height(480);
        frame.set_pixel_type(1);
        frame.set_frame_num(100);
        frame.set_exposure_time(50.5f);
        frame.set_gain(2.3f);
        frame.set_data(std::string("\xAA\xBB\xCC\xDD", 4));

        std::string data;
        frame.SerializeToString(&data);
        FrameMsg restored;
        restored.ParseFromString(data);

        TEST_ASSERT(restored.camera_id() == 7, "FrameMsg camera_id 序列化不匹配");
        TEST_ASSERT(restored.timestamp() == 111222333, "FrameMsg timestamp 序列化不匹配");
        TEST_ASSERT(restored.width() == 640, "FrameMsg width 序列化不匹配");
        TEST_ASSERT(restored.height() == 480, "FrameMsg height 序列化不匹配");
        TEST_ASSERT(restored.pixel_type() == 1, "FrameMsg pixel_type 序列化不匹配");
        TEST_ASSERT(restored.frame_num() == 100, "FrameMsg frame_num 序列化不匹配");
        TEST_ASSERT(restored.data().size() == 4, "FrameMsg data 长度不匹配");
        TEST_ASSERT(static_cast<uint8_t>(restored.data()[0]) == 0xAA, "FrameMsg data[0] 不匹配");
    }

    TEST_PASS("消息序列化/反序列化一致性验证通过");
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "  ZeroMQ 通信验证测试" << std::endl;
    std::cout << "==========================================" << std::endl;

    test_zmq_pubsub();
    test_zmq_service();
    test_serialization();

    std::cout << "\n==========================================" << std::endl;
    std::cout << "  测试结果: " << passed << " 通过, " << failed << " 失败" << std::endl;
    std::cout << "==========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
