#include "ninepoint_detector.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <iostream>

NinepointDetector::NinepointDetector() = default;

bool NinepointDetector::init() {
    ready_ = true;
    std::cout << "[NinepointDetector] 初始化完成" << std::endl;
    return true;
}

bool NinepointDetector::isReady() const {
    return ready_;
}

void NinepointDetector::setCalibConfig(const CalibConfig& cfg) {
    calib_cfg_ = cfg;
}

void NinepointDetector::setRoiYCenter(int y_center) {
    roi_y_center_ = y_center;
}

void NinepointDetector::setRoiYMargin(int y_margin) {
    roi_y_margin_ = y_margin;
}

bool NinepointDetector::loadTemplate(const std::string& image_path, double match_threshold) {
    cv::Mat tmpl = cv::imread(image_path, cv::IMREAD_COLOR);
    if (tmpl.empty()) {
        std::cerr << "[NinepointDetector] 无法加载模板图像: " << image_path << std::endl;
        return false;
    }

    template_img_ = tmpl;
    template_mask_ = cv::Mat();
    match_threshold_ = match_threshold;
    use_template_ = true;

    std::cout << "[NinepointDetector] 模板已加载: " << image_path
              << " (" << tmpl.cols << "x" << tmpl.rows << ")"
              << " 阈值: " << match_threshold << std::endl;
    return true;
}

void NinepointDetector::clearTemplate() {
    use_template_ = false;
    template_img_ = cv::Mat();
    template_mask_ = cv::Mat();
}

const std::vector<CircleInfo>& NinepointDetector::getLastCircles() const {
    return last_circles_;
}

cv::Mat NinepointDetector::frameToMat(const Frame& frame) {
    size_t expected_mono = static_cast<size_t>(frame.width) * frame.height;

    if (frame.data.size() == expected_mono) {
        cv::Mat gray(frame.height, frame.width, CV_8UC1,
                     const_cast<unsigned char*>(frame.data.data()));
        cv::Mat bgr;
        cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
        return bgr;
    }

    size_t expected_rgb = expected_mono * 3;
    if (frame.data.size() == expected_rgb) {
        if (frame.pixelType == 0x02180015) {
            return cv::Mat(frame.height, frame.width, CV_8UC3,
                           const_cast<unsigned char*>(frame.data.data())).clone();
        } else {
            cv::Mat src(frame.height, frame.width, CV_8UC3,
                        const_cast<unsigned char*>(frame.data.data()));
            cv::Mat bgr;
            cv::cvtColor(src, bgr, cv::COLOR_RGB2BGR);
            return bgr;
        }
    }

    return cv::Mat();
}

ObjectInfoList NinepointDetector::detect(const Frame& frame) {
    ObjectInfoList result;
    last_circles_.clear();

    cv::Mat image = frameToMat(frame);
    if (image.empty()) {
        return result;
    }

    if (use_template_ && !template_img_.empty()) {
        double min_dist = std::min(template_img_.cols, template_img_.rows) * 0.5;
        last_circles_ = detectCirclesByTemplate(image, template_img_, template_mask_,
                                                 match_threshold_, min_dist);
    } else {
        CalibConfig cfg = calib_cfg_;
        if (roi_y_center_ >= 0) cfg.roi_y_center = roi_y_center_;
        if (roi_y_margin_ >= 0) cfg.roi_y_margin = roi_y_margin_;
        last_circles_ = detectFourCircles(image, cfg);
    }

    for (const auto& ci : last_circles_) {
        result.add(ObjectInfo::Builder()
                       .setX(ci.center.x)
                       .setY(ci.center.y)
                       .setAngle(ci.radius)
                       .setType(0)
                       .build());
    }

    return result;
}

bool NinepointDetector::saveAnnotated(const Frame& frame, const std::string& path) {
    cv::Mat image = frameToMat(frame);
    if (image.empty()) {
        return false;
    }

    drawCircles(image, last_circles_);

    std::vector<int> params;
#ifdef IMWRITE_BMP_COMPRESSION
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".bmp") {
        params = {cv::IMWRITE_BMP_COMPRESSION, 0};
    }
#endif

    return cv::imwrite(path, image, params);
}

void NinepointDetector::drawAnnotations(cv::Mat& image) {
    drawCircles(image, last_circles_);
}
