# 摄像头抽象接口规范 (Camera Abstraction Interface Spec)

## 📋 概述

本文档基于 `vision_client` 项目中的摄像头实现代码，定义了**相机硬件的抽象接口规范**。当前系统中 `camera_node` 直接依赖海康 SDK (`HikCamera`)，缺乏对其他相机品牌/类型的支持能力。本 Spec 旨在指导如何抽象出通用的相机接口，实现多品牌相机的即插即用。

---

## 🎯 设计目标

1. **品牌无关**: 支持海康、大华、Basler、FLIR 等多品牌相机
2. **传输无关**: 支持 GigE、USB3、CameraLink 等传输协议
3. **统一 API**: 提供一致的相机控制接口（曝光、增益、触发等）
4. **热插拔**: 支持运行时设备枚举、动态连接
5. **回调驱动**: 基于异步回调的图像帧推送机制

---

## 🏗️ 架构设计

### 现有架构分析

```
┌─────────────────────────────────────────┐
│         CameraNode (业务层)              │
│   - 配置加载 (XML)                       │
│   - Service 端点处理                     │
│   - 帧消息发布 (FrameMsg)                │
└─────────────────┬───────────────────────┘
                  │ 直接依赖
                  ▼
┌─────────────────────────────────────────┐
│        HikCamera (海康实现)              │
│   - MV_CC_* SDK 调用                    │
│   - 硬编码海康特定逻辑                   │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│        海康 MVS SDK (第三方)              │
│   MvCameraControl.h                     │
└─────────────────────────────────────────┘
```

### 目标架构

```
┌─────────────────────────────────────────┐
│         CameraNode (业务层)              │
│   - 配置加载 (XML)                       │
│   - Service 端点处理                     │
│   - 帧消息发布 (FrameMsg)                │
└─────────────────┬───────────────────────┘
                  │ 依赖抽象接口
                  ▼
┌─────────────────────────────────────────┐
│        ICamera (抽象接口)                 │
│   - 设备枚举                             │
│   - 连接控制                             │
│   - 参数配置                             │
│   - 回调注册                             │
└──────────┬──────────────────┬───────────┘
           │                  │
           ▼                  ▼
┌──────────────┐    ┌────────────────┐
│ HikCamera    │    │ BaslerCamera   │
│ (海康实现)    │    │ (巴斯勒实现)    │
└──────────────┘    └────────────────┘
```

---

## 📐 接口定义

### 1. 核心数据类型

#### 1.1 相机设备信息

```cpp
struct CameraDeviceInfo {
    std::string model_name;       // 型号名称 (如 "MER-500-7GM")
    std::string serial_number;    // 序列号
    std::string transport_type;   // 传输类型 ("GigE", "USB3", "CameraLink")
    int index = 0;                // 设备索引
};
```

#### 1.2 帧信息

```cpp
struct FrameInfo {
    unsigned char*  data = nullptr;         // 图像数据指针
    unsigned int    dataLen = 0;            // 数据长度 (字节)
    unsigned short  width = 0;              // 图像宽度
    unsigned short  height = 0;             // 图像高度
    unsigned int    pixelType = 0;          // 像素格式编码
    unsigned int    frameNum = 0;           // 帧编号
    float           exposureTime = 0;       // 曝光时间 (μs)
    float           gain = 0;               // 增益值
};
```

#### 1.3 图像回调

```cpp
using ImageCallback = std::function<void(const FrameInfo&)>;
```

### 2. 枚举类型

#### 2.1 触发模式

```cpp
enum class TriggerMode {
    OFF,        // 连续采集模式
    ON          // 触发采集模式
};
```

#### 2.2 触发源

```cpp
enum class TriggerSource {
    LINE0,      // 硬件触发线 0
    LINE1,      // 硬件触发线 1
    LINE2,      // 硬件触发线 2
    SOFTWARE    // 软触发
};
```

#### 2.3 曝光自动模式

```cpp
enum class ExposureAuto {
    OFF,            // 手动曝光
    ONCE,           // 单次自动曝光
    CONTINUOUS      // 连续自动曝光
};
```

#### 2.4 增益自动模式

```cpp
enum class GainAuto {
    OFF,            // 手动增益
    ONCE,           // 单次自动增益
    CONTINUOUS      // 连续自动增益
};
```

### 3. ICamera 抽象接口

```cpp
class ICamera {
public:
    virtual ~ICamera() = default;

    // ========== 设备管理 ==========
    /// @brief 枚举可用的相机设备
    /// @param devices 输出参数，设备列表
    /// @return true 成功，false 失败
    virtual bool enumDevices(std::vector<CameraDeviceInfo>& devices) = 0;

    /// @brief 打开指定索引的相机
    /// @param deviceIndex 设备索引 (从 0 开始)
    /// @return true 成功，false 失败
    virtual bool open(int deviceIndex = 0) = 0;

    /// @brief 关闭相机
    virtual void close() = 0;

    /// @brief 检查相机是否已打开
    virtual bool isOpen() const = 0;

    // ========== 采集控制 ==========
    /// @brief 开始采集 (启动图像流)
    virtual bool startGrabbing() = 0;

    /// @brief 停止采集
    virtual bool stopGrabbing() = 0;

    /// @brief 检查是否正在采集
    virtual bool isGrabbing() const = 0;

    /// @brief 设置图像回调函数
    /// @param callback 回调函数
    virtual void setImageCallback(ImageCallback callback) = 0;

    // ========== 参数设置 ==========
    /// @brief 设置曝光自动模式
    virtual bool setExposureAuto(ExposureAuto mode) = 0;

    /// @brief 设置曝光时间 (微秒)
    virtual bool setExposureTime(float exposureTimeUs) = 0;

    /// @brief 设置增益自动模式
    virtual bool setGainAuto(GainAuto mode) = 0;

    /// @brief 设置增益值
    virtual bool setGain(float gain) = 0;

    /// @brief 设置触发模式
    virtual bool setTriggerMode(TriggerMode mode) = 0;

    /// @brief 设置触发源
    virtual bool setTriggerSource(TriggerSource source) = 0;

    /// @brief 软触发一次 (仅在触发模式下有效)
    virtual bool triggerSoftware() = 0;

    /// @brief 设置图像宽度
    virtual bool setWidth(int width) = 0;

    /// @brief 设置图像高度
    virtual bool setHeight(int height) = 0;

    /// @brief 设置像素格式 (如 "Mono8", "RGB8", "BayerRG8")
    virtual bool setPixelFormat(const std::string& format) = 0;

    /// @brief 设置帧率 (fps)
    virtual bool setFrameRate(float fps) = 0;

    // ========== 参数查询 ==========
    /// @brief 获取当前曝光时间
    /// @param exposureTimeUs 输出参数，曝光时间 (μs)
    /// @return true 成功，false 失败
    virtual bool getExposureTime(float& exposureTimeUs) = 0;

    /// @brief 获取当前增益
    /// @param gain 输出参数，增益值
    /// @return true 成功，false 失败
    virtual bool getGain(float& gain) = 0;

    /// @brief 获取当前宽度
    /// @param width 输出参数，宽度
    /// @return true 成功，false 失败
    virtual bool getWidth(int& width) = 0;

    /// @brief 获取当前高度
    /// @param height 输出参数，高度
    /// @return true 成功，false 失败
    virtual bool getHeight(int& height) = 0;

    /// @brief 获取设备信息字符串 (用于日志/调试)
    virtual std::string getDeviceInfoString() const = 0;

    // ========== 工具方法 ==========
    /// @brief 保存图像到文件 (用于调试/标定)
    /// @param filepath 文件路径
    /// @param data 图像数据
    /// @param dataLen 数据长度
    /// @param width 宽度
    /// @param height 高度
    /// @param pixelType 像素格式
    /// @param format 保存格式 (1=BMP, 2=JPEG, 3=PNG)
    /// @return true 成功，false 失败
    virtual bool saveImage(const std::string& filepath,
                           unsigned char* data, unsigned int dataLen,
                           unsigned short width, unsigned short height,
                           unsigned int pixelType, int format = 1) = 0;
};
```

---

## 🔧 实现示例：HikCamera

### 类声明

```cpp
#include "icamera.h"
#include <atomic>
#include <thread>
#include "MvCameraControl.h"  // 海康 SDK 头文件

class HikCamera : public ICamera {
public:
    HikCamera();
    ~HikCamera() override;

    // 禁止拷贝
    HikCamera(const HikCamera&) = delete;
    HikCamera& operator=(const HikCamera&) = delete;

    // ICamera 接口实现
    bool enumDevices(std::vector<CameraDeviceInfo>& devices) override;
    bool open(int deviceIndex = 0) override;
    void close() override;
    bool isOpen() const override;

    bool startGrabbing() override;
    bool stopGrabbing() override;
    bool isGrabbing() const override;
    void setImageCallback(ImageCallback callback) override;

    bool setExposureAuto(ExposureAuto mode) override;
    bool setExposureTime(float exposureTimeUs) override;
    bool setGainAuto(GainAuto mode) override;
    bool setGain(float gain) override;
    bool setTriggerMode(TriggerMode mode) override;
    bool setTriggerSource(TriggerSource source) override;
    bool triggerSoftware() override;
    bool setWidth(int width) override;
    bool setHeight(int height) override;
    bool setPixelFormat(const std::string& format) override;
    bool setFrameRate(float fps) override;

    bool getExposureTime(float& exposureTimeUs) override;
    bool getGain(float& gain) override;
    bool getWidth(int& width) override;
    bool getHeight(int& height) override;
    std::string getDeviceInfoString() const override;

    bool saveImage(const std::string& filepath,
                   unsigned char* data, unsigned int dataLen,
                   unsigned short width, unsigned short height,
                   unsigned int pixelType, int format = 1) override;

private:
    // 海康 SDK 回调桥接
    static void __stdcall imageCallbackBridge(
        unsigned char* pData,
        MV_FRAME_OUT_INFO_EX* pstFrameInfo,
        void* pUser
    );

    // 内部图像处理
    void onImageReceived(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pstFrameInfo);

    // 错误打印
    void printError(const std::string& operation, int errorCode);

    // 成员变量
    void* handle_ = nullptr;                    // 海康设备句柄
    std::atomic<bool> is_open_{false};          // 打开状态
    std::atomic<bool> is_grabbing_{false};      // 采集状态
    ImageCallback image_callback_;              // 用户回调
    MV_CC_DEVICE_INFO device_info_{};           // 设备信息
    bool has_device_info_ = false;              // 是否有设备信息
};
```

### 关键实现要点

#### 1. 设备枚举

```cpp
bool HikCamera::enumDevices(std::vector<CameraDeviceInfo>& devices) {
    devices.clear();
    
    MV_CC_DEVICE_INFO_LIST devList;
    memset(&devList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    
    // 枚举 GigE 和 USB3 设备
    int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &devList);
    if (MV_OK != ret) {
        printError("EnumDevices", ret);
        return false;
    }
    
    // 转换设备信息
    for (unsigned int i = 0; i < devList.nDeviceNum; ++i) {
        if (devList.pDeviceInfo[i] != nullptr) {
            CameraDeviceInfo info;
            info.index = static_cast<int>(i);
            
            if (devList.pDeviceInfo[i]->nTLayerType == MV_GIGE_DEVICE) {
                info.transport_type = "GigE";
                info.model_name = reinterpret_cast<const char*>(
                    devList.pDeviceInfo[i]->SpecialInfo.stGigEInfo.chModelName);
                info.serial_number = reinterpret_cast<const char*>(
                    devList.pDeviceInfo[i]->SpecialInfo.stGigEInfo.chSerialNumber);
            } else if (devList.pDeviceInfo[i]->nTLayerType == MV_USB_DEVICE) {
                info.transport_type = "USB3";
                info.model_name = reinterpret_cast<const char*>(
                    devList.pDeviceInfo[i]->SpecialInfo.stUsb3VInfo.chModelName);
                info.serial_number = reinterpret_cast<const char*>(
                    devList.pDeviceInfo[i]->SpecialInfo.stUsb3VInfo.chSerialNumber);
            }
            
            devices.push_back(info);
        }
    }
    
    return true;
}
```

#### 2. 回调桥接

```cpp
// 静态桥接函数 (SDK C 回调 → C++ 成员函数)
void __stdcall HikCamera::imageCallbackBridge(
    unsigned char* pData,
    MV_FRAME_OUT_INFO_EX* pstFrameInfo,
    void* pUser
) {
    if (pUser != nullptr) {
        static_cast<HikCamera*>(pUser)->onImageReceived(pData, pstFrameInfo);
    }
}

// 成员函数处理
void HikCamera::onImageReceived(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pstFrameInfo) {
    if (image_callback_) {
        FrameInfo info;
        info.data = pData;
        info.dataLen = pstFrameInfo->nFrameLen;
        info.width = pstFrameInfo->nWidth;
        info.height = pstFrameInfo->nHeight;
        info.pixelType = pstFrameInfo->nPixelType;
        info.frameNum = pstFrameInfo->nFrameNum;
        info.exposureTime = pstFrameInfo->fExposureTime;
        info.gain = pstFrameInfo->fGain;
        
        image_callback_(info);
    }
}
```

---

## 🔄 CameraNode 改造方案

### 改造前 (当前实现)

```cpp
#include <hik_camera.h>  // 硬编码海康实现

class CameraNode : public NodeBase {
private:
    HikCamera camera_;  // 直接依赖具体实现
    // ...
};
```

### 改造后 (抽象接口)

```cpp
#include <icamera.h>           // 抽象接口
#include <camera_factory.h>    // 相机工厂

class CameraNode : public NodeBase {
private:
    std::unique_ptr<ICamera> camera_;  // 依赖抽象
    std::string camera_type_;          // 从配置读取 ("hik", "basler", ...)
    // ...
    
    bool initializeCamera() {
        // 根据配置创建具体相机实例
        camera_ = CameraFactory::create(camera_type_);
        if (!camera_) {
            LOG_ERROR("不支持的相机类型: %s", camera_type_.c_str());
            return false;
        }
        
        // 后续操作完全基于 ICamera 接口
        std::vector<CameraDeviceInfo> devices;
        camera_->enumDevices(devices);
        // ...
    }
};
```

### 配置示例

```xml
<!-- camera_config.xml -->
<camera>
    <type>hik</type>  <!-- 相机类型标识 -->
    <camera_index>0</camera_index>
    <trigger_mode>continuous</trigger_mode>
    <pixel_format>Mono8</pixel_format>
    <exposure_auto>continuous</exposure_auto>
    <exposure_time>10000.0</exposure_time>
    <gain_auto>continuous</gain_auto>
    <gain>0.0</gain>
    <frame_rate>30.0</frame_rate>
</camera>
```

---

## 🏭 相机工厂设计

### 工厂接口

```cpp
class CameraFactory {
public:
    /// @brief 创建相机实例
    /// @param type 相机类型 ("hik", "basler", "dahua", ...)
    /// @return 相机实例指针
    static std::unique_ptr<ICamera> create(const std::string& type) {
        if (type == "hik") {
            return std::make_unique<HikCamera>();
        } else if (type == "basler") {
            return std::make_unique<BaslerCamera>();
        } else if (type == "dahua") {
            return std::make_unique<DahuaCamera>();
        } else {
            return nullptr;  // 不支持的类型
        }
    }
    
    /// @brief 注册新的相机类型 (运行时扩展)
    static void registerType(const std::string& type, 
                             std::function<std::unique_ptr<ICamera>()> creator);
};
```

### 注册机制 (插件化)

```cpp
// 第三方相机插件可以在运行时注册
class CameraTypeRegistry {
public:
    static void registerType(const std::string& type,
                             std::function<std::unique_ptr<ICamera>()> creator) {
        std::lock_guard<std::mutex> lock(mutex_);
        creators_[type] = std::move(creator);
    }
    
    static std::unique_ptr<ICamera> create(const std::string& type) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = creators_.find(type);
        if (it != creators_.end()) {
            return it->second();
        }
        return nullptr;
    }
    
private:
    static std::map<std::string, 
                    std::function<std::unique_ptr<ICamera>()>> creators_;
    static std::mutex mutex_;
};
```

---

## 📊 像素格式支持

### 通用像素格式映射

| 格式名称 | 说明 | 字节深度 | 通道数 |
|---------|------|---------|--------|
| `Mono8` | 8 位灰度 | 1 | 1 |
| `Mono16` | 16 位灰度 | 2 | 1 |
| `RGB8` | 24 位 RGB | 3 | 3 |
| `BGR8` | 24 位 BGR (OpenCV 默认) | 3 | 3 |
| `RGBA8` | 32 位 RGBA | 4 | 4 |
| `BayerRG8` | Bayer RGGB 8 位 | 1 | 1 |
| `BayerGB8` | Bayer GBRG 8 位 | 1 | 1 |
| `BayerGR8` | Bayer GRBG 8 位 | 1 | 1 |
| `BayerBG8` | Bayer BGGR 8 位 | 1 | 1 |

### 格式转换辅助

```cpp
class PixelFormatConverter {
public:
    /// @brief 将字符串格式转换为内部编码
    static unsigned int fromString(const std::string& format);
    
    /// @brief 将内部编码转换为字符串
    static std::string toString(unsigned int pixelType);
    
    /// @brief 检查格式是否需要 Bayer 解码
    static bool isBayerFormat(unsigned int pixelType);
    
    /// @brief 获取每像素字节数
    static int getBytesPerPixel(unsigned int pixelType);
};
```

---

## ⚠️ 注意事项

### 1. 线程安全

- **设备枚举和打开**: 需要加锁保护
- **回调函数**: SDK 回调在独立线程中执行，回调函数必须是线程安全的
- **参数设置**: 使用 `std::mutex` 保护并发访问

```cpp
std::mutex camera_mutex_;
camera_.setExposureTime(10000.0f);  // 需要加锁
```

### 2. 资源管理

- **句柄生命周期**: `open()` → `startGrabbing()` → `stopGrabbing()` → `close()`
- **RAII 封装**: 使用智能指针或自定义 deleter 管理资源
- **异常安全**: 确保异常情况下资源正确释放

### 3. SDK 依赖隔离

- **编译隔离**: 使用 `#ifdef HAS_HIK_SDK` 条件编译
- **头文件隔离**: 各品牌 SDK 头文件仅在实现文件中包含
- **动态加载** (可选): 使用 `dlopen`/`LoadLibrary` 动态加载 SDK

### 4. 性能优化

- **零拷贝**: 回调中传递指针而非拷贝数据
- **内存池**: 预先分配帧缓冲区，减少动态分配
- **异步处理**: 回调中只做轻量级处理，复杂处理投递到工作线程

---

## 🧪 测试策略

### 1. 单元测试

```cpp
TEST(CameraTest, EnumDevices) {
    auto camera = CameraFactory::create("hik");
    std::vector<CameraDeviceInfo> devices;
    EXPECT_TRUE(camera->enumDevices(devices));
    EXPECT_GT(devices.size(), 0);
}

TEST(CameraTest, OpenClose) {
    auto camera = CameraFactory::create("hik");
    EXPECT_TRUE(camera->open(0));
    EXPECT_TRUE(camera->isOpen());
    camera->close();
    EXPECT_FALSE(camera->isOpen());
}

TEST(CameraTest, CaptureFrame) {
    auto camera = CameraFactory::create("hik");
    camera->open(0);
    
    std::atomic<int> frame_count{0};
    camera->setImageCallback([&](const FrameInfo& info) {
        frame_count++;
        EXPECT_GT(info.width, 0);
        EXPECT_GT(info.height, 0);
        EXPECT_NE(info.data, nullptr);
    });
    
    camera->startGrabbing();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    camera->stopGrabbing();
    
    EXPECT_GT(frame_count.load(), 0);
}
```

### 2. 集成测试

- **多相机并发**: 同时打开多个相机并采集
- **热插拔**: 运行时插入/拔出相机设备
- **参数持久化**: 设置参数后重启验证

---

## 📚 参考实现

### 现有代码位置

- **抽象接口**: `vision_client/core/include/icamera.h`
- **海康实现**: `vision_client/core/include/hik_camera.h` + `.cpp`
- **类型定义**: `vision_client/core/include/camera_types.h`
- **业务使用**: `vision_distribute/camera_node/camera_node.cpp`

### 扩展实现参考

1. **Basler Pylon SDK**: 类似 Hik SDK，提供 C++ API
2. **FLIR Spinnaker SDK**: 提供 GenICam 兼容接口
3. **OpenCV VideoCapture**: 可作为 USB 摄像头的通用后端
4. **GStreamer**: 支持网络摄像头的 RTSP 流

---

## 🚀 实施路线图

### Phase 1: 接口抽象 (1-2 周)

- [x] 定义 `ICamera` 抽象接口 (已存在)
- [x] 定义 `CameraDeviceInfo`, `FrameInfo` 等数据结构 (已存在)
- [ ] 创建 `CameraFactory` 工厂类
- [ ] 改造 `CameraNode` 使用 `ICamera` 接口

### Phase 2: 配置化 (1 周)

- [ ] 在 `camera_config.xml` 中添加 `<type>` 字段
- [ ] 实现基于配置的相机类型选择
- [ ] 添加配置验证和默认值处理

### Phase 3: 新相机支持 (按需)

- [ ] 实现 `BaslerCamera` (如需支持巴斯勒)
- [ ] 实现 `DahuaCamera` (如需支持大华)
- [ ] 实现 `UsbCamera` (基于 OpenCV VideoCapture)

### Phase 4: 测试与验证 (1 周)

- [ ] 编写单元测试覆盖所有接口
- [ ] 多品牌相机兼容性测试
- [ ] 性能基准测试 (帧率、延迟、CPU 占用)

---

## 📝 总结

本 Spec 定义了摄像头硬件的抽象接口规范，核心思想是：

1. **依赖倒置**: `CameraNode` 依赖 `ICamera` 抽象，而非具体实现
2. **工厂模式**: 通过 `CameraFactory` 动态创建具体相机实例
3. **配置驱动**: 通过 XML 配置选择相机类型和参数
4. **插件扩展**: 支持运行时注册新的相机类型

实施后，系统将能够：
- ✅ 无缝切换不同品牌相机
- ✅ 降低 SDK 升级带来的维护成本
- ✅ 提高代码可测试性 (Mock ICamera)
- ✅ 支持未来新相机类型的快速集成
