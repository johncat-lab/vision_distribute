#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include "detection/detection_msg.pb.h"
#include "detection/annotation_msg.pb.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <string>
#include <filesystem>

int main() {
    std::cout << "========== 检测结果测试程序 ==========" << std::endl;
    std::cout << "订阅 camera + detector 输出，绘制检测结果并保存" << std::endl;
    std::cout << "按 Ctrl+C 退出..." << std::endl;

    NodeConfig config;
    config.transport = TransportType::ZEROMQ;
    config.node_name = "test_detection_result";
    config.base_port = 15550;

    auto factory = std::make_unique<NodeFactory>(config);

    std::atomic<bool> running{true};
    std::atomic<int> received_frames{0};
    std::atomic<bool> saved_once{false};

    std::mutex image_mutex;
    cv::Mat latest_image;
    uint32_t latest_pixel_type = 0;

    auto frame_subscriber = factory->createSubscriber<FrameMsg>("vision/frame");
    if (!frame_subscriber) {
        std::cerr << "[错误] 无法创建 FrameMsg Subscriber" << std::endl;
        return 1;
    }

    auto detection_subscriber = factory->createSubscriber<vision::messages::detection::DetectionMsg>("vision/detection");
    if (!detection_subscriber) {
        std::cerr << "[错误] 无法创建 DetectionMsg Subscriber" << std::endl;
        return 1;
    }

    auto annotation_subscriber = factory->createSubscriber<vision::messages::detection::AnnotationMsg>("vision/annotation");
    if (!annotation_subscriber) {
        std::cerr << "[错误] 无法创建 AnnotationMsg Subscriber" << std::endl;
        return 1;
    }

    frame_subscriber->subscribe([&](const FrameMsg& msg) {
        int frame_num = received_frames.fetch_add(1);
        
        uint32_t width = msg.width();
        uint32_t height = msg.height();
        uint32_t pixel_type = msg.pixel_type();
        const std::string& data_str = msg.data();

        if (width == 0 || height == 0 || data_str.empty()) {
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
            default:
                image = cv::Mat(height, width, CV_8UC1,
                                const_cast<char*>(data_str.data()), width);
                display_image = image.clone();
                break;
        }

        if (display_image.channels() == 1) {
            cv::cvtColor(display_image, display_image, cv::COLOR_GRAY2BGR);
        }

        std::lock_guard<std::mutex> lock(image_mutex);
        latest_image = display_image.clone();
        latest_pixel_type = pixel_type;

        std::cout << "\r[帧 " << frame_num << "] 图像: " << width << "x" << height
                  << " 等待检测结果..." << std::flush;
    });

    detection_subscriber->subscribe([&](const vision::messages::detection::DetectionMsg& msg) {
        std::lock_guard<std::mutex> lock(image_mutex);
        
        if (latest_image.empty()) {
            std::cout << "[警告] 收到检测结果但没有图像" << std::endl;
            return;
        }

        cv::Mat result_image = latest_image.clone();
        
        std::string protocol = msg.protocol_string();
        int object_count = msg.object_count();
        
        std::cout << "\r[检测] " << protocol 
                  << " (目标数: " << object_count << ")" << std::endl;

        if (!saved_once.exchange(true)) {
            std::filesystem::path cwd = std::filesystem::current_path();
            std::string filename = (cwd / "detection_raw.png").string();
            bool success = cv::imwrite(filename, result_image);
            if (success) {
                std::cout << "[成功] 原始图像已保存到: " << filename << std::endl;
            }
        }
    });

    annotation_subscriber->subscribe([&](const vision::messages::detection::AnnotationMsg& msg) {
        std::lock_guard<std::mutex> lock(image_mutex);
        
        if (latest_image.empty()) {
            std::cout << "[警告] 收到标注信息但没有图像" << std::endl;
            return;
        }

        cv::Mat result_image = latest_image.clone();
        
        int num_objects = msg.objects_size();
        
        for (int i = 0; i < num_objects; ++i) {
            const auto& obj = msg.objects(i);
            
            double x = obj.x();
            double y = obj.y();
            double angle = obj.angle();
            double score = obj.score();
            int type = obj.type();

            cv::circle(result_image, cv::Point2d(x, y), 5, cv::Scalar(0, 255, 0), -1);
            
            cv::circle(result_image, cv::Point2d(x, y), 20, cv::Scalar(0, 255, 255), 2);
            
            double arrow_len = 30;
            double angle_rad = angle * CV_PI / 180.0;
            cv::Point2d arrow_end(x + cos(angle_rad) * arrow_len, y + sin(angle_rad) * arrow_len);
            cv::arrowedLine(result_image, cv::Point2d(x, y), arrow_end, cv::Scalar(255, 0, 0), 2);
            
            std::string label = cv::format("Obj%d (%.2f,%.2f) %.1fdeg", 
                                           type, x, y, angle);
            cv::putText(result_image, label, 
                        cv::Point(x + 10, y - 10),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, 
                        cv::Scalar(0, 0, 255), 1);
        }

        std::string filename = std::filesystem::current_path().string() + "/detection_result.png";
        bool success = cv::imwrite(filename, result_image);
        
        if (success) {
            std::cout << "[成功] 检测结果图像已保存到: " << filename << std::endl;
            std::cout << "[提示] 可以使用 'open " << filename << "' 查看" << std::endl;
        } else {
            std::cerr << "[错误] 保存检测结果图像失败" << std::endl;
        }

        std::cout << "[标注] 绘制了 " << num_objects << " 个目标" << std::endl;
    });

    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n========== 测试结束 ==========" << std::endl;
    std::cout << "共接收 " << received_frames.load() << " 帧" << std::endl;
    return 0;
}