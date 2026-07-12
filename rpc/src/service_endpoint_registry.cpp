#include "rpc/service_endpoint_registry.h"
#include "logger/logger.h"
#include <sstream>
#include <algorithm>

void ServiceEndpointRegistry::registerEndpoint(const EndpointDesc& desc,
                                                EndpointHandler handler) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    auto entry = std::make_shared<EndpointEntry>();
    entry->desc = desc;
    entry->handler = std::move(handler);
    entry->last_reset = std::chrono::steady_clock::now();
    endpoints_[desc.name] = entry;

    std::string perms = desc.requires_admin ? "admin" : "public";
    std::string rate = desc.rate_limit_per_sec > 0
                           ? std::to_string(desc.rate_limit_per_sec) + "/s"
                           : "unlimited";
    LOG_INFO("[EndpointRegistry] 注册端点 '%s' (%s, %s)",
             desc.name.c_str(), perms.c_str(), rate.c_str());
}

ServiceResponse ServiceEndpointRegistry::handle(const std::string& endpoint_name,
                                                  const ServiceRequest& req,
                                                  bool is_admin) {
    std::shared_ptr<EndpointEntry> entry;
    {
        std::lock_guard<std::mutex> lock(endpoints_mutex_);
        auto it = endpoints_.find(endpoint_name);
        if (it == endpoints_.end()) {
            LOG_WARN("[EndpointRegistry] 端点未找到: %s", endpoint_name.c_str());
            return notFound(endpoint_name);
        }
        entry = it->second;
    }

    // 权限检查
    if (entry->desc.requires_admin && !is_admin) {
        LOG_WARN("[EndpointRegistry] 权限不足: %s", endpoint_name.c_str());
        return permissionDenied(endpoint_name);
    }

    // 限流检查
    if (entry->desc.rate_limit_per_sec > 0 && !checkRateLimit(*entry)) {
        LOG_WARN("[EndpointRegistry] 触发限流: %s", endpoint_name.c_str());
        return rateLimited(endpoint_name);
    }

    // 实际处理
    try {
        return entry->handler(req);
    } catch (const std::exception& e) {
        LOG_ERROR("[EndpointRegistry] 端点 '%s' 处理异常: %s",
                  endpoint_name.c_str(), e.what());
        ServiceResponse resp;
        resp.set_success(false);
        resp.set_data(std::string("internal_error: ") + e.what());
        return resp;
    }
}

std::vector<EndpointDesc> ServiceEndpointRegistry::listEndpoints() const {
    std::vector<EndpointDesc> result;
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    for (const auto& [name, entry] : endpoints_) {
        result.push_back(entry->desc);
    }
    return result;
}

bool ServiceEndpointRegistry::hasEndpoint(const std::string& name) const {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    return endpoints_.find(name) != endpoints_.end();
}

void ServiceEndpointRegistry::resetRateLimitCounters() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    for (const auto& [name, entry] : endpoints_) {
        std::lock_guard<std::mutex> counter_lock(entry->counter_mutex);
        entry->request_count.store(0);
        entry->last_reset = now;
    }
}

bool ServiceEndpointRegistry::checkRateLimit(EndpointEntry& entry) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(entry.counter_mutex);

    // 每 1 秒重置一次
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - entry.last_reset);
    if (elapsed.count() >= 1000) {
        entry.request_count.store(0);
        entry.last_reset = now;
    }

    int count = entry.request_count.fetch_add(1) + 1;
    return count <= entry.desc.rate_limit_per_sec;
}

ServiceResponse ServiceEndpointRegistry::permissionDenied(const std::string& endpoint) {
    ServiceResponse resp;
    resp.set_success(false);
    resp.set_data("permission_denied: " + endpoint + " requires admin");
    return resp;
}

ServiceResponse ServiceEndpointRegistry::rateLimited(const std::string& endpoint) {
    ServiceResponse resp;
    resp.set_success(false);
    resp.set_data("rate_limit_exceeded: " + endpoint);
    return resp;
}

ServiceResponse ServiceEndpointRegistry::notFound(const std::string& endpoint) {
    ServiceResponse resp;
    resp.set_success(false);
    resp.set_data("endpoint_not_found: " + endpoint);
    return resp;
}
