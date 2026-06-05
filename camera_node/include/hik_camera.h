#ifndef HIK_CAMERA_H
#define HIK_CAMERA_H

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>

// 海康机器人 SDK
#include "MvCameraControl.h"

// 触发模式
enum class HikTriggerMode {
    OFF,        // 连续采集模式
    ON          // 触发模式
};

// 触发源
enum class HikTriggerSource {
    LINE0,      // 硬件触发 Line0
    LINE1,      // 硬件触发 Line1
    LINE2,      // 硬件触发 Line2
    SOFTWARE    // 软触发
};

// 自动曝光模式
enum class HikExposureAuto {
    OFF,        // 手动曝光
    ONCE,       // 一次自动曝光
    CONTINUOUS  // 连续自动曝光
};

// 自动增益模式
enum class HikGainAuto {
    OFF,        // 手动增益
    ONCE,       // 一次自动增益
    CONTINUOUS  // 连续自动增益
};

// 图像帧信息（简化封装）
struct HikFrameInfo {
    unsigned char*  data = nullptr;
    unsigned int    dataLen = 0;
    unsigned short  width = 0;
    unsigned short  height = 0;
    unsigned int    pixelType = 0;  // MvGvspPixelType
    unsigned int    frameNum = 0;
    float           exposureTime = 0;
    float           gain = 0;
};

// 图像接收回调类型
using HikImageCallback = std::function<void(const HikFrameInfo& frameInfo)>;

class HikCamera {
public:
    HikCamera();
    ~HikCamera();

    // 禁止拷贝
    HikCamera(const HikCamera&) = delete;
    HikCamera& operator=(const HikCamera&) = delete;

    // 枚举设备
    bool enumDevices(std::vector<MV_CC_DEVICE_INFO>& devices);

    // 打开指定索引的设备 (默认第0个)
    bool open(int deviceIndex = 0);

    // 关闭设备
    void close();

    // 是否已打开
    bool isOpen() const;

    // 开始取流
    bool startGrabbing();

    // 停止取流
    bool stopGrabbing();

    // 是否正在取流
    bool isGrabbing() const;

    // 注册图像回调
    void setImageCallback(HikImageCallback callback);

    // ========== 参数配置 ==========

    // 设置自动曝光模式
    bool setExposureAuto(HikExposureAuto mode);

    // 设置曝光时间 (us)，仅在手动曝光模式下有效
    bool setExposureTime(float exposureTimeUs);

    // 设置自动增益模式
    bool setGainAuto(HikGainAuto mode);

    // 设置增益值，仅在手动增益模式下有效
    bool setGain(float gain);

    // 设置触发模式
    bool setTriggerMode(HikTriggerMode mode);

    // 设置触发源
    bool setTriggerSource(HikTriggerSource source);

    // 软触发执行一次（仅在触发模式 + 软触发源下有效）
    bool triggerSoftware();

    // 设置图像宽度
    bool setWidth(int width);

    // 设置图像高度
    bool setHeight(int height);

    // 设置像素格式
    bool setPixelFormat(const std::string& format);

    // 设置帧率
    bool setFrameRate(float fps);

    // ========== 参数查询 ==========

    // 获取当前曝光时间
    bool getExposureTime(float& exposureTimeUs);

    // 获取当前增益
    bool getGain(float& gain);

    // 获取图像宽度
    bool getWidth(int& width);

    // 获取图像高度
    bool getHeight(int& height);

    // 获取设备信息字符串
    std::string getDeviceInfoString() const;

    // 保存图像到文件 (format: 1=BMP, 2=JPEG)
    bool saveImage(const std::string& filepath,
                   unsigned char* data, unsigned int dataLen,
                   unsigned short width, unsigned short height,
                   unsigned int pixelType, int format = 1);

private:
    // SDK 图像回调桥接 (static, C-style)
    static void __stdcall imageCallbackBridge(unsigned char* pData,
                                               MV_FRAME_OUT_INFO_EX* pstFrameInfo,
                                               void* pUser);

    // 处理接收到的图像帧
    void onImageReceived(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pstFrameInfo);

    // 打印错误信息
    void printError(const std::string& operation, int errorCode);

    void* handle_ = nullptr;                    // 设备句柄
    std::atomic<bool> is_open_{false};
    std::atomic<bool> is_grabbing_{false};

    HikImageCallback image_callback_;           // 用户注册的图像回调
    MV_CC_DEVICE_INFO device_info_{};           // 当前设备信息
    bool has_device_info_ = false;
};

#endif // HIK_CAMERA_H
