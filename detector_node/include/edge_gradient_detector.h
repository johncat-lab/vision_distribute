#ifndef EDGE_GRADIENT_DETECTOR_H
#define EDGE_GRADIENT_DETECTOR_H

#include "detector.h"
#include <opencv2/core.hpp>
#include <vector>
#include <atomic>

// 边缘梯度模板匹配检测器
// 核心思想: 提取模板边缘点集 + 梯度方向, 在搜索图中通过梯度方向一致性
// 进行旋转不变匹配, 相比 NCC 具有以下优势:
//   - 光照完全无关 (只看梯度方向, 不看像素值)
//   - 部分遮挡鲁棒 (边缘点独立投票)
//   - 无需预存 360 个旋转模板 (旋转只改变梯度方向偏移)
class EdgeGradientDetector : public Detector {
public:
    // template_dir: 模版文件所在目录 (需包含 template.png 或 template_gray.png)
    // match_threshold: 梯度方向一致性匹配阈值 (0-1)
    // angle_step_coarse: 粗搜索角度步长 (度)
    // angle_step_fine: 精搜索角度步长 (度)
    EdgeGradientDetector(const std::string& template_dir = "./template",
                         float match_threshold = 0.4f,
                         int angle_step_coarse = 10,
                         int angle_step_fine = 1);

    ~EdgeGradientDetector() = default;

    EdgeGradientDetector(const EdgeGradientDetector&) = delete;
    EdgeGradientDetector& operator=(const EdgeGradientDetector&) = delete;

    bool init() override;
    bool isReady() const override;
    ObjectInfoList detect(const Frame& frame) override;
    bool saveAnnotated(const Frame& frame, const std::string& path) override;
    void drawAnnotations(cv::Mat& image) override;

    // ===== 分割参数 (与 OpenCvTemplateDetector 一致) =====
    void setHsvRange(int h_low, int h_high, int s_low, int s_high, int v_low, int v_high);
    void setAreaRange(double min_area, double max_area);
    void setSegmentMode(const std::string& mode);
    void setVThreshold(int v_threshold);
    void setGradientThreshold(int grad_threshold);
    void setRoiYCenter(int y_center);
    void setRoiYMargin(int y_margin);

    // ===== 边缘梯度特有参数 =====
    // Canny 双阈值 (用于提取边缘)
    void setCannyThresholds(double low, double high);

    // 梯度方向匹配容差 (弧度), 两个梯度方向差小于此值视为一致
    void setGradientTolerance(double tolerance_rad);

    // 模板边缘点采样间隔 (像素), 值越大采样越稀疏, 速度越快
    void setSampleStep(int step);

    // 调试信息
    int getLastCandidateCount() const { return last_candidate_count_; }
    double getLastBestScore() const { return last_best_score_; }
    int getTemplateWidth() const { return template_width_; }
    int getTemplateHeight() const { return template_height_; }
    double getInitialAngleOffset() const { return initial_angle_offset_; }

private:
    // Frame → cv::Mat (BGR)
    cv::Mat frameToBGR(const Frame& frame) const;

    // Frame → cv::Mat (灰度)
    cv::Mat frameToGray(const Frame& frame) const;

    // 候选区域分割 (复用 OpenCvTemplateDetector 的逻辑)
    std::vector<cv::Rect> findCandidateRegions(const cv::Mat& bgr) const;
    std::vector<cv::Rect> findCandidateRegionsGray(const cv::Mat& gray) const;

    // ===== 边缘梯度核心数据结构 =====

    // 模板边缘点: 相对于模板中心的偏移 + 梯度方向
    struct EdgePoint {
        float dx;           // 相对模板中心的 x 偏移
        float dy;           // 相对模板中心的 y 偏移
        float grad_angle;   // 梯度方向 (弧度, 0~2π)
    };

    // 在指定 ROI 内进行多角度边缘梯度匹配
    struct MatchResult {
        double score;
        int angle;
        cv::Point2d center;
    };
    MatchResult matchInRegion(const cv::Mat& gray, const cv::Rect& roi,
                              int coarse_step_override = 0) const;

    // 多目标匹配: 在 ROI 内找所有峰值
    std::vector<MatchResult> matchInRegionMulti(const cv::Mat& gray,
                                                 const cv::Rect& roi) const;

    // 距离 NMS
    struct DetResult {
        double x, y;
        int angle;
        double score;
    };
    std::vector<DetResult> distanceNMS(std::vector<DetResult>& results,
                                        double min_dist) const;

    // 加载模板元信息
    bool loadTemplateInfo();

    // 从模板图像提取边缘点集
    bool extractTemplateEdges(const cv::Mat& template_gray);

    // 配置参数
    std::string template_dir_;
    float match_threshold_;
    int angle_step_coarse_;
    int angle_step_fine_;

    // HSV 分割参数
    int hsv_h_low_ = 35, hsv_h_high_ = 85;
    int hsv_s_low_ = 50, hsv_s_high_ = 255;
    int hsv_v_low_ = 50, hsv_v_high_ = 255;

    // 面积筛选
    double min_area_ = 2000.0;
    double max_area_ = 500000.0;

    // 形态学核
    int morph_kernel_size_ = 7;
    cv::Mat morph_kernel_;
    cv::Mat morph_kernel_small_;

    // 分割模式
    std::string segment_mode_ = "hsv";
    int v_threshold_ = 50;
    int grad_threshold_ = 30;

    int roi_y_center_ = -1;
    int roi_y_margin_ = -1;

    // 边缘梯度特有参数
    double canny_low_ = 50.0;
    double canny_high_ = 100.0;
    double gradient_tolerance_ = 0.35;   // ~20° 容差
    int sample_step_ = 2;                // 边缘点采样间隔

    // 模板数据
    cv::Mat template_img_;               // 原始模板图像 (用于标注)
    int template_width_ = 0;
    int template_height_ = 0;
    double initial_angle_offset_ = 0.0;

    // 模板边缘点集 (核心数据)
    std::vector<EdgePoint> template_edges_;
    int template_edge_count_ = 0;

    // 模板对角线尺寸 (用于 ROI 扩展)
    int template_diag_ = 0;

    // 最近一次检测结果
    std::vector<DetResult> last_results_;

    // 调试信息
    int last_candidate_count_ = 0;
    double last_best_score_ = 0.0;

    std::atomic<bool> ready_{false};
};

#endif // EDGE_GRADIENT_DETECTOR_H
