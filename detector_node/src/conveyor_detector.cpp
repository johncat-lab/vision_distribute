#include "conveyor_detector.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <algorithm>
#include <chrono>

ConveyorDetector::ConveyorDetector(const std::string& template_dir,
                                   float match_threshold,
                                   int angle_step_fine)
    : template_dir_(template_dir)
    , match_threshold_(match_threshold)
    , angle_step_fine_(angle_step_fine) {
}

void ConveyorDetector::setRoiYCenter(int y_center) {
    roi_y_center_ = y_center;
    std::cout << "[传送带检测] ROI Y中心: " << y_center << std::endl;
}

void ConveyorDetector::setRoiYMargin(int y_margin) {
    roi_y_margin_ = y_margin;
    std::cout << "[传送带检测] ROI Y边距: " << y_margin << std::endl;
}

void ConveyorDetector::setBgRefPath(const std::string& path) {
    bg_ref_path_ = path;
    std::cout << "[传送带检测] 背景参考路径: " << path << std::endl;
}

void ConveyorDetector::setVerifyWithTemplate(bool verify) {
    verify_with_template_ = verify;
    std::cout << "[传送带检测] 模版验证: " << (verify ? "开启" : "关闭") << std::endl;
}

void ConveyorDetector::setAreaRange(double min_area, double max_area) {
    min_area_ = min_area;
    max_area_ = max_area;
    std::cout << "[传送带检测] 面积范围: [" << min_area << ", " << max_area << "]" << std::endl;
}

bool ConveyorDetector::init() {
    loadTemplateInfo();

    if (!bg_ref_path_.empty()) {
        loadBackgroundRef();
    }

    if (verify_with_template_) {
        std::string gray_path = template_dir_ + "/template_gray.png";
        template_img_ = cv::imread(gray_path, cv::IMREAD_GRAYSCALE);
        if (!template_img_.empty()) {
            use_gray_mode_ = true;
            std::cout << "[传送带检测] 灰度模版已加载: " << gray_path
                      << " (" << template_img_.cols << "x" << template_img_.rows << ")" << std::endl;
        } else {
            std::string tmpl_path = template_dir_ + "/template.png";
            template_img_ = cv::imread(tmpl_path, cv::IMREAD_GRAYSCALE);
            if (template_img_.empty()) {
                std::cerr << "[传送带检测] 无法加载模版图片: " << tmpl_path << std::endl;
                std::cerr << "[传送带检测] 将以无模版验证模式运行" << std::endl;
                verify_with_template_ = false;
            } else {
                use_gray_mode_ = true;
                std::cout << "[传送带检测] 模版已加载(灰度): " << tmpl_path
                          << " (" << template_img_.cols << "x" << template_img_.rows << ")" << std::endl;
            }
        }

        if (!template_img_.empty()) {
            std::string mask_path = template_dir_ + "/template_mask.png";
            template_mask_ = cv::imread(mask_path, cv::IMREAD_GRAYSCALE);
            if (!template_mask_.empty()) {
                cv::threshold(template_mask_, template_mask_, 128, 255, cv::THRESH_BINARY);
                cv::Scalar fg_mean = cv::mean(template_img_, template_mask_);
                template_img_.setTo(fg_mean, ~template_mask_);
                std::cout << "[传送带检测] 前景 mask 已加载, 背景已填充前景均值 (" << fg_mean << ")" << std::endl;
            }

            std::cout << "[传送带检测] 正在预计算旋转模版 (0-359°)..." << std::endl;
            rotated_templates_.resize(360);

            cv::Point2f center(template_img_.cols / 2.0f, template_img_.rows / 2.0f);
            int diag = static_cast<int>(std::ceil(
                std::sqrt(template_img_.cols * template_img_.cols +
                          template_img_.rows * template_img_.rows)));

            for (int angle = 0; angle < 360; ++angle) {
                cv::Mat rot_mat = cv::getRotationMatrix2D(center, angle, 1.0);
                rot_mat.at<double>(0, 2) += (diag - template_img_.cols) / 2.0;
                rot_mat.at<double>(1, 2) += (diag - template_img_.rows) / 2.0;

                cv::Mat rotated;
                cv::warpAffine(template_img_, rotated, rot_mat, cv::Size(diag, diag),
                               cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0));
                rotated_templates_[angle] = rotated;
            }

            std::cout << "[传送带检测] 旋转模版预计算完成 (360个, 尺寸 "
                      << diag << "x" << diag << ")" << std::endl;
        }
    }

    morph_kernel_ = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(7, 7));
    morph_kernel_small_ = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));

    ready_ = true;

    std::cout << "[传送带检测] 初始化完成" << std::endl;
    std::cout << "[传送带检测] 模版目录: " << template_dir_ << std::endl;
    std::cout << "[传送带检测] 匹配阈值: " << match_threshold_ << std::endl;
    std::cout << "[传送带检测] 差分阈值: " << diff_threshold_ << std::endl;
    std::cout << "[传送带检测] ROI Y中心: " << roi_y_center_ << std::endl;
    std::cout << "[传送带检测] ROI Y边距: " << roi_y_margin_ << std::endl;
    std::cout << "[传送带检测] 背景参考路径: " << (bg_ref_path_.empty() ? "(无)" : bg_ref_path_) << std::endl;
    std::cout << "[传送带检测] 模版验证: " << (verify_with_template_ ? "开启" : "关闭") << std::endl;
    std::cout << "[传送带检测] 面积范围: [" << min_area_ << ", " << max_area_ << "]" << std::endl;
    if (initial_angle_offset_ != 0.0) {
        std::cout << "[传送带检测] 初始角度偏移: " << initial_angle_offset_ << "°" << std::endl;
    }
    std::cout << "[传送带检测] 背景参考: " << (bg_ref_gray_.empty() ? "未加载" : "已加载") << std::endl;

    return true;
}

bool ConveyorDetector::isReady() const {
    return ready_.load() && hasBackgroundRef();
}

cv::Mat ConveyorDetector::frameToGray(const Frame& frame) const {
    size_t expected_mono = static_cast<size_t>(frame.width) * frame.height;

    if (frame.data.size() == expected_mono) {
        return cv::Mat(frame.height, frame.width, CV_8UC1,
                       const_cast<unsigned char*>(frame.data.data())).clone();
    }

    size_t expected_rgb = expected_mono * 3;
    cv::Mat result;

    if (frame.data.size() == expected_rgb) {
        cv::Mat src(frame.height, frame.width, CV_8UC3,
                    const_cast<unsigned char*>(frame.data.data()));
        if (frame.pixelType == 0x02180015) {
            cv::cvtColor(src, result, cv::COLOR_BGR2GRAY);
        } else {
            cv::Mat bgr;
            cv::cvtColor(src, bgr, cv::COLOR_RGB2BGR);
            cv::cvtColor(bgr, result, cv::COLOR_BGR2GRAY);
        }
    } else {
        return cv::Mat();
    }

    return result;
}

bool ConveyorDetector::loadBackgroundRef() {
    cv::Mat bg = cv::imread(bg_ref_path_, cv::IMREAD_GRAYSCALE);
    if (bg.empty()) {
        std::cerr << "[传送带检测] 无法加载背景参考图: " << bg_ref_path_ << std::endl;
        return false;
    }
    bg_ref_gray_ = bg;
    std::cout << "[传送带检测] 背景参考图已加载: " << bg_ref_path_
              << " (" << bg.cols << "x" << bg.rows << ")" << std::endl;
    return true;
}

cv::Mat ConveyorDetector::subtractBackground(const cv::Mat& gray) const {
    cv::Mat diff;
    cv::absdiff(gray, bg_ref_gray_, diff);

    cv::Mat binary;
    cv::threshold(diff, binary, diff_threshold_, 255, cv::THRESH_BINARY);

    if (roi_y_center_ >= 0 && roi_y_margin_ > 0) {
        int y_top = std::max(0, roi_y_center_ - roi_y_margin_);
        int y_bot = std::min(gray.rows, roi_y_center_ + roi_y_margin_);
        cv::Mat mask = cv::Mat::zeros(binary.size(), CV_8UC1);
        mask(cv::Rect(0, y_top, binary.cols, y_bot - y_top)).setTo(255);
        cv::bitwise_and(binary, mask, binary);
    }

    cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, morph_kernel_);
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, morph_kernel_);

    return binary;
}

std::vector<std::vector<cv::Point>> ConveyorDetector::findForegroundBlobs(const cv::Mat& binary) const {
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::vector<std::vector<cv::Point>> filtered;
    for (const auto& contour : contours) {
        double area = cv::contourArea(contour);
        if (area >= min_area_ && area <= max_area_) {
            filtered.push_back(contour);
        }
    }

    std::sort(filtered.begin(), filtered.end(),
              [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
                  return cv::contourArea(a) > cv::contourArea(b);
              });

    return filtered;
}

ConveyorDetector::DetResult ConveyorDetector::verifyWithTemplate(
    const cv::Mat& gray, double cx, double cy, int image_angle) {
    DetResult result;
    result.x = cx;
    result.y = cy;
    result.angle = image_angle;
    result.score = 0.0;
    result.low_confidence = true;

    if (rotated_templates_.empty()) return result;

    int tmpl_size = rotated_templates_[0].cols;
    int half = tmpl_size / 2 + 10;

    int ix = static_cast<int>(std::round(cx));
    int iy = static_cast<int>(std::round(cy));

    cv::Rect roi(
        std::max(0, ix - half),
        std::max(0, iy - half),
        std::min(gray.cols - std::max(0, ix - half), half * 2),
        std::min(gray.rows - std::max(0, iy - half), half * 2)
    );

    if (roi.width < tmpl_size || roi.height < tmpl_size) return result;

    cv::Mat search_region = gray(roi);

    int tmpl_angle1 = ((360 - image_angle) % 360 + 360) % 360;
    int tmpl_angle2 = ((tmpl_angle1 + 180) % 360 + 360) % 360;
    int tmpl_angles[2] = {tmpl_angle1, tmpl_angle2};
    double scores[2] = {0.0, 0.0};
    cv::Point locs[2];

    for (int i = 0; i < 2; ++i) {
        const cv::Mat& tmpl = rotated_templates_[tmpl_angles[i]];
        if (search_region.cols < tmpl.cols || search_region.rows < tmpl.rows) continue;

        cv::matchTemplate(search_region, tmpl, match_result_buf_, cv::TM_CCOEFF_NORMED);
        double max_val;
        cv::Point max_loc;
        cv::minMaxLoc(match_result_buf_, nullptr, &max_val, nullptr, &max_loc);
        scores[i] = max_val;
        locs[i] = max_loc;
    }

    int best_idx = (scores[0] >= scores[1]) ? 0 : 1;
    result.score = scores[best_idx];
    result.low_confidence = (result.score < match_threshold_);

    int best_tmpl_angle = tmpl_angles[best_idx];
    result.angle = ((360 - best_tmpl_angle) % 360 + 360) % 360;

    int half_tmpl = tmpl_size / 2;
    result.x = roi.x + locs[best_idx].x + half_tmpl;
    result.y = roi.y + locs[best_idx].y + half_tmpl;

    return result;
}

ObjectInfoList ConveyorDetector::detect(const Frame& frame) {
    ObjectInfoList result;

    if (!ready_) return result;

    auto t_total_start = std::chrono::high_resolution_clock::now();

    cv::Mat gray = frameToGray(frame);
    if (gray.empty()) return result;

    if (bg_ref_gray_.empty()) {
        std::cerr << "[传送带检测] 背景参考图未设置, 请先调用 captureBackground()" << std::endl;
        return result;
    }

    auto t_sub_start = std::chrono::high_resolution_clock::now();
    cv::Mat binary = subtractBackground(gray);
    auto t_sub_end = std::chrono::high_resolution_clock::now();
    double sub_ms = std::chrono::duration<double, std::milli>(t_sub_end - t_sub_start).count();

    auto t_blob_start = std::chrono::high_resolution_clock::now();
    auto contours = findForegroundBlobs(binary);
    auto t_blob_end = std::chrono::high_resolution_clock::now();
    double blob_ms = std::chrono::duration<double, std::milli>(t_blob_end - t_blob_start).count();

    last_candidate_count_ = static_cast<int>(contours.size());
    last_best_score_ = 0.0;
    last_results_.clear();
    last_contours_ = contours;

    if (contours.empty()) {
        auto t_total_end = std::chrono::high_resolution_clock::now();
        double total_ms = std::chrono::duration<double, std::milli>(t_total_end - t_total_start).count();
        std::cout << "[传送带检测] 检测耗时: 总计=" << total_ms << "ms "
                  << "(差分=" << sub_ms << "ms, 轮廓=" << blob_ms << "ms, 验证=0ms) "
                  << "候选=0 匹配=0" << std::endl;
        return result;
    }

    auto t_verify_start = std::chrono::high_resolution_clock::now();

    for (const auto& contour : contours) {
        cv::RotatedRect rect = cv::minAreaRect(contour);

        double orient = rect.angle;
        if (rect.size.width < rect.size.height) {
            orient += 90.0;
        }

        int raw_image_angle = static_cast<int>(std::round(-orient));
        raw_image_angle = ((raw_image_angle % 360) + 360) % 360;

        double cx = rect.center.x;
        double cy = rect.center.y;
        double score = 1.0;
        bool low_confidence = false;
        int verified_image_angle = raw_image_angle;

        if (verify_with_template_ && !rotated_templates_.empty()) {
            DetResult vr = verifyWithTemplate(gray, cx, cy, raw_image_angle);
            score = vr.score;
            low_confidence = vr.low_confidence;
            cx = vr.x;
            cy = vr.y;
            verified_image_angle = vr.angle;
        }

        if (score > last_best_score_) {
            last_best_score_ = score;
        }

        int final_angle = (verified_image_angle + static_cast<int>(std::round(initial_angle_offset_))) % 360;
        final_angle = ((final_angle % 360) + 360) % 360;
        int grip_angle = final_angle % 180;
        if (grip_angle >= 90) grip_angle -= 180;

        DetResult det;
        det.x = cx;
        det.y = cy;
        det.angle = grip_angle;
        det.score = score;
        det.low_confidence = low_confidence;
        last_results_.push_back(det);

        result.add(ObjectInfo::Builder()
            .setX(cx)
            .setY(cy)
            .setAngle(static_cast<double>(grip_angle))
            .setType(low_confidence ? 1 : 0)
            .build());

        break;
    }

    auto t_verify_end = std::chrono::high_resolution_clock::now();
    double verify_ms = std::chrono::duration<double, std::milli>(t_verify_end - t_verify_start).count();

    auto t_total_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t_total_end - t_total_start).count();

    std::cout << "[传送带检测] 检测耗时: 总计=" << total_ms << "ms "
              << "(差分=" << sub_ms << "ms, 轮廓=" << blob_ms << "ms, 验证=" << verify_ms << "ms) "
              << "候选=" << contours.size() << " 匹配=" << last_results_.size()
              << " 最高分=" << last_best_score_ << std::endl;

    return result;
}

void ConveyorDetector::captureBackground(const Frame& frame) {
    cv::Mat gray = frameToGray(frame);
    if (gray.empty()) {
        std::cerr << "[传送带检测] captureBackground: 帧转换失败" << std::endl;
        return;
    }
    bg_ref_gray_ = gray.clone();
    if (!bg_ref_path_.empty()) {
        cv::imwrite(bg_ref_path_, bg_ref_gray_);
        std::cout << "[传送带检测] 背景参考图已保存: " << bg_ref_path_ << std::endl;
    }
    std::cout << "[传送带检测] 背景参考图已捕获 ("
              << bg_ref_gray_.cols << "x" << bg_ref_gray_.rows << ")" << std::endl;
}

bool ConveyorDetector::hasBackgroundRef() const {
    return !bg_ref_gray_.empty();
}

bool ConveyorDetector::saveAnnotated(const Frame& frame, const std::string& path) {
    if (last_results_.empty()) return false;

    cv::Mat gray = frameToGray(frame);
    if (gray.empty()) return false;

    cv::Mat image;
    cv::cvtColor(gray, image, cv::COLOR_GRAY2BGR);

    for (size_t i = 0; i < last_results_.size(); ++i) {
        const auto& det = last_results_[i];

        if (i < last_contours_.size()) {
            cv::drawContours(image, last_contours_, static_cast<int>(i), cv::Scalar(0, 255, 0), 2);
        }

        if (!template_img_.empty()) {
            cv::RotatedRect rrect(
                cv::Point2f(static_cast<float>(det.x), static_cast<float>(det.y)),
                cv::Size2f(static_cast<float>(template_img_.cols),
                           static_cast<float>(template_img_.rows)),
                static_cast<float>(det.angle)
            );

            cv::Point2f vertices[4];
            rrect.points(vertices);
            for (int j = 0; j < 4; ++j) {
                cv::line(image, vertices[j], vertices[(j + 1) % 4],
                         cv::Scalar(0, 255, 0), 2);
            }
        }

        int cross_size = 10;
        cv::Point center(static_cast<int>(det.x), static_cast<int>(det.y));
        cv::line(image, cv::Point(center.x - cross_size, center.y),
                 cv::Point(center.x + cross_size, center.y),
                 cv::Scalar(0, 0, 255), 2);
        cv::line(image, cv::Point(center.x, center.y - cross_size),
                 cv::Point(center.x, center.y + cross_size),
                 cv::Scalar(0, 0, 255), 2);

        if (roi_y_center_ >= 0 && roi_y_margin_ > 0) {
            int y_top = std::max(0, roi_y_center_ - roi_y_margin_);
            int y_bot = std::min(image.rows, roi_y_center_ + roi_y_margin_);
            cv::rectangle(image, cv::Point(0, y_top), cv::Point(image.cols - 1, y_bot),
                          cv::Scalar(255, 0, 0), 1);
        }

        char label[128];
        snprintf(label, sizeof(label), "#%zu x=%.4f y=%.4f a=%d s=%.2f",
                 i, det.x, det.y, det.angle, det.score);
        cv::Point text_pos(center.x + 15, center.y - 10);
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
        cv::rectangle(image, text_pos + cv::Point(0, baseline),
                      text_pos + cv::Point(text_size.width, -text_size.height),
                      cv::Scalar(0, 0, 0), cv::FILLED);
        cv::putText(image, label, text_pos,
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);

        if (det.low_confidence) {
            cv::putText(image, "LOW CONF", cv::Point(center.x + 15, center.y + 15),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
        }
    }

    return cv::imwrite(path, image);
}

bool ConveyorDetector::loadTemplateInfo() {
    std::string info_path = template_dir_ + "/template_info.txt";
    std::ifstream ifs(info_path);
    if (!ifs.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        if (key == "initial_angle_offset") {
            initial_angle_offset_ = std::stod(value);
        }
    }

    ifs.close();
    return true;
}
