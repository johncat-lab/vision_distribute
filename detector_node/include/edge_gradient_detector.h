#ifndef EDGE_GRADIENT_DETECTOR_H
#define EDGE_GRADIENT_DETECTOR_H

#include "detector.h"
#include <opencv2/core.hpp>
#include <vector>
#include <atomic>

// 边缘梯度检测器
// 使用边缘提取 + 梯度方向匹配, 适用于规则形状产品的精确定位
class EdgeGradientDetector : public Detector {
public:
    EdgeGradientDetector(const std::string& template_dir = "./template",
                         float match_threshold = 0.55f,
                         int angle_step_coarse = 10,
                         int angle_step_fine = 1);

    ~EdgeGradientDetector() = default;

    // 禁止拷贝
    EdgeGradientDetector(const EdgeGradientDetector&) = delete;
    EdgeGradientDetector& operator=(const EdgeGradientDetector&) = delete;

    bool init() override;
    bool isReady() const override;
    ObjectInfoList detect(const Frame& frame) override;
    bool saveAnnotated(const Frame& frame, const std::string& path) override;

    // ===== 参数设置 =====
    void setHsvRange(int h_low, int h_high, int s_low, int s_high, int v_low, int v_high);
    void setAreaRange(double min_area, double max_area);
    void setSegmentMode(const std::string& mode);
    void setVThreshold(int v_threshold);
    void setGradientThreshold(int grad_threshold);
    void setRoiYCenter(int y_center);
    void setRoiYMargin(int y_margin);
    void setCannyThresholds(double low, double high);
    void setGradientTolerance(double tolerance_rad);
    void setSampleStep(int step);

    // ===== 调试信息 =====
    int getTemplateWidth() const { return template_width_; }
    int getTemplateHeight() const { return template_height_; }
    int getLastCandidateCount() const { return last_candidate_count_; }
    double getLastBestScore() const { return last_best_score_; }

    // ===== 绘制/保存 =====
    void drawAnnotations(cv::Mat& image);

private:
    // ===== 内部结构 =====
    struct EdgePoint {
        float x, y;
        float dx, dy;  // 梯度方向
        float grad_angle;  // 梯度角度 [0, 2π]
    };

    struct MatchResult {
        double score;
        int angle;
        cv::Point center;
    };

    struct DetResult {
        double x, y;
        int angle;
        double score;
    };

    // ===== 方法 =====
    // Frame → cv::Mat
    cv::Mat frameToBGR(const Frame& frame) const;
    cv::Mat frameToGray(const Frame& frame) const;

    // 候选区域
    std::vector<cv::Rect> findCandidateRegions(const cv::Mat& bgr) const;
    std::vector<cv::Rect> findCandidateRegionsGray(const cv::Mat& gray) const;

    // 边缘梯度匹配
    MatchResult matchInRegion(const cv::Mat& gray, const cv::Rect& roi,
                              int coarse_step_override = 0) const;
    std::vector<MatchResult> matchInRegionMulti(const cv::Mat& gray,
                                                 const cv::Rect& roi) const;

    // 提取模版边缘
    bool extractTemplateEdges(const cv::Mat& template_gray);

    // 模版元信息
    bool loadTemplateInfo();

    // 距离 NMS
    std::vector<DetResult> distanceNMS(std::vector<DetResult>& results, double min_dist) const;

    // 子像素细化
    static cv::Point2d subPixelRefine(const cv::Mat& result, const cv::Point& peak);

    // ===== 配置参数 =====
    std::string template_dir_;
    float match_threshold_;
    int angle_step_coarse_;
    int angle_step_fine_;

    // HSV 分割参数
    int hsv_h_low_ = 35, hsv_h_high_ = 85;
    int hsv_s_low_ = 50, hsv_s_high_ = 255;
    int hsv_v_low_ = 50, hsv_v_high_ = 255;

    double min_area_ = 2000.0;
    double max_area_ = 500000.0;

    std::string segment_mode_ = "hsv";
    int v_threshold_ = 50;
    int grad_threshold_ = 30;

    // 形态学
    int morph_kernel_size_ = 7;
    cv::Mat morph_kernel_;
    cv::Mat morph_kernel_small_;

    // Canny 参数
    double canny_low_ = 50.0;
    double canny_high_ = 150.0;

    // 梯度匹配参数
    double gradient_tolerance_ = 0.5;  // 弧度
    int sample_step_ = 2;

    // ROI
    int roi_y_center_ = -1;
    int roi_y_margin_ = 0;

    // 初始角度偏移
    double initial_angle_offset_ = 0.0;

    // 模版数据
    cv::Mat template_img_;
    int template_width_ = 0;
    int template_height_ = 0;
    int template_diag_ = 0;

    // 预提取的模版边缘
    std::vector<EdgePoint> template_edges_;
    int template_edge_count_ = 0;

    // 最近一次结果
    std::vector<DetResult> last_results_;

    // 调试
    int last_candidate_count_ = 0;
    double last_best_score_ = 0.0;

    std::atomic<bool> ready_{false};
};

#endif // EDGE_GRADIENT_DETECTOR_H
