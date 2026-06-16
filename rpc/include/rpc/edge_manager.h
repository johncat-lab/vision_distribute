#pragma once
#include "rpc/node_factory.h"
#include "rpc/publisher.h"
#include "rpc/subscriber.h"
#include <string>
#include <map>
#include <memory>

/// @brief 端口到 topic 的映射管理器
/// 节点通过端口名（逻辑名）创建 publisher/subscriber，
/// 实际 topic 由 --topic-map 参数或默认值决定。
class NodeEdgeManager {
public:
    /// @param factory 已有的 NodeFactory 实例
    /// @param node_name 节点类型名（如 "camera_node"）
    NodeEdgeManager(NodeFactory& factory, const std::string& node_name);

    /// @brief 解析 --topic-map 和 --instance 参数
    /// 格式: --topic-map port1=topic1,port2=topic2 --instance <name>
    /// 未映射的端口使用 default_topics 中的默认值
    void parseArgs(int argc, char* argv[]);

    /// @brief 获取实例名（--instance 参数值，默认为 node_name）
    const std::string& instanceName() const { return instance_name_; }

    /// @brief 创建发布者（通过端口名）
    /// @tparam T 消息类型
    /// @param port_name 逻辑端口名（如 "frame_output"）
    /// @param default_topic 未映射时的默认 topic（向后兼容）
    template<typename T>
    std::shared_ptr<IPublisher<T>> publish(const std::string& port_name,
                                            const std::string& default_topic = "");

    /// @brief 创建订阅者（通过端口名）
    /// @tparam T 消息类型
    /// @param port_name 逻辑端口名（如 "frame_input"）
    /// @param default_topic 未映射时的默认 topic（向后兼容）
    template<typename T>
    std::shared_ptr<ISubscriber<T>> subscribe(const std::string& port_name,
                                               const std::string& default_topic = "");

    /// @brief 获取端口对应的 topic（找不到返回空字符串）
    std::string getTopic(const std::string& port_name) const;

    /// @brief 设置默认 topic（不通过 --topic-map 时使用）
    void setDefaultTopic(const std::string& port_name, const std::string& topic);

    /// @brief 获取所有端口→topic映射
    const std::map<std::string, std::string>& topicMap() const { return topic_map_; }

private:
    std::string resolveTopic(const std::string& port_name, const std::string& fallback) const;

    NodeFactory& factory_;
    std::string node_name_;
    std::string instance_name_;
    std::map<std::string, std::string> topic_map_;      // port → topic (来自 --topic-map)
    std::map<std::string, std::string> default_topics_; // port → default topic (硬编码)
};

// ========== Template implementations ==========

template<typename T>
std::shared_ptr<IPublisher<T>> NodeEdgeManager::publish(
    const std::string& port_name, const std::string& default_topic)
{
    std::string topic = resolveTopic(port_name, default_topic);
    return factory_.createPublisher<T>(topic);
}

template<typename T>
std::shared_ptr<ISubscriber<T>> NodeEdgeManager::subscribe(
    const std::string& port_name, const std::string& default_topic)
{
    std::string topic = resolveTopic(port_name, default_topic);
    return factory_.createSubscriber<T>(topic);
}
