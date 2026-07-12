#include "main_window.h"
#include "rpc/config_loader.h"
#include "logger/logger.h"

#ifdef HAS_ROS2
#include "ros2_backend.h"
#include "vision_interfaces/srv/camera_set_exposure.hpp"
#include "vision_interfaces/srv/camera_set_gain.hpp"
#include "vision_interfaces/srv/camera_set_trigger_mode.hpp"
#include "vision_interfaces/srv/camera_soft_trigger.hpp"
#include "vision_interfaces/srv/camera_get_config.hpp"
#include "vision_interfaces/srv/detector_get_result.hpp"
#include "vision_interfaces/srv/detector_get_config.hpp"
#include "vision_interfaces/srv/detector_on_off.hpp"
#include "vision_interfaces/srv/detector_set_threshold.hpp"
#include "vision_interfaces/srv/detector_set_v_threshold.hpp"
#include "vision_interfaces/srv/detector_set_grad_threshold.hpp"
#include "vision_interfaces/srv/detector_set_segment_mode.hpp"
#include "vision_interfaces/srv/detector_reload_template.hpp"
#include "vision_interfaces/srv/comm_set_config.hpp"
#include "vision_interfaces/srv/comm_get_config.hpp"
#include "vision_interfaces/srv/comm_get_status.hpp"
#endif

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QPixmap>
#include <QImage>
#include <QMessageBox>
#include <QStatusBar>
#include <QPointer>
#include <QScrollArea>

#include <opencv2/imgproc.hpp>

#include <sstream>
#include <vector>
#include <functional>
#include <thread>

// ========== 构造/析构 ==========

MainWindow::MainWindow(const std::string& config_path,
                       int argc, char* argv[],
                       QWidget* parent)
    : QMainWindow(parent), saved_argc_(argc), saved_argv_(argv)
{
    setupUI();
    setupRPC(config_path);

    display_timer_ = new QTimer(this);
    status_timer_  = new QTimer(this);

    connect(display_timer_, &QTimer::timeout, this, &MainWindow::onUpdateDisplay);
    connect(status_timer_,  &QTimer::timeout, this, &MainWindow::onRefreshStatus);

    display_timer_->start(33);   // ~30 fps
    status_timer_->start(2000);  // 2 s

    // 延迟初始化：RPC连接建立后拉取节点当前配置填充UI初始值
    QTimer::singleShot(500, this, &MainWindow::initUIFromNodes);
}

MainWindow::~MainWindow()
{
    display_timer_->stop();
    status_timer_->stop();
}

// ========== 辅助: 构建一行「控件 + Apply按钮」并加入 QFormLayout ==========

static QHBoxLayout* makeRowWithApply(QWidget* control, QPushButton* btn_apply)
{
    auto* row = new QHBoxLayout();
    row->addWidget(control, 1);
    row->addWidget(btn_apply, 0);
    return row;
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
    status_fps_      = new QLabel("FPS: --", this);
    status_fps_->setStyleSheet("color: blue; font-weight: bold;");
    status_layout->addWidget(status_camera_);
    status_layout->addWidget(status_detector_);
    status_layout->addWidget(status_comm_);
    status_layout->addWidget(status_fps_);
    main_layout->addLayout(status_layout);

    // ---- 选项卡 ----
    auto* tabs = new QTabWidget(this);
    main_layout->addWidget(tabs);

    // == Camera 选项卡 ==
    {
        auto* w = new QWidget(this);
        auto* form = new QFormLayout(w);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        // Exposure: [SpinBox] [Apply]
        spin_exposure_ = new QDoubleSpinBox(this);
        spin_exposure_->setRange(0, 100000);
        spin_exposure_->setSingleStep(100);
        spin_exposure_->setSuffix(" us");
        auto* btn_apply_exp = new QPushButton("Apply", this);
        btn_apply_exp->setFixedWidth(60);
        form->addRow("Exposure:", makeRowWithApply(spin_exposure_, btn_apply_exp));

        // Gain: [SpinBox] [Apply]
        spin_gain_ = new QDoubleSpinBox(this);
        spin_gain_->setRange(0, 100);
        spin_gain_->setSingleStep(0.1);
        auto* btn_apply_gain = new QPushButton("Apply", this);
        btn_apply_gain->setFixedWidth(60);
        form->addRow("Gain:", makeRowWithApply(spin_gain_, btn_apply_gain));

        // Trigger Mode: [ComboBox] [Apply]
        // "continuous" = 连续采集（TriggerMode::OFF），其余为触发模式
        combo_trigger_mode_ = new QComboBox(this);
        combo_trigger_mode_->addItems({"continuous", "software", "line0", "line1", "line2"});
        auto* btn_apply_trigger = new QPushButton("Apply", this);
        btn_apply_trigger->setFixedWidth(60);
        form->addRow("Trigger Mode:", makeRowWithApply(combo_trigger_mode_, btn_apply_trigger));

        // Soft Trigger (独立按钮，仅 software 模式可用)
        btn_soft_trigger_ = new QPushButton("Soft Trigger", this);
        btn_soft_trigger_->setEnabled(false);  // 初始禁用，等 get_config 后按实际模式启用
        form->addRow("", btn_soft_trigger_);

        // 联动：切换触发模式时更新 Soft Trigger 按钮可用状态
        connect(combo_trigger_mode_, &QComboBox::currentTextChanged,
                this, [this](const QString& text) {
                    btn_soft_trigger_->setEnabled(text == "software");
                });

        // Camera Info (只读文本)
        text_camera_info_ = new QTextEdit(this);
        text_camera_info_->setReadOnly(true);
        text_camera_info_->setMaximumHeight(80);
        auto* btn_refresh_cam = new QPushButton("Refresh", this);
        btn_refresh_cam->setFixedWidth(60);
        form->addRow("Camera Info:", makeRowWithApply(text_camera_info_, btn_refresh_cam));

        connect(btn_apply_exp,     &QPushButton::clicked, this, &MainWindow::onCameraSetExposure);
        connect(btn_apply_gain,    &QPushButton::clicked, this, &MainWindow::onCameraSetGain);
        connect(btn_apply_trigger, &QPushButton::clicked, this, &MainWindow::onCameraSetTriggerMode);
        connect(btn_soft_trigger_, &QPushButton::clicked, this, &MainWindow::onCameraSoftTrigger);
        connect(btn_refresh_cam,   &QPushButton::clicked, this, &MainWindow::onCameraGetConfig);

        tabs->addTab(w, "Camera");
    }

    // == Detector 选项卡 ==
    {
        auto* w = new QWidget(this);
        auto* form = new QFormLayout(w);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        // Match Threshold: [SpinBox] [Apply]
        spin_match_threshold_ = new QDoubleSpinBox(this);
        spin_match_threshold_->setRange(0.0, 1.0);
        spin_match_threshold_->setSingleStep(0.01);
        spin_match_threshold_->setDecimals(3);
        auto* btn_apply_threshold = new QPushButton("Apply", this);
        btn_apply_threshold->setFixedWidth(60);
        form->addRow("Match Threshold:", makeRowWithApply(spin_match_threshold_, btn_apply_threshold));

        // Segment Mode: [ComboBox] [Apply]
        combo_segment_mode_ = new QComboBox(this);
        combo_segment_mode_->addItems({"value", "gradient", "hsv"});
        auto* btn_apply_seg = new QPushButton("Apply", this);
        btn_apply_seg->setFixedWidth(60);
        form->addRow("Segment Mode:", makeRowWithApply(combo_segment_mode_, btn_apply_seg));

        // V Threshold: [SpinBox] [Apply]
        spin_v_threshold_ = new QSpinBox(this);
        spin_v_threshold_->setRange(0, 255);
        auto* btn_apply_vth = new QPushButton("Apply", this);
        btn_apply_vth->setFixedWidth(60);
        form->addRow("V Threshold:", makeRowWithApply(spin_v_threshold_, btn_apply_vth));

        // Grad Threshold: [SpinBox] [Apply]
        spin_grad_threshold_ = new QSpinBox(this);
        spin_grad_threshold_->setRange(0, 255);
        auto* btn_apply_gth = new QPushButton("Apply", this);
        btn_apply_gth->setFixedWidth(60);
        form->addRow("Grad Threshold:", makeRowWithApply(spin_grad_threshold_, btn_apply_gth));

        // Detector Info (只读)
        text_detector_info_ = new QTextEdit(this);
        text_detector_info_->setReadOnly(true);
        text_detector_info_->setMaximumHeight(100);
        auto* btn_refresh_det = new QPushButton("Refresh", this);
        btn_refresh_det->setFixedWidth(60);
        form->addRow("Detector Info:", makeRowWithApply(text_detector_info_, btn_refresh_det));

        // Reload Template (独立按钮)
        btn_reload_template_ = new QPushButton("Reload Template", this);
        form->addRow("", btn_reload_template_);

        // Detector On/Off Switch (独立按钮)
        btn_detector_onoff_ = new QPushButton("Enable Detector", this);
        btn_detector_onoff_->setStyleSheet("QPushButton { background-color: red; color: white; }");
        form->addRow("", btn_detector_onoff_);

        connect(btn_apply_threshold,   &QPushButton::clicked, this, &MainWindow::onDetectorSetThreshold);
        connect(btn_apply_seg,         &QPushButton::clicked, this, &MainWindow::onDetectorSetSegmentMode);
        connect(btn_apply_vth,         &QPushButton::clicked, this, &MainWindow::onDetectorSetVThreshold);
        connect(btn_apply_gth,         &QPushButton::clicked, this, &MainWindow::onDetectorSetGradThreshold);
        connect(btn_refresh_det,       &QPushButton::clicked, this, &MainWindow::onDetectorGetConfig);
        connect(btn_reload_template_,  &QPushButton::clicked, this, &MainWindow::onDetectorReloadTemplate);
        connect(btn_detector_onoff_,   &QPushButton::clicked, this, &MainWindow::onDetectorOnOff);

        tabs->addTab(w, "Detector");
    }

    // == Communication 选项卡 ==
    {
        auto* w = new QWidget(this);
        auto* form = new QFormLayout(w);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        // Host: [LineEdit] [Apply]
        edit_comm_host_ = new QLineEdit(this);
        edit_comm_host_->setText("0.0.0.0");
        auto* btn_apply_host = new QPushButton("Apply", this);
        btn_apply_host->setFixedWidth(60);
        form->addRow("Host:", makeRowWithApply(edit_comm_host_, btn_apply_host));

        // Port: [SpinBox] [Apply]
        spin_comm_port_ = new QSpinBox(this);
        spin_comm_port_->setRange(1, 65535);
        spin_comm_port_->setValue(7930);
        auto* btn_apply_port = new QPushButton("Apply", this);
        btn_apply_port->setFixedWidth(60);
        form->addRow("Port:", makeRowWithApply(spin_comm_port_, btn_apply_port));

        // Mode: [ComboBox] [Apply]
        combo_comm_mode_ = new QComboBox(this);
        combo_comm_mode_->addItems({"server", "client"});
        auto* btn_apply_mode = new QPushButton("Apply", this);
        btn_apply_mode->setFixedWidth(60);
        form->addRow("Mode:", makeRowWithApply(combo_comm_mode_, btn_apply_mode));

        // Comm Info (只读)
        text_comm_info_ = new QTextEdit(this);
        text_comm_info_->setReadOnly(true);
        text_comm_info_->setMaximumHeight(80);
        auto* btn_refresh_comm = new QPushButton("Refresh", this);
        btn_refresh_comm->setFixedWidth(60);
        form->addRow("Comm Info:", makeRowWithApply(text_comm_info_, btn_refresh_comm));

        // Check Status (独立按钮)
        auto* btn_check_comm = new QPushButton("Check Status", this);
        form->addRow("", btn_check_comm);

        connect(btn_apply_host,  &QPushButton::clicked, this, &MainWindow::onCommSetHost);
        connect(btn_apply_port,  &QPushButton::clicked, this, &MainWindow::onCommSetPort);
        connect(btn_apply_mode,  &QPushButton::clicked, this, &MainWindow::onCommSetMode);
        connect(btn_refresh_comm,&QPushButton::clicked, this, &MainWindow::onCommGetConfig);
        connect(btn_check_comm,  &QPushButton::clicked, this, &MainWindow::onCommGetStatus);

        tabs->addTab(w, "Communication");
    }
}

// ========== RPC 初始化 ==========

void MainWindow::setupRPC(const std::string& config_path)
{
    NodeConfig config = ConfigLoader::loadSystemConfig(config_path);
    config.node_name = "manager_node";
    factory_ = std::make_unique<NodeFactory>(config);

    // DAG 端口管理器
    edges_ = std::make_unique<NodeEdgeManager>(*factory_, "manager");
    edges_->setDefaultTopic("frame_input",      "vision/frame");
    edges_->setDefaultTopic("detection_input",  "vision/detection");
    edges_->setDefaultTopic("annotation_input", "vision/annotation");
    // 让 NodeEdgeManager 自己解析 --topic-map 和 --instance
    if (saved_argv_) {
        edges_->parseArgs(saved_argc_, saved_argv_);
    }

    frame_sub_      = edges_->subscribe<FrameMsg>("frame_input", "vision/frame");
    detection_sub_  = edges_->subscribe<DetectionMsg>("detection_input", "vision/detection");
    annotation_sub_ = edges_->subscribe<AnnotationMsg>("annotation_input", "vision/annotation");
    LOG_INFO("[Manager] 订阅 topics: frame=%s, detection=%s, annotation=%s",
             frame_sub_->getTopic().c_str(),
             detection_sub_->getTopic().c_str(),
             annotation_sub_->getTopic().c_str());

    camera_service_   = factory_->createService<ServiceRequest, ServiceResponse>("camera");
    detector_service_ = factory_->createService<ServiceRequest, ServiceResponse>("detector");
    comm_service_     = factory_->createService<ServiceRequest, ServiceResponse>("comm");

#ifdef HAS_ROS2
    // 注册原生 ROS2 service 类型映射（客户端侧转换，使 call() 通过原生 client 调用）
    // toNativeReq: ServiceRequest → 原生请求 (客户端发送时使用)
    // fromNativeResp: 原生响应 → ServiceResponse (客户端接收时使用)
    // toSrvReq 和 fromSrvResp 在 manager 中不需要（manager 不是服务端），传空 lambda

    if (auto* rs = dynamic_cast<Ros2Service<ServiceRequest, ServiceResponse>*>(camera_service_.get())) {
        rs->registerNativeEndpoint<vision_interfaces::srv::CameraSetExposure>(
            "set_exposure",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CameraSetExposure::Request> {
                auto req = std::make_shared<vision_interfaces::srv::CameraSetExposure::Request>();
                req->exposure_time = std::stof(sr.payload);
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CameraSetGain>(
            "set_gain",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CameraSetGain::Request> {
                auto req = std::make_shared<vision_interfaces::srv::CameraSetGain::Request>();
                req->gain = std::stof(sr.payload);
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CameraSetTriggerMode>(
            "set_trigger_mode",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CameraSetTriggerMode::Request> {
                auto req = std::make_shared<vision_interfaces::srv::CameraSetTriggerMode::Request>();
                req->mode = sr.payload;
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CameraSoftTrigger>(
            "soft_trigger",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CameraSoftTrigger::Request> {
                return std::make_shared<vision_interfaces::srv::CameraSoftTrigger::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CameraGetConfig>(
            "get_config",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CameraGetConfig::Request> {
                return std::make_shared<vision_interfaces::srv::CameraGetConfig::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->config_data;
                return sr;
            });
    }

    if (auto* rs = dynamic_cast<Ros2Service<ServiceRequest, ServiceResponse>*>(detector_service_.get())) {
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorGetResult>(
            "get_result",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::DetectorGetResult::Request> {
                return std::make_shared<vision_interfaces::srv::DetectorGetResult::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data.assign(resp->detection_data.begin(), resp->detection_data.end());
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorGetConfig>(
            "get_config",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::DetectorGetConfig::Request> {
                return std::make_shared<vision_interfaces::srv::DetectorGetConfig::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->ready;
                sr.data = resp->config_data;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorOnOff>(
            "onoff",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorOnOff::Request> {
                auto req = std::make_shared<vision_interfaces::srv::DetectorOnOff::Request>();
                req->command = sr.payload;
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorSetThreshold>(
            "set_threshold",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetThreshold::Request> {
                auto req = std::make_shared<vision_interfaces::srv::DetectorSetThreshold::Request>();
                req->threshold = std::stof(sr.payload);
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorSetVThreshold>(
            "set_v_threshold",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetVThreshold::Request> {
                auto req = std::make_shared<vision_interfaces::srv::DetectorSetVThreshold::Request>();
                req->threshold = std::stoi(sr.payload);
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorSetGradThreshold>(
            "set_grad_threshold",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetGradThreshold::Request> {
                auto req = std::make_shared<vision_interfaces::srv::DetectorSetGradThreshold::Request>();
                req->threshold = std::stoi(sr.payload);
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorSetSegmentMode>(
            "set_segment_mode",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetSegmentMode::Request> {
                auto req = std::make_shared<vision_interfaces::srv::DetectorSetSegmentMode::Request>();
                req->mode = sr.payload;
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorReloadTemplate>(
            "reload_template",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::DetectorReloadTemplate::Request> {
                return std::make_shared<vision_interfaces::srv::DetectorReloadTemplate::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
    }

    if (auto* rs = dynamic_cast<Ros2Service<ServiceRequest, ServiceResponse>*>(comm_service_.get())) {
        rs->registerNativeEndpoint<vision_interfaces::srv::CommSetConfig>(
            "set_config",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CommSetConfig::Request> {
                auto req = std::make_shared<vision_interfaces::srv::CommSetConfig::Request>();
                req->config_data = sr.payload;
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CommGetConfig>(
            "get_config",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CommGetConfig::Request> {
                return std::make_shared<vision_interfaces::srv::CommGetConfig::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->config_data;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CommGetStatus>(
            "get_status",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse&, auto) {},
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CommGetStatus::Request> {
                return std::make_shared<vision_interfaces::srv::CommGetStatus::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->status_data;
                return sr;
            });
    }
#endif

    // 预连接：提前创建所有已注册 endpoint 的原生 client，避免首次调用延迟
    camera_service_->preconnect();
    detector_service_->preconnect();
    comm_service_->preconnect();

    frame_sub_->subscribe([this](const FrameMsg& msg) {
        QMutexLocker locker(&frame_mutex_);
        
        int height = msg.height();
        int width = msg.width();
        int pixel_type = msg.pixel_type();
        int cv_type = (pixel_type == 1) ? CV_8UC3 : CV_8UC1;
        
        // 创建 Mat 并直接从 Protobuf 数据复制
        current_frame_ = cv::Mat(height, width, cv_type);
        const std::string& data = msg.data();
        if (data.size() == static_cast<size_t>(height * width * (pixel_type == 1 ? 3 : 1))) {
            std::memcpy(current_frame_.data, data.data(), data.size());
        } else {
            LOG_ERROR("[Manager] 帧数据大小不匹配: 期望=%d, 实际=%zu", 
                     height * width * (pixel_type == 1 ? 3 : 1), data.size());
        }
        
        current_frame_num_ = msg.frame_num();
        frame_updated_ = true;
        LOG_DEBUG("[Manager] 收到帧#%d, 尺寸=%dx%d, 像素类型=%d", msg.frame_num(), width, height, pixel_type);
        
        // 统计帧率
        frame_count_++;
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (last_fps_time_ == 0) {
            last_fps_time_ = now;
        } else if (now - last_fps_time_ >= 1000) {  // 每秒计算一次
            double elapsed = (now - last_fps_time_) / 1000.0;
            current_fps_ = static_cast<float>(frame_count_ / elapsed);
            frame_count_ = 0;
            last_fps_time_ = now;
        }
    });

    detection_sub_->subscribe([this](const DetectionMsg& msg) {
        QMutexLocker locker(&detection_mutex_);
        latest_detection_ = msg;
        detection_updated_ = true;
    });

    annotation_sub_->subscribe([this](const AnnotationMsg& msg) {
        QMutexLocker locker(&annotation_mutex_);
        latest_annotation_ = msg;
        annotation_updated_ = true;
        LOG_DEBUG("[Manager] 收到 AnnotationMsg: 帧#%d, 物体数=%d, 模板尺寸=%dx%d", msg.frame_num(), msg.objects.size(), msg.template_width(), msg.template_height());
    });
}

// ========== 从节点拉取配置，填充UI初始值 ==========

void MainWindow::initUIFromNodes()
{
    // 拉取 camera 配置
    callService("camera", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (resp.success() && !resp.data().empty()) {
                parseAndApplyCameraConfig(resp.data());
            }
        });

    // 拉取 detector 配置
    callService("detector", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (!resp.data().empty()) {
                parseAndApplyDetectorConfig(resp.data());
                text_detector_info_->setText(QString::fromStdString(resp.data()));
            }
        });

    // 拉取 comm 配置
    callService("comm", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (resp.success() && !resp.data().empty()) {
                parseAndApplyCommConfig(resp.data());
            }
        });
}

// ========== 配置字符串解析辅助 ==========

// 从 key=val 格式中提取指定 key 的值
// 支持 ',' 和 '\n' 两种分隔符，token 两端空白会被忽略
static std::string extractValue(const std::string& data, const std::string& key)
{
    // 先将 '\n' 统一替换为 ','，再按 ',' 分割
    std::string normalized = data;
    for (char& c : normalized) {
        if (c == '\n' || c == '\r') c = ',';
    }
    std::istringstream ss(normalized);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // 去掉首尾空白
        auto start = token.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        token = token.substr(start);
        auto eq = token.find('=');
        if (eq != std::string::npos && token.substr(0, eq) == key) {
            return token.substr(eq + 1);
        }
    }
    return {};
}

void MainWindow::parseAndApplyCameraConfig(const std::string& data)
{
    // camera get_config 返回格式（\n 分隔）:
    // camera_index=0\ntrigger_mode=continuous\nexposure_time=10000\ngain=0\n...
    std::string exp_str     = extractValue(data, "exposure_time");
    std::string gain_str    = extractValue(data, "gain");
    std::string trigger_str = extractValue(data, "trigger_mode");

    if (!exp_str.empty()) {
        try { spin_exposure_->setValue(std::stod(exp_str)); } catch (...) {}
    }
    if (!gain_str.empty()) {
        try { spin_gain_->setValue(std::stod(gain_str)); } catch (...) {}
    }
    if (!trigger_str.empty()) {
        // camera 节点 "off" 与 "continuous" 均表示连续模式，统一映射到 combo 的 "continuous"
        if (trigger_str == "off") trigger_str = "continuous";
        int idx = combo_trigger_mode_->findText(QString::fromStdString(trigger_str));
        if (idx >= 0) combo_trigger_mode_->setCurrentIndex(idx);
    }
    text_camera_info_->setText(QString::fromStdString(data));
}

void MainWindow::parseAndApplyDetectorConfig(const std::string& data)
{
    // detector get_config 返回格式:
    // detector=opencv,template_dir=...,match_threshold=0.85,segment_mode=value,
    // v_threshold=50,grad_threshold=30,...,ready=1
    std::string thr_str  = extractValue(data, "match_threshold");
    std::string seg_str  = extractValue(data, "segment_mode");
    std::string vth_str  = extractValue(data, "v_threshold");
    std::string gth_str  = extractValue(data, "grad_threshold");

    if (!thr_str.empty()) {
        try { spin_match_threshold_->setValue(std::stod(thr_str)); } catch (...) {}
    }
    if (!seg_str.empty()) {
        int idx = combo_segment_mode_->findText(QString::fromStdString(seg_str));
        if (idx >= 0) combo_segment_mode_->setCurrentIndex(idx);
    }
    if (!vth_str.empty()) {
        try { spin_v_threshold_->setValue(std::stoi(vth_str)); } catch (...) {}
    }
    if (!gth_str.empty()) {
        try { spin_grad_threshold_->setValue(std::stoi(gth_str)); } catch (...) {}
    }
}

void MainWindow::parseAndApplyCommConfig(const std::string& data)
{
    // comm get_config 返回格式:
    // host=0.0.0.0,port=7930,mode=server,server_mode=2,interval_ms=100
    std::string host_str = extractValue(data, "host");
    std::string port_str = extractValue(data, "port");
    std::string mode_str = extractValue(data, "mode");

    if (!host_str.empty()) {
        edit_comm_host_->setText(QString::fromStdString(host_str));
    }
    if (!port_str.empty()) {
        try { spin_comm_port_->setValue(std::stoi(port_str)); } catch (...) {}
    }
    if (!mode_str.empty()) {
        int idx = combo_comm_mode_->findText(QString::fromStdString(mode_str));
        if (idx >= 0) combo_comm_mode_->setCurrentIndex(idx);
    }
    text_comm_info_->setText(QString::fromStdString(data));
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

    // 更新帧率显示
    float fps = current_fps_;
    if (fps > 0) {
        status_fps_->setText(QString("FPS: %1").arg(fps, 0, 'f', 1));
    } else {
        status_fps_->setText("FPS: --");
    }

    if (need_frame) {
        LOG_DEBUG("[Manager] onUpdateDisplay: 帧#%d, 图像尺寸=%dx%d", current_frame_num_, frame.cols, frame.rows);
        
        // 优先使用 AnnotationMsg (支持完整绘制信息)
        bool need_annotation_overlay = false;
        AnnotationMsg ann;
        {
            QMutexLocker locker(&annotation_mutex_);
            if (annotation_updated_) {
                ann = latest_annotation_;
                annotation_updated_ = false;
                need_annotation_overlay = true;
                LOG_DEBUG("[Manager] 使用 AnnotationMsg 绘制: 帧#%d, 物体数=%d", ann.frame_num(), ann.objects.size());
            }
        }
        
        if (need_annotation_overlay) {
            overlayAnnotations(frame, ann);
        } else {
            // 回退到 DetectionMsg (简单绘制)
            bool need_detection_overlay = false;
            DetectionMsg det;
            {
                QMutexLocker locker(&detection_mutex_);
                if (detection_updated_) {
                    det = latest_detection_;
                    detection_updated_ = false;
                    need_detection_overlay = true;
                    LOG_DEBUG("[Manager] 使用 DetectionMsg 绘制");
                }
            }
            if (need_detection_overlay) {
                overlayDetections(frame, det);
            }
        }
        updateImageDisplay(frame);
    }
}

void MainWindow::onRefreshStatus()
{
    callService("camera", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (resp.success()) {
                status_camera_->setText("Camera: OK");
                status_camera_->setStyleSheet("color: green; font-weight: bold;");
            } else {
                status_camera_->setText("Camera: Error");
                status_camera_->setStyleSheet("color: red; font-weight: bold;");
            }
        });

    callService("detector", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (resp.success()) {
                status_detector_->setText("Detector: OK");
                status_detector_->setStyleSheet("color: green; font-weight: bold;");
            } else {
                status_detector_->setText("Detector: Not Ready");
                status_detector_->setStyleSheet("color: orange; font-weight: bold;");
            }
        });

    callService("comm", "get_status", "",
        [this](const ServiceResponse& resp) {
            if (resp.success()) {
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
            if (resp.success()) {
                parseAndApplyCameraConfig(resp.data());
            }
            text_camera_info_->setText(
                resp.success() ? QString::fromStdString(resp.data())
                             : "Error: " + QString::fromStdString(resp.data()));
        });
}

// ========== Detector 槽函数 ==========

void MainWindow::onDetectorSetThreshold()
{
    callService("detector", "set_threshold",
                std::to_string(spin_match_threshold_->value()), nullptr);
}

void MainWindow::onDetectorSetSegmentMode()
{
    callService("detector", "set_segment_mode",
                combo_segment_mode_->currentText().toStdString(), nullptr);
}

void MainWindow::onDetectorSetVThreshold()
{
    callService("detector", "set_v_threshold",
                std::to_string(spin_v_threshold_->value()), nullptr);
}

void MainWindow::onDetectorSetGradThreshold()
{
    callService("detector", "set_grad_threshold",
                std::to_string(spin_grad_threshold_->value()), nullptr);
}

void MainWindow::onDetectorGetConfig()
{
    callService("detector", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (!resp.data().empty()) {
                parseAndApplyDetectorConfig(resp.data());
            }
            text_detector_info_->setText(
                resp.success() ? QString::fromStdString(resp.data())
                             : "Not Ready: " + QString::fromStdString(resp.data()));
        });
}

void MainWindow::onDetectorReloadTemplate()
{
    btn_reload_template_->setEnabled(false);
    btn_reload_template_->setText("Reloading...");
    callService("detector", "reload_template", "",
        [this](const ServiceResponse& resp) {
            btn_reload_template_->setEnabled(true);
            btn_reload_template_->setText("Reload Template");
            if (resp.success()) {
                statusBar()->showMessage("Template reloaded successfully", 3000);
                // 刷新配置显示
                onDetectorGetConfig();
            } else {
                QMessageBox::warning(this, "Reload Template Failed",
                    QString::fromStdString(resp.data()));
            }
        });
}

void MainWindow::onDetectorOnOff()
{
    btn_detector_onoff_->setEnabled(false);
    
    std::string cmd = detector_enabled_ ? "off" : "on";
    callService("detector", "onoff", cmd,
        [this](const ServiceResponse& resp) {
            btn_detector_onoff_->setEnabled(true);
            if (resp.success()) {
                detector_enabled_ = !detector_enabled_;
                if (detector_enabled_) {
                    btn_detector_onoff_->setText("Disable Detector");
                    btn_detector_onoff_->setStyleSheet("QPushButton { background-color: green; color: white; }");
                    statusBar()->showMessage("Detector enabled", 3000);
                } else {
                    btn_detector_onoff_->setText("Enable Detector");
                    btn_detector_onoff_->setStyleSheet("QPushButton { background-color: red; color: white; }");
                    statusBar()->showMessage("Detector disabled", 3000);
                }
            } else {
                QMessageBox::warning(this, "Detector On/Off Failed",
                    QString::fromStdString(resp.data()));
            }
        });
}

// ========== Comm 槽函数 ==========

void MainWindow::onCommSetHost()
{
    std::string payload = "host=" + edit_comm_host_->text().toStdString();
    callService("comm", "set_config", payload, nullptr);
}

void MainWindow::onCommSetPort()
{
    std::string payload = "port=" + std::to_string(spin_comm_port_->value());
    callService("comm", "set_config", payload, nullptr);
}

void MainWindow::onCommSetMode()
{
    std::string payload = "mode=" + combo_comm_mode_->currentText().toStdString();
    callService("comm", "set_config", payload, nullptr);
}

void MainWindow::onCommGetConfig()
{
    callService("comm", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (resp.success()) {
                parseAndApplyCommConfig(resp.data());
            }
            text_comm_info_->setText(
                resp.success() ? QString::fromStdString(resp.data())
                             : "Error: " + QString::fromStdString(resp.data()));
        });
}

void MainWindow::onCommGetStatus()
{
    callService("comm", "get_status", "",
        [this](const ServiceResponse& resp) {
            text_comm_info_->setText(
                resp.success() ? QString::fromStdString(resp.data())
                             : "Error: " + QString::fromStdString(resp.data()));
        });
}

// ========== 通用服务调用（异步，不阻塞 UI） ==========

void MainWindow::callService(const std::string& service_name,
                              const std::string& endpoint,
                              const std::string& payload,
                              const std::function<void(const ServiceResponse&)>& callback)
{
    ServiceRequest req;
    req.set_endpoint(endpoint);
    req.set_payload(payload);

    std::shared_ptr<IService<ServiceRequest, ServiceResponse>> svc;
    std::atomic<bool>* guard = nullptr;

    if (service_name == "camera") {
        if (camera_call_pending_.exchange(true)) return;
        svc = camera_service_;
        guard = &camera_call_pending_;
    } else if (service_name == "detector") {
        if (detector_call_pending_.exchange(true)) return;
        svc = detector_service_;
        guard = &detector_call_pending_;
    } else if (service_name == "comm") {
        if (comm_call_pending_.exchange(true)) return;
        svc = comm_service_;
        guard = &comm_call_pending_;
    } else {
        statusBar()->showMessage(QString("Unknown service: %1").arg(QString::fromStdString(service_name)));
        return;
    }

    // 在后台线程执行阻塞的 ZMQ 调用，完成后通过 invokeMethod 回到主线程
    QPointer<MainWindow> self(this);
    LOG_DEBUG("[callService] %s/%s payload=%s dispatching...", service_name.c_str(), endpoint.c_str(), payload.c_str());
    std::thread([self, svc, endpoint, req, callback, guard, service_name]() {
        ServiceResponse resp;
        try {
            resp = svc->call(endpoint, req);
            LOG_DEBUG("[callService] %s/%s SUCCESS, data=%s", service_name.c_str(), endpoint.c_str(), resp.data().substr(0, 80).c_str());
        } catch (const std::exception& e) {
            resp.set_success(false);
            resp.set_data(e.what());
            LOG_ERROR("[callService] %s/%s FAILED: %s", service_name.c_str(), endpoint.c_str(), e.what());
        }

        // 回到主线程执行回调
        if (self) {
            QMetaObject::invokeMethod(self.data(), [self, callback, resp, guard]() {
                guard->store(false);
                if (callback) callback(resp);
            }, Qt::QueuedConnection);
        } else {
            guard->store(false);
        }
    }).detach();
}

// ========== 图像显示 ==========

void MainWindow::updateImageDisplay(const cv::Mat& mat)
{
    if (mat.empty()) return;

    cv::Mat rgb;
    QImage qimg;

    if (mat.type() == CV_8UC1) {
        qimg = QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                      QImage::Format_Grayscale8);
    } else if (mat.type() == CV_8UC3) {
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
        qimg = QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                      QImage::Format_RGB888);
    } else {
        return;
    }

    if (qimg.isNull()) return;

    QSize label_size = image_label_->size();
    if (!label_size.isValid() || label_size.isEmpty()) return;

    QPixmap pixmap = QPixmap::fromImage(qimg).scaled(
        label_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    if (pixmap.isNull()) return;

    image_label_->setPixmap(pixmap);
}

void MainWindow::overlayDetections(cv::Mat& mat, const DetectionMsg& msg)
{
    if (msg.protocol_string().empty() || msg.protocol_string() == "NG") {
        return;
    }

    // 解析协议字符串: "TA,x,y,a,t,..."  多个目标以 ';' 分隔
    std::istringstream stream(msg.protocol_string());
    std::string token;
    while (std::getline(stream, token, ';')) {
        if (token.empty()) continue;

        std::vector<std::string> parts;
        std::istringstream ss(token);
        std::string part;
        while (std::getline(ss, part, ',')) {
            parts.push_back(part);
        }

        if (parts.size() < 5) continue;

        try {
            double x = std::stod(parts[1]);
            double y = std::stod(parts[2]);
            double a = std::stod(parts[3]);

            int ix = static_cast<int>(x);
            int iy = static_cast<int>(y);

            cv::Scalar color(0, 255, 0);
            cv::line(mat, cv::Point(ix - 10, iy), cv::Point(ix + 10, iy), color, 2);
            cv::line(mat, cv::Point(ix, iy - 10), cv::Point(ix, iy + 10), color, 2);
            cv::circle(mat, cv::Point(ix, iy), 15, color, 2);

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

void MainWindow::overlayAnnotations(cv::Mat& mat, const AnnotationMsg& msg)
{
    LOG_DEBUG("[Manager] overlayAnnotations: 物体数=%d, 图像尺寸=%dx%d", msg.objects.size(), mat.cols, mat.rows);
    
    if (msg.objects.empty()) {
        LOG_DEBUG("[Manager] overlayAnnotations: 物体列表为空，跳过绘制");
        return;
    }

    int template_w = static_cast<int>(msg.template_width());
    int template_h = static_cast<int>(msg.template_height());
    
    LOG_DEBUG("[Manager] overlayAnnotations: 模板尺寸=%dx%d", template_w, template_h);

    for (const auto& obj : msg.objects) {
        LOG_DEBUG("[Manager] 绘制物体#%d: x=%f, y=%f, angle=%f", obj.id, obj.x, obj.y, obj.angle);
        
        // 绘制旋转矩形
        cv::RotatedRect rrect(
            cv::Point2f(static_cast<float>(obj.x), static_cast<float>(obj.y)),
            cv::Size2f(static_cast<float>(template_w),
                       static_cast<float>(template_h)),
            static_cast<float>(obj.angle)
        );

        cv::Point2f vertices[4];
        rrect.points(vertices);
        for (int j = 0; j < 4; ++j) {
            cv::line(mat, vertices[j], vertices[(j + 1) % 4],
                     cv::Scalar(0, 255, 0), 2);
        }

        // 绘制中心十字
        int cs = 15;
        cv::Point center(static_cast<int>(obj.x), static_cast<int>(obj.y));
        cv::line(mat, cv::Point(center.x - cs, center.y),
                 cv::Point(center.x + cs, center.y), cv::Scalar(0, 0, 255), 2);
        cv::line(mat, cv::Point(center.x, center.y - cs),
                 cv::Point(center.x, center.y + cs), cv::Scalar(0, 0, 255), 2);

        // 绘制物体编号标签
        char idx_label[32];
        std::snprintf(idx_label, sizeof(idx_label), "#%d", obj.id);
        cv::putText(mat, idx_label, cv::Point(center.x + 10, center.y - 10),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2);

        // 绘制坐标信息
        char coord_label[64];
        std::snprintf(coord_label, sizeof(coord_label),
                      "(%.2f, %.2f, a=%.1f)", obj.x, obj.y, obj.angle);
        cv::putText(mat, coord_label, cv::Point(center.x + 10, center.y + 15),
                    cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);

        // 绘制角度指示线
        double rad = obj.angle * CV_PI / 180.0;
        cv::line(mat, center,
                 cv::Point(center.x + static_cast<int>(25 * std::cos(rad)),
                           center.y + static_cast<int>(25 * std::sin(rad))),
                 cv::Scalar(0, 255, 0), 2);
    }
    
    LOG_DEBUG("[Manager] overlayAnnotations: 绘制完成");
}
