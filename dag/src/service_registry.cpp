#include "dag/service_registry.h"
#include "logger/logger.h"
#include <algorithm>
#include <thread>

ServiceRegistry::ServiceRegistry(NodeFactory& factory)
    : factory_(factory)
{
}

void ServiceRegistry::registerRole(const std::string& role,
                                    const std::string& instance_name,
                                    const std::string& service_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    ServiceEndpoint ep;
    ep.instance_name = instance_name;
    ep.service_name = service_name;
    ep.role = role;
    registry_[role].push_back(ep);
    LOG_INFO("[ServiceRegistry] 注册: role='%s' instance='%s' service='%s'",
             role.c_str(), instance_name.c_str(), service_name.c_str());
}

std::vector<ServiceEndpoint> ServiceRegistry::discover(const std::string& role) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registry_.find(role);
    if (it != registry_.end()) {
        return it->second;
    }
    return {};
}

bool ServiceRegistry::waitForRole(const std::string& role, int timeout_ms) const {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = registry_.find(role);
            if (it != registry_.end() && !it->second.empty()) {
                return true;
            }
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            LOG_WARN("[ServiceRegistry] 等待 role='%s' 超时 (%d ms)", role.c_str(), timeout_ms);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::shared_ptr<IService<ServiceRequest, ServiceResponse>>
ServiceRegistry::createClient(const std::string& role, int index) {
    auto endpoints = discover(role);
    if (endpoints.empty()) {
        LOG_ERROR("[ServiceRegistry] 无法创建 client: role='%s' 无 provider", role.c_str());
        return nullptr;
    }
    if (index < 0 || static_cast<size_t>(index) >= endpoints.size()) {
        LOG_ERROR("[ServiceRegistry] 无法创建 client: role='%s' index=%d 越界 (共 %zu 个)",
                  role.c_str(), index, endpoints.size());
        return nullptr;
    }
    const auto& ep = endpoints[index];
    LOG_INFO("[ServiceRegistry] 创建 client: role='%s' -> instance='%s' service='%s'",
             role.c_str(), ep.instance_name.c_str(), ep.service_name.c_str());
    return factory_.createService<ServiceRequest, ServiceResponse>(ep.service_name);
}

void ServiceRegistry::unregister(const std::string& instance_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [role, endpoints] : registry_) {
        endpoints.erase(
            std::remove_if(endpoints.begin(), endpoints.end(),
                [&](const ServiceEndpoint& ep) {
                    return ep.instance_name == instance_name;
                }),
            endpoints.end());
    }
    LOG_INFO("[ServiceRegistry] 注销: instance='%s'", instance_name.c_str());
}
