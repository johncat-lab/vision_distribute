# Camera Node 抽象接口改造总结

## ✅ 改造完成

**完成时间**: 2024-06-26  
**改造目标**: 将 `camera_node` 从硬编码依赖 `HikCamera` 改为使用 `ICamera` 抽象接口，支持多品牌相机即插即用

---

## 📋 改造内容

### 1. 新增文件

#### `camera_node/include/camera_factory.h`
- **功能**: 相机工厂类头文件
- **核心方法**:
  - `create(type)`: 根据类型创建相机实例
  - `registerType(type, creator)`: 运行时注册新相机类型
  - `getRegisteredTypes()`: 获取所有已注册类型

#### `camera_node/src/camera_factory.cpp`
- **功能**: 相机工厂实现
- **内置注册**: 自动注册 `hik` 类型
- **扩展机制**: 支持运行时注册新类型（basler, dahua, opencv 等）

#### 复制的 vision_client 文件
- `camera_node/include/icamera.h` - 相机抽象接口（46 行）
- `camera_node/include/camera_types.h` - 数据类型定义（60 行）
- `camera_node/include/hik_camera.h` - 海康实现（71 行，继承 ICamera）
- `camera_node/src/hik_camera.cpp` - 海康实现（628 行）

### 2. 修改文件

#### `camera_node/camera_node.h`
**改动**:
```cpp
// 修改前
#include <hik_camera.h>
HikCamera camera_;  // 硬编码

// 修改后
#include <icamera.h>
#include <camera_factory.h>
std::unique_ptr<ICamera> camera_;  // 抽象接口

// 新增配置字段
struct CameraConfig {
    std::string camera_type = "hik";  // 新增：相机类型
    // ... 其他字段
};
```

#### `camera_node/camera_node.cpp`
**主要改动**:

1. **配置加载** (line 41-52):
```cpp
// 新增 camera_type 字段加载
fs["camera_type"] >> cfg.camera_type;
LOG_INFO("相机配置已加载: %s, 类型=%s", path.c_str(), cfg.camera_type.c_str());
```

2. **start() 方法** (line 253-330):
```cpp
// 1. 根据配置创建相机实例
camera_ = CameraFactory::create(cam_cfg_.camera_type);

// 2. 使用抽象接口调用
camera_->enumDevices(devices);
camera_->open(cam_cfg_.camera_index);
camera_->setPixelFormat(...);
// ... 所有 camera_. 改为 camera_->
```

3. **stop() 方法** (line 333-342):
```cpp
if (camera_) {
    camera_->stopGrabbing();
    camera_->close();
}
```

4. **所有 handle 函数** (line 352-455):
- `handleSetExposure`: `camera_` → `camera_->` + 空指针检查
- `handleSetGain`: 同上
- `handleSetTriggerMode`: 同上
- `handleSoftTrigger`: 同上

#### `camera_node/CMakeLists.txt`
**改动**:
```cmake
# 新增 camera_factory.cpp
add_executable(camera_node
    main.cpp
    camera_node.cpp
    src/camera_factory.cpp  # 新增
    src/hik_camera.cpp
)

# include 路径保持不变（本地目录）
target_include_directories(camera_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HIK_SDK_INCLUDE_DIR}
)
```

#### `config/camera_config.xml`
**新增字段**:
```xml
<!-- 相机类型: hik(海康), basler(巴斯勒), dahua(大华), opencv(USB摄像头) -->
<camera_type>hik</camera_type>
```

---

## 🎯 改造效果

### Before (硬编码)
```cpp
class CameraNode {
    HikCamera camera_;  // 只能使用海康相机
};
```

### After (抽象接口)
```cpp
class CameraNode {
    std::unique_ptr<ICamera> camera_;  // 支持任意品牌相机
};

// 配置文件指定类型
<camera_type>hik</camera_type>  <!-- 或 "basler", "dahua", "opencv" -->
```

---

## 📊 架构对比

### 改造前
```
CameraNode → HikCamera (硬编码)
                ↓
         MvCameraControl SDK
```

### 改造后
```
CameraNode → ICamera (抽象接口)
                ↓
        ┌───────┼───────┐
        ↓       ↓       ↓
   HikCamera  Basler  Dahua
        ↓
   MvCameraControl SDK
```

---

## 🔧 扩展新相机品牌

### 示例：添加 Basler 相机支持

#### 1. 实现 BaslerCamera 类
```cpp
// camera_node/include/basler_camera.h
#include "icamera.h"
#include <pylon/PylonIncludes.h>

class BaslerCamera : public ICamera {
public:
    bool enumDevices(std::vector<CameraDeviceInfo>& devices) override;
    bool open(int deviceIndex = 0) override;
    // ... 实现所有 ICamera 接口
private:
    Pylon::CInstantCamera camera_;
};
```

#### 2. 注册到工厂
```cpp
// camera_node/src/camera_factory.cpp
void CameraFactory::registerBuiltinTypes() {
    registerType("hik", []() {
        return std::make_unique<HikCamera>();
    });
    
    // 新增 Basler
    registerType("basler", []() {
        return std::make_unique<BaslerCamera>();
    });
}
```

#### 3. 配置文件使用
```xml
<camera_type>basler</camera_type>
```

---

## ✅ 编译验证

```bash
cd /Users/johncat/workspace/工业机器人/上下料/汇川scara/vision_distribute
./build.sh

# 编译成功输出
[100%] Built target camera_node
```

---

## 📝 测试建议

### 1. 单元测试
```cpp
TEST(CameraFactoryTest, CreateHikCamera) {
    auto camera = CameraFactory::create("hik");
    ASSERT_NE(camera, nullptr);
    
    std::vector<CameraDeviceInfo> devices;
    EXPECT_TRUE(camera->enumDevices(devices));
}
```

### 2. 集成测试
- 使用真实海康相机测试所有功能
- 验证配置文件加载正确
- 测试 Service 端点调用

### 3. 性能测试
- 对比改造前后的帧率
- 验证回调机制无性能退化
- 测试多相机并发采集

---

## ⚠️ 注意事项

### 1. 头文件依赖
- `camera_factory.h` 必须包含完整的 `icamera.h`（不能只前向声明）
- 原因: `std::unique_ptr<ICamera>` 需要完整定义才能正确析构

### 2. 类型冲突
- vision_distribute 本地有旧版 `hik_camera.h`（未继承 ICamera）
- 必须用 vision_client 的新版本替换
- 旧版定义了独立的枚举类型，与新版本冲突

### 3. 空指针检查
- 所有 `camera_` 调用前需检查 `if (!camera_)` 
- 特别是 handle 函数中

### 4. 线程安全
- `camera_mutex_` 保护所有相机操作
- 回调函数在独立线程执行，需线程安全

---

## 🚀 后续工作

### Phase 1: 已完成 ✅
- [x] 创建 CameraFactory 工厂类
- [x] 修改 CameraNode 使用 ICamera 接口
- [x] 更新配置文件添加 type 字段
- [x] 编译验证改造结果

### Phase 2: 新相机支持 (按需)
- [ ] 实现 BaslerCamera (需 Pylon SDK)
- [ ] 实现 DahuaCamera (需大华 SDK)
- [ ] 实现 OpenCVCamera (基于 OpenCV VideoCapture)

### Phase 3: 测试与文档
- [ ] 编写单元测试
- [ ] 编写集成测试
- [ ] 性能基准测试
- [ ] 更新用户文档

---

## 📚 参考文档

- [摄像头抽象接口规范](./camera_abstraction_spec.md) - 完整的设计文档
- [ICamera 接口定义](../camera_node/include/icamera.h) - 抽象接口
- [CameraFactory 实现](../camera_node/src/camera_factory.cpp) - 工厂模式
- [HikCamera 实现](../camera_node/src/hik_camera.cpp) - 海康相机

---

## 🎉 总结

本次改造成功将 `camera_node` 从硬编码依赖改为抽象接口，实现了：

✅ **品牌无关**: 通过配置切换相机品牌  
✅ **即插即用**: 运行时注册新相机类型  
✅ **向后兼容**: 默认使用 hik 类型，保持原有行为  
✅ **可扩展性**: 轻松添加新相机品牌支持  
✅ **可测试性**: Mock ICamera 接口进行单元测试  

**代码质量**: 编译通过，无警告无错误  
**架构改进**: 依赖倒置原则 (DIP) 落地  
**维护性提升**: SDK 升级只需修改实现，不影响业务层  
