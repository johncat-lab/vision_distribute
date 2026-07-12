#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>

// 测试 Router/Dealer 模式的并发性能
int main() {
    std::cout << "========== ZMQ Router/Dealer 并发测试 ==========" << std::endl;
    
    // 初始化日志
    // (假设已有日志初始化，这里省略)
    
    // 创建服务端 NodeFactory
    NodeConfig server_config;
    server_config.transport = TransportType::ZEROMQ;
    server_config.node_name = "test_service_server";
    server_config.base_port = 15550;
    server_config.zmq_service_workers = 4;  // 4 个 Worker 线程
    
    auto server_factory = std::make_unique<NodeFactory>(server_config);
    
    // 创建 Service
    auto service = server_factory->createService<ServiceRequest, ServiceResponse>("test_service");
    
    // 注册一个耗时的 handler (模拟 100ms 处理时间)
    service->serve("slow_endpoint", [](const ServiceRequest& req) -> ServiceResponse {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ServiceResponse resp;
        resp.set_success(true);
        resp.set_data("Processed: " + req.payload());
        return resp;
    });
    
    std::cout << "[Server] Service 已启动，等待 1 秒..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // 创建客户端 NodeFactory
    NodeConfig client_config;
    client_config.transport = TransportType::ZEROMQ;
    client_config.node_name = "test_service_client";
    client_config.base_port = 15550;
    
    auto client_factory = std::make_unique<NodeFactory>(client_config);
    
    // 测试1: 串行调用 (10 次)
    std::cout << "\n[测试1] 串行调用 10 次 (每次 100ms)..." << std::endl;
    auto start1 = std::chrono::steady_clock::now();
    
    auto client1 = client_factory->createService<ServiceRequest, ServiceResponse>("test_service");
    for (int i = 0; i < 10; ++i) {
        ServiceRequest req;
        req.set_payload("request_" + std::to_string(i));
        auto resp = client1->call("slow_endpoint", req);
        std::cout << "  请求 " << i << ": " << resp.data() << std::endl;
    }
    
    auto end1 = std::chrono::steady_clock::now();
    auto duration1 = std::chrono::duration_cast<std::chrono::milliseconds>(end1 - start1).count();
    std::cout << "[测试1] 总耗时: " << duration1 << " ms (预期 ~1000ms)" << std::endl;
    
    // 测试2: 并发调用 (10 个线程同时调用)
    std::cout << "\n[测试2] 并发调用 10 次 (4 Worker 线程)..." << std::endl;
    auto start2 = std::chrono::steady_clock::now();
    
    std::atomic<int> completed{0};
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&, i]() {
            auto client = client_factory->createService<ServiceRequest, ServiceResponse>("test_service");
            ServiceRequest req;
            req.set_payload("concurrent_" + std::to_string(i));
            auto resp = client->call("slow_endpoint", req);
            completed.fetch_add(1);
            std::cout << "  线程 " << i << " 完成: " << resp.data() << std::endl;
        });
    }
    
    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }
    
    auto end2 = std::chrono::steady_clock::now();
    auto duration2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
    std::cout << "[测试2] 总耗时: " << duration2 << " ms (预期 ~300ms)" << std::endl;
    std::cout << "[测试2] 吞吐量: " << (10000.0 / duration2) << " req/s" << std::endl;
    
    // 总结
    std::cout << "\n========== 测试结果 ==========" << std::endl;
    std::cout << "串行调用: " << duration1 << " ms" << std::endl;
    std::cout << "并发调用: " << duration2 << " ms" << std::endl;
    std::cout << "性能提升: " << (duration1 / duration2) << "x" << std::endl;
    
    std::cout << "\n========== 测试完成 ==========" << std::endl;
    return 0;
}
