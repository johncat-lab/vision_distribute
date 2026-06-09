#include "main_window.h"
#include "rpc/config_loader.h"

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
        combo_trigger_mode_ = new QComboBox(this);
        combo_trigger_mode_->addItems({"off", "line0", "line1", "line2", "software"});
        auto* btn_apply_trigger = new QPushButton("Apply", this);
        btn_apply_trigger->setFixedWidth(60);
        form->addRow("Trigger Mode:", makeRowWithApply(combo_trigger_mode_, btn_apply_trigger));

        // Soft Trigger (独立按钮)
        btn_soft_trigger_ = new QPushButton("Soft Trigger", this);
        form->addRow("", btn_soft_trigger_);

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

        connect(btn_apply_threshold,   &QPushButton::clicked, this, &MainWindow::onDetectorSetThreshold);
        connect(btn_apply_seg,         &QPushButton::clicked, this, &MainWindow::onDetectorSetSegmentMode);
        connect(btn_apply_vth,         &QPushButton::clicked, this, &MainWindow::onDetectorSetVThreshold);
        connect(btn_apply_gth,         &QPushButton::clicked, this, &MainWindow::onDetectorSetGradThreshold);
        connect(btn_refresh_det,       &QPushButton::clicked, this, &MainWindow::onDetectorGetConfig);
        connect(btn_reload_template_,  &QPushButton::clicked, this, &MainWindow::onDetectorReloadTemplate);

        tabs->addTab(w, "Detector");
    }

    // == Communication 选项卡 ==
    {
        auto* w = new QWidget(this);
        auto* form = new QFormLayout(w);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        // Host: [LineEdit] [Apply]
        edit_comm_host_ = new QLineEdit(this);
        auto* btn_apply_host = new QPushButton("Apply", this);
        btn_apply_host->setFixedWidth(60);
        form->addRow("Host:", makeRowWithApply(edit_comm_host_, btn_apply_host));

        // Port: [SpinBox] [Apply]
        spin_comm_port_ = new QSpinBox(this);
        spin_comm_port_->setRange(1, 65535);
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

// ========== 从节点拉取配置，填充UI初始值 ==========

void MainWindow::initUIFromNodes()
{
    // 拉取 camera 配置
    callService("camera", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (resp.success && !resp.data.empty()) {
                parseAndApplyCameraConfig(resp.data);
            }
        });

    // 拉取 detector 配置
    callService("detector", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (!resp.data.empty()) {
                parseAndApplyDetectorConfig(resp.data);
                text_detector_info_->setText(QString::fromStdString(resp.data));
            }
        });

    // 拉取 comm 配置
    callService("comm", "get_config", "",
        [this](const ServiceResponse& resp) {
            if (resp.success && !resp.data.empty()) {
                parseAndApplyCommConfig(resp.data);
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
                status_detector_->setText("Detector: Not Ready");
                status_detector_->setStyleSheet("color: orange; font-weight: bold;");
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
            if (resp.success) {
                parseAndApplyCameraConfig(resp.data);
            }
            text_camera_info_->setText(
                resp.success ? QString::fromStdString(resp.data)
                             : "Error: " + QString::fromStdString(resp.data));
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
            if (!resp.data.empty()) {
                parseAndApplyDetectorConfig(resp.data);
            }
            text_detector_info_->setText(
                resp.success ? QString::fromStdString(resp.data)
                             : "Not Ready: " + QString::fromStdString(resp.data));
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
            if (resp.success) {
                statusBar()->showMessage("Template reloaded successfully", 3000);
                // 刷新配置显示
                onDetectorGetConfig();
            } else {
                QMessageBox::warning(this, "Reload Template Failed",
                    QString::fromStdString(resp.data));
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
            if (resp.success) {
                parseAndApplyCommConfig(resp.data);
            }
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

// ========== 通用服务调用（异步，不阻塞 UI） ==========

void MainWindow::callService(const std::string& service_name,
                              const std::string& endpoint,
                              const std::string& payload,
                              const std::function<void(const ServiceResponse&)>& callback)
{
    ServiceRequest req;
    req.endpoint = endpoint;
    req.payload  = payload;

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
    std::cerr << "[callService] " << service_name << "/" << endpoint
              << " payload=" << payload << " dispatching..." << std::endl;
    std::thread([self, svc, endpoint, req, callback, guard, service_name]() {
        ServiceResponse resp;
        try {
            resp = svc->call(endpoint, req);
            std::cerr << "[callService] " << service_name << "/" << endpoint
                      << " SUCCESS, data=" << resp.data.substr(0, 80) << std::endl;
        } catch (const std::exception& e) {
            resp.success = false;
            resp.data = e.what();
            std::cerr << "[callService] " << service_name << "/" << endpoint
                      << " FAILED: " << e.what() << std::endl;
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
    if (msg.protocol_string.empty() || msg.protocol_string == "NG") {
        return;
    }

    // 解析协议字符串: "TA,x,y,a,t,..."  多个目标以 ';' 分隔
    std::istringstream stream(msg.protocol_string);
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
