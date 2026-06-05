#include "main_window.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPixmap>
#include <QImage>
#include <QMessageBox>
#include <QStatusBar>

#include <opencv2/imgproc.hpp>

#include <sstream>
#include <vector>
#include <functional>

// ========== 构造/析构 ==========

MainWindow::MainWindow(const std::string& config_path, QWidget* parent)
    : QMainWindow(parent)
{
    setupUI();
    setupRPC(config_path);

    display_timer_ = new QTimer(this);
    status_timer_  = new QTimer(this);

    connect(display_timer_, &QTimer::timeout, this, &MainWindow::onUpdateDisplay);
    connect(status_timer_,  &QTimer::timeout, this, &MainWindow::onRefreshStatus);

    display_timer_->start(33);   // ~30 fps
    status_timer_->start(2000);  // 2 s
}

MainWindow::~MainWindow()
{
    display_timer_->stop();
    status_timer_->stop();
}

// ========== UI 布局 ==========

void MainWindow::setupUI()
{
    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* main_layout = new QVBoxLayout(central);

    // ---- 图像显示 ----
    image_label_ = new QLabel(this);
    image_label_->setMinimumSize(640, 480);
    image_label_->setAlignment(Qt::AlignCenter);
    image_label_->setStyleSheet("background-color: black;");
    image_label_->setScaledContents(false);
    main_layout->addWidget(image_label_);

    // ---- 状态栏 ----
    auto* status_layout = new QHBoxLayout();
    status_camera_   = new QLabel("Camera: --", this);
    status_detector_ = new QLabel("Detector: --", this);
    status_comm_     = new QLabel("Comm: --", this);
    status_layout->addWidget(status_camera_);
    status_layout->addWidget(status_detector_);
    status_layout->addWidget(status_comm_);
    main_layout->addLayout(status_layout);

    // ---- 选项卡 ----
    auto* tabs = new QTabWidget(this);
    main_layout->addWidget(tabs);

    // == Camera 选项卡 ==
    {
        auto* w = new QWidget(this);
        auto* form = new QFormLayout(w);

        spin_exposure_ = new QDoubleSpinBox(this);
        spin_exposure_->setRange(0, 100000);
        spin_exposure_->setSingleStep(100);
        spin_exposure_->setSuffix(" us");
        form->addRow("Exposure:", spin_exposure_);

        spin_gain_ = new QDoubleSpinBox(this);
        spin_gain_->setRange(0, 100);
        spin_gain_->setSingleStep(0.1);
        form->addRow("Gain:", spin_gain_);

        combo_trigger_mode_ = new QComboBox(this);
        combo_trigger_mode_->addItems({"off", "line0", "line1", "line2", "software"});
        form->addRow("Trigger Mode:", combo_trigger_mode_);

        btn_soft_trigger_ = new QPushButton("Soft Trigger", this);
        form->addRow(btn_soft_trigger_);

        text_camera_info_ = new QTextEdit(this);
        text_camera_info_->setReadOnly(true);
        text_camera_info_->setMaximumHeight(80);
        form->addRow("Camera Info:", text_camera_info_);

        auto* btn_apply_exp = new QPushButton("Apply Exposure", this);
        auto* btn_apply_gain = new QPushButton("Apply Gain", this);
        auto* btn_apply_trigger = new QPushButton("Apply Trigger", this);
        auto* btn_refresh_cam = new QPushButton("Refresh Config", this);

        form->addRow(btn_apply_exp);
        form->addRow(btn_apply_gain);
        form->addRow(btn_apply_trigger);
        form->addRow(btn_refresh_cam);

        connect(btn_apply_exp, &QPushButton::clicked, this, &MainWindow::onCameraSetExposure);
        connect(btn_apply_gain, &QPushButton::clicked, this, &MainWindow::onCameraSetGain);
        connect(btn_apply_trigger, &QPushButton::clicked, this, &MainWindow::onCameraSetTriggerMode);
        connect(btn_soft_trigger_, &QPushButton::clicked, this, &MainWindow::onCameraSoftTrigger);
        connect(btn_refresh_cam, &QPushButton::clicked, this, &MainWindow::onCameraGetConfig);

        tabs->addTab(w, "Camera");
    }

    // == Detector 选项卡 ==
    {
        auto* w = new QWidget(this);
        auto* layout = new QVBoxLayout(w);

        text_detector_info_ = new QTextEdit(this);
        text_detector_info_->setReadOnly(true);
        text_detector_info_->setMaximumHeight(120);
        layout->addWidget(text_detector_info_);

        auto* btn_refresh_det = new QPushButton("Refresh Config", this);
        layout->addWidget(btn_refresh_det);

        connect(btn_refresh_det, &QPushButton::clicked, this, &MainWindow::onDetectorGetConfig);

        tabs->addTab(w, "Detector");
    }

    // == Communication 选项卡 ==
    {
        auto* w = new QWidget(this);
        auto* form = new QFormLayout(w);

        edit_comm_host_ = new QLineEdit(this);
        form->addRow("Host:", edit_comm_host_);

        spin_comm_port_ = new QSpinBox(this);
        spin_comm_port_->setRange(1, 65535);
        form->addRow("Port:", spin_comm_port_);

        combo_comm_mode_ = new QComboBox(this);
        combo_comm_mode_->addItems({"server", "client"});
        form->addRow("Mode:", combo_comm_mode_);

        text_comm_info_ = new QTextEdit(this);
        text_comm_info_->setReadOnly(true);
        text_comm_info_->setMaximumHeight(80);
        form->addRow("Comm Info:", text_comm_info_);

        auto* btn_apply_comm = new QPushButton("Apply", this);
        auto* btn_refresh_comm = new QPushButton("Refresh Config", this);
        auto* btn_check_comm = new QPushButton("Check Status", this);

        form->addRow(btn_apply_comm);
        form->addRow(btn_refresh_comm);
        form->addRow(btn_check_comm);

        connect(btn_apply_comm, &QPushButton::clicked, this, &MainWindow::onCommSetConfig);
        connect(btn_refresh_comm, &QPushButton::clicked, this, &MainWindow::onCommGetConfig);
        connect(btn_check_comm, &QPushButton::clicked, this, &MainWindow::onCommGetStatus);

        tabs->addTab(w, "Communication");
    }
}

// ========== RPC 初始化 ==========

void MainWindow::setupRPC(const std::string& config_path)
{
    NodeConfig config = ConfigLoader::loadSystemConfig(config_path);
    factory_ = std::make_unique<NodeFactory>(config);

    frame_sub_     = factory_->createSubscriber<FrameMsg>("vision/frame");
    detection_sub_ = factory_->createSubscriber<DetectionMsg>("vision/detection");

    camera_service_   = factory_->createService<ServiceRequest, ServiceResponse>("camera");
    detector_service_ = factory_->createService<ServiceRequest, ServiceResponse>("detector");
    comm_service_     = factory_->createService<ServiceRequest, ServiceResponse>("comm");

    frame_sub_->subscribe([this](const FrameMsg& msg) {
        QMutexLocker locker(&frame_mutex_);
        current_frame_ = cv::Mat(msg.height, msg.width,
                                 msg.pixel_type == 1 ? CV_8UC3 : CV_8UC1,
                                 const_cast<uint8_t*>(msg.data.data())).clone();
        current_frame_num_ = msg.frame_num;
        frame_updated_ = true;
    });

    detection_sub_->subscribe([this](const DetectionMsg& msg) {
        QMutexLocker locker(&detection_mutex_);
        latest_detection_ = msg;
        detection_updated_ = true;
    });
}

// ========== 定时刷新 ==========

void MainWindow::onUpdateDisplay()
{
    cv::Mat frame;
    bool need_frame = false;

    {
        QMutexLocker locker(&frame_mutex_);
        if (frame_updated_) {
            frame = current_frame_.clone();
            frame_updated_ = false;
            need_frame = true;
        }
    }

    if (need_frame) {
        bool need_overlay = false;
        DetectionMsg det;
        {
            QMutexLocker locker(&detection_mutex_);
            if (detection_updated_) {
                det = latest_detection_;
                detection_updated_ = false;
                need_overlay = true;
            }
        }
        if (need_overlay) {
            overlayDetections(frame, det);
        }
        updateImageDisplay(frame);
    }
}

void MainWindow::onRefreshStatus()
{
    callService("camera", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (resp.success) {
                status_camera_->setText("Camera: OK");
                status_camera_->setStyleSheet("color: green; font-weight: bold;");
            } else {
                status_camera_->setText("Camera: Error");
                status_camera_->setStyleSheet("color: red; font-weight: bold;");
            }
        });

    callService("detector", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (resp.success) {
                status_detector_->setText("Detector: OK");
                status_detector_->setStyleSheet("color: green; font-weight: bold;");
            } else {
                status_detector_->setText("Detector: Error");
                status_detector_->setStyleSheet("color: red; font-weight: bold;");
            }
        });

    callService("comm", "get_status", "",
        [this](const ServiceResponse& resp) {
            if (resp.success) {
                status_comm_->setText("Comm: OK");
                status_comm_->setStyleSheet("color: green; font-weight: bold;");
            } else {
                status_comm_->setText("Comm: Error");
                status_comm_->setStyleSheet("color: red; font-weight: bold;");
            }
        });
}

// ========== Camera 槽函数 ==========

void MainWindow::onCameraSetExposure()
{
    callService("camera", "set_exposure",
                std::to_string(spin_exposure_->value()), nullptr);
}

void MainWindow::onCameraSetGain()
{
    callService("camera", "set_gain",
                std::to_string(spin_gain_->value()), nullptr);
}

void MainWindow::onCameraSetTriggerMode()
{
    callService("camera", "set_trigger_mode",
                combo_trigger_mode_->currentText().toStdString(), nullptr);
}

void MainWindow::onCameraSoftTrigger()
{
    callService("camera", "soft_trigger", "", nullptr);
}

void MainWindow::onCameraGetConfig()
{
    callService("camera", "get_config", "",
        [this](const ServiceResponse& resp) {
            text_camera_info_->setText(
                resp.success ? QString::fromStdString(resp.data)
                             : "Error: " + QString::fromStdString(resp.data));
        });
}

// ========== Detector 槽函数 ==========

void MainWindow::onDetectorGetConfig()
{
    callService("detector", "get_config", "",
        [this](const ServiceResponse& resp) {
            text_detector_info_->setText(
                resp.success ? QString::fromStdString(resp.data)
                             : "Error: " + QString::fromStdString(resp.data));
        });
}

// ========== Comm 槽函数 ==========

void MainWindow::onCommSetConfig()
{
    std::string payload = "host=" + edit_comm_host_->text().toStdString()
                        + ",port=" + std::to_string(spin_comm_port_->value())
                        + ",mode=" + combo_comm_mode_->currentText().toStdString();
    callService("comm", "set_config", payload, nullptr);
}

void MainWindow::onCommGetConfig()
{
    callService("comm", "get_config", "",
        [this](const ServiceResponse& resp) {
            text_comm_info_->setText(
                resp.success ? QString::fromStdString(resp.data)
                             : "Error: " + QString::fromStdString(resp.data));
        });
}

void MainWindow::onCommGetStatus()
{
    callService("comm", "get_status", "",
        [this](const ServiceResponse& resp) {
            text_comm_info_->setText(
                resp.success ? QString::fromStdString(resp.data)
                             : "Error: " + QString::fromStdString(resp.data));
        });
}

// ========== 通用服务调用 ==========

void MainWindow::callService(const std::string& service_name,
                              const std::string& endpoint,
                              const std::string& payload,
                              const std::function<void(const ServiceResponse&)>& callback)
{
    ServiceRequest req;
    req.endpoint = endpoint;
    req.payload  = payload;

    std::shared_ptr<IService<ServiceRequest, ServiceResponse>> svc;
    if (service_name == "camera") {
        svc = camera_service_;
    } else if (service_name == "detector") {
        svc = detector_service_;
    } else if (service_name == "comm") {
        svc = comm_service_;
    } else {
        statusBar()->showMessage(QString("Unknown service: %1").arg(QString::fromStdString(service_name)));
        return;
    }

    try {
        ServiceResponse resp = svc->call(endpoint, req);
        if (callback) {
            callback(resp);
        }
    } catch (const std::exception& e) {
        statusBar()->showMessage(
            QString("Service call failed [%1/%2]: %3")
                .arg(QString::fromStdString(service_name))
                .arg(QString::fromStdString(endpoint))
                .arg(e.what()));
    }
}

// ========== 图像显示 ==========

void MainWindow::updateImageDisplay(const cv::Mat& mat)
{
    QImage qimg;
    if (mat.type() == CV_8UC1) {
        qimg = QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                      QImage::Format_Grayscale8);
    } else if (mat.type() == CV_8UC3) {
        cv::Mat rgb;
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
        qimg = QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                      QImage::Format_RGB888);
    } else {
        return;
    }

    QPixmap pixmap = QPixmap::fromImage(qimg).scaled(
        image_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image_label_->setPixmap(pixmap);
}

void MainWindow::overlayDetections(cv::Mat& mat, const DetectionMsg& msg)
{
    if (msg.protocol_string.empty() || msg.protocol_string == "NG") {
        return;
    }

    // 解析协议字符串: "TA,x,y,a,t,..."  多个目标以 ';' 分隔
    std::istringstream stream(msg.protocol_string);
    std::string token;
    while (std::getline(stream, token, ';')) {
        if (token.empty()) continue;

        // 按 ',' 分割
        std::vector<std::string> parts;
        std::istringstream ss(token);
        std::string part;
        while (std::getline(ss, part, ',')) {
            parts.push_back(part);
        }

        // 至少需要: type, x, y, a, t
        if (parts.size() < 5) continue;

        try {
            double x = std::stod(parts[1]);
            double y = std::stod(parts[2]);
            double a = std::stod(parts[3]);

            int ix = static_cast<int>(x);
            int iy = static_cast<int>(y);

            // 画十字
            cv::Scalar color(0, 255, 0);
            cv::line(mat, cv::Point(ix - 10, iy), cv::Point(ix + 10, iy), color, 2);
            cv::line(mat, cv::Point(ix, iy - 10), cv::Point(ix, iy + 10), color, 2);

            // 画圆
            cv::circle(mat, cv::Point(ix, iy), 15, color, 2);

            // 画角度指示线
            double rad = a * CV_PI / 180.0;
            cv::line(mat, cv::Point(ix, iy),
                     cv::Point(ix + static_cast<int>(20 * std::cos(rad)),
                               iy + static_cast<int>(20 * std::sin(rad))),
                     color, 2);
        } catch (...) {
            continue;
        }
    }
}
