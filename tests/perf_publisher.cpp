/**
 * 性能测试 Publisher 节点
 * 独立进程运行，发布帧数据
 */

#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <cstring>

constexpr int FRAME_WIDTH = 640;
constexpr int FRAME_HEIGHT = 480;
constexpr int FRAME_SIZE = FRAME_WIDTH * FRAME_HEIGHT;

void fill_frame_data(std::vector<uint8_t>& data) {
    data.resize(FRAME_SIZE);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (auto& byte : data) {
        byte = static_cast<uint8_t>(dis(gen));
    }
}

int main(int argc, char* argv[]) {
    std::string transport = "zmq";
    std::string topic = "perf/frame";
    int fps = 30;
    int duration_sec = 10;
    
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ros2") == 0) transport = "ros2";
        else if (strcmp(argv[i], "--zmq") == 0) transport = "zmq";
        else if (strcmp(argv[i], "--topic") == 0 && i+1 < argc) topic = argv[++i];
        else if (strcmp(argv[i], "--fps") == 0 && i+1 < argc) fps = std::stoi(argv[++i]);
        else if (strcmp(argv[i], "--duration") == 0 && i+1 < argc) duration_sec = std::stoi(argv[++i]);
    }
    
    vision::LogConfig log_config;
    log_config.level = vision::LogLevel::DEBUG;
    log_config.output = vision::LogOutput::CONSOLE;
    log_config.node_name = "perf_publisher";
    vision::Logger::init(log_config);
    
    NodeConfig config;
    config.transport = (transport == "ros2") ? TransportType::ROS2 : TransportType::ZEROMQ;
    config.node_name = "perf_publisher";
    config.base_port = 7777;
    
    NodeFactory factory(config);
    auto pub = factory.createPublisher<FrameMsg>(topic);
    
    std::vector<uint8_t> image_data;
    fill_frame_data(image_data);
    
    LOG_INFO("Publisher 启动 - 传输层: %s, Topic: %s, FPS: %d, 时长: %ds", 
             transport.c_str(), topic.c_str(), fps, duration_sec);
    
    const int interval_us = 1000000 / fps;
    const int total_frames = fps * duration_sec;
    
    for (int i = 0; i < total_frames; ++i) {
        auto frame_start = std::chrono::high_resolution_clock::now();
        int64_t timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            frame_start.time_since_epoch()).count();
        
        FrameMsg msg;
        msg.camera_id = 0;
        msg.timestamp = timestamp_us / 1000;
        msg.width = FRAME_WIDTH;
        msg.height = FRAME_HEIGHT;
        msg.pixel_type = 1;
        msg.frame_num = i;
        msg.data = image_data;
        
        pub->publish(msg);
        
        LOG_DEBUG("[SEND] frame=%d, timestamp=%lld us", i, timestamp_us);
        
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - frame_start).count();
        
        if (elapsed < interval_us) {
            std::this_thread::sleep_for(std::chrono::microseconds(interval_us - elapsed));
        }
    }
    
    LOG_INFO("Publisher 完成 - 共发送 %d 帧", total_frames);
    
    return 0;
}