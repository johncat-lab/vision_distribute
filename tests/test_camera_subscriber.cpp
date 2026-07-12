#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <filesystem>

int main() {
    std::cout << "========== Camera Node 测试程序 ==========" << std::endl;
    std::cout << "订阅 camera_node 发布的图像消息并保存到 temp.png" << std::endl;
    std::cout << "按 Ctrl+C 退出..." << std::endl;

    NodeConfig config;
    config.transport = TransportType::ZEROMQ;
    config.node_name = "test_camera_subscriber";
    config.base_port = 15550;

    auto factory = std::make_unique<NodeFactory>(config);

    std::atomic<bool> running{true};
    std::atomic<int> received_frames{0};
    std::atomic<bool> saved_once{false};

    auto subscriber = factory->createSubscriber<FrameMsg>("vision/frame");
    if (!subscriber) {
        std::cerr << "[错误] 无法创建 Subscriber" << std::endl;
        return 1;
    }

    subscriber->subscribe([&](const FrameMsg& msg) {
        int frame_num = received_frames.fetch_add(1);

        uint32_t width = msg.width();
        uint32_t height = msg.height();
        uint32_t pixel_type = msg.pixel_type();
        const std::string& data_str = msg.data();

        std::string format_name;
        int channels = 1;
        int bytes_per_pixel = 1;

        switch (pixel_type) {
            case 1: format_name = "Mono8"; channels = 1; bytes_per_pixel = 1; break;
            case 2: format_name = "Mono16"; channels = 1; bytes_per_pixel = 2; break;
            case 3: format_name = "RGB8"; channels = 3; bytes_per_pixel = 3; break;
            case 4: format_name = "BGR8"; channels = 3; bytes_per_pixel = 3; break;
            case 5: format_name = "RGB16"; channels = 3; bytes_per_pixel = 6; break;
            case 6: format_name = "BGR16"; channels = 3; bytes_per_pixel = 6; break;
            case 7: format_name = "RGBA8"; channels = 4; bytes_per_pixel = 4; break;
            case 8: format_name = "BGRA8"; channels = 4; bytes_per_pixel = 4; break;
            case 9: format_name = "YUV422Packed"; channels = 3; bytes_per_pixel = 2; break;
            default: 
                format_name = "Unknown(" + std::to_string(pixel_type) + ")";
                channels = 1; bytes_per_pixel = 1;
                break;
        }

        std::cout << "\n[帧 " << frame_num << "] 收到图像: "
                  << width << "x" << height
                  << ", format=" << format_name
                  << ", 数据大小=" << data_str.size() << " bytes" << std::endl;

        if (width == 0 || height == 0 || data_str.empty()) {
            std::cerr << "[警告] 图像数据无效" << std::endl;
            return;
        }

        size_t expected_size = static_cast<size_t>(width) * height * bytes_per_pixel;
        if (data_str.size() != expected_size) {
            std::cerr << "[警告] 数据大小不匹配: 期望 " << expected_size
                      << ", 实际 " << data_str.size() << std::endl;
            return;
        }

        cv::Mat image;
        cv::Mat display_image;
        
        switch (pixel_type) {
            case 1:
                image = cv::Mat(height, width, CV_8UC1,
                                const_cast<char*>(data_str.data()), width);
                display_image = image.clone();
                break;
            case 2:
                image = cv::Mat(height, width, CV_16UC1,
                                const_cast<char*>(data_str.data()), width * 2);
                image.convertTo(display_image, CV_8UC1, 1.0 / 256.0);
                break;
            case 3:
                image = cv::Mat(height, width, CV_8UC3,
                                const_cast<char*>(data_str.data()), width * 3);
                cv::cvtColor(image, display_image, cv::COLOR_RGB2BGR);
                break;
            case 4:
                image = cv::Mat(height, width, CV_8UC3,
                                const_cast<char*>(data_str.data()), width * 3);
                display_image = image.clone();
                break;
            case 7:
                image = cv::Mat(height, width, CV_8UC4,
                                const_cast<char*>(data_str.data()), width * 4);
                cv::cvtColor(image, display_image, cv::COLOR_RGBA2BGR);
                break;
            case 8:
                image = cv::Mat(height, width, CV_8UC4,
                                const_cast<char*>(data_str.data()), width * 4);
                cv::cvtColor(image, display_image, cv::COLOR_BGRA2BGR);
                break;
            case 9:
                image = cv::Mat(height, width, CV_8UC2,
                                const_cast<char*>(data_str.data()), width * 2);
                cv::cvtColor(image, display_image, cv::COLOR_YUV2BGR_YUYV);
                break;
            default:
                std::cerr << "[警告] 未知像素格式: " << pixel_type << ", 尝试以灰度图加载" << std::endl;
                image = cv::Mat(height, width, CV_8UC1,
                                const_cast<char*>(data_str.data()), width);
                display_image = image.clone();
                break;
        }

        if (!saved_once.exchange(true)) {
            std::filesystem::path cwd = std::filesystem::current_path();
            std::string filename = (cwd / "temp.png").string();
            bool success = cv::imwrite(filename, display_image);
            if (success) {
                std::cout << "[成功] 图像已保存到: " << filename << std::endl;
                std::cout << "[提示] 可以使用 'open " << filename << "' 打开" << std::endl;
            } else {
                std::cerr << "[错误] 保存图像失败" << std::endl;
            }
        }

        std::cout << "[状态] 已接收 " << received_frames.load() << " 帧" << std::endl;
    });

    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n========== 测试结束 ==========" << std::endl;
    std::cout << "共接收 " << received_frames.load() << " 帧" << std::endl;
    return 0;
}