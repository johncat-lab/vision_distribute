#include "opencv_template_detector.h"
#include "logger/logger.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <string>
#include <cmath>
#include <algorithm>
#include <chrono>

OpenCvTemplateDetector::OpenCvTemplateDetector(const std::string& template_dir,
                                               float match_threshold,
                                               int angle_step_coarse,
                                               int angle_step_fine)
    : template_dir_(template_dir)
    , match_threshold_(match_threshold)
    , angle_step_coarse_(angle_step_coarse)
    , angle_step_fine_(angle_step_fine) {
}

void OpenCvTemplateDetector::setHsvRange(int h_low, int h_high,
                                         int s_low, int s_high,
                                         int v_low, int v_high) {
    hsv_h_low_ = h_low;   hsv_h_high_ = h_high;
    hsv_s_low_ = s_low;   hsv_s_high_ = s_high;
    hsv_v_low_ = v_low;   hsv_v_high_ = v_high;
}

void OpenCvTemplateDetector::setAreaRange(double min_area, double max_area) {
    min_area_ = min_area;
    max_area_ = max_area;
}

void OpenCvTemplateDetector::setSegmentMode(const std::string& mode) {
    if (mode == "hsv" || mode == "value" || mode == "gradient") {
        segment_mode_ = mode;
        LOG_INFO("[模版检测] 分割模式: %s", mode.c_str());
    } else {
        LOG_WARN("[模版检测] 无效分割模式 '%s', 使用默认 'hsv'", mode.c_str());
    }
}

void OpenCvTemplateDetector::setVThreshold(int v_threshold) {
    v_threshold_ = v_threshold;
    LOG_INFO("[模版检测] V 通道阈值: %d", v_threshold);
}

void OpenCvTemplateDetector::setGradientThreshold(int grad_threshold) {
    grad_threshold_ = grad_threshold;
    LOG_INFO("[模版检测] 梯度阈值: %d", grad_threshold);
}

bool OpenCvTemplateDetector::init() {
    // 加载模版元信息
    loadTemplateInfo();

    // 优先尝试加载灰度模版
    std::string gray_path = template_dir_ + "/template_gray.png";
    template_img_ = cv::imread(gray_path, cv::IMREAD_GRAYSCALE);
    if (!template_img_.empty()) {
        use_gray_mode_ = true;
        LOG_INFO("[模版检测] 灰度模版已加载: %s (%dx%d)", gray_path.c_str(), template_img_.cols, template_img_.rows);
    } else {
        // 回退加载彩色模版
        std::string tmpl_path = template_dir_ + "/template.png";
        template_img_ = cv::imread(tmpl_path, cv::IMREAD_COLOR);
        if (template_img_.empty()) {
            LOG_ERROR("[模版检测] 无法加载模版图片: %s", tmpl_path.c_str());
            return false;
        }
        use_gray_mode_ = false;
        LOG_INFO("[模版检测] 彩色模版已加载: %s (%dx%d)", tmpl_path.c_str(), template_img_.cols, template_img_.rows);
    }

    if (use_gray_mode_) {
        LOG_INFO("[模版检测] 匹配模式: 灰度 (单通道, 高速)");
    } else {
        LOG_INFO("[模版检测] 匹配模式: 彩色 (三通道)");
    }

    if (initial_angle_offset_ != 0.0) {
        LOG_INFO("[模版检测] 初始角度偏移: %.1f°", initial_angle_offset_);
    }

    // 尝试加载前景 mask (GrabCut 生成)
    std::string mask_path = template_dir_ + "/template_mask.png";
    template_mask_ = cv::imread(mask_path, cv::IMREAD_GRAYSCALE);
    if (!template_mask_.empty()) {
        // 确保 mask 是二值的
        cv::threshold(template_mask_, template_mask_, 128, 255, cv::THRESH_BINARY);
        double mask_ratio = cv::countNonZero(template_mask_) / (double)(template_mask_.rows * template_mask_.cols);
        LOG_INFO("[模版检测] 前景 mask 已加载: %s (前景 %.1f%%)", mask_path.c_str(), mask_ratio * 100);

        // 用前景均值填充背景像素 → CCOEFF 自动忽略 (T-Tmean≈0)
        cv::Scalar fg_mean = cv::mean(template_img_, template_mask_);
        template_img_.setTo(fg_mean, ~template_mask_);
        LOG_INFO("[模版检测] 背景已填充前景均值");
    } else {
        LOG_INFO("[模版检测] 未找到前景 mask, 使用无 mask 模式");
    }

    // 预计算 360 个旋转版本的模版 (每1°一个)
    LOG_INFO("[模版检测] 正在预计算旋转模版 (0-359°)...");
    rotated_templates_.resize(360);
    rotated_masks_.resize(360);

    cv::Point2f center(template_img_.cols / 2.0f, template_img_.rows / 2.0f);

    // 计算旋转后不被截断的图像尺寸
    int diag = static_cast<int>(std::ceil(
        std::sqrt(template_img_.cols * template_img_.cols +
                  template_img_.rows * template_img_.rows)));

    // 旋转边界填充值: 使用 0 (黑色)
    // 对于暗背景场景, 黑色边界在 CCOEFF_NORMED 中能与暗背景产生正相关, 提升分数
    cv::Scalar border_val = use_gray_mode_ ? cv::Scalar(0) : cv::Scalar(0, 0, 0);

    for (int angle = 0; angle < 360; ++angle) {
        cv::Mat rot_mat = cv::getRotationMatrix2D(center, angle, 1.0);

        // 调整旋转中心到新画布中心
        rot_mat.at<double>(0, 2) += (diag - template_img_.cols) / 2.0;
        rot_mat.at<double>(1, 2) += (diag - template_img_.rows) / 2.0;

        cv::Mat rotated;
        cv::warpAffine(template_img_, rotated, rot_mat, cv::Size(diag, diag),
                       cv::INTER_LINEAR, cv::BORDER_CONSTANT, border_val);

        // 同步旋转 mask (用最近邻插值保持二值性)
        if (!template_mask_.empty()) {
            cv::Mat rotated_mask;
            cv::warpAffine(template_mask_, rotated_mask, rot_mat, cv::Size(diag, diag),
                           cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));
            rotated_masks_[angle] = rotated_mask;
        }

        rotated_templates_[angle] = rotated;
    }

    LOG_INFO("[模版检测] 旋转模版预计算完成 (360个, 尺寸 %dx%d)", diag, diag);

    // 预计算 1/4 尺寸旋转模版 (金字塔粗搜索用)
    if (diag >= 64) {
        rotated_templates_quarter_.resize(360);
        int quarter_size = (diag + 3) / 4;
        for (int angle = 0; angle < 360; ++angle) {
            cv::resize(rotated_templates_[angle], rotated_templates_quarter_[angle],
                       cv::Size(quarter_size, quarter_size), 0, 0, cv::INTER_AREA);
        }
        LOG_INFO("[模版检测] 金字塔模板已预计算 (1/4尺寸: %dx%d)", quarter_size, quarter_size);
    }

    // 预计算 1/2 尺寸旋转模版 (金字塔精搜索用, 大模板时显著加速)
    if (diag >= 200) {
        rotated_templates_half_.resize(360);
        int half_size = (diag + 1) / 2;
        for (int angle = 0; angle < 360; ++angle) {
            cv::resize(rotated_templates_[angle], rotated_templates_half_[angle],
                       cv::Size(half_size, half_size), 0, 0, cv::INTER_AREA);
        }
        LOG_INFO("[模版检测] 金字塔模板已预计算 (1/2尺寸: %dx%d)", half_size, half_size);
    }

    morph_kernel_ = cv::getStructuringElement(cv::MORPH_RECT,
        cv::Size(morph_kernel_size_, morph_kernel_size_));
    morph_kernel_small_ = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));

    ready_ = true;
    return true;
}

bool OpenCvTemplateDetector::isReady() const {
    return ready_.load();
}

cv::Mat OpenCvTemplateDetector::frameToBGR(const Frame& frame) const {
    size_t expected_mono = static_cast<size_t>(frame.width) * frame.height;
    size_t expected_rgb = expected_mono * 3;

    cv::Mat result;

    if (frame.data.size() == expected_rgb) {
        cv::Mat src(frame.height, frame.width, CV_8UC3,
                    const_cast<unsigned char*>(frame.data.data()));
        if (frame.pixelType == 0x02180015) {  // BGR8
            result = src.clone();
        } else {
            cv::cvtColor(src, result, cv::COLOR_RGB2BGR);
        }
    } else {
        // Mono8 或其他 → 转为 BGR
        cv::Mat src(frame.height, frame.width, CV_8UC1,
                    const_cast<unsigned char*>(frame.data.data()));
        cv::cvtColor(src, result, cv::COLOR_GRAY2BGR);
    }

    return result;
}

std::vector<cv::Rect> OpenCvTemplateDetector::findCandidateRegions(const cv::Mat& bgr) const {
    std::vector<cv::Rect> regions;

    if (segment_mode_ == "value") {
        // ===== V 通道阈值模式: 适用于暗色背景 =====
        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

        // 二值化: 亮度 > v_threshold_ 为前景 (产品)
        cv::Mat product_mask;
        cv::threshold(gray, product_mask, v_threshold_, 255, cv::THRESH_BINARY);

        // 形态学处理
        cv::morphologyEx(product_mask, product_mask, cv::MORPH_CLOSE, morph_kernel_);
        cv::morphologyEx(product_mask, product_mask, cv::MORPH_OPEN, morph_kernel_);

        // 找轮廓
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(product_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

        int tmpl_diag = rotated_templates_.empty() ? 0 : rotated_templates_[0].cols;

        for (const auto& contour : contours) {
            double area = cv::contourArea(contour);
            if (area < min_area_) continue;

            if (area <= max_area_) {
                cv::Rect bbox = cv::boundingRect(contour);
                regions.push_back(bbox);
            } else if (tmpl_diag > 0) {
                // 大轮廓: 以质心为中心裁剪出模板大小的搜索 ROI
                cv::Moments m = cv::moments(contour);
                if (m.m00 > 0) {
                    int cx = static_cast<int>(m.m10 / m.m00);
                    int cy = static_cast<int>(m.m01 / m.m00);
                    int half = tmpl_diag;
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

        LOG_DEBUG("[模版检测] value 模式: V>%d 找到 %d 个候选区域", v_threshold_, regions.size());
        return regions;
    }

    if (segment_mode_ == "gradient") {
        // ===== Sobel 梯度分割模式: 适用于纹理产品 + 均匀背景 =====
        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

        // Sobel 梯度幅值
        cv::Mat grad_x, grad_y;
        cv::Sobel(gray, grad_x, CV_16S, 1, 0, 3);
        cv::Sobel(gray, grad_y, CV_16S, 0, 1, 3);
        cv::convertScaleAbs(grad_x, grad_x);
        cv::convertScaleAbs(grad_y, grad_y);
        cv::Mat grad_mag;
        cv::addWeighted(grad_x, 0.5, grad_y, 0.5, 0, grad_mag);

        // 二值化: 高梯度 → 纹理区域 (产品)
        cv::Mat edge_mask;
        cv::threshold(grad_mag, edge_mask, grad_threshold_, 255, cv::THRESH_BINARY);

        // 形态学: 先膨胀连接断裂边缘, 再闭运算填充内部空隙
        cv::Mat kernel_small = morph_kernel_small_;
        cv::Mat kernel_big = morph_kernel_;
        cv::dilate(edge_mask, edge_mask, kernel_small);
        cv::morphologyEx(edge_mask, edge_mask, cv::MORPH_CLOSE, kernel_big);
        cv::morphologyEx(edge_mask, edge_mask, cv::MORPH_OPEN, kernel_big);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(edge_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

        int tmpl_diag = rotated_templates_.empty() ? 0 : rotated_templates_[0].cols;

        for (const auto& contour : contours) {
            double area = cv::contourArea(contour);
            if (area < min_area_) continue;

            if (area <= max_area_) {
                cv::Rect bbox = cv::boundingRect(contour);
                regions.push_back(bbox);
            } else if (tmpl_diag > 0) {
                cv::Moments m = cv::moments(contour);
                if (m.m00 > 0) {
                    int cx = static_cast<int>(m.m10 / m.m00);
                    int cy = static_cast<int>(m.m01 / m.m00);
                    int half = tmpl_diag;
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

        LOG_DEBUG("[模版检测] gradient 模式: grad>%d 找到 %d 个候选区域", grad_threshold_, regions.size());
        return regions;
    }

    // ===== HSV 绿色背景分割模式 (默认) =====
    // BGR → HSV
    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);

    // 生成绿色背景 mask
    cv::Mat green_mask;
    cv::inRange(hsv,
                cv::Scalar(hsv_h_low_, hsv_s_low_, hsv_v_low_),
                cv::Scalar(hsv_h_high_, hsv_s_high_, hsv_v_high_),
                green_mask);

    // 检测绿色背景占比，如果太低则发出警告
    double green_ratio = static_cast<double>(cv::countNonZero(green_mask)) / green_mask.total();
    if (green_ratio < 0.05) {
        LOG_WARN("[模版检测] 警告: 绿色背景占比仅 %.1f%%! 建议使用 --segment-mode value (暗色背景模式)", green_ratio * 100);
    }

    // 取反 → 非绿色区域 (产品候选)
    cv::Mat product_mask;
    cv::bitwise_not(green_mask, product_mask);

    // 形态学处理: 先闭运算填充小孔, 再开运算去除噪点
    cv::morphologyEx(product_mask, product_mask, cv::MORPH_CLOSE, morph_kernel_);
    cv::morphologyEx(product_mask, product_mask, cv::MORPH_OPEN, morph_kernel_);

    // 找轮廓
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(product_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    int tmpl_diag = rotated_templates_.empty() ? 0 : rotated_templates_[0].cols;

    // 面积筛选
    for (const auto& contour : contours) {
        double area = cv::contourArea(contour);
        if (area < min_area_) continue;

        if (area <= max_area_) {
            cv::Rect bbox = cv::boundingRect(contour);
            regions.push_back(bbox);
        } else if (tmpl_diag > 0) {
            cv::Moments m = cv::moments(contour);
            if (m.m00 > 0) {
                int cx = static_cast<int>(m.m10 / m.m00);
                int cy = static_cast<int>(m.m01 / m.m00);
                int half = tmpl_diag;
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

OpenCvTemplateDetector::MatchResult
OpenCvTemplateDetector::matchInRegion(const cv::Mat& image, const cv::Rect& roi, int coarse_step_override) const {
    MatchResult best;
    best.score = 0.0;
    best.angle = 0;
    best.center = cv::Point(roi.x + roi.width / 2, roi.y + roi.height / 2);

    int effective_coarse_step = (coarse_step_override > 0) ? coarse_step_override : angle_step_coarse_;

    // 扩展 ROI (确保模版匹配有足够空间)
    int tmpl_diag = rotated_templates_[0].cols;  // 旋转后的对角线尺寸
    int expand = tmpl_diag / 2 + 10;

    cv::Rect expanded_roi(
        std::max(0, roi.x - expand),
        std::max(0, roi.y - expand),
        std::min(image.cols - std::max(0, roi.x - expand), roi.width + 2 * expand),
        std::min(image.rows - std::max(0, roi.y - expand), roi.height + 2 * expand)
    );

    // 确保 ROI 足够大以进行模版匹配
    if (expanded_roi.width < tmpl_diag || expanded_roi.height < tmpl_diag) {
        return best;
    }

    // =====================================================================
    // 金字塔模式: 1/4 尺寸粗搜索 + 原尺寸小 ROI 精搜索
    // =====================================================================
    if (!rotated_templates_quarter_.empty()) {
        const int scale = 4;
        int quarter_tmpl_diag = rotated_templates_quarter_[0].cols;

        // Phase 1: 1/4 尺寸粗搜索
        cv::Mat search_small;
        cv::resize(image(expanded_roi), search_small,
                   cv::Size(), 1.0 / scale, 1.0 / scale, cv::INTER_AREA);

        int best_coarse_angle = 0;
        double best_coarse_score = 0.0;
        cv::Point best_coarse_loc;

        for (int angle = 0; angle < 360; angle += effective_coarse_step) {
            const cv::Mat& tmpl = rotated_templates_quarter_[angle];
            if (search_small.cols < tmpl.cols || search_small.rows < tmpl.rows) continue;

            cv::matchTemplate(search_small, tmpl, match_result_buf_, cv::TM_CCOEFF_NORMED);

            double max_val;
            cv::Point max_loc;
            cv::minMaxLoc(match_result_buf_, nullptr, &max_val, nullptr, &max_loc);

            if (max_val > best_coarse_score) {
                best_coarse_score = max_val;
                best_coarse_angle = angle;
                best_coarse_loc = max_loc;
            }

            if (best_coarse_score > 0.85) break;
        }

        // 粗搜索分数太低，直接返回 (放宽阈值: 低分辨率下分数普遍偏低)
        if (best_coarse_score < match_threshold_ * 0.3) {
            return best;
        }

        // Phase 2: 精搜索 — 在粗匹配位置周围裁剪小 ROI
        // 将粗搜索位置映射回原图坐标
        int coarse_cx = expanded_roi.x + static_cast<int>((best_coarse_loc.x + quarter_tmpl_diag / 2.0) * scale);
        int coarse_cy = expanded_roi.y + static_cast<int>((best_coarse_loc.y + quarter_tmpl_diag / 2.0) * scale);

        // 精搜索 ROI: 模板大小 + 定位不确定余量 (增大余量以容纳粗搜索偏差)
        int fine_half = tmpl_diag / 2 + scale * 8;  // ±32px 余量
        cv::Rect fine_roi(
            std::max(0, coarse_cx - fine_half),
            std::max(0, coarse_cy - fine_half),
            std::min(image.cols - std::max(0, coarse_cx - fine_half), fine_half * 2),
            std::min(image.rows - std::max(0, coarse_cy - fine_half), fine_half * 2)
        );

        if (fine_roi.width < tmpl_diag || fine_roi.height < tmpl_diag) {
            fine_roi = expanded_roi;  // 回退到全展开 ROI
        }

        // 精搜索角度范围: ±(粗步长/2 + 2)°
        int fine_half_range = effective_coarse_step / 2 + 2;
        int fine_start = best_coarse_angle - fine_half_range;
        int fine_end = best_coarse_angle + fine_half_range;

        double best_fine_score = 0.0;
        int best_fine_angle = best_coarse_angle;
        cv::Point best_fine_loc;
        int fine_scale = 1;  // 精搜索使用的缩放倍率

        // 优先使用 1/2 尺寸模板精搜索 (大幅减少计算量)
        if (!rotated_templates_half_.empty()) {
            fine_scale = 2;
            int half_tmpl_size = rotated_templates_half_[0].cols;
            cv::Mat fine_search;
            cv::resize(image(fine_roi), fine_search,
                       cv::Size(), 0.5, 0.5, cv::INTER_AREA);

            for (int angle = fine_start; angle <= fine_end; angle += angle_step_fine_) {
                int norm_angle = ((angle % 360) + 360) % 360;
                const cv::Mat& tmpl = rotated_templates_half_[norm_angle];

                if (fine_search.cols < tmpl.cols || fine_search.rows < tmpl.rows) continue;

                cv::matchTemplate(fine_search, tmpl, match_result_buf_, cv::TM_CCOEFF_NORMED);

                double max_val;
                cv::Point max_loc;
                cv::minMaxLoc(match_result_buf_, nullptr, &max_val, nullptr, &max_loc);

                if (max_val > best_fine_score) {
                    best_fine_score = max_val;
                    best_fine_angle = norm_angle;
                    best_fine_loc = max_loc;
                }

                // 高分早停
                if (best_fine_score > 0.92) break;
            }
        } else {
            // 回退: 全尺寸精搜索
            cv::Mat fine_search = image(fine_roi);

            for (int angle = fine_start; angle <= fine_end; angle += angle_step_fine_) {
                int norm_angle = ((angle % 360) + 360) % 360;
                const cv::Mat& tmpl = rotated_templates_[norm_angle];

                if (fine_search.cols < tmpl.cols || fine_search.rows < tmpl.rows) continue;

                cv::matchTemplate(fine_search, tmpl, match_result_buf_, cv::TM_CCOEFF_NORMED);

                double max_val;
                cv::Point max_loc;
                cv::minMaxLoc(match_result_buf_, nullptr, &max_val, nullptr, &max_loc);

                if (max_val > best_fine_score) {
                    best_fine_score = max_val;
                    best_fine_angle = norm_angle;
                    best_fine_loc = max_loc;
                }

                // 高分早停
                if (best_fine_score > 0.92) break;
            }
        }

        // 映射回原图坐标
        best.score = best_fine_score;
        best.angle = best_fine_angle;
        if (fine_scale == 2) {
            int half_tmpl_center = rotated_templates_half_[0].cols / 2;
            best.center = cv::Point(
                fine_roi.x + (best_fine_loc.x + half_tmpl_center) * 2,
                fine_roi.y + (best_fine_loc.y + half_tmpl_center) * 2
            );
        } else {
            int half_tmpl_orig = tmpl_diag / 2;
            best.center = cv::Point(
                fine_roi.x + best_fine_loc.x + half_tmpl_orig,
                fine_roi.y + best_fine_loc.y + half_tmpl_orig
            );
        }

        return best;
    }

    // =====================================================================
    // 原始模式 (无金字塔模板时的回退)
    // =====================================================================
    cv::Mat search_region = image(expanded_roi);

    // 粗搜索: 每 angle_step_coarse_ 度匹配一次
    int best_coarse_angle = 0;
    double best_coarse_score = 0.0;
    cv::Point best_coarse_loc;

    for (int angle = 0; angle < 360; angle += effective_coarse_step) {
        const cv::Mat& tmpl = rotated_templates_[angle];

        if (search_region.cols < tmpl.cols || search_region.rows < tmpl.rows) {
            continue;
        }

        cv::matchTemplate(search_region, tmpl, match_result_buf_, cv::TM_CCOEFF_NORMED);

        double max_val;
        cv::Point max_loc;
        cv::minMaxLoc(match_result_buf_, nullptr, &max_val, nullptr, &max_loc);

        if (max_val > best_coarse_score) {
            best_coarse_score = max_val;
            best_coarse_angle = angle;
            best_coarse_loc = max_loc;
        }

        if (best_coarse_score > 0.85) break;
    }

    if (best_coarse_score < match_threshold_ * 0.5) {
        return best;
    }

    // 精搜索: 在最佳粗角度附近匹配
    int fine_start = best_coarse_angle - effective_coarse_step / 2;
    int fine_end = best_coarse_angle + effective_coarse_step / 2;

    int best_fine_angle = best_coarse_angle;
    double best_fine_score = best_coarse_score;
    cv::Point best_fine_loc = best_coarse_loc;

    for (int angle = fine_start; angle <= fine_end; angle += angle_step_fine_) {
        int norm_angle = ((angle % 360) + 360) % 360;

        if (norm_angle == best_coarse_angle) continue;

        const cv::Mat& tmpl = rotated_templates_[norm_angle];

        if (search_region.cols < tmpl.cols || search_region.rows < tmpl.rows) {
            continue;
        }

        cv::matchTemplate(search_region, tmpl, match_result_buf_, cv::TM_CCOEFF_NORMED);

        double max_val;
        cv::Point max_loc;
        cv::minMaxLoc(match_result_buf_, nullptr, &max_val, nullptr, &max_loc);

        if (max_val > best_fine_score) {
            best_fine_score = max_val;
            best_fine_angle = norm_angle;
            best_fine_loc = max_loc;
        }

        // 高分早停
        if (best_fine_score > 0.92) break;
    }

    int half_tmpl = tmpl_diag / 2;
    best.score = best_fine_score;
    best.angle = best_fine_angle;
    best.center = cv::Point(
        expanded_roi.x + best_fine_loc.x + half_tmpl,
        expanded_roi.y + best_fine_loc.y + half_tmpl
    );

    return best;
}

std::vector<OpenCvTemplateDetector::DetResult>
OpenCvTemplateDetector::distanceNMS(std::vector<DetResult>& results, double min_dist) const {
    if (results.empty()) return results;

    // 按分数降序排序
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

ObjectInfoList OpenCvTemplateDetector::detect(const Frame& frame) {
    ObjectInfoList result;

    if (!ready_) return result;

    auto t_total_start = std::chrono::high_resolution_clock::now();

    cv::Mat bgr = frameToBGR(frame);
    if (bgr.empty()) return result;

    // 1. 颜色分割找候选区域
    auto t_seg_start = std::chrono::high_resolution_clock::now();
    std::vector<cv::Rect> candidates = findCandidateRegions(bgr);
    auto t_seg_end = std::chrono::high_resolution_clock::now();
    double seg_ms = std::chrono::duration<double, std::milli>(t_seg_end - t_seg_start).count();

    int tmpl_diag = rotated_templates_[0].cols;
    cv::Point img_center(bgr.cols / 2, bgr.rows / 2);

    // 按距图像中心距离升序排序 (中心优先: 传送带位置固定)
    const int max_candidates = 5;
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

    last_candidate_count_ = static_cast<int>(candidates.size());
    last_best_score_ = 0.0;

    if (candidates.empty()) {
        auto t_total_end = std::chrono::high_resolution_clock::now();
        double total_ms = std::chrono::duration<double, std::milli>(t_total_end - t_total_start).count();
        LOG_DEBUG("[模版检测] 检测耗时: 总计=%.1fms (分割=%.1fms, 匹配=0ms, NMS=0ms) 候选=0 匹配=0", total_ms, seg_ms);
        last_results_.clear();
        return result;
    }

    // 2. 准备匹配用图像
    cv::Mat match_img;
    if (use_gray_mode_) {
        size_t expected_mono = static_cast<size_t>(frame.width) * frame.height;
        if (frame.data.size() == expected_mono) {
            match_img = cv::Mat(frame.height, frame.width, CV_8UC1,
                                const_cast<unsigned char*>(frame.data.data())).clone();
        } else {
            cv::cvtColor(bgr, match_img, cv::COLOR_BGR2GRAY);
        }
    } else {
        match_img = bgr;
    }

    // 3. 对每个候选区域进行模版匹配
    auto t_match_start = std::chrono::high_resolution_clock::now();
    std::vector<DetResult> detections;

    for (const auto& roi : candidates) {
        MatchResult match = matchInRegion(match_img, roi);

        if (match.score > last_best_score_) {
            last_best_score_ = match.score;
        }

        if (match.score >= match_threshold_) {
            DetResult det;
            det.x = match.center.x;
            det.y = match.center.y;
            int image_angle = ((360 - match.angle) % 360 + 360) % 360;
            int grip_angle = image_angle % 180;
            if (grip_angle >= 90) grip_angle -= 180;
            det.angle = grip_angle;
            det.score = match.score;
            detections.push_back(det);

            if (match.score > 0.90) break;
        }
    }

    // 低分回退: 候选区域匹配分数偏低时, 用全图 1/4 缩放 + 5° 步长粗搜 + 1/2 缩放精搜
    // 使用配置的匹配阈值作为回退触发条件
    if (last_best_score_ < match_threshold_ && last_best_score_ > 0.0
        && !rotated_templates_quarter_.empty()) {
        LOG_DEBUG("[模版检测] 候选区域最高分=%.4f < %.4f, 启动全图回退搜索...", last_best_score_, match_threshold_);

        const int scale = 4;
        int quarter_tmpl_diag = rotated_templates_quarter_[0].cols;
        int fb_coarse_step = 5;

        cv::Mat search_small;
        cv::resize(match_img, search_small,
                   cv::Size(), 1.0 / scale, 1.0 / scale, cv::INTER_AREA);

        int fb_best_angle = 0;
        double fb_best_score = 0.0;
        cv::Point fb_best_loc;

        for (int angle = 0; angle < 360; angle += fb_coarse_step) {
            const cv::Mat& tmpl = rotated_templates_quarter_[angle];
            if (search_small.cols < tmpl.cols || search_small.rows < tmpl.rows) continue;

            cv::matchTemplate(search_small, tmpl, match_result_buf_, cv::TM_CCOEFF_NORMED);

            double max_val;
            cv::Point max_loc;
            cv::minMaxLoc(match_result_buf_, nullptr, &max_val, nullptr, &max_loc);

            if (max_val > fb_best_score) {
                fb_best_score = max_val;
                fb_best_angle = angle;
                fb_best_loc = max_loc;
            }
        }

        if (fb_best_score > match_threshold_ * 0.3) {
            int coarse_cx = static_cast<int>((fb_best_loc.x + quarter_tmpl_diag / 2.0) * scale);
            int coarse_cy = static_cast<int>((fb_best_loc.y + quarter_tmpl_diag / 2.0) * scale);

            int fb_half = tmpl_diag / 2 + scale * 8;
            cv::Rect fb_roi(
                std::max(0, coarse_cx - fb_half),
                std::max(0, coarse_cy - fb_half),
                std::min(match_img.cols - std::max(0, coarse_cx - fb_half), fb_half * 2),
                std::min(match_img.rows - std::max(0, coarse_cy - fb_half), fb_half * 2)
            );

            double fb_fine_score = 0.0;
            int fb_fine_angle = fb_best_angle;
            cv::Point fb_fine_center(coarse_cx, coarse_cy);

            if (!rotated_templates_half_.empty() &&
                fb_roi.width >= tmpl_diag && fb_roi.height >= tmpl_diag) {
                int half_tmpl_size = rotated_templates_half_[0].cols;
                cv::Mat fine_search;
                cv::resize(match_img(fb_roi), fine_search,
                           cv::Size(), 0.5, 0.5, cv::INTER_AREA);

                int fine_half_range = fb_coarse_step / 2 + 2;
                int fine_start = fb_best_angle - fine_half_range;
                int fine_end = fb_best_angle + fine_half_range;
                cv::Point best_fine_loc;

                for (int angle = fine_start; angle <= fine_end; angle += angle_step_fine_) {
                    int norm_angle = ((angle % 360) + 360) % 360;
                    const cv::Mat& tmpl = rotated_templates_half_[norm_angle];
                    if (fine_search.cols < tmpl.cols || fine_search.rows < tmpl.rows) continue;

                    cv::matchTemplate(fine_search, tmpl, match_result_buf_, cv::TM_CCOEFF_NORMED);
                    double max_val;
                    cv::Point max_loc;
                    cv::minMaxLoc(match_result_buf_, nullptr, &max_val, nullptr, &max_loc);

                    if (max_val > fb_fine_score) {
                        fb_fine_score = max_val;
                        fb_fine_angle = norm_angle;
                        best_fine_loc = max_loc;
                    }
                }

                if (fb_fine_score > 0.0) {
                    int half_tmpl_center = half_tmpl_size / 2;
                    fb_fine_center = cv::Point(
                        fb_roi.x + (best_fine_loc.x + half_tmpl_center) * 2,
                        fb_roi.y + (best_fine_loc.y + half_tmpl_center) * 2
                    );
                }
            } else {
                fb_fine_score = fb_best_score;
            }

            if (fb_fine_score > last_best_score_) {
                LOG_DEBUG("[模版检测] 全图回退找到更优匹配: 分数=%.4f 角度=%d 位置=(%d,%d)", fb_fine_score, fb_fine_angle, fb_fine_center.x, fb_fine_center.y);
                last_best_score_ = fb_fine_score;
                if (fb_fine_score >= match_threshold_) {
                    detections.clear();
                    DetResult det;
                    det.x = fb_fine_center.x;
                    det.y = fb_fine_center.y;
                    int image_angle = ((360 - fb_fine_angle) % 360 + 360) % 360;
                    int grip_angle = image_angle % 180;
                    if (grip_angle >= 90) grip_angle -= 180;
                    det.angle = grip_angle;
                    det.score = fb_fine_score;
                    detections.push_back(det);
                }
            }
        }
    }

    auto t_match_end = std::chrono::high_resolution_clock::now();
    double match_ms = std::chrono::duration<double, std::milli>(t_match_end - t_match_start).count();

    // 4. 距离 NMS (模版对角线长度的一半作为最小距离)
    auto t_nms_start = std::chrono::high_resolution_clock::now();
    std::vector<DetResult> final_results;
    if (detections.size() <= 1) {
        final_results = std::move(detections);
    } else {
        double min_nms_dist = rotated_templates_[0].cols * 0.4;
        final_results = distanceNMS(detections, min_nms_dist);
        if (final_results.size() > 1) {
            final_results.resize(1);
        }
    }
    auto t_nms_end = std::chrono::high_resolution_clock::now();
    double nms_ms = std::chrono::duration<double, std::milli>(t_nms_end - t_nms_start).count();

    // 5. 转换为 ObjectInfoList
    for (const auto& det : final_results) {
        ObjectInfo obj = ObjectInfo::Builder()
            .setX(det.x)
            .setY(det.y)
            .setAngle(static_cast<double>(det.angle))
            .setType(0)
            .build();
        result.add(obj);
    }

    // 保存结果供 saveAnnotated 使用
    last_results_ = final_results;

    auto t_total_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t_total_end - t_total_start).count();

    LOG_DEBUG("[模版检测] 检测耗时: 总计=%.1fms (分割=%.1fms, 匹配=%.1fms, NMS=%.1fms) 候选=%d 匹配=%d 最高分=%.4f", 
              total_ms, seg_ms, match_ms, nms_ms, candidates.size(), final_results.size(), last_best_score_);

    return result;
}

bool OpenCvTemplateDetector::saveAnnotated(const Frame& frame, const std::string& path) {
    if (last_results_.empty()) return false;

    cv::Mat image = frameToBGR(frame);
    if (image.empty()) return false;

    for (size_t i = 0; i < last_results_.size(); ++i) {
        const auto& det = last_results_[i];

        // 绘制旋转矩形框
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
        snprintf(label, sizeof(label), "#%zu x=%.1f y=%.1f a=%d s=%.2f",
                 i, det.x, det.y, det.angle, det.score);

        cv::Point text_pos(center.x + 15, center.y - 10);
        // 文字背景
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

bool OpenCvTemplateDetector::loadTemplateInfo() {
    std::string info_path = template_dir_ + "/template_info.txt";
    std::ifstream ifs(info_path);
    if (!ifs.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(ifs, line)) {
        // 跳过注释和空行
        if (line.empty() || line[0] == '#') continue;

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        if (key == "initial_angle_offset") {
            initial_angle_offset_ = std::stod(value);
        }
        // 其他字段暂不需要在检测器中使用
    }

    ifs.close();
    return true;
}
