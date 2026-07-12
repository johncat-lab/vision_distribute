#include <opencv2/opencv.hpp>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

int main() {
    std::string image_path = "./test.png";
    std::string template_path = "./template/template_gray.png";
    
    if (!fs::exists(image_path)) {
        std::cerr << "图像文件不存在: " << image_path << std::endl;
        return 1;
    }
    if (!fs::exists(template_path)) {
        std::cerr << "模板文件不存在: " << template_path << std::endl;
        return 1;
    }
    
    cv::Mat image = cv::imread(image_path, cv::IMREAD_GRAYSCALE);
    cv::Mat tmpl = cv::imread(template_path, cv::IMREAD_GRAYSCALE);
    
    std::cout << "图像尺寸: " << image.cols << "x" << image.rows << std::endl;
    std::cout << "模板尺寸: " << tmpl.cols << "x" << tmpl.rows << std::endl;
    
    if (image.empty() || tmpl.empty()) {
        std::cerr << "无法读取图像或模板" << std::endl;
        return 1;
    }
    
    // 直接在整个图像上进行模板匹配
    cv::Mat result;
    cv::matchTemplate(image, tmpl, result, cv::TM_CCOEFF_NORMED);
    
    double max_val, min_val;
    cv::Point max_loc, min_loc;
    cv::minMaxLoc(result, &min_val, &max_val, &min_loc, &max_loc);
    
    std::cout << "匹配分数: " << max_val << std::endl;
    std::cout << "最佳匹配位置: (" << max_loc.x << ", " << max_loc.y << ")" << std::endl;
    
    // 计算目标中心
    cv::Point2d center(max_loc.x + tmpl.cols / 2.0, max_loc.y + tmpl.rows / 2.0);
    std::cout << "目标中心坐标: (" << center.x << ", " << center.y << ")" << std::endl;
    
    // 绘制结果
    cv::Mat display;
    cv::cvtColor(image, display, cv::COLOR_GRAY2BGR);
    cv::rectangle(display, max_loc, cv::Point(max_loc.x + tmpl.cols, max_loc.y + tmpl.rows), 
                  cv::Scalar(0, 255, 0), 2);
    cv::circle(display, cv::Point(center.x, center.y), 5, cv::Scalar(0, 0, 255), -1);
    
    cv::imwrite("test_template_match_result.png", display);
    std::cout << "结果已保存到: test_template_match_result.png" << std::endl;
    
    return 0;
}