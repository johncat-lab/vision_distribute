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

    void onCameraSetExposure();
    void onCameraSetGain();
    void onCameraSetTriggerMode();
    void onCameraSoftTrigger();
    void onCameraGetConfig();

    void onDetectorGetConfig();

    void onCommSetConfig();
    void onCommGetConfig();
    void onCommGetStatus();

private:
    void setupUI();
    void setupRPC(const std::string& config_path);
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
    QTextEdit* text_detector_info_;

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
};
