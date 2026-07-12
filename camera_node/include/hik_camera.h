#ifndef HIK_CAMERA_H
#define HIK_CAMERA_H

#include "icamera.h"
#include <atomic>
#include <thread>
#include <iostream>

#include "MvCameraControl.h"

class HikCamera : public ICamera {
public:
    HikCamera();
    ~HikCamera() override;

    HikCamera(const HikCamera&) = delete;
    HikCamera& operator=(const HikCamera&) = delete;

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
    static void __stdcall imageCallbackBridge(unsigned char* pData,
                                               MV_FRAME_OUT_INFO_EX* pstFrameInfo,
                                               void* pUser);

    void onImageReceived(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pstFrameInfo);

    void printError(const std::string& operation, int errorCode);

    void* handle_ = nullptr;
    std::atomic<bool> is_open_{false};
    std::atomic<bool> is_grabbing_{false};

    ImageCallback image_callback_;
    MV_CC_DEVICE_INFO device_info_{};
    bool has_device_info_ = false;
};

#endif // HIK_CAMERA_H
