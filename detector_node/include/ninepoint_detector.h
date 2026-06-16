#ifndef NINEPOINT_DETECTOR_H
#define NINEPOINT_DETECTOR_H

#include "detector.h"
#include "calibration_core.h"
#include <opencv2/core.hpp>
#include <string>

class NinepointDetector : public Detector {
public:
    NinepointDetector();
    ~NinepointDetector() override = default;

    bool init() override;
    bool isReady() const override;
    ObjectInfoList detect(const Frame& frame) override;
    bool saveAnnotated(const Frame& frame, const std::string& path) override;
    void drawAnnotations(cv::Mat& image) override;

    void setCalibConfig(const CalibConfig& cfg);

    void setRoiYCenter(int y_center);
    void setRoiYMargin(int y_margin);

    bool loadTemplate(const std::string& image_path, double match_threshold = 0.7);
    void clearTemplate();

    const std::vector<CircleInfo>& getLastCircles() const;

private:
    cv::Mat frameToMat(const Frame& frame);

    CalibConfig calib_cfg_;
    bool ready_ = false;

    int roi_y_center_ = -1;
    int roi_y_margin_ = -1;

    bool use_template_ = false;
    cv::Mat template_img_;
    cv::Mat template_mask_;
    double match_threshold_ = 0.7;

    std::vector<CircleInfo> last_circles_;
};

#endif // NINEPOINT_DETECTOR_H
