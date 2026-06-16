#include "rpc/edge_manager.h"
#include "logger/logger.h"
#include <sstream>
#include <algorithm>

NodeEdgeManager::NodeEdgeManager(NodeFactory& factory, const std::string& node_name)
    : factory_(factory), node_name_(node_name), instance_name_(node_name)
{
}

void NodeEdgeManager::parseArgs(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // --instance <name>
        if (arg == "--instance" && i + 1 < argc) {
            instance_name_ = argv[++i];
            continue;
        }

        // --topic-map port1=topic1,port2=topic2
        if (arg == "--topic-map" && i + 1 < argc) {
            std::string map_str = argv[++i];
            std::istringstream iss(map_str);
            std::string token;
            while (std::getline(iss, token, ',')) {
                auto eq = token.find('=');
                if (eq != std::string::npos) {
                    std::string port = token.substr(0, eq);
                    std::string topic = token.substr(eq + 1);
                    // trim whitespace
                    port.erase(0, port.find_first_not_of(" \t"));
                    port.erase(port.find_last_not_of(" \t") + 1);
                    topic.erase(0, topic.find_first_not_of(" \t"));
                    topic.erase(topic.find_last_not_of(" \t") + 1);
                    topic_map_[port] = topic;
                }
            }
            continue;
        }
    }

    if (!topic_map_.empty()) {
        LOG_INFO("[EdgeManager:%s] topic-map 已加载 (%zu 条映射):",
                 node_name_.c_str(), topic_map_.size());
        for (const auto& [port, topic] : topic_map_) {
            LOG_INFO("  %s -> %s", port.c_str(), topic.c_str());
        }
    }
    if (instance_name_ != node_name_) {
        LOG_INFO("[EdgeManager:%s] 实例名: %s", node_name_.c_str(), instance_name_.c_str());
    }
}

std::string NodeEdgeManager::getTopic(const std::string& port_name) const {
    // 1. 先查 --topic-map
    auto it = topic_map_.find(port_name);
    if (it != topic_map_.end()) {
        return it->second;
    }
    // 2. 再查 default_topics
    auto dit = default_topics_.find(port_name);
    if (dit != default_topics_.end()) {
        return dit->second;
    }
    return "";
}

void NodeEdgeManager::setDefaultTopic(const std::string& port_name, const std::string& topic) {
    default_topics_[port_name] = topic;
}

std::string NodeEdgeManager::resolveTopic(const std::string& port_name,
                                            const std::string& fallback) const {
    // 1. --topic-map 优先
    auto it = topic_map_.find(port_name);
    if (it != topic_map_.end()) {
        LOG_INFO("[EdgeManager:%s] 端口 '%s' -> topic '%s' (topic-map)",
                 node_name_.c_str(), port_name.c_str(), it->second.c_str());
        return it->second;
    }
    // 2. default_topics
    auto dit = default_topics_.find(port_name);
    if (dit != default_topics_.end()) {
        LOG_INFO("[EdgeManager:%s] 端口 '%s' -> topic '%s' (default)",
                 node_name_.c_str(), port_name.c_str(), dit->second.c_str());
        return dit->second;
    }
    // 3. fallback 参数
    if (!fallback.empty()) {
        LOG_INFO("[EdgeManager:%s] 端口 '%s' -> topic '%s' (fallback)",
                 node_name_.c_str(), port_name.c_str(), fallback.c_str());
        return fallback;
    }
    LOG_WARN("[EdgeManager:%s] 端口 '%s' 无映射，无默认值，无 fallback",
             node_name_.c_str(), port_name.c_str());
    return port_name;  // 最后兜底：用端口名本身作为 topic
}
