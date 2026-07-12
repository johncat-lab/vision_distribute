#include "camera_factory.h"
#include <hik_camera.h>  // vision_client 中的 HikCamera（实现 ICamera 接口）
#include "logger/logger.h"
#include <iostream>

// 静态成员初始化
std::map<std::string, std::function<std::unique_ptr<ICamera>()>> CameraFactory::creators_;
std::mutex CameraFactory::mutex_;
bool CameraFactory::initialized_ = false;

std::unique_ptr<ICamera> CameraFactory::create(const std::string& type) {
    // 首次调用时注册内置类型
    if (!initialized_) {
        registerBuiltinTypes();
        initialized_ = true;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = creators_.find(type);
    if (it != creators_.end()) {
        LOG_INFO("[CameraFactory] 创建相机实例: %s", type.c_str());
        return it->second();
    }
    
    LOG_ERROR("[CameraFactory] 不支持的相机类型: %s", type.c_str());
    LOG_INFO("[CameraFactory] 已注册的类型:");
    for (const auto& [name, _] : creators_) {
        LOG_INFO("  - %s", name.c_str());
    }
    
    return nullptr;
}

void CameraFactory::registerType(const std::string& type,
                                  std::function<std::unique_ptr<ICamera>()> creator) {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_INFO("[CameraFactory] 注册相机类型: %s", type.c_str());
    creators_[type] = std::move(creator);
}

std::vector<std::string> CameraFactory::getRegisteredTypes() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> types;
    for (const auto& [name, _] : creators_) {
        types.push_back(name);
    }
    return types;
}

void CameraFactory::registerBuiltinTypes() {
    LOG_INFO("[CameraFactory] 注册内置相机类型");
    
    // 注册海康相机
    registerType("hik", []() {
        return std::make_unique<HikCamera>();
    });
    
    // 未来可以在这里添加更多相机类型
    // registerType("basler", []() {
    //     return std::make_unique<BaslerCamera>();
    // });
    // registerType("dahua", []() {
    //     return std::make_unique<DahuaCamera>();
    // });
    // registerType("opencv", []() {
    //     return std::make_unique<OpenCVCamera>();
    // });
}
