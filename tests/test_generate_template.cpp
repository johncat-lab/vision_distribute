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
#include <fstream>

std::atomic<bool> running{true};
std::atomic<bool> got_frame{false};
cv::Mat latest_frame;
std::string latest_format;
int latest_width = 0;
int latest_height = 0;

void generateTemplate(const cv::Mat& image, const cv::Rect& roi, const std::string& output_dir) {
    cv::Mat template_color = image(roi).clone();
    cv::Mat template_gray;
    cv::cvtColor(template_color, template_gray, cv::COLOR_BGR2GRAY);

    std::filesystem::create_directories(output_dir);

    cv::imwrite(output_dir + "/template.png", template_color);
    cv::imwrite(output_dir + "/template_gray.png", template_gray);

    cv::Mat mask;
    cv::threshold(template_gray, mask, 128, 255, cv::THRESH_BINARY_INV);
    cv::imwrite(output_dir + "/template_mask.png", mask);

    std::ofstream info_file(output_dir + "/template_info.txt");
    if (info_file.is_open()) {
        info_file << "# Template Info (v2)\n";
        info_file << "template_width=" << template_color.cols << "\n";
        info_file << "template_height=" << template_color.rows << "\n";
        info_file << "roi_x=" << roi.x << "\n";
        info_file << "roi_y=" << roi.y << "\n";
        info_file << "source_width=" << image.cols << "\n";
        info_file << "source_height=" << image.rows << "\n";
        info_file << "initial_angle_offset=0\n";
        info_file << "rotated_rect_cx=" << (roi.x + roi.width / 2.0) << "\n";
        info_file << "rotated_rect_cy=" << (roi.y + roi.height / 2.0) << "\n";
        info_file << "rotated_rect_w=" << template_color.cols << "\n";
        info_file << "rotated_rect_h=" << template_color.rows << "\n";
        info_file << "rotated_rect_angle=0\n";
        info_file << "template_mode=gray\n";
        info_file.close();
    }

    std::cout << "[成功] 模板已生成到: " << output_dir << std::endl;
    std::cout << "  - template.png: " << template_color.cols << "x" << template_color.rows << std::endl;
    std::cout << "  - template_gray.png: " << template_gray.cols << "x" << template_gray.rows << std::endl;
    std::cout << "  - template_mask.png: " << mask.cols << "x" << mask.rows << std::endl;
    std::cout << "  - template_info.txt: 模板元信息" << std::endl;
    std::cout << "  - ROI: (" << roi.x << ", " << roi.y << ") - (" 
              << roi.x + roi.width << ", " << roi.y + roi.height << ")" << std::endl;
}

int main() {
    std::cout << "========== 模板生成工具 ==========" << std::endl;
    std::cout << "步骤1: 订阅相机图像，按 ESC 保存当前帧" << std::endl;
    std::cout << "步骤2: 在图像窗口中拖拽选择ROI区域" << std::endl;
    std::cout << "步骤3: 生成模板文件" << std::endl;

    NodeConfig config;
    config.transport = TransportType::ZEROMQ;
    config.node_name = "template_generator";
    config.base_port = 15550;

    auto factory = std::make_unique<NodeFactory>(config);

    auto subscriber = factory->createSubscriber<FrameMsg>("vision/frame");
    if (!subscriber) {
        std::cerr << "[错误] 无法创建 Subscriber" << std::endl;
        return 1;
    }

    subscriber->subscribe([&](const FrameMsg& msg) {
        uint32_t width = msg.width();
        uint32_t height = msg.height();
        uint32_t pixel_type = msg.pixel_type();
        const std::string& data_str = msg.data();

        cv::Mat image;
        cv::Mat display_image;

        if (pixel_type == 1) {
            image = cv::Mat(height, width, CV_8UC1,
                            const_cast<char*>(data_str.data()), width);
            cv::cvtColor(image, display_image, cv::COLOR_GRAY2BGR);
            latest_format = "Mono8";
        } else if (pixel_type == 4) {
            image = cv::Mat(height, width, CV_8UC3,
                            const_cast<char*>(data_str.data()), width * 3);
            display_image = image.clone();
            latest_format = "BGR8";
        } else {
            std::cerr << "[警告] 未知像素格式: " << pixel_type << ", 尝试以灰度图加载" << std::endl;
            image = cv::Mat(height, width, CV_8UC1,
                            const_cast<char*>(data_str.data()), width);
            cv::cvtColor(image, display_image, cv::COLOR_GRAY2BGR);
            latest_format = "Unknown";
        }

        std::lock_guard<std::mutex> lock(*(new std::mutex()));
        latest_frame = display_image.clone();
        latest_width = width;
        latest_height = height;
        got_frame.store(true);

        std::cout << "[帧] " << width << "x" << height << " " << latest_format << std::endl;
    });

    std::thread display_thread([&]() {
        while (running.load()) {
            if (got_frame.load() && !latest_frame.empty()) {
                cv::Mat display;
                cv::resize(latest_frame, display, cv::Size(1280, 720));
                cv::imshow("Camera View - Press ESC to capture", display);
                
                int key = cv::waitKey(30);
                if (key == 27) {
                    running.store(false);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });

    display_thread.join();
    cv::destroyAllWindows();

    if (latest_frame.empty()) {
        std::cerr << "[错误] 未收到相机图像" << std::endl;
        return 1;
    }

    std::cout << "\n[步骤2] 在图像窗口中拖拽选择ROI区域..." << std::endl;
    cv::Mat display;
    cv::resize(latest_frame, display, cv::Size(1280, 720));
    cv::namedWindow("Select ROI", cv::WINDOW_NORMAL);
    cv::resizeWindow("Select ROI", 1280, 720);

    cv::Rect roi = cv::selectROI("Select ROI", display);
    cv::destroyAllWindows();

    if (roi.width <= 0 || roi.height <= 0) {
        std::cerr << "[错误] 无效的ROI选择" << std::endl;
        return 1;
    }

    double scale_x = static_cast<double>(latest_width) / display.cols;
    double scale_y = static_cast<double>(latest_height) / display.rows;
    cv::Rect actual_roi(
        static_cast<int>(roi.x * scale_x),
        static_cast<int>(roi.y * scale_y),
        static_cast<int>(roi.width * scale_x),
        static_cast<int>(roi.height * scale_y)
    );

    std::string output_dir = "./template_new";
    generateTemplate(latest_frame, actual_roi, output_dir);

    std::cout << "\n[提示] 将生成的模板复制到 detector_node/template/ 目录即可使用" << std::endl;
    std::cout << "cp -r " << output_dir << "/* build/install/bins/detector_node/template/" << std::endl;

    return 0;
}