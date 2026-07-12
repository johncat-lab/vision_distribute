#pragma once
#include "icamera.h"           // 需要完整定义，不能只前向声明
#include <memory>
#include <string>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

/// @brief 相机工厂：根据配置类型创建具体的相机实例
/// 
/// 使用示例:
///   auto camera = CameraFactory::create("hik");
///   auto camera = CameraFactory::create("basler");
/// 
/// 注册新类型:
///   CameraFactory::registerType("basler", []() {
///       return std::make_unique<BaslerCamera>();
///   });
class CameraFactory {
public:
    /// @brief 创建相机实例
    /// @param type 相机类型 ("hik", "basler", "dahua", "opencv", ...)
    /// @return 相机实例指针，失败返回 nullptr
    static std::unique_ptr<ICamera> create(const std::string& type);

    /// @brief 注册新的相机类型 (运行时扩展)
    /// @param type 类型标识
    /// @param creator 创建函数
    static void registerType(const std::string& type,
                             std::function<std::unique_ptr<ICamera>()> creator);

    /// @brief 获取所有已注册的相机类型
    static std::vector<std::string> getRegisteredTypes();

private:
    // 默认注册内置类型
    static void registerBuiltinTypes();
    
    // 类型注册表
    static std::map<std::string, std::function<std::unique_ptr<ICamera>()>> creators_;
    static std::mutex mutex_;
    static bool initialized_;
};
