#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include <opencv2/opencv.hpp>
#include <chrono>
#include <fstream>
#include <vector>
#include <atomic>
#include <numeric>

int main(int argc, char** argv) {
    std::string transport = "zmq";
    std::string mode = "publisher";
    std::string image_path = "";
    int duration = 5;
    int fps = 30;

    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--ros2") transport = "ros2";
        else if (std::string(argv[i]) == "--zmq") transport = "zmq";
        else if (std::string(argv[i]) == "--publisher") mode = "publisher";
        else if (std::string(argv[i]) == "--subscriber") mode = "subscriber";
        else if (std::string(argv[i]) == "--image" && i+1 < argc) image_path = argv[++i];
        else if (std::string(argv[i]) == "--duration" && i+1 < argc) duration = std::stoi(argv[++i]);
        else if (std::string(argv[i]) == "--fps" && i+1 < argc) fps = std::stoi(argv[++i]);
    }

    vision::LogConfig log_config;
    log_config.level = vision::LogLevel::DEBUG;
    log_config.output = vision::LogOutput::CONSOLE;
    log_config.node_name = "perf_image_" + mode;
    vision::Logger::init(log_config);

    LOG_INFO("图像性能测试启动 - 传输层: %s, 模式: %s, FPS: %d, 时长: %ds", 
             transport.c_str(), mode.c_str(), fps, duration);

    NodeConfig config;
    config.transport = (transport == "ros2") ? TransportType::ROS2 : TransportType::ZEROMQ;
    config.node_name = "perf_image_" + mode;
    config.base_port = 7778;

    if (mode == "publisher") {
        cv::Mat test_image;
        if (!image_path.empty()) {
            test_image = cv::imread(image_path, cv::IMREAD_COLOR);
        }
        if (test_image.empty()) {
            LOG_INFO("使用生成的测试图像 (640x480 RGB)");
            test_image = cv::Mat(480, 640, CV_8UC3, cv::Scalar(128, 128, 128));
            cv::rectangle(test_image, cv::Rect(100, 100, 200, 150), cv::Scalar(255, 0, 0), 2);
        }

        size_t image_size = test_image.total() * test_image.elemSize();
        LOG_INFO("图像尺寸: %dx%d, 数据大小: %zu bytes", 
                 test_image.cols, test_image.rows, image_size);

        std::vector<uint8_t> image_data(test_image.data, test_image.data + image_size);

        NodeFactory factory(config);
        auto pub = factory.createPublisher<FrameMsg>("perf/image");

        const int interval_us = 1000000 / fps;
        const int total_frames = fps * duration;

        for (int i = 0; i < total_frames; ++i) {
            auto frame_start = std::chrono::high_resolution_clock::now();
            int64_t timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                frame_start.time_since_epoch()).count();

            FrameMsg msg;
            msg.camera_id = 0;
            msg.timestamp = timestamp_us / 1000;
            msg.width = test_image.cols;
            msg.height = test_image.rows;
            msg.pixel_type = 3;
            msg.frame_num = i;
            msg.data = image_data;

            pub->publish(msg);

            LOG_DEBUG("[SEND] frame=%d, timestamp=%lld us, size=%zu bytes", 
                      i, timestamp_us, image_size);

            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - frame_start).count();

            if (elapsed < interval_us) {
                std::this_thread::sleep_for(std::chrono::microseconds(interval_us - elapsed));
            }
        }

        LOG_INFO("发布完成: %d 帧", total_frames);
    } else {
        std::vector<double> latencies;
        std::atomic<int> received_count{0};
        auto start_time = std::chrono::high_resolution_clock::now();

        NodeFactory factory(config);
        auto sub = factory.createSubscriber<FrameMsg>("perf/image");
        sub->subscribe([&](const FrameMsg& msg) {
            auto now = std::chrono::high_resolution_clock::now();
            auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count();
            double latency_ms = (now_us - msg.timestamp * 1000) / 1000.0;
            latencies.push_back(latency_ms);
            int count = received_count++;

            if (count % 10 == 0) {
                LOG_DEBUG("[RECV] frame=%d, latency=%.2f ms, size=%zu bytes", 
                          msg.frame_num, latency_ms, msg.data.size());
            }
        });

        std::this_thread::sleep_for(std::chrono::seconds(duration));

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start_time).count();

        if (!latencies.empty()) {
            double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
            double avg = sum / latencies.size();
            std::sort(latencies.begin(), latencies.end());
            double p99 = latencies[latencies.size() * 0.99];
            double min_lat = latencies.front();
            double max_lat = latencies.back();

            LOG_INFO("=== 图像测试结果 ===");
            LOG_INFO("接收帧数: %d", received_count.load());
            LOG_INFO("测试时长: %lld ms", elapsed);
            LOG_INFO("平均帧率: %.1f FPS", received_count.load() * 1000.0 / elapsed);
            LOG_INFO("平均延迟: %.2f ms", avg);
            LOG_INFO("最小延迟: %.2f ms", min_lat);
            LOG_INFO("最大延迟: %.2f ms", max_lat);
            LOG_INFO("P99 延迟: %.2f ms", p99);

            std::ofstream ofs((transport + "_image_latency.csv").c_str());
            ofs << "frame,latency_ms,size_bytes" << std::endl;
            for (size_t i = 0; i < latencies.size(); i++) {
                ofs << i << "," << latencies[i] << "," << std::endl;
            }
        } else {
            LOG_WARN("未接收到任何图像数据");
        }
    }

    return 0;
}