#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QTreeWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSplitter>
#include <QGroupBox>
#include <QTabWidget>
#include <QTimer>
#include <QStatusBar>
#include <QTableWidget>

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>

#include "rpc/message_types.h"
#include "rpc/publisher.h"
#include "rpc/subscriber.h"
#include "rpc/service.h"
#include "rpc/node_factory.h"
#include "rpc/types.h"
#include "dag/dag_scheduler.h"

// ============================================================================
//  Topic/Service 信息结构
// ============================================================================

struct TopicInfo {
    std::string topic;       // topic 名称
    std::string from_node;   // 发布者节点
    std::string to_node;     // 订阅者节点
    std::string msg_type;    // 消息类型 (frame/detection/annotation)
};

struct ServiceEndpointInfo {
    std::string role;        // 节点角色 (camera/detector/comm)
    std::string endpoint;    // 端点名称
    std::string instance;    // 实例名称
    std::string description; // 描述
};

struct NodeInfo {
    std::string name;        // 实例名
    std::string role;        // 角色
    std::string binary;      // 可执行文件
};

// ============================================================================
//  InspectorWindow - 系统可视化调试工具
// ============================================================================

class InspectorWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit InspectorWindow(const std::string& pipeline_xml,
                             QWidget* parent = nullptr);
    ~InspectorWindow() override;

private slots:
    void onSubscribeTopic();
    void onUnsubscribeAll();
    void onSendService();
    void onClearLog();
    void onRefreshTopology();

private:
    // UI 构建
    void setupUI();
    QWidget* createTopologyPanel();
    QWidget* createViewerPanel();
    QWidget* createServicePanel();
    QWidget* createLogPanel();

    // 拓扑解析
    void parseTopology();
    void populateTopologyTree();
    void populateServiceTable();

    // 消息处理
    void handleFrameMsg(const FrameMsg& msg);
    void handleDetectionMsg(const DetectionMsg& msg);
    void handleAnnotationMsg(const AnnotationMsg& msg);

    // 图像显示
    void displayImage(const uint8_t* data, size_t size,
                      uint32_t width, uint32_t height,
                      uint32_t pixel_type);

    // 日志
    void appendLog(const QString& msg, const QString& color = "black");

    // Service 客户端管理
    std::shared_ptr<IService<ServiceRequest, ServiceResponse>>
    getOrCreateServiceClient(const std::string& role);

    // 成员
    std::string pipeline_path_;
    DagScheduler scheduler_;
    std::unique_ptr<NodeFactory> factory_;
    NodeConfig node_config_;

    // 拓扑数据
    std::vector<NodeInfo> nodes_;
    std::vector<TopicInfo> topics_;
    std::vector<ServiceEndpointInfo> endpoints_;

    // Subscriber 管理
    std::shared_ptr<ISubscriber<FrameMsg>> frame_sub_;
    std::shared_ptr<ISubscriber<DetectionMsg>> detection_sub_;
    std::shared_ptr<ISubscriber<AnnotationMsg>> annotation_sub_;
    std::string subscribed_topic_;

    // Service 客户端缓存
    std::map<std::string, std::shared_ptr<IService<ServiceRequest, ServiceResponse>>>
        service_clients_;
    std::map<std::string, std::atomic<bool>> service_call_pending_;

    // UI 组件
    QTreeWidget*       topology_tree_ = nullptr;
    QTableWidget*      service_table_ = nullptr;
    QLabel*            image_label_ = nullptr;
    QTextEdit*         msg_viewer_ = nullptr;
    QTextEdit*         log_text_ = nullptr;
    QComboBox*         topic_combo_ = nullptr;
    QPushButton*       btn_subscribe_ = nullptr;
    QComboBox*         role_combo_ = nullptr;
    QComboBox*         endpoint_combo_ = nullptr;
    QLineEdit*         payload_edit_ = nullptr;
    QPushButton*       btn_send_ = nullptr;
    QTextEdit*         response_text_ = nullptr;
    QLabel*            status_info_ = nullptr;

    // FPS 计算
    int frame_count_ = 0;
    double fps_ = 0.0;
    QTimer* fps_timer_ = nullptr;
};
