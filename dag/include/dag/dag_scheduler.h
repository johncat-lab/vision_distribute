#pragma once
#include "rpc/node_manifest.h"
#include <string>
#include <vector>
#include <map>
#include <set>

/// @brief DAG 调度器：解析 pipeline.xml，校验拓扑，计算启动顺序
class DagScheduler {
public:
    /// @brief pipeline.xml 中的模板定义
    struct TemplateDef {
        std::string name;
        std::string binary;
        std::vector<NodeManifest::PortInfo> inputs;
        std::vector<NodeManifest::PortInfo> outputs;
        std::vector<NodeManifest::ServiceInfo> services;
    };

    /// @brief pipeline.xml 中的实例定义
    struct InstanceDef {
        std::string name;
        std::string template_name;
        std::string config_file;
    };

    /// @brief pipeline.xml 中的连线
    struct WireDef {
        std::string from_instance;
        std::string from_port;
        std::string to_instance;
        std::string to_port;
        std::string topic;
    };

    /// @brief 从 pipeline.xml 加载 DAG 定义
    bool loadFromXml(const std::string& pipeline_path);

    /// @brief 校验 DAG：无环、无缺失依赖、端口类型匹配、所有 input 已连接
    /// @param error_msg 校验失败时的错误描述
    /// @return true=校验通过
    bool validate(std::string& error_msg) const;

    /// @brief 拓扑排序，计算启动顺序（数据流 + service 依赖）
    /// @return 按启动顺序排列的实例名列表
    std::vector<std::string> computeStartupOrder() const;

    /// @brief 获取某个实例对应的 binary 名
    std::string getBinary(const std::string& instance_name) const;

    /// @brief 获取某个实例的配置文件路径
    std::string getConfig(const std::string& instance_name) const;

    /// @brief 获取某个实例的 --topic-map 字符串
    std::string getTopicMap(const std::string& instance_name) const;

    // ========== Getters ==========
    const std::map<std::string, TemplateDef>& templates() const { return templates_; }
    const std::map<std::string, InstanceDef>& instances() const { return instances_; }
    const std::vector<WireDef>& wires() const { return wires_; }

private:
    /// @brief 基于当前 wires + service requires 做拓扑排序
    std::vector<std::string> topoSort() const;

    std::map<std::string, TemplateDef> templates_;  // name → TemplateDef
    std::map<std::string, InstanceDef> instances_;  // name → InstanceDef
    std::vector<WireDef> wires_;
};
