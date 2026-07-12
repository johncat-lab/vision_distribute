#include <opencv2/opencv.hpp>
#include <iostream>

int main() {
    std::string image_path = "./test.png";
    cv::Mat image = cv::imread(image_path, cv::IMREAD_GRAYSCALE);
    
    // 模板位置 (从之前的匹配结果)
    cv::Rect tmpl_region(2692, 1060, 108, 107);
    cv::Mat tmpl_area = image(tmpl_region);
    
    // 计算模板区域的亮度统计
    cv::Scalar mean, stddev;
    cv::meanStdDev(tmpl_area, mean, stddev);
    std::cout << "模板区域亮度: 均值=" << mean[0] << ", 标准差=" << stddev[0] << std::endl;
    
    // 计算背景区域的亮度统计 (选择远离目标的区域)
    cv::Rect bg_region(0, 0, 500, 500);
    cv::Mat bg_area = image(bg_region);
    cv::meanStdDev(bg_area, mean, stddev);
    std::cout << "背景区域(0,0)亮度: 均值=" << mean[0] << ", 标准差=" << stddev[0] << std::endl;
    
    // 另一个背景区域
    cv::Rect bg_region2(3500, 2500, 500, 400);
    cv::Mat bg_area2 = image(bg_region2);
    cv::meanStdDev(bg_area2, mean, stddev);
    std::cout << "背景区域(3500,2500)亮度: 均值=" << mean[0] << ", 标准差=" << stddev[0] << std::endl;
    
    // 测试不同阈值
    std::cout << "\n不同阈值的前景占比:" << std::endl;
    for (int thresh = 50; thresh <= 200; thresh += 20) {
        cv::Mat mask;
        cv::threshold(image, mask, thresh, 255, cv::THRESH_BINARY);
        int non_zero = cv::countNonZero(mask);
        double ratio = non_zero * 100.0 / (image.cols * image.rows);
        std::cout << "  v_threshold=" << thresh << ": 前景占比=" << ratio << "%";
        
        // 在该阈值下找轮廓
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(mask.clone(), contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        
        // 检查是否有接近模板位置的轮廓
        bool found_target = false;
        for (const auto& contour : contours) {
            double area = cv::contourArea(contour);
            if (area < 2000 || area > 500000) continue;
            cv::Rect bbox = cv::boundingRect(contour);
            cv::Point center(bbox.x + bbox.width/2, bbox.y + bbox.height/2);
            double dist = std::sqrt(std::pow(center.x - 2746, 2) + std::pow(center.y - 1113.5, 2));
            if (dist < 200) {
                found_target = true;
                break;
            }
        }
        std::cout << ", 找到目标区域: " << (found_target ? "是" : "否") << std::endl;
    }
    
    return 0;
}