#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>

// 测试连接池优化效果
int main() {
    std::cout << "========== ZMQ 连接池优化测试 ==========" << std::endl;
    
    // 创建服务端
    NodeConfig server_config;
    server_config.transport = TransportType::ZEROMQ;
    server_config.node_name = "test_service_server";
    server_config.base_port = 15560;
    server_config.zmq_service_workers = 4;
    
    auto server_factory = std::make_unique<NodeFactory>(server_config);
    auto service = server_factory->createService<ServiceRequest, ServiceResponse>("test_service");
    
    // 注册快速 handler (1ms)
    service->serve("fast_endpoint", [](const ServiceRequest& req) -> ServiceResponse {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ServiceResponse resp;
        resp.set_success(true);
        resp.set_data("OK");
        return resp;
    });
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // 创建客户端
    NodeConfig client_config;
    client_config.transport = TransportType::ZEROMQ;
    client_config.node_name = "test_service_client";
    client_config.base_port = 15560;
    
    auto client_factory = std::make_unique<NodeFactory>(client_config);
    auto client = client_factory->createService<ServiceRequest, ServiceResponse>("test_service");
    
    // 初始化客户端连接池
    client->preconnect();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // 等待连接建立
    
    // 测试1: 首次调用 (包含连接建立)
    std::cout << "\n[测试1] 首次调用 (包含连接池初始化)..." << std::endl;
    auto start1 = std::chrono::steady_clock::now();
    
    ServiceRequest req1;
    req1.set_payload("test1");
    auto resp1 = client->call("fast_endpoint", req1);
    
    auto end1 = std::chrono::steady_clock::now();
    auto duration1 = std::chrono::duration_cast<std::chrono::microseconds>(end1 - start1).count();
    std::cout << "[测试1] 首次调用耗时: " << duration1 << " μs" << std::endl;
    
    // 测试2: 连续调用 100 次 (复用连接)
    std::cout << "\n[测试2] 连续调用 100 次 (连接复用)..." << std::endl;
    auto start2 = std::chrono::steady_clock::now();
    
    for (int i = 0; i < 100; ++i) {
        ServiceRequest req;
        req.set_payload("test_" + std::to_string(i));
        auto resp = client->call("fast_endpoint", req);
    }
    
    auto end2 = std::chrono::steady_clock::now();
    auto duration2 = std::chrono::duration_cast<std::chrono::microseconds>(end2 - start2).count();
    auto avg2 = duration2 / 100;
    std::cout << "[测试2] 总耗时: " << duration2 << " μs" << std::endl;
    std::cout << "[测试2] 平均耗时: " << avg2 << " μs/次" << std::endl;
    std::cout << "[测试2] 吞吐量: " << (1000000.0 / avg2) << " req/s" << std::endl;
    
    // 测试3: 并发调用 (测试连接池轮询)
    std::cout << "\n[测试3] 并发调用 50 次 (复用同一连接池)..." << std::endl;
    auto start3 = std::chrono::steady_clock::now();
    
    std::atomic<int> completed{0};
    std::vector<std::thread> threads;
    
    // 复用同一个客户端实例 (共享连接池)
    for (int i = 0; i < 50; ++i) {
        threads.emplace_back([&, i]() {
            ServiceRequest req;
            req.set_payload("concurrent_" + std::to_string(i));
            auto resp = client->call("fast_endpoint", req);  // 使用同一个 client
            completed.fetch_add(1);
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end3 = std::chrono::steady_clock::now();
    auto duration3 = std::chrono::duration_cast<std::chrono::microseconds>(end3 - start3).count();
    auto avg3 = duration3 / 50;
    std::cout << "[测试3] 总耗时: " << duration3 << " μs" << std::endl;
    std::cout << "[测试3] 平均耗时: " << avg3 << " μs/次" << std::endl;
    std::cout << "[测试3] 吞吐量: " << (50000000.0 / duration3) << " req/s" << std::endl;
    
    // 总结
    std::cout << "\n========== 测试结果 ==========" << std::endl;
    std::cout << "首次调用: " << duration1 << " μs (含连接建立)" << std::endl;
    std::cout << "连续调用: " << avg2 << " μs/次 (连接复用)" << std::endl;
    std::cout << "并发调用: " << avg3 << " μs/次 (连接池轮询)" << std::endl;
    
    if (duration1 > 0 && avg2 > 0) {
        std::cout << "连接复用加速: " << (duration1 / avg2) << "x" << std::endl;
    }
    
    std::cout << "\n========== 测试完成 ==========" << std::endl;
    return 0;
}
