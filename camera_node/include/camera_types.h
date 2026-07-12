#ifndef CAMERA_TYPES_H
#define CAMERA_TYPES_H

#include <string>
#include <functional>
#include <vector>

enum class TriggerMode {
    OFF,
    ON
};

enum class TriggerSource {
    LINE0,
    LINE1,
    LINE2,
    SOFTWARE
};

enum class ExposureAuto {
    OFF,
    ONCE,
    CONTINUOUS
};

enum class GainAuto {
    OFF,
    ONCE,
    CONTINUOUS
};

struct CameraDeviceInfo {
    std::string model_name;
    std::string serial_number;
    std::string transport_type;
    int index = 0;
};

struct FrameInfo {
    unsigned char*  data = nullptr;
    unsigned int    dataLen = 0;
    unsigned short  width = 0;
    unsigned short  height = 0;
    unsigned int    pixelType = 0;
    unsigned int    frameNum = 0;
    float           exposureTime = 0;
    float           gain = 0;
};

using ImageCallback = std::function<void(const FrameInfo&)>;

using HikTriggerMode = TriggerMode;
using HikTriggerSource = TriggerSource;
using HikExposureAuto = ExposureAuto;
using HikGainAuto = GainAuto;
using HikFrameInfo = FrameInfo;
using HikImageCallback = ImageCallback;

#endif // CAMERA_TYPES_H
