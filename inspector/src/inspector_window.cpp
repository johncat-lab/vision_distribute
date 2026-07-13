#include "inspector_window.h"
#include "logger/logger.h"

#include <tinyxml2.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QScrollArea>
#include <QPixmap>
#include <QImage>
#include <QDateTime>

#include <opencv2/imgproc.hpp>

#include <sstream>
#include <set>
#include <thread>
#include <chrono>

// ============================================================================
//  构造/析构
// ============================================================================

InspectorWindow::InspectorWindow(const std::string& pipeline_xml, QWidget* parent)
    : QMainWindow(parent), pipeline_path_(pipeline_xml)
{
    if (!scheduler_.loadFromXml(pipeline_xml)) {
        appendLog(QString("[错误] 无法加载 pipeline 配置: %1").arg(
            QString::fromStdString(pipeline_xml)), "red");
    }

    node_config_.transport = TransportType::ZEROMQ;
    node_config_.base_port = 15550;
    node_config_.zmq_service_workers = 2;
    factory_ = std::make_unique<NodeFactory>(node_config_);

    setupUI();
    parseTopology();
    populateTopologyTree();
    populateServiceTable();

    fps_timer_ = new QTimer(this);
    connect(fps_timer_, &QTimer::timeout, this, [this]() {
        fps_ = frame_count_ / 1.0;
        frame_count_ = 0;
        if (status_info_) {
            status_info_->setText(QString("FPS: %1 | Topics: %2 | Services: %3")
                .arg(fps_, 0, 'f', 1)
                .arg(topics_.size())
                .arg(endpoints_.size()));
        }
    });
    fps_timer_->start(1000);

    setWindowTitle("System Inspector - " +
        QString::fromStdString(pipeline_xml.substr(pipeline_xml.find_last_of("/\\") + 1)));
    resize(1400, 900);
}

InspectorWindow::~InspectorWindow()
{
    fps_timer_->stop();
}

// ============================================================================
//  UI 布局
// ============================================================================

void InspectorWindow::setupUI()
{
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* main_layout = new QHBoxLayout(central);

    auto* left_panel = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left_panel);
    left_layout->addWidget(createTopologyPanel());
    left_layout->addWidget(createLogPanel());
    left_panel->setFixedWidth(380);

    auto* center_panel = createViewerPanel();

    auto* right_panel = createServicePanel();
    right_panel->setFixedWidth(380);

    main_layout->addWidget(left_panel);
    main_layout->addWidget(center_panel, 1);
    main_layout->addWidget(right_panel);

    status_info_ = new QLabel("Ready", this);
    statusBar()->addWidget(status_info_);
}

QWidget* InspectorWindow::createTopologyPanel()
{
    auto* group = new QGroupBox("Pipeline 拓扑", this);
    auto* layout = new QVBoxLayout(group);

    topology_tree_ = new QTreeWidget(this);
    topology_tree_->setHeaderLabels({"名称", "类型", "详情"});
    topology_tree_->setRootIsDecorated(true);
    topology_tree_->setAlternatingRowColors(true);
    layout->addWidget(topology_tree_);

    auto* sub_group = new QGroupBox("Topic 订阅", this);
    auto* sub_layout = new QVBoxLayout(sub_group);

    topic_combo_ = new QComboBox(this);
    sub_layout->addWidget(topic_combo_);

    auto* btn_layout = new QHBoxLayout();
    btn_subscribe_ = new QPushButton("订阅", this);
    auto* btn_unsub = new QPushButton("取消全部订阅", this);
    btn_layout->addWidget(btn_subscribe_);
    btn_layout->addWidget(btn_unsub);
    sub_layout->addLayout(btn_layout);

    connect(btn_subscribe_, &QPushButton::clicked, this, &InspectorWindow::onSubscribeTopic);
    connect(btn_unsub, &QPushButton::clicked, this, &InspectorWindow::onUnsubscribeAll);

    layout->addWidget(sub_group);

    auto* btn_refresh = new QPushButton("刷新拓扑", this);
    connect(btn_refresh, &QPushButton::clicked, this, &InspectorWindow::onRefreshTopology);
    layout->addWidget(btn_refresh);

    return group;
}

QWidget* InspectorWindow::createViewerPanel()
{
    auto* group = new QGroupBox("消息查看器", this);
    auto* layout = new QVBoxLayout(group);

    image_label_ = new QLabel(this);
    image_label_->setMinimumSize(640, 480);
    image_label_->setAlignment(Qt::AlignCenter);
    image_label_->setStyleSheet("background-color: #1a1a1a; border: 1px solid gray;");
    image_label_->setText("等待订阅...");
    image_label_->setScaledContents(false);
    layout->addWidget(image_label_, 1);

    msg_viewer_ = new QTextEdit(this);
    msg_viewer_->setReadOnly(true);
    msg_viewer_->setMaximumHeight(200);
    msg_viewer_->setPlaceholderText("消息内容将在此显示...");
    layout->addWidget(msg_viewer_);

    return group;
}

QWidget* InspectorWindow::createServicePanel()
{
    auto* group = new QGroupBox("Service Caller", this);
    auto* layout = new QVBoxLayout(group);

    service_table_ = new QTableWidget(this);
    service_table_->setColumnCount(3);
    service_table_->setHorizontalHeaderLabels({"Role", "Endpoint", "Instance"});
    service_table_->horizontalHeader()->setStretchLastSection(true);
    service_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    service_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    service_table_->setMaximumHeight(200);
    layout->addWidget(service_table_);

    auto* form = new QFormLayout();

    role_combo_ = new QComboBox(this);
    form->addRow("Role:", role_combo_);

    endpoint_combo_ = new QComboBox(this);
    form->addRow("Endpoint:", endpoint_combo_);

    payload_edit_ = new QLineEdit(this);
    payload_edit_->setPlaceholderText("key=value,key=value ...");
    form->addRow("Payload:", payload_edit_);

    btn_send_ = new QPushButton("发送", this);
    btn_send_->setStyleSheet("background-color: #4CAF50; color: white; font-weight: bold;");
    form->addRow("", btn_send_);

    layout->addLayout(form);

    auto* resp_group = new QGroupBox("响应", this);
    auto* resp_layout = new QVBoxLayout(resp_group);
    response_text_ = new QTextEdit(this);
    response_text_->setReadOnly(true);
    response_text_->setMaximumHeight(150);
    resp_layout->addWidget(response_text_);
    layout->addWidget(resp_group);

    connect(btn_send_, &QPushButton::clicked, this, &InspectorWindow::onSendService);
    connect(role_combo_, &QComboBox::currentTextChanged, this, [this](const QString& role) {
        endpoint_combo_->clear();
        for (const auto& ep : endpoints_) {
            if (QString::fromStdString(ep.role) == role) {
                endpoint_combo_->addItem(QString::fromStdString(ep.endpoint));
            }
        }
    });

    connect(service_table_, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
        if (row >= 0 && row < static_cast<int>(endpoints_.size())) {
            const auto& ep = endpoints_[row];
            role_combo_->setCurrentText(QString::fromStdString(ep.role));
            endpoint_combo_->setCurrentText(QString::fromStdString(ep.endpoint));
        }
    });

    return group;
}

QWidget* InspectorWindow::createLogPanel()
{
    auto* group = new QGroupBox("日志", this);
    auto* layout = new QVBoxLayout(group);

    log_text_ = new QTextEdit(this);
    log_text_->setReadOnly(true);
    log_text_->setMaximumHeight(150);
    layout->addWidget(log_text_);

    auto* btn_clear = new QPushButton("清除日志", this);
    connect(btn_clear, &QPushButton::clicked, this, &InspectorWindow::onClearLog);
    layout->addWidget(btn_clear);

    return group;
}

// ============================================================================
//  拓扑解析
// ============================================================================

void InspectorWindow::parseTopology()
{
    nodes_.clear();
    topics_.clear();
    endpoints_.clear();

    const auto& templates = scheduler_.templates();
    const auto& instances = scheduler_.instances();
    const auto& wires = scheduler_.wires();

    // 解析节点信息
    for (const auto& [inst_name, inst] : instances) {
        NodeInfo ni;
        ni.name = inst_name;
        auto tmpl_it = templates.find(inst.template_name);
        if (tmpl_it != templates.end()) {
            ni.binary = tmpl_it->second.binary;
            // 从 template 的 services 中获取 role
            if (!tmpl_it->second.services.empty()) {
                ni.role = tmpl_it->second.services[0].role;
            }
        }
        nodes_.push_back(ni);
    }

    // 解析连线 (topics) - 从 output port type 推导消息类型
    for (const auto& wire : wires) {
        TopicInfo ti;
        ti.topic = wire.topic;
        ti.from_node = wire.from_instance;
        ti.to_node = wire.to_instance;

        // 从源实例的 template 中查找 output port 的 type
        auto inst_it = instances.find(wire.from_instance);
        if (inst_it != instances.end()) {
            auto tmpl_it = templates.find(inst_it->second.template_name);
            if (tmpl_it != templates.end()) {
                for (const auto& port : tmpl_it->second.outputs) {
                    if (port.port == wire.from_port) {
                        ti.msg_type = port.type;
                        break;
                    }
                }
            }
        }
        topics_.push_back(ti);
    }

    // 从 template 的 services 中提取 endpoints
    for (const auto& [inst_name, inst] : instances) {
        auto tmpl_it = templates.find(inst.template_name);
        if (tmpl_it != templates.end()) {
            for (const auto& svc : tmpl_it->second.services) {
                for (const auto& ep : svc.endpoints) {
                    ServiceEndpointInfo sei;
                    sei.role = svc.role;
                    sei.endpoint = ep;
                    sei.instance = inst_name;
                    endpoints_.push_back(sei);
                }
            }
        }
    }

    // 尝试从 XML 解析 service_routing 补充 description
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(pipeline_path_.c_str()) == tinyxml2::XML_SUCCESS) {
        auto* root = doc.FirstChildElement("pipeline");
        if (root) {
            auto* sr = root->FirstChildElement("service_routing");
            if (sr) {
                for (auto* route = sr->FirstChildElement("route");
                     route; route = route->NextSiblingElement("route")) {
                    const char* path = route->Attribute("path");
                    const char* target = route->Attribute("target");
                    const char* endpoint = route->Attribute("endpoint");
                    if (target && endpoint) {
                        // 找到对应的 endpoint 并添加 path 作为 description
                        for (auto& ep : endpoints_) {
                            if (ep.role == target && ep.endpoint == endpoint) {
                                ep.description = path ? path : "";
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    appendLog(QString("[拓扑] 解析完成: %1 节点, %2 topics, %3 endpoints")
        .arg(nodes_.size()).arg(topics_.size()).arg(endpoints_.size()), "blue");
}

void InspectorWindow::populateTopologyTree()
{
    topology_tree_->clear();
    topic_combo_->clear();

    // 节点
    auto* nodes_item = new QTreeWidgetItem(topology_tree_, {"节点", "", ""});
    nodes_item->setExpanded(true);
    for (const auto& n : nodes_) {
        new QTreeWidgetItem(nodes_item, {
            QString::fromStdString(n.name),
            QString::fromStdString(n.role),
            QString::fromStdString(n.binary)
        });
    }

    // Topics
    auto* topics_item = new QTreeWidgetItem(topology_tree_, {"Topics", "", ""});
    topics_item->setExpanded(true);
    for (const auto& t : topics_) {
        auto* item = new QTreeWidgetItem(topics_item, {
            QString::fromStdString(t.topic),
            QString::fromStdString(t.msg_type),
            QString("%1 → %2").arg(
                QString::fromStdString(t.from_node),
                QString::fromStdString(t.to_node))
        });
        topic_combo_->addItem(QString::fromStdString(t.topic),
                              QString::fromStdString(t.msg_type));
    }

    // Services
    auto* svc_item = new QTreeWidgetItem(topology_tree_, {"Services", "", ""});
    svc_item->setExpanded(true);
    for (const auto& ep : endpoints_) {
        new QTreeWidgetItem(svc_item, {
            QString::fromStdString(ep.endpoint),
            QString::fromStdString(ep.role),
            QString::fromStdString(ep.instance)
        });
    }
}

void InspectorWindow::populateServiceTable()
{
    service_table_->setRowCount(static_cast<int>(endpoints_.size()));
    role_combo_->clear();

    std::set<std::string> roles;
    for (int i = 0; i < static_cast<int>(endpoints_.size()); ++i) {
        const auto& ep = endpoints_[i];
        service_table_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(ep.role)));
        service_table_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(ep.endpoint)));
        service_table_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(ep.instance)));
        roles.insert(ep.role);
    }

    for (const auto& r : roles) {
        role_combo_->addItem(QString::fromStdString(r));
    }
}

// ============================================================================
//  Topic 订阅
// ============================================================================

void InspectorWindow::onSubscribeTopic()
{
    if (topic_combo_->currentIndex() < 0) {
        appendLog("[警告] 请先选择一个 topic", "orange");
        return;
    }

    std::string topic = topic_combo_->currentText().toStdString();
    std::string msg_type = topic_combo_->currentData().toString().toStdString();

    onUnsubscribeAll();
    subscribed_topic_ = topic;

    if (msg_type == "FrameMsg" || msg_type == "frame") {
        frame_sub_ = factory_->createSubscriber<FrameMsg>(topic);
        frame_sub_->subscribe([this](const FrameMsg& msg) {
            handleFrameMsg(msg);
        });
        appendLog(QString("[订阅] 已订阅 %1 (FrameMsg)").arg(
            QString::fromStdString(topic)), "green");
    } else if (msg_type == "DetectionMsg" || msg_type == "detection") {
        detection_sub_ = factory_->createSubscriber<DetectionMsg>(topic);
        detection_sub_->subscribe([this](const DetectionMsg& msg) {
            handleDetectionMsg(msg);
        });
        appendLog(QString("[订阅] 已订阅 %1 (DetectionMsg)").arg(
            QString::fromStdString(topic)), "green");
    } else if (msg_type == "AnnotationMsg" || msg_type == "annotation") {
        annotation_sub_ = factory_->createSubscriber<AnnotationMsg>(topic);
        annotation_sub_->subscribe([this](const AnnotationMsg& msg) {
            handleAnnotationMsg(msg);
        });
        appendLog(QString("[订阅] 已订阅 %1 (AnnotationMsg)").arg(
            QString::fromStdString(topic)), "green");
    } else {
        appendLog(QString("[警告] 未知消息类型: %1").arg(
            QString::fromStdString(msg_type)), "orange");
    }
}

void InspectorWindow::onUnsubscribeAll()
{
    frame_sub_.reset();
    detection_sub_.reset();
    annotation_sub_.reset();
    subscribed_topic_.clear();
    appendLog("[取消] 已取消所有订阅", "gray");
}

// ============================================================================
//  消息处理
// ============================================================================

void InspectorWindow::handleFrameMsg(const FrameMsg& msg)
{
    ++frame_count_;
    uint32_t w = msg.width();
    uint32_t h = msg.height();
    uint32_t pt = msg.pixel_type();
    const std::string& raw_data = msg.data();
    std::vector<uint8_t> data(raw_data.begin(), raw_data.end());

    QMetaObject::invokeMethod(this, [this, data = std::move(data), w, h, pt,
                                     camera_id = msg.camera_id(),
                                     frame_num = msg.frame_num(),
                                     exposure = msg.exposure_time(),
                                     gain_val = msg.gain(),
                                     ts = msg.timestamp()]() {
        displayImage(data.data(), data.size(), w, h, pt);

        msg_viewer_->setPlainText(QString(
            "FrameMsg:\n"
            "  camera_id: %1\n"
            "  frame_num: %2\n"
            "  size: %3 x %4\n"
            "  pixel_type: %5\n"
            "  exposure: %6 us\n"
            "  gain: %7 dB\n"
            "  data_size: %8 bytes\n"
            "  timestamp: %9"
        ).arg(camera_id)
         .arg(frame_num)
         .arg(w).arg(h)
         .arg(pt)
         .arg(exposure, 0, 'f', 1)
         .arg(gain_val, 0, 'f', 2)
         .arg(data.size())
         .arg(ts));
    }, Qt::QueuedConnection);
}

void InspectorWindow::handleDetectionMsg(const DetectionMsg& msg)
{
    QMetaObject::invokeMethod(this, [this,
                                     frame_num = msg.frame_num(),
                                     object_count = msg.object_count(),
                                     ts = msg.timestamp(),
                                     proto = msg.protocol_string()]() {
        QString info = QString(
            "DetectionMsg:\n"
            "  frame_num: %1\n"
            "  object_count: %2\n"
            "  timestamp: %3\n"
            "  protocol: %4"
        ).arg(frame_num)
         .arg(object_count)
         .arg(ts)
         .arg(QString::fromStdString(proto));

        msg_viewer_->setPlainText(info);
        appendLog(QString("[检测] frame=%1 objects=%2")
            .arg(frame_num).arg(object_count), "blue");
    }, Qt::QueuedConnection);
}

void InspectorWindow::handleAnnotationMsg(const AnnotationMsg& msg)
{
    QMetaObject::invokeMethod(this, [this,
                                     frame_num = msg.frame_num(),
                                     tw = msg.template_width(),
                                     th = msg.template_height(),
                                     num_objs = msg.objects_size(),
                                     msg]() {
        QString info = QString(
            "AnnotationMsg:\n"
            "  frame_num: %1\n"
            "  template: %2 x %3\n"
            "  objects: %4\n"
        ).arg(frame_num)
         .arg(tw)
         .arg(th)
         .arg(num_objs);

        for (int i = 0; i < msg.objects_size(); ++i) {
            const auto& obj = msg.objects(i);
            info += QString("  [%1] x=%2 y=%3 angle=%4 score=%5 type=%6 id=%7\n")
                .arg(i)
                .arg(obj.x(), 0, 'f', 1)
                .arg(obj.y(), 0, 'f', 1)
                .arg(obj.angle(), 0, 'f', 2)
                .arg(obj.score(), 0, 'f', 3)
                .arg(obj.type())
                .arg(obj.id());
        }

        msg_viewer_->setPlainText(info);
    }, Qt::QueuedConnection);
}

// ============================================================================
//  图像显示
// ============================================================================

void InspectorWindow::displayImage(const uint8_t* data, size_t size,
                                   uint32_t width, uint32_t height,
                                   uint32_t pixel_type)
{
    if (!data || size == 0 || width == 0 || height == 0) {
        image_label_->setText("无图像数据");
        return;
    }

    cv::Mat cv_img;

    if (pixel_type == 0) {
        cv_img = cv::Mat(height, width, CV_8UC1, const_cast<uint8_t*>(data));
    } else if (pixel_type == 1) {
        cv::Mat bayer(height, width, CV_8UC1, const_cast<uint8_t*>(data));
        cv::cvtColor(bayer, cv_img, cv::COLOR_BayerRG2RGB);
    } else if (pixel_type == 2) {
        cv_img = cv::Mat(height, width, CV_8UC3, const_cast<uint8_t*>(data));
    } else if (pixel_type == 3) {
        cv_img = cv::Mat(height, width, CV_8UC3, const_cast<uint8_t*>(data));
    } else {
        cv_img = cv::Mat(height, width, CV_8UC1, const_cast<uint8_t*>(data));
    }

    QImage qimg;
    if (cv_img.channels() == 1) {
        qimg = QImage(cv_img.data, cv_img.cols, cv_img.rows,
                      static_cast<int>(cv_img.step), QImage::Format_Grayscale8);
    } else {
        cv::Mat rgb;
        cv::cvtColor(cv_img, rgb, cv::COLOR_BGR2RGB);
        qimg = QImage(rgb.data, rgb.cols, rgb.rows,
                      static_cast<int>(rgb.step), QImage::Format_RGB888);
    }

    QPixmap pixmap = QPixmap::fromImage(qimg).scaled(
        image_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image_label_->setPixmap(pixmap);
}

// ============================================================================
//  Service 调用
// ============================================================================

std::shared_ptr<IService<ServiceRequest, ServiceResponse>>
InspectorWindow::getOrCreateServiceClient(const std::string& role)
{
    auto it = service_clients_.find(role);
    if (it != service_clients_.end()) {
        return it->second;
    }

    auto svc = factory_->createService<ServiceRequest, ServiceResponse>(role);
    service_clients_[role] = svc;
    appendLog(QString("[Service] 创建客户端: %1").arg(
        QString::fromStdString(role)), "blue");
    return svc;
}

void InspectorWindow::onSendService()
{
    std::string role = role_combo_->currentText().toStdString();
    std::string endpoint = endpoint_combo_->currentText().toStdString();
    std::string payload = payload_edit_->text().toStdString();

    if (role.empty() || endpoint.empty()) {
        appendLog("[警告] 请选择 role 和 endpoint", "orange");
        return;
    }

    auto svc = getOrCreateServiceClient(role);
    if (!svc) {
        appendLog("[错误] 无法创建 service client", "red");
        return;
    }

    if (service_call_pending_[role].exchange(true)) {
        appendLog("[警告] 正在处理中，请稍等...", "orange");
        return;
    }

    btn_send_->setEnabled(false);
    auto t0 = std::chrono::steady_clock::now();

    appendLog(QString("[调用] %1/%2 payload=%3")
        .arg(QString::fromStdString(role),
             QString::fromStdString(endpoint),
             QString::fromStdString(payload)), "blue");

    std::thread([this, svc, endpoint, payload, role, t0]() {
        ServiceRequest req;
        req.set_endpoint(endpoint);
        req.set_payload(payload);

        try {
            ServiceResponse resp = svc->call(endpoint, req);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();

            QMetaObject::invokeMethod(this, [this, resp, elapsed, role]() {
                service_call_pending_[role].store(false);
                btn_send_->setEnabled(true);

                if (resp.success()) {
                    response_text_->setPlainText(
                        QString("Success (%1 ms)\n\n%2")
                            .arg(elapsed)
                            .arg(QString::fromStdString(resp.data())));
                    appendLog(QString("[响应] %1: 成功 (%2 ms)")
                        .arg(QString::fromStdString(role)).arg(elapsed), "green");
                } else {
                    response_text_->setPlainText(
                        QString("Failed (%1 ms)\n\n%2")
                            .arg(elapsed)
                            .arg(QString::fromStdString(resp.data())));
                    appendLog(QString("[响应] %1: 失败 (%2 ms)")
                        .arg(QString::fromStdString(role)).arg(elapsed), "red");
                }
            }, Qt::QueuedConnection);
        } catch (const std::exception& e) {
            std::string err = e.what();
            QMetaObject::invokeMethod(this, [this, err, role]() {
                service_call_pending_[role].store(false);
                btn_send_->setEnabled(true);
                response_text_->setPlainText(QString("异常: %1").arg(
                    QString::fromStdString(err)));
                appendLog(QString("[错误] %1: %2")
                    .arg(QString::fromStdString(role),
                         QString::fromStdString(err)), "red");
            }, Qt::QueuedConnection);
        }
    }).detach();
}

// ============================================================================
//  其他槽函数
// ============================================================================

void InspectorWindow::onClearLog()
{
    log_text_->clear();
}

void InspectorWindow::onRefreshTopology()
{
    if (!scheduler_.loadFromXml(pipeline_path_)) {
        appendLog("[错误] 无法重新加载 pipeline", "red");
        return;
    }
    parseTopology();
    populateTopologyTree();
    populateServiceTable();
    appendLog("[刷新] 拓扑已更新", "green");
}

// ============================================================================
//  日志
// ============================================================================

void InspectorWindow::appendLog(const QString& msg, const QString& color)
{
    if (!log_text_) return;
    QString time = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    log_text_->append(QString("<span style='color:%1'>[%2] %3</span>")
        .arg(color, time, msg));
}
