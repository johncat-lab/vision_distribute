#ifndef ICAMERA_H
#define ICAMERA_H

#include "camera_types.h"
#include <vector>
#include <string>

class ICamera {
public:
    virtual ~ICamera() = default;

    virtual bool enumDevices(std::vector<CameraDeviceInfo>& devices) = 0;
    virtual bool open(int deviceIndex = 0) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual bool startGrabbing() = 0;
    virtual bool stopGrabbing() = 0;
    virtual bool isGrabbing() const = 0;
    virtual void setImageCallback(ImageCallback callback) = 0;

    virtual bool setExposureAuto(ExposureAuto mode) = 0;
    virtual bool setExposureTime(float exposureTimeUs) = 0;
    virtual bool setGainAuto(GainAuto mode) = 0;
    virtual bool setGain(float gain) = 0;
    virtual bool setTriggerMode(TriggerMode mode) = 0;
    virtual bool setTriggerSource(TriggerSource source) = 0;
    virtual bool triggerSoftware() = 0;
    virtual bool setWidth(int width) = 0;
    virtual bool setHeight(int height) = 0;
    virtual bool setPixelFormat(const std::string& format) = 0;
    virtual bool setFrameRate(float fps) = 0;

    virtual bool getExposureTime(float& exposureTimeUs) = 0;
    virtual bool getGain(float& gain) = 0;
    virtual bool getWidth(int& width) = 0;
    virtual bool getHeight(int& height) = 0;
    virtual std::string getDeviceInfoString() const = 0;

    virtual bool saveImage(const std::string& filepath,
                           unsigned char* data, unsigned int dataLen,
                           unsigned short width, unsigned short height,
                           unsigned int pixelType, int format = 1) = 0;
};

#endif // ICAMERA_H
