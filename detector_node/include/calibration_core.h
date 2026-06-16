#ifndef CALIBRATION_CORE_H
#define CALIBRATION_CORE_H

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>
#include <cstdint>

class ObjectInfoList;

enum class CoordinateMode {
    PIXEL = 0,           // 输出像素坐标
    CAMERA_PHYSICAL = 1, // 输出相机物理mm坐标（默认）
    ROBOT_WORLD = 2      // 输出机器人世界mm坐标
};

enum class ShapeType {
    NONE = 0,
    CIRCLE = 1,
    RECTANGLE = 2,
    OTHER = 3
};

struct ShapeInfo {
    ShapeType type = ShapeType::NONE;
    cv::Point2d center;
    double radius = 0;
    cv::RotatedRect rrect;
    double circularity = 0;
    double area = 0;
};

struct CalibConfig {
    int camera_index = 0;
    std::string calib_path;
    std::string save_dir = "./calibration";
    std::string output_path = "./calibration_result.yaml";
    double min_circle_area = 1000;
    double max_circle_area = 100000;
    double min_circularity = 0.7;
    double reproj_error_threshold = 1.0;
    int min_poses = 4;
    double min_angle_range = 90.0;
    std::string tcp_output_path = "./tcp_calibration_result.yaml";
    int roi_y_center = -1;
    int roi_y_margin = -1;
    std::string affine_xml_path;  // 仿射矩阵XML文件路径（生产模式加载用）
};

struct CircleInfo {
    cv::Point2d center;
    double area;
    double circularity;
    double radius;
    int index;
};

struct FlangePose {
    double x;
    double y;
    double theta;
};

struct TCPCalibResult {
    double dx;
    double dy;
    double dtheta;
    double tcp_world_x;
    double tcp_world_y;
    double position_mean_error;
    double position_max_error;
    int num_poses;
    double angle_range;
};

struct IntrinsicCalibResult {
    cv::Mat camera_matrix;
    cv::Mat dist_coeffs;
    std::vector<cv::Mat> rvecs;
    std::vector<cv::Mat> tvecs;
    double rms_error = 0.0;
    int image_count = 0;
    int image_width = 0;
    int image_height = 0;
    bool valid = false;
};

enum class ConveyorOriginMode {
    TOP_LEFT = 0,
    TOP_RIGHT = 1,
    BOTTOM_LEFT = 2,
    BOTTOM_RIGHT = 3
};

struct ConveyorCalibResult {
    cv::Mat H_pixel_to_world;
    cv::Mat H_world_to_pixel;
    cv::Mat conveyor_origin;
    double conveyor_direction = 0.0;
    double rms_error = 0.0;
    bool valid = false;
    int origin_mode = 0;
};

struct AffineResult {
    cv::Mat affine_robot;   // pixel → robot world (mm)
    cv::Mat affine_camera;  // pixel → camera physical (mm)
    double mean_error = 0.0;
    double max_error = 0.0;
    bool valid = false;
};

cv::Point2d refineCircleCenter(const cv::Mat& gray,
                               const cv::Point2d& rough_center,
                               double radius,
                               const std::vector<cv::Point>& contour,
                               double* out_fitted_radius = nullptr);

cv::Point2d refineCrossCenterByHough(const cv::Mat& gray,
                                       const cv::Point2d& rough_center,
                                       double radius);

cv::Point2d refineCrossCenterByBrightCentroid(const cv::Mat& gray,
                                                const cv::Point2d& rough_center,
                                                double radius);

std::vector<CircleInfo> detectCircles(const cv::Mat& image, const CalibConfig& cfg);

cv::Rect findCalibrationBoardROI(const cv::Mat& gray, int roi_y_center = -1, int roi_y_margin = -1);

double rectangleFitError(const std::vector<CircleInfo>& four_points);

std::vector<CircleInfo> chooseFourPoints(std::vector<CircleInfo>& candidates);

std::vector<CircleInfo> detectFourCircles(const cv::Mat& image, const CalibConfig& cfg);

std::vector<CircleInfo> refineFourCirclesByTemplate(const cv::Mat& gray,
                                                     const cv::Mat& search_mask,
                                                     const std::vector<CircleInfo>& rough_circles,
                                                     double match_threshold = 0.6);

bool detectSingleCircle(const cv::Mat& image, const cv::Rect& roi,
                        cv::Point2d& out_center, double& out_radius);

ShapeInfo detectShape(const cv::Mat& image, const cv::Rect& roi);

cv::Mat cropShapeTemplate(const cv::Mat& image,
                           const ShapeInfo& shape,
                           double padding = 1.3,
                           cv::Mat* out_mask = nullptr);

cv::Mat cropCircleTemplate(const cv::Mat& image,
                           const cv::Point2d& center, double radius,
                           double padding = 1.3,
                           cv::Mat* out_mask = nullptr);

std::vector<CircleInfo> detectCirclesByTemplate(const cv::Mat& image,
                                                 const cv::Mat& tmpl,
                                                 const cv::Mat& tmpl_mask,
                                                 double match_threshold = 0.7,
                                                 double min_dist = 30.0);

void drawCircles(cv::Mat& image, const std::vector<CircleInfo>& circles);

AffineResult computeAffine(const std::vector<cv::Point2d>& pixel_pts,
                           const std::vector<cv::Point2d>& robot_pts,
                           const CalibConfig& cfg);

cv::Point2d pixelToRobot(const cv::Point2d& pixel, const cv::Mat& affine_matrix);

std::vector<cv::Point2d> generateCameraPhysicalCoords(const std::vector<cv::Point2d>& robot_pts);

cv::Mat loadAffineMatrix(const std::string& path);

void transformObjectList(ObjectInfoList& list, const cv::Mat& affine_matrix);

bool solveTCPOffset(const std::vector<FlangePose>& input_poses,
                    const CalibConfig& cfg,
                    TCPCalibResult& result);

double calibrateRotationOffsetMethodA(double theta_flange, double theta_ref);

double calibrateRotationOffsetMethodB(double theta_flange,
                                      cv::Point2d refA,
                                      cv::Point2d refB);

double calibrateRotationOffsetMethodC(double dx, double dy, bool leftSide);

bool saveTCPCalibResult(const TCPCalibResult& result, const std::string& output_path);

bool saveAffineResult(const AffineResult& result, const std::string& output_path);

CalibConfig loadCalibConfig(const std::string& config_path);

bool saveCalibConfig(const CalibConfig& cfg, const std::string& config_path);

std::vector<cv::Point2d> loadRobotCoords(const std::string& filepath);

std::vector<FlangePose> loadFlangePoses(const std::string& filepath);

cv::Mat frameToBGR(const uint8_t* data, int width, int height, int channels, uint32_t pixel_type = 0);

bool detectCheckerboard(const cv::Mat& image,
                        const cv::Size& board_size,
                        std::vector<cv::Point2f>& corners);

IntrinsicCalibResult calibrateIntrinsic(
    const std::vector<std::string>& image_paths,
    const cv::Size& board_size,
    double square_size);

bool saveIntrinsicCalibResult(const IntrinsicCalibResult& result,
                              const std::string& output_path);

bool loadIntrinsicCalibResult(const std::string& input_path,
                              IntrinsicCalibResult& result);

ConveyorCalibResult calibrateConveyor(
    const cv::Mat& image,
    const cv::Size& board_size,
    double square_size,
    int origin_mode = 0,
    const cv::Mat& camera_matrix = cv::Mat(),
    const cv::Mat& dist_coeffs = cv::Mat());

cv::Point2d pixelToConveyorWorld(const cv::Point2d& pixel,
                                  const ConveyorCalibResult& result);

cv::Point2d conveyorWorldToPixel(const cv::Point2d& world,
                                  const ConveyorCalibResult& result);

bool saveConveyorCalibResult(const ConveyorCalibResult& result,
                              const std::string& output_path);

bool loadConveyorCalibResult(const std::string& input_path,
                              ConveyorCalibResult& result);

void ensureDir(const std::string& dir);

#endif // CALIBRATION_CORE_H
