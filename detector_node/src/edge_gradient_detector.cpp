#include "edge_gradient_detector.h"
#include "logger/logger.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <string>
#include <cmath>
#include <algorithm>
#include <chrono>

EdgeGradientDetector::EdgeGradientDetector(const std::string& template_dir,
                                           float match_threshold,
                                           int angle_step_coarse,
                                           int angle_step_fine)
    : template_dir_(template_dir)
    , match_threshold_(match_threshold)
    , angle_step_coarse_(angle_step_coarse)
    , angle_step_fine_(angle_step_fine) {
}

void EdgeGradientDetector::setHsvRange(int h_low, int h_high,
                                       int s_low, int s_high,
                                       int v_low, int v_high) {
    hsv_h_low_ = h_low;   hsv_h_high_ = h_high;
    hsv_s_low_ = s_low;   hsv_s_high_ = s_high;
    hsv_v_low_ = v_low;   hsv_v_high_ = v_high;
}

void EdgeGradientDetector::setAreaRange(double min_area, double max_area) {
    min_area_ = min_area;
    max_area_ = max_area;
}

void EdgeGradientDetector::setSegmentMode(const std::string& mode) {
    if (mode == "hsv" || mode == "value" || mode == "gradient") {
        segment_mode_ = mode;
        LOG_INFO("[边缘梯度] 分割模式: %s", mode.c_str());
    } else {
        LOG_WARN("[边缘梯度] 无效分割模式 '%s', 使用默认 'hsv'", mode.c_str());
    }
}

void EdgeGradientDetector::setVThreshold(int v_threshold) {
    v_threshold_ = v_threshold;
    LOG_INFO("[边缘梯度] V 通道阈值: %d", v_threshold);
}

void EdgeGradientDetector::setGradientThreshold(int grad_threshold) {
    grad_threshold_ = grad_threshold;
    LOG_INFO("[边缘梯度] 梯度阈值: %d", grad_threshold);
}

void EdgeGradientDetector::setRoiYCenter(int y_center) {
    roi_y_center_ = y_center;
}

void EdgeGradientDetector::setRoiYMargin(int y_margin) {
    roi_y_margin_ = y_margin;
}

void EdgeGradientDetector::setCannyThresholds(double low, double high) {
    canny_low_ = low;
    canny_high_ = high;
    LOG_INFO("[边缘梯度] Canny 阈值: %f / %f", low, high);
}

void EdgeGradientDetector::setGradientTolerance(double tolerance_rad) {
    gradient_tolerance_ = tolerance_rad;
    LOG_INFO("[边缘梯度] 梯度方向容差: %f°", tolerance_rad * 180.0 / M_PI);
}

void EdgeGradientDetector::setSampleStep(int step) {
    sample_step_ = std::max(1, step);
    LOG_INFO("[边缘梯度] 采样间隔: %d", sample_step_);
}

bool EdgeGradientDetector::init() {
    loadTemplateInfo();

    // 加载模板图像 (灰度)
    std::string gray_path = template_dir_ + "/template_gray.png";
    cv::Mat template_gray = cv::imread(gray_path, cv::IMREAD_GRAYSCALE);
    if (!template_gray.empty()) {
        template_width_ = template_gray.cols;
        template_height_ = template_gray.rows;
        LOG_INFO("[边缘梯度] 灰度模版已加载: %s (%dx%d)", gray_path.c_str(), template_width_, template_height_);
    } else {
        std::string tmpl_path = template_dir_ + "/template.png";
        cv::Mat tmpl_color = cv::imread(tmpl_path, cv::IMREAD_COLOR);
        if (tmpl_color.empty()) {
            LOG_ERROR("[边缘梯度] 无法加载模版图片: %s", tmpl_path.c_str());
            return false;
        }
        cv::cvtColor(tmpl_color, template_gray, cv::COLOR_BGR2GRAY);
        template_width_ = tmpl_color.cols;
        template_height_ = tmpl_color.rows;
        template_img_ = tmpl_color;
        LOG_INFO("[边缘梯度] 彩色模版已加载(转灰度): %s (%dx%d)", tmpl_path.c_str(), template_width_, template_height_);
    }

    if (template_img_.empty()) {
        cv::cvtColor(template_gray, template_img_, cv::COLOR_GRAY2BGR);
    }

    if (initial_angle_offset_ != 0.0) {
        LOG_INFO("[边缘梯度] 初始角度偏移: %f°", initial_angle_offset_);
    }

    std::string mask_path = template_dir_ + "/template_mask.png";
    cv::Mat template_mask = cv::imread(mask_path, cv::IMREAD_GRAYSCALE);
    if (!template_mask.empty()) {
        cv::threshold(template_mask, template_mask, 128, 255, cv::THRESH_BINARY);
        double mask_ratio = cv::countNonZero(template_mask) / (double)(template_mask.rows * template_mask.cols);
        LOG_INFO("[边缘梯度] 前景 mask 已加载: %s (前景 %.1f%%)", mask_path.c_str(), mask_ratio * 100);
        cv::Scalar bg_mean = cv::mean(template_gray, template_mask);
        template_gray.setTo(bg_mean, ~template_mask);
    } else {
        LOG_INFO("[边缘梯度] 未找到前景 mask, 使用全图边缘");
    }

    if (!extractTemplateEdges(template_gray)) {
        LOG_ERROR("[边缘梯度] 模板边缘提取失败");
        return false;
    }

    template_diag_ = static_cast<int>(std::ceil(
        std::sqrt(template_width_ * template_width_ +
                  template_height_ * template_height_)));

    morph_kernel_ = cv::getStructuringElement(cv::MORPH_RECT,
        cv::Size(morph_kernel_size_, morph_kernel_size_));
    morph_kernel_small_ = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));

    if (roi_y_center_ >= 0 && roi_y_margin_ > 0) {
        LOG_INFO("[边缘梯度] ROI Y中心: %d 边距: %d", roi_y_center_, roi_y_margin_);
    }

    ready_ = true;
    return true;
}

bool EdgeGradientDetector::extractTemplateEdges(const cv::Mat& template_gray) {
    // 1. Canny 边缘检测
    cv::Mat edges;
    cv::Canny(template_gray, edges, canny_low_, canny_high_);

    // 2. 计算梯度方向 (Sobel)
    cv::Mat grad_x, grad_y;
    cv::Sobel(template_gray, grad_x, CV_32F, 1, 0, 3);
    cv::Sobel(template_gray, grad_y, CV_32F, 0, 1, 3);

    // 3. 模板中心
    float cx = template_width_ / 2.0f;
    float cy = template_height_ / 2.0f;

    // 4. 采样边缘点
    template_edges_.clear();

    for (int y = 0; y < edges.rows; y += sample_step_) {
        for (int x = 0; x < edges.cols; x += sample_step_) {
            if (edges.at<uchar>(y, x) == 0) continue;

            float gx = grad_x.at<float>(y, x);
            float gy = grad_y.at<float>(y, x);
            float mag = std::sqrt(gx * gx + gy * gy);
            if (mag < 1e-5f) continue;

            EdgePoint ep;
            ep.dx = static_cast<float>(x) - cx;
            ep.dy = static_cast<float>(y) - cy;
            ep.grad_angle = std::atan2(gy, gx);  // [-π, π]
            if (ep.grad_angle < 0) ep.grad_angle += static_cast<float>(2.0 * M_PI);

            template_edges_.push_back(ep);
        }
    }

    template_edge_count_ = static_cast<int>(template_edges_.size());

    if (template_edge_count_ < 10) {
        LOG_ERROR("[边缘梯度] 模板边缘点过少: %d, 请调整 Canny 阈值", template_edge_count_);
        return false;
    }

    LOG_INFO("[边缘梯度] 模板边缘点数: %d (采样间隔=%d)", template_edge_count_, sample_step_);

    return true;
}

bool EdgeGradientDetector::isReady() const {
    return ready_.load();
}

cv::Mat EdgeGradientDetector::frameToBGR(const Frame& frame) const {
    size_t expected_mono = static_cast<size_t>(frame.width) * frame.height;
    size_t expected_rgb = expected_mono * 3;

    cv::Mat result;

    if (frame.data.size() == expected_rgb) {
        cv::Mat src(frame.height, frame.width, CV_8UC3,
                    const_cast<unsigned char*>(frame.data.data()));
        if (frame.pixelType == 0x02180015) {
            result = src.clone();
        } else {
            cv::cvtColor(src, result, cv::COLOR_RGB2BGR);
        }
    } else {
        cv::Mat src(frame.height, frame.width, CV_8UC1,
                    const_cast<unsigned char*>(frame.data.data()));
        cv::cvtColor(src, result, cv::COLOR_GRAY2BGR);
    }

    return result;
}

cv::Mat EdgeGradientDetector::frameToGray(const Frame& frame) const {
    size_t expected_mono = static_cast<size_t>(frame.width) * frame.height;

    if (frame.data.size() == expected_mono) {
        return cv::Mat(frame.height, frame.width, CV_8UC1,
                       const_cast<unsigned char*>(frame.data.data())).clone();
    }

    cv::Mat bgr = frameToBGR(frame);
    if (bgr.empty()) return cv::Mat();

    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

// ============================================================================
// 候选区域分割 (与 OpenCvTemplateDetector 逻辑一致)
// ============================================================================

std::vector<cv::Rect> EdgeGradientDetector::findCandidateRegions(const cv::Mat& bgr) const {
    std::vector<cv::Rect> regions;

    if (segment_mode_ == "value") {
        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

        cv::Mat product_mask;
        cv::threshold(gray, product_mask, v_threshold_, 255, cv::THRESH_BINARY);

        cv::morphologyEx(product_mask, product_mask, cv::MORPH_CLOSE, morph_kernel_);
        cv::morphologyEx(product_mask, product_mask, cv::MORPH_OPEN, morph_kernel_);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(product_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

        for (const auto& contour : contours) {
            double area = cv::contourArea(contour);
            if (area < min_area_) continue;

            if (area <= max_area_) {
                regions.push_back(cv::boundingRect(contour));
            } else if (template_diag_ > 0) {
                cv::Moments m = cv::moments(contour);
                if (m.m00 > 0) {
                    int cx = static_cast<int>(m.m10 / m.m00);
                    int cy = static_cast<int>(m.m01 / m.m00);
                    int half = template_diag_;
                    cv::Rect clipped(
                        std::max(0, cx - half),
                        std::max(0, cy - half),
                        std::min(bgr.cols - std::max(0, cx - half), half * 2),
                        std::min(bgr.rows - std::max(0, cy - half), half * 2)
                    );
                    regions.push_back(clipped);
                }
            }
        }

        LOG_DEBUG("[边缘梯度] value 模式: V>%d 找到 %d 个候选区域", v_threshold_, regions.size());
        return regions;
    }

    if (segment_mode_ == "gradient") {
        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

        cv::Mat grad_x, grad_y;
        cv::Sobel(gray, grad_x, CV_16S, 1, 0, 3);
        cv::Sobel(gray, grad_y, CV_16S, 0, 1, 3);
        cv::convertScaleAbs(grad_x, grad_x);
        cv::convertScaleAbs(grad_y, grad_y);
        cv::Mat grad_mag;
        cv::addWeighted(grad_x, 0.5, grad_y, 0.5, 0, grad_mag);

        cv::Mat edge_mask;
        cv::threshold(grad_mag, edge_mask, grad_threshold_, 255, cv::THRESH_BINARY);

        cv::dilate(edge_mask, edge_mask, morph_kernel_small_);
        cv::morphologyEx(edge_mask, edge_mask, cv::MORPH_CLOSE, morph_kernel_);
        cv::morphologyEx(edge_mask, edge_mask, cv::MORPH_OPEN, morph_kernel_);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(edge_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

        for (const auto& contour : contours) {
            double area = cv::contourArea(contour);
            if (area < min_area_) continue;

            if (area <= max_area_) {
                regions.push_back(cv::boundingRect(contour));
            } else if (template_diag_ > 0) {
                cv::Moments m = cv::moments(contour);
                if (m.m00 > 0) {
                    int cx = static_cast<int>(m.m10 / m.m00);
                    int cy = static_cast<int>(m.m01 / m.m00);
                    int half = template_diag_;
                    cv::Rect clipped(
                        std::max(0, cx - half),
                        std::max(0, cy - half),
                        std::min(bgr.cols - std::max(0, cx - half), half * 2),
                        std::min(bgr.rows - std::max(0, cy - half), half * 2)
                    );
                    regions.push_back(clipped);
                }
            }
        }

        LOG_DEBUG("[边缘梯度] gradient 模式: grad>%d 找到 %d 个候选区域", grad_threshold_, regions.size());
        return regions;
    }

    // HSV 绿色背景分割 (默认)
    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);

    cv::Mat green_mask;
    cv::inRange(hsv,
                cv::Scalar(hsv_h_low_, hsv_s_low_, hsv_v_low_),
                cv::Scalar(hsv_h_high_, hsv_s_high_, hsv_v_high_),
                green_mask);

    double green_ratio = static_cast<double>(cv::countNonZero(green_mask)) / green_mask.total();
    if (green_ratio < 0.05) {
        LOG_WARN("[边缘梯度] 警告: 绿色背景占比仅 %.1f%%! 建议使用 --segment-mode value", green_ratio * 100);
    }

    cv::Mat product_mask;
    cv::bitwise_not(green_mask, product_mask);

    cv::morphologyEx(product_mask, product_mask, cv::MORPH_CLOSE, morph_kernel_);
    cv::morphologyEx(product_mask, product_mask, cv::MORPH_OPEN, morph_kernel_);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(product_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    for (const auto& contour : contours) {
        double area = cv::contourArea(contour);
        if (area < min_area_) continue;

        if (area <= max_area_) {
            regions.push_back(cv::boundingRect(contour));
        } else if (template_diag_ > 0) {
            cv::Moments m = cv::moments(contour);
            if (m.m00 > 0) {
                int cx = static_cast<int>(m.m10 / m.m00);
                int cy = static_cast<int>(m.m01 / m.m00);
                int half = template_diag_;
                cv::Rect clipped(
                    std::max(0, cx - half),
                    std::max(0, cy - half),
                    std::min(bgr.cols - std::max(0, cx - half), half * 2),
                    std::min(bgr.rows - std::max(0, cy - half), half * 2)
                );
                regions.push_back(clipped);
            }
        }
    }

    return regions;
}

std::vector<cv::Rect> EdgeGradientDetector::findCandidateRegionsGray(const cv::Mat& gray) const {
    std::vector<cv::Rect> regions;

    if (segment_mode_ == "value") {
        cv::Mat product_mask;
        cv::threshold(gray, product_mask, v_threshold_, 255, cv::THRESH_BINARY);

        cv::morphologyEx(product_mask, product_mask, cv::MORPH_CLOSE, morph_kernel_);
        cv::morphologyEx(product_mask, product_mask, cv::MORPH_OPEN, morph_kernel_);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(product_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

        for (const auto& contour : contours) {
            double area = cv::contourArea(contour);
            if (area < min_area_) continue;

            if (area <= max_area_) {
                regions.push_back(cv::boundingRect(contour));
            } else if (template_diag_ > 0) {
                cv::Moments m = cv::moments(contour);
                if (m.m00 > 0) {
                    int cx = static_cast<int>(m.m10 / m.m00);
                    int cy = static_cast<int>(m.m01 / m.m00);
                    int half = template_diag_;
                    cv::Rect clipped(
                        std::max(0, cx - half),
                        std::max(0, cy - half),
                        std::min(gray.cols - std::max(0, cx - half), half * 2),
                        std::min(gray.rows - std::max(0, cy - half), half * 2)
                    );
                    regions.push_back(clipped);
                }
            }
        }

        LOG_DEBUG("[边缘梯度] value 模式: V>%d 找到 %d 个候选区域", v_threshold_, regions.size());
        return regions;
    }

    if (segment_mode_ == "gradient") {
        cv::Mat grad_x, grad_y;
        cv::Sobel(gray, grad_x, CV_16S, 1, 0, 3);
        cv::Sobel(gray, grad_y, CV_16S, 0, 1, 3);
        cv::convertScaleAbs(grad_x, grad_x);
        cv::convertScaleAbs(grad_y, grad_y);
        cv::Mat grad_mag;
        cv::addWeighted(grad_x, 0.5, grad_y, 0.5, 0, grad_mag);

        cv::Mat edge_mask;
        cv::threshold(grad_mag, edge_mask, grad_threshold_, 255, cv::THRESH_BINARY);

        cv::dilate(edge_mask, edge_mask, morph_kernel_small_);
        cv::morphologyEx(edge_mask, edge_mask, cv::MORPH_CLOSE, morph_kernel_);
        cv::morphologyEx(edge_mask, edge_mask, cv::MORPH_OPEN, morph_kernel_);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(edge_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

        for (const auto& contour : contours) {
            double area = cv::contourArea(contour);
            if (area < min_area_) continue;

            if (area <= max_area_) {
                regions.push_back(cv::boundingRect(contour));
            } else if (template_diag_ > 0) {
                cv::Moments m = cv::moments(contour);
                if (m.m00 > 0) {
                    int cx = static_cast<int>(m.m10 / m.m00);
                    int cy = static_cast<int>(m.m01 / m.m00);
                    int half = template_diag_;
                    cv::Rect clipped(
                        std::max(0, cx - half),
                        std::max(0, cy - half),
                        std::min(gray.cols - std::max(0, cx - half), half * 2),
                        std::min(gray.rows - std::max(0, cy - half), half * 2)
                    );
                    regions.push_back(clipped);
                }
            }
        }

        LOG_DEBUG("[边缘梯度] gradient 模式: grad>%d 找到 %d 个候选区域", grad_threshold_, regions.size());
        return regions;
    }

    // hsv 模式需要彩色信息, 回退到 BGR 路径
    cv::Mat bgr;
    cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
    return findCandidateRegions(bgr);
}

// ============================================================================
// 核心匹配算法: 边缘梯度方向一致性匹配
// ============================================================================

EdgeGradientDetector::MatchResult
EdgeGradientDetector::matchInRegion(const cv::Mat& gray, const cv::Rect& roi,
                                     int coarse_step_override) const {
    MatchResult best;
    best.score = 0.0;
    best.angle = 0;
    best.center = cv::Point2d(roi.x + roi.width / 2.0, roi.y + roi.height / 2.0);

    int effective_coarse_step = (coarse_step_override > 0) ? coarse_step_override : angle_step_coarse_;

    // 扩展 ROI
    int expand = template_diag_ / 2 + 10;
    cv::Rect expanded_roi(
        std::max(0, roi.x - expand),
        std::max(0, roi.y - expand),
        std::min(gray.cols - std::max(0, roi.x - expand), roi.width + 2 * expand),
        std::min(gray.rows - std::max(0, roi.y - expand), roi.height + 2 * expand)
    );

    if (expanded_roi.width < template_diag_ || expanded_roi.height < template_diag_) {
        return best;
    }

    // =====================================================================
    // 提取搜索图边缘 + 梯度方向 (只做一次!)
    // =====================================================================
    auto t_edge_start = std::chrono::high_resolution_clock::now();

    cv::Mat search_gray = gray(expanded_roi);

    // Canny 边缘
    cv::Mat search_edges;
    cv::Canny(search_gray, search_edges, canny_low_, canny_high_);

    // Sobel 梯度方向
    cv::Mat search_grad_x, search_grad_y;
    cv::Sobel(search_gray, search_grad_x, CV_32F, 1, 0, 3);
    cv::Sobel(search_gray, search_grad_y, CV_32F, 0, 1, 3);

    // 构建搜索图边缘点索引: 位置 → 梯度方向
    // 使用方向梯度直方图 (DGH) 加速: 将搜索图边缘点按梯度方向分桶
    const int NUM_BINS = 72;  // 每 5° 一个桶
    const double bin_width = 2.0 * M_PI / NUM_BINS;

    struct SearchEdgePoint {
        int x, y;
        float grad_angle;
    };

    std::vector<SearchEdgePoint> search_edge_points;
    std::vector<std::vector<int>> angle_bins(NUM_BINS);

    for (int y = 0; y < search_edges.rows; y += sample_step_) {
        for (int x = 0; x < search_edges.cols; x += sample_step_) {
            if (search_edges.at<uchar>(y, x) == 0) continue;

            float gx = search_grad_x.at<float>(y, x);
            float gy = search_grad_y.at<float>(y, x);
            float mag = std::sqrt(gx * gx + gy * gy);
            if (mag < 1e-5f) continue;

            float angle = std::atan2(gy, gx);
            if (angle < 0) angle += static_cast<float>(2.0 * M_PI);

            int idx = static_cast<int>(search_edge_points.size());
            search_edge_points.push_back({x, y, angle});

            int bin = static_cast<int>(angle / bin_width) % NUM_BINS;
            angle_bins[bin].push_back(idx);
        }
    }

    auto t_edge_end = std::chrono::high_resolution_clock::now();
    double edge_ms = std::chrono::duration<double, std::milli>(t_edge_end - t_edge_start).count();

    if (search_edge_points.empty()) {
        return best;
    }

    // =====================================================================
    // 粗搜索: 对每个候选角度, 计算梯度方向一致性分数
    // =====================================================================
    auto t_coarse_start = std::chrono::high_resolution_clock::now();

    int best_coarse_angle = 0;
    double best_coarse_score = 0.0;
    cv::Point2d best_coarse_center(expanded_roi.width / 2.0, expanded_roi.height / 2.0);

    // 预计算: 对每个模板边缘点, 其梯度方向所属的 bin
    // 旋转 θ 后, 梯度方向变为 (grad_angle + θ) mod 2π
    // 因此旋转 θ 时, 模板边缘点 grad_angle 对应的搜索图 bin 为:
    //   target_bin = (template_bin + θ_bin_offset) % NUM_BINS

    // 为加速, 预计算每个模板边缘点的 bin
    std::vector<int> tmpl_bins(template_edge_count_);
    for (int i = 0; i < template_edge_count_; ++i) {
        tmpl_bins[i] = static_cast<int>(template_edges_[i].grad_angle / bin_width) % NUM_BINS;
    }

    // 对每个候选角度, 使用投票法计算匹配分数
    // 核心思路: 对于旋转角度 θ, 模板中心在 (cx, cy) 时,
    //   模板边缘点 i 旋转后的位置为:
    //     px = cx + dx*cos(θ) - dy*sin(θ)
    //     py = cy + dx*sin(θ) + dy*cos(θ)
    //   其期望的梯度方向为: (grad_angle_i + θ) mod 2π
    //   如果搜索图在 (px, py) 附近有边缘点且梯度方向一致, 则投票 +1

    // 为了高效, 使用两阶段策略:
    //   Phase 1 (粗搜索): 只用梯度方向直方图匹配, 不计算精确位置
    //     对每个角度 θ, 统计搜索图中有多少边缘点的梯度方向
    //     与旋转后的模板边缘点梯度方向一致
    //   Phase 2 (精搜索): 在最佳角度附近, 计算精确位置匹配

    // Phase 1: 梯度方向直方图匹配 (极快, O(360/step * N_template))
    for (int angle = 0; angle < 360; angle += effective_coarse_step) {
        double theta = angle * M_PI / 180.0;
        int theta_bin_offset = static_cast<int>(std::round(theta / bin_width)) % NUM_BINS;

        // 计算方向一致性: 模板边缘点旋转后, 搜索图中有多少对应方向的边缘点
        double direction_score = 0.0;
        for (int i = 0; i < template_edge_count_; ++i) {
            int target_bin = (tmpl_bins[i] + theta_bin_offset) % NUM_BINS;

            // 检查目标 bin 及相邻 bin (容差)
            int count = 0;
            for (int db = -1; db <= 1; ++db) {
                int b = (target_bin + db + NUM_BINS) % NUM_BINS;
                count += static_cast<int>(angle_bins[b].size());
            }

            // 归一化: 如果搜索图在该方向有边缘点, 则模板该点"有可能"匹配
            if (count > 0) {
                direction_score += 1.0;
            }
        }

        direction_score /= template_edge_count_;

        if (direction_score > best_coarse_score) {
            best_coarse_score = direction_score;
            best_coarse_angle = angle;
        }

        // 方向一致性很高时提前退出
        if (best_coarse_score > 0.9) break;
    }

    auto t_coarse_end = std::chrono::high_resolution_clock::now();
    double coarse_ms = std::chrono::duration<double, std::milli>(t_coarse_end - t_coarse_start).count();

    // 粗搜索分数太低, 直接返回
    if (best_coarse_score < 0.2) {
        return best;
    }

    // =====================================================================
    // 精搜索: 在最佳角度附近, 计算精确位置匹配
    // =====================================================================
    auto t_fine_start = std::chrono::high_resolution_clock::now();

    int fine_half_range = effective_coarse_step / 2 + 2;
    int fine_start = best_coarse_angle - fine_half_range;
    int fine_end = best_coarse_angle + fine_half_range;

    double best_fine_score = 0.0;
    int best_fine_angle = best_coarse_angle;
    cv::Point2d best_fine_center(expanded_roi.width / 2.0, expanded_roi.height / 2.0);

    // 构建搜索图边缘点的空间索引 (用于快速查找邻近点)
    // 使用简单的网格索引
    const int grid_size = 4;  // 每个网格 4x4 像素
    int grid_w = (search_edges.cols + grid_size - 1) / grid_size;
    int grid_h = (search_edges.rows + grid_size - 1) / grid_size;
    std::vector<std::vector<int>> spatial_grid(grid_w * grid_h);

    for (int i = 0; i < static_cast<int>(search_edge_points.size()); ++i) {
        const auto& pt = search_edge_points[i];
        int gx = pt.x / grid_size;
        int gy = pt.y / grid_size;
        if (gx >= 0 && gx < grid_w && gy >= 0 && gy < grid_h) {
            spatial_grid[gy * grid_w + gx].push_back(i);
        }
    }

    // 对搜索图边缘点预计算梯度方向, 用于快速查找
    // 构建方向 → 边缘点索引的映射 (用于方向一致性检查)
    // 已有 angle_bins, 直接使用

    for (int angle = fine_start; angle <= fine_end; angle += angle_step_fine_) {
        int norm_angle = ((angle % 360) + 360) % 360;
        double theta = norm_angle * M_PI / 180.0;
        double cos_t = std::cos(theta);
        double sin_t = std::sin(theta);
        int theta_bin_offset = static_cast<int>(std::round(theta / bin_width)) % NUM_BINS;

        // 使用 Hough 投票法: 对每个搜索图边缘点, 计算它可能对应的模板中心位置
        // 如果搜索图边缘点 (sx, sy) 的梯度方向与模板边缘点 i 旋转后的梯度方向一致,
        // 则模板中心可能在:
        //   cx = sx - (dx*cos_t - dy*sin_t)
        //   cy = sy - (dx*sin_t + dy*cos_t)

        // 使用 2D 投票矩阵 (降分辨率加速)
        const int vote_scale = 2;  // 投票矩阵为原图 1/2
        int vote_w = (search_edges.cols + vote_scale - 1) / vote_scale;
        int vote_h = (search_edges.rows + vote_scale - 1) / vote_scale;
        std::vector<float> vote_matrix(vote_w * vote_h, 0.0f);

        // 只使用梯度方向匹配的搜索图边缘点进行投票
        // 收集该角度下所有匹配的搜索图边缘点
        std::vector<int> matched_search_pts;
        for (int i = 0; i < template_edge_count_; ++i) {
            int target_bin = (tmpl_bins[i] + theta_bin_offset) % NUM_BINS;
            for (int db = -1; db <= 1; ++db) {
                int b = (target_bin + db + NUM_BINS) % NUM_BINS;
                for (int idx : angle_bins[b]) {
                    matched_search_pts.push_back(idx);
                }
            }
        }

        // 去重
        std::sort(matched_search_pts.begin(), matched_search_pts.end());
        matched_search_pts.erase(
            std::unique(matched_search_pts.begin(), matched_search_pts.end()),
            matched_search_pts.end());

        // 对每个匹配的搜索图边缘点, 用所有模板边缘点投票
        // 为了效率, 采样模板边缘点 (最多 200 个)
        int tmpl_sample_step = std::max(1, template_edge_count_ / 200);

        for (int sidx : matched_search_pts) {
            const auto& spt = search_edge_points[sidx];

            // 查找与该搜索点梯度方向最接近的模板边缘点
            float s_angle = spt.grad_angle;
            // 旋转回模板坐标系: 模板中的梯度方向 = s_angle - theta
            float tmpl_angle = s_angle - static_cast<float>(theta);
            if (tmpl_angle < 0) tmpl_angle += static_cast<float>(2.0 * M_PI);
            int tmpl_target_bin = static_cast<int>(tmpl_angle / bin_width) % NUM_BINS;

            // 在模板中找方向匹配的边缘点, 投票到中心位置
            for (int db = -1; db <= 1; ++db) {
                int b = (tmpl_target_bin + db + NUM_BINS) % NUM_BINS;

                // 遍历该 bin 中的模板边缘点 (采样)
                for (int ti = 0; ti < template_edge_count_; ti += tmpl_sample_step) {
                    if (tmpl_bins[ti] != b) continue;

                    const auto& tep = template_edges_[ti];

                    // 计算模板中心位置
                    float cx = spt.x - (tep.dx * cos_t - tep.dy * sin_t);
                    float cy = spt.y - (tep.dx * sin_t + tep.dy * cos_t);

                    // 投票
                    int vx = static_cast<int>(cx / vote_scale);
                    int vy = static_cast<int>(cy / vote_scale);
                    if (vx >= 0 && vx < vote_w && vy >= 0 && vy < vote_h) {
                        vote_matrix[vy * vote_w + vx] += 1.0f;
                    }
                }
            }
        }

        // 找投票矩阵峰值
        float max_vote = 0.0f;
        int max_vx = vote_w / 2, max_vy = vote_h / 2;
        for (int vy = 0; vy < vote_h; ++vy) {
            for (int vx = 0; vx < vote_w; ++vx) {
                if (vote_matrix[vy * vote_w + vx] > max_vote) {
                    max_vote = vote_matrix[vy * vote_w + vx];
                    max_vx = vx;
                    max_vy = vy;
                }
            }
        }

        // 分数 = 峰值投票数 / 模板边缘点数
        double score = static_cast<double>(max_vote) / template_edge_count_;

        if (score > best_fine_score) {
            best_fine_score = score;
            best_fine_angle = norm_angle;

            // 亚像素精化: 在投票矩阵峰值周围做质心计算
            double sum_w = 0.0, sum_wx = 0.0, sum_wy = 0.0;
            int refine_radius = 2;
            for (int dy = -refine_radius; dy <= refine_radius; ++dy) {
                for (int dx = -refine_radius; dx <= refine_radius; ++dx) {
                    int vx = max_vx + dx;
                    int vy = max_vy + dy;
                    if (vx >= 0 && vx < vote_w && vy >= 0 && vy < vote_h) {
                        float w = vote_matrix[vy * vote_w + vx];
                        sum_w += w;
                        sum_wx += w * (vx + 0.5) * vote_scale;
                        sum_wy += w * (vy + 0.5) * vote_scale;
                    }
                }
            }

            if (sum_w > 0) {
                best_fine_center = cv::Point2d(sum_wx / sum_w, sum_wy / sum_w);
            } else {
                best_fine_center = cv::Point2d((max_vx + 0.5) * vote_scale,
                                                (max_vy + 0.5) * vote_scale);
            }
        }

        if (best_fine_score > 0.7) break;
    }

    auto t_fine_end = std::chrono::high_resolution_clock::now();
    double fine_ms = std::chrono::duration<double, std::milli>(t_fine_end - t_fine_start).count();

    // 映射回原图坐标
    best.score = best_fine_score;
    best.angle = best_fine_angle;
    best.center = cv::Point2d(
        expanded_roi.x + best_fine_center.x,
        expanded_roi.y + best_fine_center.y
    );

    return best;
}

std::vector<EdgeGradientDetector::MatchResult>
EdgeGradientDetector::matchInRegionMulti(const cv::Mat& gray, const cv::Rect& roi) const {
    std::vector<MatchResult> results;

    if (template_edge_count_ == 0) return results;

    cv::Rect expanded_roi = roi;
    int pad = template_diag_ / 2 + 10;
    expanded_roi.x = std::max(0, expanded_roi.x - pad);
    expanded_roi.y = std::max(0, expanded_roi.y - pad);
    expanded_roi.width = std::min(gray.cols - expanded_roi.x, expanded_roi.width + 2 * pad);
    expanded_roi.height = std::min(gray.rows - expanded_roi.y, expanded_roi.height + 2 * pad);

    if (expanded_roi.width < template_diag_ || expanded_roi.height < template_diag_) return results;

    // 先找最佳角度
    MatchResult best_match = matchInRegion(gray, roi);
    if (best_match.score < match_threshold_) return results;

    // 用最佳角度做全区域投票, 找所有峰值
    int norm_angle = best_match.angle;
    double theta = norm_angle * M_PI / 180.0;
    double cos_t = std::cos(theta);
    double sin_t = std::sin(theta);

    cv::Mat search_gray = gray(expanded_roi);
    cv::Mat search_edges;
    cv::Canny(search_gray, search_edges, canny_low_, canny_high_);
    cv::Mat search_grad_x, search_grad_y;
    cv::Sobel(search_gray, search_grad_x, CV_32F, 1, 0, 3);
    cv::Sobel(search_gray, search_grad_y, CV_32F, 0, 1, 3);

    const int NUM_BINS = 72;
    const double bin_width = 2.0 * M_PI / NUM_BINS;
    int theta_bin_offset = static_cast<int>(std::round(theta / bin_width)) % NUM_BINS;

    // 构建搜索图边缘点
    struct SearchPt { int x, y; float grad_angle; };
    std::vector<SearchPt> search_pts;

    for (int y = 0; y < search_edges.rows; y += sample_step_) {
        for (int x = 0; x < search_edges.cols; x += sample_step_) {
            if (search_edges.at<uchar>(y, x) == 0) continue;
            float gx = search_grad_x.at<float>(y, x);
            float gy = search_grad_y.at<float>(y, x);
            float mag = std::sqrt(gx * gx + gy * gy);
            if (mag < 1e-5f) continue;
            float angle = std::atan2(gy, gx);
            if (angle < 0) angle += static_cast<float>(2.0 * M_PI);
            search_pts.push_back({x, y, angle});
        }
    }

    // 投票
    const int vote_scale = 2;
    int vote_w = (search_edges.cols + vote_scale - 1) / vote_scale;
    int vote_h = (search_edges.rows + vote_scale - 1) / vote_scale;
    std::vector<float> vote_matrix(vote_w * vote_h, 0.0f);

    std::vector<int> tmpl_bins(template_edge_count_);
    for (int i = 0; i < template_edge_count_; ++i) {
        tmpl_bins[i] = static_cast<int>(template_edges_[i].grad_angle / bin_width) % NUM_BINS;
    }

    int tmpl_sample_step = std::max(1, template_edge_count_ / 200);

    for (const auto& spt : search_pts) {
        float tmpl_angle = spt.grad_angle - static_cast<float>(theta);
        if (tmpl_angle < 0) tmpl_angle += static_cast<float>(2.0 * M_PI);
        int tmpl_target_bin = static_cast<int>(tmpl_angle / bin_width) % NUM_BINS;

        for (int db = -1; db <= 1; ++db) {
            int b = (tmpl_target_bin + db + NUM_BINS) % NUM_BINS;
            for (int ti = 0; ti < template_edge_count_; ti += tmpl_sample_step) {
                if (tmpl_bins[ti] != b) continue;
                const auto& tep = template_edges_[ti];
                float cx = spt.x - (tep.dx * cos_t - tep.dy * sin_t);
                float cy = spt.y - (tep.dx * sin_t + tep.dy * cos_t);
                int vx = static_cast<int>(cx / vote_scale);
                int vy = static_cast<int>(cy / vote_scale);
                if (vx >= 0 && vx < vote_w && vy >= 0 && vy < vote_h) {
                    vote_matrix[vy * vote_w + vx] += 1.0f;
                }
            }
        }
    }

    // 峰值提取 (非极大值抑制)
    int suppress_radius = template_diag_ / (2 * vote_scale);
    cv::Mat vote_copy(vote_h, vote_w, CV_32F, vote_matrix.data());
    std::vector<float> vote_buf = vote_matrix;
    int max_objects = 20;

    for (int i = 0; i < max_objects; ++i) {
        float max_val = 0.0f;
        int max_vx = 0, max_vy = 0;
        for (int vy = 0; vy < vote_h; ++vy) {
            for (int vx = 0; vx < vote_w; ++vx) {
                if (vote_buf[vy * vote_w + vx] > max_val) {
                    max_val = vote_buf[vy * vote_w + vx];
                    max_vx = vx;
                    max_vy = vy;
                }
            }
        }

        double score = static_cast<double>(max_val) / template_edge_count_;
        if (score < match_threshold_) break;

        // 亚像素质心
        double sum_w = 0.0, sum_wx = 0.0, sum_wy = 0.0;
        int rr = 2;
        for (int dy = -rr; dy <= rr; ++dy) {
            for (int dx = -rr; dx <= rr; ++dx) {
                int vx = max_vx + dx;
                int vy = max_vy + dy;
                if (vx >= 0 && vx < vote_w && vy >= 0 && vy < vote_h) {
                    float w = vote_matrix[vy * vote_w + vx];
                    sum_w += w;
                    sum_wx += w * (vx + 0.5) * vote_scale;
                    sum_wy += w * (vy + 0.5) * vote_scale;
                }
            }
        }

        MatchResult mr;
        mr.score = score;
        mr.angle = norm_angle;
        if (sum_w > 0) {
            mr.center = cv::Point2d(expanded_roi.x + sum_wx / sum_w,
                                     expanded_roi.y + sum_wy / sum_w);
        } else {
            mr.center = cv::Point2d(expanded_roi.x + (max_vx + 0.5) * vote_scale,
                                     expanded_roi.y + (max_vy + 0.5) * vote_scale);
        }
        results.push_back(mr);

        // 抑制
        int x0 = std::max(0, max_vx - suppress_radius);
        int y0 = std::max(0, max_vy - suppress_radius);
        int x1 = std::min(vote_w - 1, max_vx + suppress_radius);
        int y1 = std::min(vote_h - 1, max_vy + suppress_radius);
        for (int vy = y0; vy <= y1; ++vy) {
            for (int vx = x0; vx <= x1; ++vx) {
                vote_buf[vy * vote_w + vx] = 0.0f;
            }
        }
    }

    return results;
}

std::vector<EdgeGradientDetector::DetResult>
EdgeGradientDetector::distanceNMS(std::vector<DetResult>& results, double min_dist) const {
    if (results.empty()) return results;

    std::sort(results.begin(), results.end(),
              [](const DetResult& a, const DetResult& b) { return a.score > b.score; });

    std::vector<DetResult> kept;
    std::vector<bool> suppressed(results.size(), false);

    for (size_t i = 0; i < results.size(); ++i) {
        if (suppressed[i]) continue;
        kept.push_back(results[i]);

        for (size_t j = i + 1; j < results.size(); ++j) {
            if (suppressed[j]) continue;
            double dx = results[i].x - results[j].x;
            double dy = results[i].y - results[j].y;
            double dist = std::sqrt(dx * dx + dy * dy);
            if (dist < min_dist) {
                suppressed[j] = true;
            }
        }
    }

    return kept;
}

// ============================================================================
// 主检测函数
// ============================================================================

ObjectInfoList EdgeGradientDetector::detect(const Frame& frame) {
    ObjectInfoList result;

    if (!ready_) return result;

    auto t_total_start = std::chrono::high_resolution_clock::now();

    size_t expected_mono = static_cast<size_t>(frame.width) * frame.height;
    bool is_mono8 = (frame.data.size() == expected_mono);

    cv::Mat bgr, gray;
    if (is_mono8) {
        gray = cv::Mat(frame.height, frame.width, CV_8UC1,
                       const_cast<unsigned char*>(frame.data.data())).clone();
    } else {
        bgr = frameToBGR(frame);
        if (bgr.empty()) return result;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    }

    // 1. 颜色分割找候选区域
    auto t_seg_start = std::chrono::high_resolution_clock::now();
    std::vector<cv::Rect> candidates;
    if (!gray.empty()) {
        candidates = findCandidateRegionsGray(gray);
    } else {
        candidates = findCandidateRegions(bgr);
    }
    auto t_seg_end = std::chrono::high_resolution_clock::now();
    double seg_ms = std::chrono::duration<double, std::milli>(t_seg_end - t_seg_start).count();

    int img_w = bgr.empty() ? gray.cols : bgr.cols;
    int img_h = bgr.empty() ? gray.rows : bgr.rows;
    cv::Point img_center(img_w / 2, img_h / 2);

    // 按距图像中心距离升序排序
    const int max_candidates = 20;
    std::sort(candidates.begin(), candidates.end(),
        [&img_center](const cv::Rect& a, const cv::Rect& b) {
            cv::Point ca(a.x + a.width / 2, a.y + a.height / 2);
            cv::Point cb(b.x + b.width / 2, b.y + b.height / 2);
            double da = (ca.x - img_center.x) * (ca.x - img_center.x) +
                        (ca.y - img_center.y) * (ca.y - img_center.y);
            double db = (cb.x - img_center.x) * (cb.x - img_center.x) +
                        (cb.y - img_center.y) * (cb.y - img_center.y);
            return da < db;
        });
    if (static_cast<int>(candidates.size()) > max_candidates) {
        candidates.resize(max_candidates);
    }

    // ROI Y 轴过滤
    if (roi_y_center_ >= 0 && roi_y_margin_ > 0) {
        int y_lo = roi_y_center_ - roi_y_margin_;
        int y_hi = roi_y_center_ + roi_y_margin_;
        candidates.erase(
            std::remove_if(candidates.begin(), candidates.end(),
                [y_lo, y_hi](const cv::Rect& r) {
                    int cy = r.y + r.height / 2;
                    return cy < y_lo || cy > y_hi;
                }),
            candidates.end());
    }

    last_candidate_count_ = static_cast<int>(candidates.size());
    last_best_score_ = 0.0;

    if (candidates.empty()) {
        auto t_total_end = std::chrono::high_resolution_clock::now();
        double total_ms = std::chrono::duration<double, std::milli>(t_total_end - t_total_start).count();
        LOG_DEBUG("[边缘梯度] 检测耗时: 总计=%.1fms (分割=%.1fms, 匹配=0ms, NMS=0ms) 候选=0 匹配=0", 
                  total_ms, seg_ms);
        last_results_.clear();
        return result;
    }

    // 2. 对候选区域进行边缘梯度匹配
    auto t_match_start = std::chrono::high_resolution_clock::now();
    std::vector<DetResult> detections;

    int best_global_angle = 0;
    double best_global_score = 0.0;

    for (const auto& roi : candidates) {
        MatchResult match = matchInRegion(gray, roi);

        if (match.score > best_global_score) {
            best_global_score = match.score;
            best_global_angle = match.angle;
        }
    }

    if (best_global_score > last_best_score_) {
        last_best_score_ = best_global_score;
    }

    // 3. 用最佳角度对全图匹配, 找所有峰值
    if (best_global_score >= match_threshold_ && template_edge_count_ > 0) {
        // 全图搜索 ROI
        cv::Rect full_roi(0, 0, img_w, img_h);
        if (roi_y_center_ >= 0 && roi_y_margin_ > 0) {
            int y_lo = std::max(0, roi_y_center_ - roi_y_margin_);
            int y_hi = std::min(img_h, roi_y_center_ + roi_y_margin_);
            full_roi = cv::Rect(0, y_lo, img_w, y_hi - y_lo);
        }

        std::vector<MatchResult> multi_results = matchInRegionMulti(gray, full_roi);

        for (const auto& mr : multi_results) {
            DetResult det;
            det.x = mr.center.x;
            det.y = mr.center.y;
            int image_angle = ((360 - mr.angle) % 360 + 360) % 360;
            int grip_angle = image_angle % 180;
            if (grip_angle >= 90) grip_angle -= 180;
            det.angle = grip_angle;
            det.score = mr.score;
            detections.push_back(det);

            if (mr.score > last_best_score_) {
                last_best_score_ = mr.score;
            }
        }
    }

    // 4. 低分回退: 全图粗搜
    if (last_best_score_ < match_threshold_ && last_best_score_ > 0.0) {
        LOG_DEBUG("[边缘梯度] 候选区域最高分=%.4f < %.4f, 启动全图回退搜索...", 
                  last_best_score_, match_threshold_);

        cv::Rect full_roi(0, 0, img_w, img_h);
        if (roi_y_center_ >= 0 && roi_y_margin_ > 0) {
            int y_lo = std::max(0, roi_y_center_ - roi_y_margin_);
            int y_hi = std::min(img_h, roi_y_center_ + roi_y_margin_);
            full_roi = cv::Rect(0, y_lo, img_w, y_hi - y_lo);
        }

        MatchResult fb_match = matchInRegion(gray, full_roi, 5);

        if (fb_match.score >= match_threshold_) {
            DetResult det;
            det.x = fb_match.center.x;
            det.y = fb_match.center.y;
            int image_angle = ((360 - fb_match.angle) % 360 + 360) % 360;
            int grip_angle = image_angle % 180;
            if (grip_angle >= 90) grip_angle -= 180;
            det.angle = grip_angle;
            det.score = fb_match.score;
            detections.clear();
            detections.push_back(det);

            if (fb_match.score > last_best_score_) {
                last_best_score_ = fb_match.score;
            }
        }
    }

    auto t_match_end = std::chrono::high_resolution_clock::now();
    double match_ms = std::chrono::duration<double, std::milli>(t_match_end - t_match_start).count();

    // 5. 距离 NMS
    auto t_nms_start = std::chrono::high_resolution_clock::now();
    std::vector<DetResult> final_results;
    if (detections.size() <= 1) {
        final_results = std::move(detections);
    } else {
        double min_nms_dist = template_diag_ * 0.4;
        final_results = distanceNMS(detections, min_nms_dist);
    }
    auto t_nms_end = std::chrono::high_resolution_clock::now();
    double nms_ms = std::chrono::duration<double, std::milli>(t_nms_end - t_nms_start).count();

    // 6. 转换为 ObjectInfoList
    for (const auto& det : final_results) {
        ObjectInfo obj = ObjectInfo::Builder()
            .setX(det.x)
            .setY(det.y)
            .setAngle(static_cast<double>(det.angle))
            .setType(0)
            .build();
        result.add(obj);
    }

    last_results_ = final_results;

    auto t_total_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t_total_end - t_total_start).count();

    LOG_DEBUG("[边缘梯度] 检测耗时: 总计=%.1fms (分割=%.1fms, 匹配=%.1fms, NMS=%.1fms) 候选=%d 匹配=%d 最高分=%.4f", 
              total_ms, seg_ms, match_ms, nms_ms, candidates.size(), final_results.size(), last_best_score_);
    for (size_t i = 0; i < final_results.size(); ++i) {
        LOG_DEBUG("[边缘梯度]   #%d x=%f y=%f angle=%d score=%.4f", 
                  i, final_results[i].x, final_results[i].y, final_results[i].angle, final_results[i].score);
    }

    return result;
}

bool EdgeGradientDetector::saveAnnotated(const Frame& frame, const std::string& path) {
    if (last_results_.empty()) return false;

    cv::Mat image = frameToBGR(frame);
    if (image.empty()) return false;

    for (size_t i = 0; i < last_results_.size(); ++i) {
        const auto& det = last_results_[i];

        // 绘制旋转矩形框
        cv::RotatedRect rrect(
            cv::Point2f(static_cast<float>(det.x), static_cast<float>(det.y)),
            cv::Size2f(static_cast<float>(template_width_),
                       static_cast<float>(template_height_)),
            static_cast<float>(det.angle)
        );

        cv::Point2f vertices[4];
        rrect.points(vertices);
        for (int j = 0; j < 4; ++j) {
            cv::line(image, vertices[j], vertices[(j + 1) % 4],
                     cv::Scalar(0, 255, 0), 2);
        }

        // 绘制中心十字
        int cross_size = 10;
        cv::Point center(static_cast<int>(det.x), static_cast<int>(det.y));
        cv::line(image, cv::Point(center.x - cross_size, center.y),
                 cv::Point(center.x + cross_size, center.y),
                 cv::Scalar(0, 0, 255), 2);
        cv::line(image, cv::Point(center.x, center.y - cross_size),
                 cv::Point(center.x, center.y + cross_size),
                 cv::Scalar(0, 0, 255), 2);

        // 绘制文本标签
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
    }

    return cv::imwrite(path, image);
}

void EdgeGradientDetector::drawAnnotations(cv::Mat& image) {
    for (size_t i = 0; i < last_results_.size(); ++i) {
        const auto& det = last_results_[i];

        cv::RotatedRect rrect(
            cv::Point2f(static_cast<float>(det.x), static_cast<float>(det.y)),
            cv::Size2f(static_cast<float>(template_width_),
                       static_cast<float>(template_height_)),
            static_cast<float>(det.angle)
        );

        cv::Point2f vertices[4];
        rrect.points(vertices);
        for (int j = 0; j < 4; ++j) {
            cv::line(image, vertices[j], vertices[(j + 1) % 4],
                     cv::Scalar(0, 255, 0), 2);
        }

        int cs = 15;
        cv::Point center(static_cast<int>(det.x), static_cast<int>(det.y));
        cv::line(image, cv::Point(center.x - cs, center.y),
                 cv::Point(center.x + cs, center.y), cv::Scalar(0, 0, 255), 2);
        cv::line(image, cv::Point(center.x, center.y - cs),
                 cv::Point(center.x, center.y + cs), cv::Scalar(0, 0, 255), 2);

        char idx_label[32];
        std::snprintf(idx_label, sizeof(idx_label), "#%zu", i);
        cv::putText(image, idx_label, cv::Point(center.x + 10, center.y - 10),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2);

        char coord_label[64];
        std::snprintf(coord_label, sizeof(coord_label),
                      "(%.4f, %.4f, a=%.4f)", det.x, det.y, static_cast<double>(det.angle));
        cv::putText(image, coord_label, cv::Point(center.x + 10, center.y + 15),
                    cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);
    }
}

bool EdgeGradientDetector::loadTemplateInfo() {
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
