#include "dag/dag_scheduler.h"
#include <tinyxml2.h>
#include <algorithm>
#include <sstream>
#include <queue>
#include <stdexcept>

using namespace tinyxml2;

// ========== 辅助：分割逗号分隔字符串 ==========
static std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, ',')) {
        // trim
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (!token.empty()) result.push_back(token);
    }
    return result;
}

// ========== loadFromXml ==========
bool DagScheduler::loadFromXml(const std::string& pipeline_path) {
    XMLDocument doc;
    if (doc.LoadFile(pipeline_path.c_str()) != XML_SUCCESS) {
        return false;
    }

    XMLElement* root = doc.FirstChildElement("pipeline");
    if (!root) return false;

    // ===== <templates> =====
    XMLElement* templates_elem = root->FirstChildElement("templates");
    if (templates_elem) {
        for (XMLElement* t = templates_elem->FirstChildElement("template"); t; t = t->NextSiblingElement("template")) {
            TemplateDef td;
            td.name = t->Attribute("name") ? t->Attribute("name") : "";

            XMLElement* bin = t->FirstChildElement("binary");
            if (bin && bin->GetText()) td.binary = bin->GetText();

            for (XMLElement* in = t->FirstChildElement("in"); in; in = in->NextSiblingElement("in")) {
                NodeManifest::PortInfo pi;
                pi.port = in->Attribute("port") ? in->Attribute("port") : "";
                pi.type = in->Attribute("type") ? in->Attribute("type") : "";
                td.inputs.push_back(pi);
            }
            for (XMLElement* out = t->FirstChildElement("out"); out; out = out->NextSiblingElement("out")) {
                NodeManifest::PortInfo pi;
                pi.port = out->Attribute("port") ? out->Attribute("port") : "";
                pi.type = out->Attribute("type") ? out->Attribute("type") : "";
                td.outputs.push_back(pi);
            }
            for (XMLElement* svc = t->FirstChildElement("service"); svc; svc = svc->NextSiblingElement("service")) {
                NodeManifest::ServiceInfo si;
                si.role = svc->Attribute("role") ? svc->Attribute("role") : "";
                const char* eps = svc->Attribute("endpoints");
                if (eps) si.endpoints = splitComma(eps);
                td.services.push_back(si);
            }

            templates_[td.name] = td;
        }
    }

    // ===== <instances> =====
    XMLElement* instances_elem = root->FirstChildElement("instances");
    if (instances_elem) {
        for (XMLElement* inst = instances_elem->FirstChildElement("instance"); inst; inst = inst->NextSiblingElement("instance")) {
            InstanceDef id;
            id.name = inst->Attribute("name") ? inst->Attribute("name") : "";
            id.template_name = inst->Attribute("template") ? inst->Attribute("template") : "";

            XMLElement* cfg = inst->FirstChildElement("config");
            if (cfg && cfg->GetText()) id.config_file = cfg->GetText();

            instances_[id.name] = id;
        }
    }

    // ===== <wiring> =====
    XMLElement* wiring_elem = root->FirstChildElement("wiring");
    if (wiring_elem) {
        for (XMLElement* w = wiring_elem->FirstChildElement("wire"); w; w = w->NextSiblingElement("wire")) {
            WireDef wd;
            wd.from_instance = w->Attribute("from") ? w->Attribute("from") : "";
            wd.from_port     = w->Attribute("port") ? w->Attribute("port") : "";
            wd.to_instance   = w->Attribute("to")   ? w->Attribute("to")   : "";
            wd.to_port       = w->Attribute("port") ? w->Attribute("port") : "";
            // 注意: XML 中 from 和 to 各有自己的 port 属性
            // 重新读取：<wire from="X" port="P1" to="Y" port="P2"> 是不合法的（两个 port）
            // spec 格式: <wire from="cam_left" port="frame_output" to="det_left" port="frame_input" topic="..."/>
            // 但 XML 中不能有两个同名属性。需要看 spec 中的实际格式。
            // 实际 spec 中: from="cam_left" port="frame_output" to="det_left" port="frame_input"
            // 这里 port 出现两次，XML 不允许。需要调整为 from_port 和 to_port。
            // 但 spec 定义如此，这里先用 Attribute 读取（tinyxml2 只保留最后一个）。
            // 实际上需要修改 spec 中的 wire 格式，或使用 from_port/to_port。
            wd.topic = w->Attribute("topic") ? w->Attribute("topic") : "";

            // 重新正确解析: 由于 XML 不允许重复属性名，
            // spec 中的 port 在 wire 中有歧义。
            // 实际使用: from="X" from_port="P" to="Y" to_port="Q" topic="T"
            // 但 spec 写的是 port="..." 两次。这里先用 from_port / to_port 尝试读取，
            // 回退到 port（如果只有一个）。
            const char* from_port = w->Attribute("from_port");
            const char* to_port   = w->Attribute("to_port");
            const char* port_attr = w->Attribute("port");

            if (from_port) wd.from_port = from_port;
            if (to_port)   wd.to_port = to_port;

            // 如果 XML 只有单个 port 属性（spec 兼容），用 port 作为 from_port
            if (!from_port && !to_port && port_attr) {
                wd.from_port = port_attr;
                wd.to_port = port_attr;  // 不推荐，但兼容
            }

            wires_.push_back(wd);
        }
    }

    return true;
}

// ========== validate ==========
bool DagScheduler::validate(std::string& error_msg) const {
    std::ostringstream errors;

    // 1. 检查所有实例引用的模板是否存在
    for (const auto& [name, inst] : instances_) {
        if (!inst.template_name.empty() && templates_.find(inst.template_name) == templates_.end()) {
            errors << "实例 '" << name << "' 引用了不存在的模板 '" << inst.template_name << "'\n";
        }
    }

    // 2. 检查 wire 引用的实例和端口是否存在
    for (const auto& w : wires_) {
        // 检查实例
        if (instances_.find(w.from_instance) == instances_.end()) {
            errors << "wire: 源实例 '" << w.from_instance << "' 不存在\n";
            continue;
        }
        if (instances_.find(w.to_instance) == instances_.end()) {
            errors << "wire: 目标实例 '" << w.to_instance << "' 不存在\n";
            continue;
        }

        // 检查端口
        const auto& from_tmpl = templates_.at(instances_.at(w.from_instance).template_name);
        const auto& to_tmpl   = templates_.at(instances_.at(w.to_instance).template_name);

        bool from_port_found = false;
        for (const auto& p : from_tmpl.outputs) {
            if (p.port == w.from_port) { from_port_found = true; break; }
        }
        if (!from_port_found) {
            errors << "wire: 源实例 '" << w.from_instance << "' 的 output 端口 '" << w.from_port << "' 不存在\n";
        }

        bool to_port_found = false;
        for (const auto& p : to_tmpl.inputs) {
            if (p.port == w.to_port) { to_port_found = true; break; }
        }
        if (!to_port_found) {
            errors << "wire: 目标实例 '" << w.to_instance << "' 的 input 端口 '" << w.to_port << "' 不存在\n";
        }

        // 3. 类型匹配
        if (from_port_found && to_port_found) {
            std::string from_type, to_type;
            for (const auto& p : from_tmpl.outputs)
                if (p.port == w.from_port) { from_type = p.type; break; }
            for (const auto& p : to_tmpl.inputs)
                if (p.port == w.to_port) { to_type = p.type; break; }
            if (from_type != to_type) {
                errors << "wire: 类型不匹配 " << w.from_instance << "/" << w.from_port
                       << " (" << from_type << ") -> " << w.to_instance << "/" << w.to_port
                       << " (" << to_type << ")\n";
            }
        }

        // 4. 自连接禁止
        if (w.from_instance == w.to_instance) {
            errors << "wire: 自连接禁止 " << w.from_instance << " -> " << w.to_instance << "\n";
        }
    }

    // 5. 环检测
    auto order = topoSort();
    if (order.empty() && !instances_.empty()) {
        errors << "DAG 中存在环\n";
    }

    std::string result = errors.str();
    if (!result.empty()) {
        error_msg = result;
        return false;
    }
    return true;
}

// ========== topoSort (Kahn's algorithm) ==========
std::vector<std::string> DagScheduler::topoSort() const {
    // 构建邻接表 + 入度
    std::map<std::string, std::set<std::string>> adj;    // from → {to}
    std::map<std::string, int> in_degree;

    for (const auto& [name, _] : instances_) {
        in_degree[name] = 0;
        adj[name] = {};
    }

    // 数据流依赖
    for (const auto& w : wires_) {
        if (adj[w.from_instance].insert(w.to_instance).second) {
            in_degree[w.to_instance]++;
        }
    }

    // Service 依赖: requires_services 推导
    // 构建 role → provider instances 映射
    std::map<std::string, std::vector<std::string>> role_providers;
    for (const auto& [name, inst] : instances_) {
        auto tit = templates_.find(inst.template_name);
        if (tit == templates_.end()) continue;
        for (const auto& svc : tit->second.services) {
            role_providers[svc.role].push_back(name);
        }
    }

    // 对每个有 requires_services 的实例，添加依赖边
    for (const auto& [name, inst] : instances_) {
        auto tit = templates_.find(inst.template_name);
        if (tit == templates_.end()) continue;

        // 检查 requires_services（存在 manifest 中，但这里从 template 中读取）
        // TODO: 当前 template 中没有 requires_services 字段，
        // 后续可以从 --describe 扫描时写入 pipeline.xml 的 template
        // 暂时从 XML 的 <template> 中解析 <requires> 元素
    }

    // Kahn's algorithm
    std::queue<std::string> q;
    for (const auto& [name, deg] : in_degree) {
        if (deg == 0) q.push(name);
    }

    std::vector<std::string> result;
    while (!q.empty()) {
        std::string curr = q.front();
        q.pop();
        result.push_back(curr);

        for (const auto& next : adj[curr]) {
            if (--in_degree[next] == 0) {
                q.push(next);
            }
        }
    }

    // 如果结果数量 != 实例数量，说明有环
    if (result.size() != instances_.size()) {
        return {};  // 空 = 有环
    }
    return result;
}

// ========== computeStartupOrder ==========
std::vector<std::string> DagScheduler::computeStartupOrder() const {
    return topoSort();
}

// ========== getBinary ==========
std::string DagScheduler::getBinary(const std::string& instance_name) const {
    auto iit = instances_.find(instance_name);
    if (iit == instances_.end()) return "";
    auto tit = templates_.find(iit->second.template_name);
    if (tit == templates_.end()) return "";
    return tit->second.binary;
}

// ========== getConfig ==========
std::string DagScheduler::getConfig(const std::string& instance_name) const {
    auto iit = instances_.find(instance_name);
    if (iit == instances_.end()) return "";
    return iit->second.config_file;
}

// ========== getTopicMap ==========
std::string DagScheduler::getTopicMap(const std::string& instance_name) const {
    // 收集该实例参与的所有 wire，生成 port=topic 映射
    std::map<std::string, std::string> port_topics;

    for (const auto& w : wires_) {
        if (w.from_instance == instance_name && !w.from_port.empty() && !w.topic.empty()) {
            port_topics[w.from_port] = w.topic;
        }
        if (w.to_instance == instance_name && !w.to_port.empty() && !w.topic.empty()) {
            port_topics[w.to_port] = w.topic;
        }
    }

    if (port_topics.empty()) return "";

    std::ostringstream oss;
    bool first = true;
    for (const auto& [port, topic] : port_topics) {
        if (!first) oss << ",";
        oss << port << "=" << topic;
        first = false;
    }
    return oss.str();
}
