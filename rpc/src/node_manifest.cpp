#include "rpc/node_manifest.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

using json = nlohmann::json;

std::string NodeManifest::toJson() const {
    json j;
    j["name"] = name;
    j["binary"] = binary;
    j["version"] = version;
    j["config_file"] = config_file;

    // data_ports
    json dp;
    json inputs_arr = json::array();
    for (const auto& p : inputs) {
        inputs_arr.push_back({{"port", p.port}, {"type", p.type}, {"desc", p.desc}});
    }
    json outputs_arr = json::array();
    for (const auto& p : outputs) {
        outputs_arr.push_back({{"port", p.port}, {"type", p.type}, {"desc", p.desc}});
    }
    dp["inputs"] = inputs_arr;
    dp["outputs"] = outputs_arr;
    j["data_ports"] = dp;

    // provides_services
    json svc_arr = json::array();
    for (const auto& s : provides_services) {
        svc_arr.push_back({{"role", s.role}, {"endpoints", s.endpoints}});
    }
    j["provides_services"] = svc_arr;

    // requires_services
    j["requires_services"] = requires_services;

    return j.dump(2);
}

NodeManifest NodeManifest::fromJson(const std::string& str) {
    NodeManifest m;
    json j;
    try {
        j = json::parse(str);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("NodeManifest JSON parse error: ") + e.what());
    }

    m.name        = j.value("name", "");
    m.binary      = j.value("binary", "");
    m.version     = j.value("version", "");
    m.config_file = j.value("config_file", "");

    // data_ports
    if (j.contains("data_ports")) {
        const auto& dp = j["data_ports"];
        if (dp.contains("inputs")) {
            for (const auto& p : dp["inputs"]) {
                PortInfo pi;
                pi.port = p.value("port", "");
                pi.type = p.value("type", "");
                pi.desc = p.value("desc", "");
                m.inputs.push_back(pi);
            }
        }
        if (dp.contains("outputs")) {
            for (const auto& p : dp["outputs"]) {
                PortInfo pi;
                pi.port = p.value("port", "");
                pi.type = p.value("type", "");
                pi.desc = p.value("desc", "");
                m.outputs.push_back(pi);
            }
        }
    }

    // provides_services
    if (j.contains("provides_services")) {
        for (const auto& s : j["provides_services"]) {
            ServiceInfo si;
            si.role = s.value("role", "");
            if (s.contains("endpoints")) {
                si.endpoints = s["endpoints"].get<std::vector<std::string>>();
            }
            m.provides_services.push_back(si);
        }
    }

    // requires_services
    if (j.contains("requires_services")) {
        m.requires_services = j["requires_services"].get<std::vector<std::string>>();
    }

    return m;
}
