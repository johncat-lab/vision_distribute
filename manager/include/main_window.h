#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QTabWidget>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QMutex>

#include <atomic>

#include <opencv2/core.hpp>

#include "rpc/node_factory.h"
#include "rpc/message_types.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const std::string& config_path, QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onUpdateDisplay();
    void onRefreshStatus();

    // Camera
    void onCameraSetExposure();
    void onCameraSetGain();
    void onCameraSetTriggerMode();
    void onCameraSoftTrigger();
    void onCameraGetConfig();

    // Detector
    void onDetectorSetThreshold();
    void onDetectorSetSegmentMode();
    void onDetectorSetVThreshold();
    void onDetectorSetGradThreshold();
    void onDetectorGetConfig();
    void onDetectorReloadTemplate();

    // Comm
    void onCommSetHost();
    void onCommSetPort();
    void onCommSetMode();
    void onCommGetConfig();
    void onCommGetStatus();

private:
    void setupUI();
    void setupRPC(const std::string& config_path);
    void initUIFromNodes();
    void parseAndApplyCameraConfig(const std::string& data);
    void parseAndApplyDetectorConfig(const std::string& data);
    void parseAndApplyCommConfig(const std::string& data);
    void updateImageDisplay(const cv::Mat& mat);
    void overlayDetections(cv::Mat& mat, const DetectionMsg& msg);
    void callService(const std::string& service_name,
                     const std::string& endpoint,
                     const std::string& payload,
                     const std::function<void(const ServiceResponse&)>& callback);

    // RPC components
    std::unique_ptr<NodeFactory> factory_;
    std::shared_ptr<ISubscriber<FrameMsg>> frame_sub_;
    std::shared_ptr<ISubscriber<DetectionMsg>> detection_sub_;
    std::shared_ptr<IService<ServiceRequest, ServiceResponse>> camera_service_;
    std::shared_ptr<IService<ServiceRequest, ServiceResponse>> detector_service_;
    std::shared_ptr<IService<ServiceRequest, ServiceResponse>> comm_service_;

    // Image display
    QLabel* image_label_;

    // Thread-safe frame / detection storage
    QMutex frame_mutex_;
    cv::Mat current_frame_;
    uint32_t current_frame_num_ = 0;
    bool frame_updated_ = false;

    QMutex detection_mutex_;
    DetectionMsg latest_detection_;
    bool detection_updated_ = false;

    // Camera config panel
    QDoubleSpinBox* spin_exposure_;
    QDoubleSpinBox* spin_gain_;
    QComboBox* combo_trigger_mode_;
    QPushButton* btn_soft_trigger_;
    QTextEdit* text_camera_info_;

    // Detector config panel
    QDoubleSpinBox* spin_match_threshold_;
    QComboBox* combo_segment_mode_;
    QSpinBox* spin_v_threshold_;
    QSpinBox* spin_grad_threshold_;
    QTextEdit* text_detector_info_;
    QPushButton* btn_reload_template_;

    // Communication config panel
    QLineEdit* edit_comm_host_;
    QSpinBox* spin_comm_port_;
    QComboBox* combo_comm_mode_;
    QTextEdit* text_comm_info_;

    // Status indicators
    QLabel* status_camera_;
    QLabel* status_detector_;
    QLabel* status_comm_;

    // Timers
    QTimer* display_timer_;
    QTimer* status_timer_;

    // Async call guards (防止并发的服务调用堆积)
    std::atomic<bool> camera_call_pending_{false};
    std::atomic<bool> detector_call_pending_{false};
    std::atomic<bool> comm_call_pending_{false};
};
