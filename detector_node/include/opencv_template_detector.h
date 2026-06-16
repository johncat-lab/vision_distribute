#ifndef OPENCV_TEMPLATE_DETECTOR_H
#define OPENCV_TEMPLATE_DETECTOR_H

#include "detector.h"
#include <opencv2/core.hpp>
#include <vector>
#include <atomic>

// OpenCV 模版匹配检测器
// 使用颜色预分割 + 多角度 NCC 模版匹配
// 适用于: 有纹理的固定尺寸产品, 绿色传送带背景
class OpenCvTemplateDetector : public Detector {
public:
    // template_dir: 模版文件所在目录 (需包含 template.png)
    // match_threshold: NCC 匹配阈值 (0-1)
    // angle_step_coarse: 粗搜索角度步长 (度)
    // angle_step_fine: 精搜索角度步长 (度)
    OpenCvTemplateDetector(const std::string& template_dir = "./template",
                           float match_threshold = 0.55f,
                           int angle_step_coarse = 10,
                           int angle_step_fine = 1);

    ~OpenCvTemplateDetector() = default;

    // 禁止拷贝
    OpenCvTemplateDetector(const OpenCvTemplateDetector&) = delete;
    OpenCvTemplateDetector& operator=(const OpenCvTemplateDetector&) = delete;

    // 加载模版并预计算旋转版本
    bool init() override;

    bool isReady() const override;

    // 执行模版匹配检测
    ObjectInfoList detect(const Frame& frame) override;

    bool saveAnnotated(const Frame& frame, const std::string& path) override;

    void drawAnnotations(cv::Mat& image) override;

    // ===== HSV 分割参数 (可调整) =====
    void setHsvRange(int h_low, int h_high, int s_low, int s_high, int v_low, int v_high);
    void setAreaRange(double min_area, double max_area);

    // 分割模式: "hsv" (绿色背景, 默认) / "value" (暗色背景, V 通道阈值)
    // "value" 模式下 findCandidateRegions 用 V > v_threshold 找前景区域
    void setSegmentMode(const std::string& mode);
    void setVThreshold(int v_threshold);
    void setGradientThreshold(int grad_threshold);

    void setMatchThreshold(float threshold) { match_threshold_ = threshold; }
    float getMatchThreshold() const { return match_threshold_; }

    void setRoiYCenter(int y_center);
    void setRoiYMargin(int y_margin);

    // 获取调试信息 (最近一帧的候选区域数、匹配分数)
    int getLastCandidateCount() const { return last_candidate_count_; }
    double getLastBestScore() const { return last_best_score_; }

    // 获取模板尺寸 (用于绘制检测结果边界)
    int getTemplateWidth() const { return template_img_.cols; }
    int getTemplateHeight() const { return template_img_.rows; }

    // 获取初始角度偏移 (用于反算图像中的视觉角度)
    double getInitialAngleOffset() const { return initial_angle_offset_; }

private:
    // Frame → cv::Mat (BGR)
    cv::Mat frameToBGR(const Frame& frame) const;

    // Frame → cv::Mat (灰度, Mono8 帧直接使用, 否则从 BGR 转换)
    cv::Mat frameToGray(const Frame& frame) const;

    // HSV 颜色分割，获取候选区域 (非绿色区域)
    std::vector<cv::Rect> findCandidateRegions(const cv::Mat& bgr) const;

    // 灰度分割，获取候选区域 (value/gradient 模式, Mono8 直连)
    std::vector<cv::Rect> findCandidateRegionsGray(const cv::Mat& gray) const;

    // 在指定 ROI 内进行多角度模版匹配 (支持灰度或彩色图像)
    // 返回: 最佳匹配分数, 最佳角度, 匹配中心位置 (图像坐标系)
    struct MatchResult {
        double score;
        int angle;
        cv::Point2d center;
    };
    MatchResult matchInRegion(const cv::Mat& image, const cv::Rect& roi, int coarse_step_override = 0) const;
    std::vector<MatchResult> matchInRegionMulti(const cv::Mat& image, const cv::Rect& roi, int coarse_step_override = 0) const;

    // 简单距离 NMS: 去除距离过近的重复检测
    struct DetResult {
        double x, y;
        int angle;
        double score;
    };
    std::vector<DetResult> distanceNMS(std::vector<DetResult>& results, double min_dist) const;

    // 配置参数
    std::string template_dir_;
    float match_threshold_;
    int angle_step_coarse_;
    int angle_step_fine_;

    // HSV 分割参数 (排除绿色背景)
    int hsv_h_low_ = 35, hsv_h_high_ = 85;
    int hsv_s_low_ = 50, hsv_s_high_ = 255;
    int hsv_v_low_ = 50, hsv_v_high_ = 255;

    // 面积筛选范围
    double min_area_ = 2000.0;
    double max_area_ = 500000.0;

    // 形态学核大小
    int morph_kernel_size_ = 7;
    cv::Mat morph_kernel_;
    cv::Mat morph_kernel_small_;

    // 分割模式: "hsv" / "value" / "gradient" (Sobel 梯度, 纹理产品+均匀bg)
    std::string segment_mode_ = "hsv";
    int v_threshold_ = 50;  // value 模式下的 V 通道阈值
    int grad_threshold_ = 30;  // gradient 模式下的 Sobel 幅值阈值

    int roi_y_center_ = -1;
    int roi_y_margin_ = -1;

    // 灰度匹配模式
    bool use_gray_mode_ = false;
    double initial_angle_offset_ = 0.0;  // 模板初始角度偏移 (度)

    // 从 template_info.txt 加载模板元信息
    bool loadTemplateInfo();

    // 预计算的旋转模版 (360 个, 每度一个)
    std::vector<cv::Mat> rotated_templates_;
    std::vector<cv::Mat> rotated_masks_;      // 对应的旋转 mask (可为空)
    std::vector<cv::Mat> rotated_templates_quarter_;  // 1/4 尺寸旋转模版 (金字塔粗搜索)
    std::vector<cv::Mat> rotated_templates_half_;     // 1/2 尺寸旋转模版 (金字塔精搜索)

    // 原始模版
    cv::Mat template_img_;
    cv::Mat template_mask_;                    // 前景 mask (0=背景, 255=产品)

    // 最近一次检测结果 (供 saveAnnotated 使用)
    std::vector<DetResult> last_results_;

    // matchTemplate 结果矩阵复用缓冲区
    mutable cv::Mat match_result_buf_;

    // 调试信息
    int last_candidate_count_ = 0;
    double last_best_score_ = 0.0;

    std::atomic<bool> ready_{false};
};

#endif // OPENCV_TEMPLATE_DETECTOR_H
