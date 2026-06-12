#ifndef CONVEYOR_DETECTOR_H
#define CONVEYOR_DETECTOR_H

#include "detector.h"
#include <opencv2/core.hpp>
#include <vector>
#include <atomic>

// 传送带运动检测器
// 使用帧差法 + 模版匹配, 适用于传送带上连续运动的产品检测
class ConveyorDetector : public Detector {
public:
    ConveyorDetector(const std::string& template_dir = "./template",
                     float match_threshold = 0.55f,
                     int angle_step_fine = 1);

    ~ConveyorDetector() = default;

    // 禁止拷贝
    ConveyorDetector(const ConveyorDetector&) = delete;
    ConveyorDetector& operator=(const ConveyorDetector&) = delete;

    bool init() override;
    bool isReady() const override;
    ObjectInfoList detect(const Frame& frame) override;
    bool saveAnnotated(const Frame& frame, const std::string& path) override;

    // ===== 参数设置 =====
    void setRoiYCenter(int y_center);
    void setRoiYMargin(int y_margin);
    void setBgRefPath(const std::string& path);
    void setVerifyWithTemplate(bool verify);
    void setAreaRange(double min_area, double max_area);

    // ===== 调试信息 =====
    int getLastCandidateCount() const { return last_candidate_count_; }
    double getLastBestScore() const { return last_best_score_; }
    int getTemplateWidth() const { return template_img_.cols; }
    int getTemplateHeight() const { return template_img_.rows; }

private:
    // ===== 内部结构 =====
    struct MatchResult {
        double score;
        int angle;
        cv::Point center;
    };

    struct DetResult {
        double x, y;
        int angle;
        double score;
        bool low_confidence = false;
    };

    // ===== 方法 =====
    // Frame → cv::Mat
    cv::Mat frameToGray(const Frame& frame) const;

    // 帧差法获取运动区域
    std::vector<cv::Rect> findMotionRegions(const cv::Mat& gray);

    // 背景减除
    cv::Mat subtractBackground(const cv::Mat& gray) const;

    // 前景连通域提取
    std::vector<std::vector<cv::Point>> findForegroundBlobs(const cv::Mat& binary) const;

    // 模版验证 (二次确认)
    DetResult verifyWithTemplate(const cv::Mat& gray, double cx, double cy, int raw_image_angle);

    // 在指定 ROI 内进行多角度模版匹配
    MatchResult matchInRegion(const cv::Mat& image, const cv::Rect& roi) const;

    // 距离 NMS
    std::vector<DetResult> distanceNMS(std::vector<DetResult>& results, double min_dist) const;

    // 加载背景参考
    bool loadBackgroundRef();
    bool hasBackgroundRef() const;

    // 拍摄背景参考帧
    void captureBackground(const Frame& frame);

    // 加载模版元信息
    bool loadTemplateInfo();

    // ===== 成员变量 =====
    std::string template_dir_;
    float match_threshold_;
    int angle_step_fine_;

    // 帧差法参数
    int diff_threshold_ = 30;

    // ROI 参数
    int roi_y_center_ = -1;
    int roi_y_margin_ = 0;

    // 面积范围
    double min_area_ = 2000.0;
    double max_area_ = 500000.0;

    // 背景参考
    std::string bg_ref_path_;
    cv::Mat bg_ref_gray_;

    // 模版验证
    bool verify_with_template_ = true;

    // 灰度匹配
    bool use_gray_mode_ = true;
    double initial_angle_offset_ = 0.0;

    // 预计算旋转模版
    std::vector<cv::Mat> rotated_templates_;
    cv::Mat template_img_;
    cv::Mat template_mask_;
    cv::Mat morph_kernel_;
    cv::Mat morph_kernel_small_;

    // 匹配缓冲区
    mutable cv::Mat match_result_buf_;

    // 最近一次结果
    std::vector<DetResult> last_results_;
    std::vector<std::vector<cv::Point>> last_contours_;

    int last_candidate_count_ = 0;
    double last_best_score_ = 0.0;

    std::atomic<bool> ready_{false};
};

#endif // CONVEYOR_DETECTOR_H
