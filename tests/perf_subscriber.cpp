/**
 * 性能测试 Subscriber 节点
 * 独立进程运行，接收帧数据并记录延迟
 */

#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <mutex>
#include <fstream>
#include <iomanip>

struct LatencyStats {
    std::vector<double> latencies_ms;
    std::mutex mutex;
    int frame_count = 0;
    int lost_count = 0;
    int64_t first_timestamp = 0;
    int64_t last_timestamp = 0;
};

static bool g_running = true;

void sigint_handler(int) {
    g_running = false;
    LOG_INFO("收到停止信号");
}

int main(int argc, char* argv[]) {
    std::string transport = "zmq";
    std::string topic = "perf/frame";
    std::string output_file = "latency_results.csv";
    
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ros2") == 0) transport = "ros2";
        else if (strcmp(argv[i], "--zmq") == 0) transport = "zmq";
        else if (strcmp(argv[i], "--topic") == 0 && i+1 < argc) topic = argv[++i];
        else if (strcmp(argv[i], "--output") == 0 && i+1 < argc) output_file = argv[++i];
    }
    
    vision::LogConfig log_config;
    log_config.level = vision::LogLevel::DEBUG;
    log_config.output = vision::LogOutput::CONSOLE;
    log_config.node_name = "perf_subscriber";
    vision::Logger::init(log_config);
    
    NodeConfig config;
    config.transport = (transport == "ros2") ? TransportType::ROS2 : TransportType::ZEROMQ;
    config.node_name = "perf_subscriber";
    config.base_port = 7777;
    
    NodeFactory factory(config);
    auto sub = factory.createSubscriber<FrameMsg>(topic);
    
    LatencyStats stats;
    
    LOG_INFO("Subscriber 启动 - 传输层: %s, Topic: %s", transport.c_str(), topic.c_str());
    
    std::signal(SIGINT, sigint_handler);
    
    sub->subscribe([&](const FrameMsg& msg) {
        auto recv_time = std::chrono::high_resolution_clock::now();
        int64_t recv_timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            recv_time.time_since_epoch()).count();
        
        double latency_ms = (recv_timestamp_us - msg.timestamp * 1000) / 1000.0;
        
        std::lock_guard<std::mutex> lock(stats.mutex);
        
        if (stats.first_timestamp == 0) {
            stats.first_timestamp = msg.timestamp;
        }
        stats.last_timestamp = msg.timestamp;
        stats.frame_count++;
        stats.latencies_ms.push_back(latency_ms);
        
        LOG_DEBUG("[RECV] frame=%d, latency=%.2f ms, timestamp=%lld ms", 
                  msg.frame_num, latency_ms, msg.timestamp);
    });
    
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    {
        std::lock_guard<std::mutex> lock(stats.mutex);
        
        if (stats.latencies_ms.empty()) {
            LOG_ERROR("未收到任何帧");
            return 1;
        }
        
        std::sort(stats.latencies_ms.begin(), stats.latencies_ms.end());
        
        double avg_latency = std::accumulate(stats.latencies_ms.begin(), stats.latencies_ms.end(), 0.0) 
                          / stats.latencies_ms.size();
        double min_latency = stats.latencies_ms.front();
        double max_latency = stats.latencies_ms.back();
        size_t p99_idx = static_cast<size_t>(stats.latencies_ms.size() * 0.99);
        double p99_latency = p99_idx < stats.latencies_ms.size() 
                          ? stats.latencies_ms[p99_idx] 
                          : max_latency;
        
        double duration_ms = (stats.last_timestamp - stats.first_timestamp);
        double throughput = duration_ms > 0 ? (stats.frame_count / (duration_ms / 1000.0)) : 0;
        
        LOG_INFO("========================================");
        LOG_INFO("性能测试结果 - %s", transport.c_str());
        LOG_INFO("========================================");
        LOG_INFO("接收帧数: %d", stats.frame_count);
        LOG_INFO("平均延迟: %.2f ms", avg_latency);
        LOG_INFO("最小延迟: %.2f ms", min_latency);
        LOG_INFO("最大延迟: %.2f ms", max_latency);
        LOG_INFO("P99 延迟: %.2f ms", p99_latency);
        LOG_INFO("吞吐量: %.2f msg/s", throughput);
        LOG_INFO("========================================");
        
        std::ofstream ofs(output_file);
        if (ofs.is_open()) {
            ofs << "frame_num,latency_ms\n";
            for (size_t i = 0; i < stats.latencies_ms.size(); ++i) {
                ofs << i << "," << std::fixed << std::setprecision(4) << stats.latencies_ms[i] << "\n";
            }
            LOG_INFO("延迟数据已保存到: %s", output_file.c_str());
        } else {
            LOG_ERROR("无法打开输出文件: %s", output_file.c_str());
        }
    }
    
    return 0;
}