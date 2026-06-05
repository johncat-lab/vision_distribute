#include "hik_camera.h"
#include <cstring>
#include <string>
#include <utility>

HikCamera::HikCamera() {}

HikCamera::~HikCamera() {
    close();
}

bool HikCamera::enumDevices(std::vector<CameraDeviceInfo>& devices) {
    devices.clear();

    MV_CC_DEVICE_INFO_LIST devList;
    memset(&devList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));

    int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &devList);
    if (MV_OK != ret) {
        printError("EnumDevices", ret);
        return false;
    }

    if (devList.nDeviceNum == 0) {
        std::cout << "[海康相机] 未发现设备" << std::endl;
        return true;
    }

    for (unsigned int i = 0; i < devList.nDeviceNum; ++i) {
        if (devList.pDeviceInfo[i] != nullptr) {
            CameraDeviceInfo info;
            info.index = static_cast<int>(i);

            if (devList.pDeviceInfo[i]->nTLayerType == MV_GIGE_DEVICE) {
                info.transport_type = "GigE";
                info.model_name = reinterpret_cast<const char*>(devList.pDeviceInfo[i]->SpecialInfo.stGigEInfo.chModelName);
                info.serial_number = reinterpret_cast<const char*>(devList.pDeviceInfo[i]->SpecialInfo.stGigEInfo.chSerialNumber);
            } else if (devList.pDeviceInfo[i]->nTLayerType == MV_USB_DEVICE) {
                info.transport_type = "USB3";
                info.model_name = reinterpret_cast<const char*>(devList.pDeviceInfo[i]->SpecialInfo.stUsb3VInfo.chModelName);
                info.serial_number = reinterpret_cast<const char*>(devList.pDeviceInfo[i]->SpecialInfo.stUsb3VInfo.chSerialNumber);
            }

            devices.push_back(info);
        }
    }

    std::cout << "[海康相机] 发现 " << devices.size() << " 个设备" << std::endl;
    return true;
}

bool HikCamera::open(int deviceIndex) {
    if (is_open_.load()) {
        std::cerr << "[海康相机] 设备已打开" << std::endl;
        return false;
    }

    MV_CC_DEVICE_INFO_LIST devList;
    memset(&devList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));

    int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &devList);
    if (MV_OK != ret) {
        printError("EnumDevices", ret);
        return false;
    }

    if (devList.nDeviceNum == 0) {
        std::cerr << "[海康相机] 未发现设备" << std::endl;
        return false;
    }

    if (deviceIndex < 0 || static_cast<unsigned int>(deviceIndex) >= devList.nDeviceNum) {
        std::cerr << "[海康相机] 设备索引超出范围: " << deviceIndex
                  << ", 可用数量: " << devList.nDeviceNum << std::endl;
        return false;
    }

    ret = MV_CC_CreateHandle(&handle_, devList.pDeviceInfo[deviceIndex]);
    if (MV_OK != ret) {
        printError("CreateHandle", ret);
        handle_ = nullptr;
        return false;
    }

    ret = MV_CC_OpenDevice(handle_, MV_ACCESS_Exclusive, 0);
    if (MV_OK != ret) {
        printError("OpenDevice", ret);
        MV_CC_DestroyHandle(handle_);
        handle_ = nullptr;
        return false;
    }

    device_info_ = *devList.pDeviceInfo[deviceIndex];
    has_device_info_ = true;

    is_open_.store(true);
    std::cout << "[海康相机] 设备已打开: " << getDeviceInfoString() << std::endl;

    ret = MV_CC_RegisterImageCallBackEx(handle_, imageCallbackBridge, this);
    if (MV_OK != ret) {
        printError("RegisterImageCallBackEx", ret);
    }

    return true;
}

void HikCamera::close() {
    if (!is_open_.load()) {
        return;
    }

    stopGrabbing();

    if (handle_ != nullptr) {
        MV_CC_CloseDevice(handle_);
        MV_CC_DestroyHandle(handle_);
        handle_ = nullptr;
    }

    is_open_.store(false);
    has_device_info_ = false;
    std::cout << "[海康相机] 设备已关闭" << std::endl;
}

bool HikCamera::isOpen() const {
    return is_open_.load();
}

bool HikCamera::startGrabbing() {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开，无法开始取流" << std::endl;
        return false;
    }

    if (is_grabbing_.load()) {
        std::cout << "[海康相机] 已在取流中" << std::endl;
        return true;
    }

    int ret = MV_CC_StartGrabbing(handle_);
    if (MV_OK != ret) {
        printError("StartGrabbing", ret);
        return false;
    }

    is_grabbing_.store(true);
    std::cout << "[海康相机] 开始取流" << std::endl;
    return true;
}

bool HikCamera::stopGrabbing() {
    if (!is_grabbing_.load()) {
        return true;
    }

    if (handle_ != nullptr) {
        int ret = MV_CC_StopGrabbing(handle_);
        if (MV_OK != ret) {
            printError("StopGrabbing", ret);
            return false;
        }
    }

    is_grabbing_.store(false);
    std::cout << "[海康相机] 停止取流" << std::endl;
    return true;
}

bool HikCamera::isGrabbing() const {
    return is_grabbing_.load();
}

void HikCamera::setImageCallback(ImageCallback callback) {
    image_callback_ = callback;
}

bool HikCamera::setExposureAuto(ExposureAuto mode) {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    unsigned int value = 0;
    switch (mode) {
        case ExposureAuto::OFF:        value = MV_EXPOSURE_AUTO_MODE_OFF;        break;
        case ExposureAuto::ONCE:       value = MV_EXPOSURE_AUTO_MODE_ONCE;       break;
        case ExposureAuto::CONTINUOUS: value = MV_EXPOSURE_AUTO_MODE_CONTINUOUS; break;
    }

    int ret = MV_CC_SetEnumValue(handle_, "ExposureAuto", value);
    if (MV_OK != ret) {
        printError("Set ExposureAuto", ret);
        return false;
    }

    std::cout << "[海康相机] 自动曝光模式已设置: " << static_cast<int>(mode) << std::endl;
    return true;
}

bool HikCamera::setExposureTime(float exposureTimeUs) {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    int ret = MV_CC_SetFloatValue(handle_, "ExposureTime", exposureTimeUs);
    if (MV_OK != ret) {
        printError("Set ExposureTime", ret);
        return false;
    }

    std::cout << "[海康相机] 曝光时间已设置: " << exposureTimeUs << " us" << std::endl;
    return true;
}

bool HikCamera::setGainAuto(GainAuto mode) {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    unsigned int value = 0;
    switch (mode) {
        case GainAuto::OFF:        value = MV_GAIN_MODE_OFF;        break;
        case GainAuto::ONCE:       value = MV_GAIN_MODE_ONCE;       break;
        case GainAuto::CONTINUOUS: value = MV_GAIN_MODE_CONTINUOUS; break;
    }

    int ret = MV_CC_SetEnumValue(handle_, "GainAuto", value);
    if (MV_OK != ret) {
        printError("Set GainAuto", ret);
        return false;
    }

    std::cout << "[海康相机] 自动增益模式已设置: " << static_cast<int>(mode) << std::endl;
    return true;
}

bool HikCamera::setGain(float gain) {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    int ret = MV_CC_SetFloatValue(handle_, "Gain", gain);
    if (MV_OK != ret) {
        printError("Set Gain", ret);
        return false;
    }

    std::cout << "[海康相机] 增益已设置: " << gain << std::endl;
    return true;
}

bool HikCamera::setTriggerMode(TriggerMode mode) {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    unsigned int value = (mode == TriggerMode::ON) ? MV_TRIGGER_MODE_ON : MV_TRIGGER_MODE_OFF;

    int ret = MV_CC_SetEnumValue(handle_, "TriggerMode", value);
    if (MV_OK != ret) {
        printError("Set TriggerMode", ret);
        return false;
    }

    std::cout << "[海康相机] 触发模式已设置: " << (value == MV_TRIGGER_MODE_ON ? "ON" : "OFF") << std::endl;
    return true;
}

bool HikCamera::setTriggerSource(TriggerSource source) {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    unsigned int value = MV_TRIGGER_SOURCE_SOFTWARE;
    switch (source) {
        case TriggerSource::LINE0:     value = MV_TRIGGER_SOURCE_LINE0;     break;
        case TriggerSource::LINE1:     value = MV_TRIGGER_SOURCE_LINE1;     break;
        case TriggerSource::LINE2:     value = MV_TRIGGER_SOURCE_LINE2;     break;
        case TriggerSource::SOFTWARE:  value = MV_TRIGGER_SOURCE_SOFTWARE;  break;
    }

    int ret = MV_CC_SetEnumValue(handle_, "TriggerSource", value);
    if (MV_OK != ret) {
        printError("Set TriggerSource", ret);
        return false;
    }

    std::cout << "[海康相机] 触发源已设置: " << static_cast<int>(source) << std::endl;
    return true;
}

bool HikCamera::triggerSoftware() {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    int ret = MV_CC_SetCommandValue(handle_, "TriggerSoftware");
    if (MV_OK != ret) {
        printError("TriggerSoftware", ret);
        return false;
    }

    return true;
}

bool HikCamera::setWidth(int width) {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    int ret = MV_CC_SetIntValueEx(handle_, "Width", width);
    if (MV_OK != ret) {
        printError("Set Width", ret);
        return false;
    }

    std::cout << "[海康相机] 图像宽度已设置: " << width << std::endl;
    return true;
}

bool HikCamera::setHeight(int height) {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    int ret = MV_CC_SetIntValueEx(handle_, "Height", height);
    if (MV_OK != ret) {
        printError("Set Height", ret);
        return false;
    }

    std::cout << "[海康相机] 图像高度已设置: " << height << std::endl;
    return true;
}

bool HikCamera::setPixelFormat(const std::string& format) {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    static const std::vector<std::pair<std::string, unsigned int>> format_map = {
        {"Mono8",     0x01080001},
        {"Mono10",    0x01100003},
        {"Mono12",    0x01100005},
        {"BayerGB8",  0x01080005},
        {"BayerGB10", 0x0110000D},
        {"BayerGB12", 0x01100015},
        {"BGR8",      0x02180015},
        {"RGB8",      0x02180014},
    };

    MVCC_ENUMVALUE stEnumVal;
    memset(&stEnumVal, 0, sizeof(MVCC_ENUMVALUE));
    int ret = MV_CC_GetEnumValue(handle_, "PixelFormat", &stEnumVal);
    if (MV_OK == ret) {
        std::cout << "[海康相机] 当前像素格式: 0x" << std::hex << stEnumVal.nCurValue << std::dec << std::endl;
        std::cout << "[海康相机] 支持的像素格式 (" << stEnumVal.nSupportedNum << "):";
        for (unsigned int i = 0; i < stEnumVal.nSupportedNum && i < MV_MAX_XML_SYMBOLIC_NUM; ++i) {
            std::cout << " 0x" << std::hex << stEnumVal.nSupportValue[i] << std::dec;
        }
        std::cout << std::endl;
    }

    unsigned int format_value = 0;
    bool found = false;
    for (const auto& entry : format_map) {
        if (format == entry.first) {
            format_value = entry.second;
            found = true;
            break;
        }
    }

    bool set_ok = false;
    if (found) {
        if (MV_OK == ret) {
            bool supported = false;
            for (unsigned int i = 0; i < stEnumVal.nSupportedNum && i < MV_MAX_XML_SYMBOLIC_NUM; ++i) {
                if (stEnumVal.nSupportValue[i] == format_value) {
                    supported = true;
                    break;
                }
            }
            if (!supported) {
                std::cerr << "[海康相机] 相机不支持像素格式: " << format
                          << " (0x" << std::hex << format_value << std::dec << ")" << std::endl;
                return false;
            }
        }

        int nret = MV_CC_SetEnumValue(handle_, "PixelFormat", format_value);
        if (MV_OK == nret) {
            set_ok = true;
            std::cout << "[海康相机] 通过数值设置像素格式: " << format
                      << " (0x" << std::hex << format_value << std::dec << ")" << std::endl;
        } else {
            printError("SetEnumValue PixelFormat (" + format + ", 0x" +
                       std::to_string(format_value) + ")", nret);
            std::cout << "[海康相机] 数值方式失败，尝试字符串方式..." << std::endl;
        }
    }

    if (!set_ok) {
        int nret = MV_CC_SetEnumValueByString(handle_, "PixelFormat", format.c_str());
        if (MV_OK != nret) {
            printError("SetEnumValueByString PixelFormat (" + format + ")", nret);
            std::cerr << "[海康相机] 提示: 数值和字符串方式均失败，请确认相机支持该像素格式" << std::endl;
            return false;
        }
        set_ok = true;
    }

    MVCC_ENUMVALUE stNewEnumVal;
    memset(&stNewEnumVal, 0, sizeof(MVCC_ENUMVALUE));
    int ret2 = MV_CC_GetEnumValue(handle_, "PixelFormat", &stNewEnumVal);
    if (MV_OK == ret2) {
        std::cout << "[海康相机] 像素格式已设置: " << format
                  << " (0x" << std::hex << stNewEnumVal.nCurValue << std::dec << ")" << std::endl;
    } else {
        std::cout << "[海康相机] 像素格式已设置: " << format << std::endl;
    }
    return true;
}

bool HikCamera::setFrameRate(float fps) {
    if (!is_open_.load()) {
        std::cerr << "[海康相机] 设备未打开" << std::endl;
        return false;
    }

    int ret = MV_CC_SetFloatValue(handle_, "AcquisitionFrameRate", fps);
    if (MV_OK != ret) {
        printError("Set AcquisitionFrameRate", ret);
        return false;
    }

    std::cout << "[海康相机] 帧率已设置: " << fps << " fps" << std::endl;
    return true;
}

bool HikCamera::getExposureTime(float& exposureTimeUs) {
    if (!is_open_.load()) {
        return false;
    }

    MVCC_FLOATVALUE value;
    memset(&value, 0, sizeof(MVCC_FLOATVALUE));

    int ret = MV_CC_GetFloatValue(handle_, "ExposureTime", &value);
    if (MV_OK != ret) {
        printError("Get ExposureTime", ret);
        return false;
    }

    exposureTimeUs = value.fCurValue;
    return true;
}

bool HikCamera::getGain(float& gain) {
    if (!is_open_.load()) {
        return false;
    }

    MVCC_FLOATVALUE value;
    memset(&value, 0, sizeof(MVCC_FLOATVALUE));

    int ret = MV_CC_GetFloatValue(handle_, "Gain", &value);
    if (MV_OK != ret) {
        printError("Get Gain", ret);
        return false;
    }

    gain = value.fCurValue;
    return true;
}

bool HikCamera::getWidth(int& width) {
    if (!is_open_.load()) {
        return false;
    }

    MVCC_INTVALUE_EX value;
    memset(&value, 0, sizeof(MVCC_INTVALUE_EX));

    int ret = MV_CC_GetIntValueEx(handle_, "Width", &value);
    if (MV_OK != ret) {
        printError("Get Width", ret);
        return false;
    }

    width = static_cast<int>(value.nCurValue);
    return true;
}

bool HikCamera::getHeight(int& height) {
    if (!is_open_.load()) {
        return false;
    }

    MVCC_INTVALUE_EX value;
    memset(&value, 0, sizeof(MVCC_INTVALUE_EX));

    int ret = MV_CC_GetIntValueEx(handle_, "Height", &value);
    if (MV_OK != ret) {
        printError("Get Height", ret);
        return false;
    }

    height = static_cast<int>(value.nCurValue);
    return true;
}

std::string HikCamera::getDeviceInfoString() const {
    if (!has_device_info_) {
        return "未知设备";
    }

    std::string info;
    if (device_info_.nTLayerType == MV_GIGE_DEVICE) {
        info = "GigE: ";
        info += reinterpret_cast<const char*>(device_info_.SpecialInfo.stGigEInfo.chModelName);
        info += " (";
        info += reinterpret_cast<const char*>(device_info_.SpecialInfo.stGigEInfo.chSerialNumber);
        info += ")";
    } else if (device_info_.nTLayerType == MV_USB_DEVICE) {
        info = "USB3: ";
        info += reinterpret_cast<const char*>(device_info_.SpecialInfo.stUsb3VInfo.chModelName);
        info += " (";
        info += reinterpret_cast<const char*>(device_info_.SpecialInfo.stUsb3VInfo.chSerialNumber);
        info += ")";
    } else {
        info = "Unknown Device Type";
    }

    return info;
}

void __stdcall HikCamera::imageCallbackBridge(unsigned char* pData,
                                               MV_FRAME_OUT_INFO_EX* pstFrameInfo,
                                               void* pUser) {
    if (pUser != nullptr && pData != nullptr && pstFrameInfo != nullptr) {
        HikCamera* camera = static_cast<HikCamera*>(pUser);
        camera->onImageReceived(pData, pstFrameInfo);
    }
}

void HikCamera::onImageReceived(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pstFrameInfo) {
    if (image_callback_) {
        FrameInfo info;
        info.data = pData;
        info.dataLen = pstFrameInfo->nFrameLen;
        info.width = pstFrameInfo->nWidth;
        info.height = pstFrameInfo->nHeight;
        info.pixelType = static_cast<unsigned int>(pstFrameInfo->enPixelType);
        info.frameNum = pstFrameInfo->nFrameNum;
        info.exposureTime = pstFrameInfo->fExposureTime;
        info.gain = pstFrameInfo->fGain;

        static unsigned int last_pixel_type = 0;
        if (info.pixelType != last_pixel_type) {
            std::cout << "[海康相机] 像素格式变化: 0x" << std::hex << last_pixel_type
                      << " -> 0x" << info.pixelType << std::dec
                      << " (frame #" << info.frameNum << ")" << std::endl;
            last_pixel_type = info.pixelType;
        }

        image_callback_(info);
    }
}

bool HikCamera::saveImage(const std::string& filepath,
                          unsigned char* data, unsigned int dataLen,
                          unsigned short width, unsigned short height,
                          unsigned int pixelType, int format) {
    if (!is_open_.load() || handle_ == nullptr) {
        std::cerr << "[海康相机] 设备未打开，无法保存图像" << std::endl;
        return false;
    }

    unsigned int bufferSize = width * height * 4 + 2048;
    std::vector<unsigned char> buffer(bufferSize);

    MV_SAVE_IMAGE_PARAM_EX saveParam;
    memset(&saveParam, 0, sizeof(MV_SAVE_IMAGE_PARAM_EX));
    saveParam.pData         = data;
    saveParam.nDataLen      = dataLen;
    saveParam.enPixelType   = static_cast<MvGvspPixelType>(pixelType);
    saveParam.nWidth        = width;
    saveParam.nHeight       = height;
    saveParam.pImageBuffer  = buffer.data();
    saveParam.nBufferSize   = bufferSize;
    saveParam.enImageType   = (format == 2) ? MV_Image_Jpeg : MV_Image_Bmp;
    saveParam.nJpgQuality   = 80;
    saveParam.iMethodValue  = 1;

    int ret = MV_CC_SaveImageEx2(handle_, &saveParam);
    if (MV_OK != ret) {
        printError("SaveImageEx2", ret);
        return false;
    }

    FILE* fp = fopen(filepath.c_str(), "wb");
    if (fp == nullptr) {
        std::cerr << "[海康相机] 无法创建文件: " << filepath << std::endl;
        return false;
    }

    fwrite(saveParam.pImageBuffer, 1, saveParam.nImageLen, fp);
    fclose(fp);

    std::cout << "[海康相机] 图像已保存: " << filepath
              << " (" << width << "x" << height << ", " << saveParam.nImageLen << " bytes)" << std::endl;
    return true;
}

void HikCamera::printError(const std::string& operation, int errorCode) {
    std::cerr << "[海康相机] " << operation << " 失败, 错误码: 0x"
              << std::hex << errorCode << std::dec << std::endl;
}
