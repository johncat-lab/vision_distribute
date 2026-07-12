#include <opencv2/opencv.hpp>
#include <iostream>

int main() {
    std::string image_path = "./test.png";
    cv::Mat image = cv::imread(image_path, cv::IMREAD_GRAYSCALE);
    
    std::cout << "图像尺寸: " << image.cols << "x" << image.rows << std::endl;
    
    // 统计亮度分布
    int hist[256] = {0};
    for (int y = 0; y < image.rows; y++) {
        for (int x = 0; x < image.cols; x++) {
            hist[image.at<uchar>(y, x)]++;
        }
    }
    
    std::cout << "亮度分布 (部分):" << std::endl;
    for (int i = 0; i < 256; i += 10) {
        std::cout << "  " << i << ": " << hist[i] << std::endl;
    }
    
    // value 模式分割 (v_threshold=50)
    cv::Mat mask;
    cv::threshold(image, mask, 50, 255, cv::THRESH_BINARY);
    
    int non_zero = cv::countNonZero(mask);
    std::cout << "v_threshold=50 时前景像素数: " << non_zero << std::endl;
    std::cout << "占比: " << (non_zero * 100.0 / (image.cols * image.rows)) << "%" << std::endl;
    
    // 找轮廓
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    std::cout << "找到轮廓数: " << contours.size() << std::endl;
    
    for (size_t i = 0; i < contours.size(); i++) {
        double area = cv::contourArea(contours[i]);
        if (area > 2000) {
            cv::Rect bbox = cv::boundingRect(contours[i]);
            std::cout << "  轮廓 " << i << ": 面积=" << area << ", 位置=(" << bbox.x << "," << bbox.y << "), 尺寸=" << bbox.width << "x" << bbox.height << std::endl;
        }
    }
    
    // 显示原始图像的一部分
    cv::Rect roi(2650, 1020, 150, 150);
    cv::Mat crop = image(roi);
    cv::imwrite("test_segmentation_crop.png", crop);
    std::cout << "裁剪区域已保存到: test_segmentation_crop.png" << std::endl;
    
    return 0;
}