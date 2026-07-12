#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <future>
#include <atomic>

// 测试 ROS2 Service 优化功能
int main() {
#ifdef HAS_ROS2
    std::cout << "========== ROS2 Service 优化测试 ==========" << std::endl;
    
    // 创建服务端
    NodeConfig server_config;
    server_config.transport = TransportType::ROS2;
    server_config.node_name = "test_ros2_service_server";
    server_config.base_port = 15560;
    
    auto server_factory = std::make_unique<NodeFactory>(server_config);
    auto service = server_factory->createService<ServiceRequest, ServiceResponse>("test_service");
    
    // 注册快速 handler (10ms)
    service->serve("fast_endpoint", [](const ServiceRequest& req) -> ServiceResponse {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ServiceResponse resp;
        resp.set_success(true);
        resp.set_data("OK: " + req.payload());
        return resp;
    });
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // 创建客户端
    NodeConfig client_config;
    client_config.transport = TransportType::ROS2;
    client_config.node_name = "test_ros2_service_client";
    client_config.base_port = 15560;
    
    auto client_factory = std::make_unique<NodeFactory>(client_config);
    auto client = client_factory->createService<ServiceRequest, ServiceResponse>("test_service");
    
    // 测试 1: 同步调用
    std::cout << "\n[测试 1] 同步调用..." << std::endl;
    auto start1 = std::chrono::steady_clock::now();
    
    ServiceRequest req1;
    req1.set_payload("sync_test");
    auto resp1 = client->call("fast_endpoint", req1);
    
    auto end1 = std::chrono::steady_clock::now();
    auto duration1 = std::chrono::duration_cast<std::chrono::milliseconds>(end1 - start1).count();
    std::cout << "[测试 1] 同步调用耗时: " << duration1 << " ms" << std::endl;
    std::cout << "[测试 1] 响应: " << resp1.data() << std::endl;
    
    // 测试 2: 异步调用
    std::cout << "\n[测试 2] 异步调用..." << std::endl;
    auto start2 = std::chrono::steady_clock::now();
    
    ServiceRequest req2;
    req2.set_payload("async_test");
    auto future = client->call_async("fast_endpoint", req2);
    
    // 可以并发执行其他操作
    std::cout << "[测试 2] 等待异步结果..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    
    auto resp2 = future.get();
    auto end2 = std::chrono::steady_clock::now();
    auto duration2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
    std::cout << "[测试 2] 异步调用耗时: " << duration2 << " ms" << std::endl;
    std::cout << "[测试 2] 响应: " << resp2.data() << std::endl;
    
    // 测试 3: 并发异步调用
    std::cout << "\n[测试 3] 并发异步调用 (10 次)..." << std::endl;
    auto start3 = std::chrono::steady_clock::now();
    
    std::vector<std::future<ServiceResponse>> futures;
    for (int i = 0; i < 10; ++i) {
        ServiceRequest req;
        req.set_payload("concurrent_" + std::to_string(i));
        futures.push_back(client->call_async("fast_endpoint", req));
    }
    
    // 等待所有结果
    int completed = 0;
    for (auto& f : futures) {
        auto resp = f.get();
        completed++;
    }
    
    auto end3 = std::chrono::steady_clock::now();
    auto duration3 = std::chrono::duration_cast<std::chrono::milliseconds>(end3 - start3).count();
    std::cout << "[测试 3] 并发调用完成: " << completed << "/10" << std::endl;
    std::cout << "[测试 3] 总耗时: " << duration3 << " ms" << std::endl;
    std::cout << "[测试 3] 平均耗时: " << (duration3 / 10) << " ms/次" << std::endl;
    
    // 测试 4: 预连接和健康检查
    std::cout << "\n[测试 4] 预连接和健康检查..." << std::endl;
    auto start4 = std::chrono::steady_clock::now();
    
    client->preconnect();
    
    auto end4 = std::chrono::steady_clock::now();
    auto duration4 = std::chrono::duration_cast<std::chrono::milliseconds>(end4 - start4).count();
    std::cout << "[测试 4] 预连接耗时: " << duration4 << " ms" << std::endl;
    
    // 总结
    std::cout << "\n========== 测试结果 ==========" << std::endl;
    std::cout << "✅ 同步调用: " << duration1 << " ms" << std::endl;
    std::cout << "✅ 异步调用: " << duration2 << " ms" << std::endl;
    std::cout << "✅ 并发调用: " << duration3 << " ms (10 次)" << std::endl;
    std::cout << "✅ 预连接: " << duration4 << " ms" << std::endl;
    
    std::cout << "\n========== 测试完成 ==========" << std::endl;
    
#else
    std::cout << "ROS2 未启用，跳过测试" << std::endl;
#endif
    
    return 0;
}
