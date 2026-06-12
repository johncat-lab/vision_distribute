#include "hik_camera.h"
#include "logger/logger.h"
#include <cstring>

// ========== 构造函数 / 析构函数 ==========

HikCamera::HikCamera() {}

HikCamera::~HikCamera() {
    close();
}

// ========== 设备管理 ==========

bool HikCamera::enumDevices(std::vector<MV_CC_DEVICE_INFO>& devices) {
    devices.clear();

    MV_CC_DEVICE_INFO_LIST devList;
    memset(&devList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));

    int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &devList);
    if (MV_OK != ret) {
        printError("EnumDevices", ret);
        return false;
    }

    if (devList.nDeviceNum == 0) {
        LOG_INFO("[海康相机] 未发现设备");
        return true;
    }

    for (unsigned int i = 0; i < devList.nDeviceNum; ++i) {
        if (devList.pDeviceInfo[i] != nullptr) {
            devices.push_back(*devList.pDeviceInfo[i]);
        }
    }

    LOG_INFO("[海康相机] 发现 %d 个设备", devices.size());
    return true;
}

bool HikCamera::open(int deviceIndex) {
    if (is_open_.load()) {
        LOG_WARN("[海康相机] 设备已打开");
        return false;
    }

    // 枚举设备
    MV_CC_DEVICE_INFO_LIST devList;
    memset(&devList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));

    int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &devList);
    if (MV_OK != ret) {
        printError("EnumDevices", ret);
        return false;
    }

    if (devList.nDeviceNum == 0) {
        LOG_ERROR("[海康相机] 未发现设备");
        return false;
    }

    if (deviceIndex < 0 || static_cast<unsigned int>(deviceIndex) >= devList.nDeviceNum) {
        LOG_ERROR("[海康相机] 设备索引超出范围: %d, 可用数量: %d", deviceIndex, devList.nDeviceNum);
        return false;
    }

    // 创建设备句柄
    ret = MV_CC_CreateHandle(&handle_, devList.pDeviceInfo[deviceIndex]);
    if (MV_OK != ret) {
        printError("CreateHandle", ret);
        handle_ = nullptr;
        return false;
    }

    // 打开设备
    ret = MV_CC_OpenDevice(handle_, MV_ACCESS_Exclusive, 0);
    if (MV_OK != ret) {
        printError("OpenDevice", ret);
        MV_CC_DestroyHandle(handle_);
        handle_ = nullptr;
        return false;
    }

    // 保存设备信息
    device_info_ = *devList.pDeviceInfo[deviceIndex];
    has_device_info_ = true;

    is_open_.store(true);
    LOG_INFO("[海康相机] 设备已打开: %s", getDeviceInfoString().c_str());

    // 注册图像回调
    ret = MV_CC_RegisterImageCallBackEx(handle_, imageCallbackBridge, this);
    if (MV_OK != ret) {
        printError("RegisterImageCallBackEx", ret);
        // 非致命错误，继续
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
    LOG_INFO("[海康相机] 设备已关闭");
}

bool HikCamera::isOpen() const {
    return is_open_.load();
}

// ========== 取流控制 ==========

bool HikCamera::startGrabbing() {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开，无法开始取流");
        return false;
    }

    if (is_grabbing_.load()) {
        LOG_INFO("[海康相机] 已在取流中");
        return true;
    }

    int ret = MV_CC_StartGrabbing(handle_);
    if (MV_OK != ret) {
        printError("StartGrabbing", ret);
        return false;
    }

    is_grabbing_.store(true);
    LOG_INFO("[海康相机] 开始取流");
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
    LOG_INFO("[海康相机] 停止取流");
    return true;
}

bool HikCamera::isGrabbing() const {
    return is_grabbing_.load();
}

void HikCamera::setImageCallback(HikImageCallback callback) {
    image_callback_ = callback;
}

// ========== 参数配置 ==========

bool HikCamera::setExposureAuto(HikExposureAuto mode) {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开");
        return false;
    }

    unsigned int value = 0;
    switch (mode) {
        case HikExposureAuto::OFF:        value = MV_EXPOSURE_AUTO_MODE_OFF;        break;
        case HikExposureAuto::ONCE:       value = MV_EXPOSURE_AUTO_MODE_ONCE;       break;
        case HikExposureAuto::CONTINUOUS: value = MV_EXPOSURE_AUTO_MODE_CONTINUOUS; break;
    }

    int ret = MV_CC_SetEnumValue(handle_, "ExposureAuto", value);
    if (MV_OK != ret) {
        printError("Set ExposureAuto", ret);
        return false;
    }

    LOG_INFO("[海康相机] 自动曝光模式已设置: %d", static_cast<int>(mode));
    return true;
}

bool HikCamera::setExposureTime(float exposureTimeUs) {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开");
        return false;
    }

    int ret = MV_CC_SetFloatValue(handle_, "ExposureTime", exposureTimeUs);
    if (MV_OK != ret) {
        printError("Set ExposureTime", ret);
        return false;
    }

    LOG_INFO("[海康相机] 曝光时间已设置: %f us", exposureTimeUs);
    return true;
}

bool HikCamera::setGainAuto(HikGainAuto mode) {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开");
        return false;
    }

    unsigned int value = 0;
    switch (mode) {
        case HikGainAuto::OFF:        value = MV_GAIN_MODE_OFF;        break;
        case HikGainAuto::ONCE:       value = MV_GAIN_MODE_ONCE;       break;
        case HikGainAuto::CONTINUOUS: value = MV_GAIN_MODE_CONTINUOUS; break;
    }

    int ret = MV_CC_SetEnumValue(handle_, "GainAuto", value);
    if (MV_OK != ret) {
        printError("Set GainAuto", ret);
        return false;
    }

    LOG_INFO("[海康相机] 自动增益模式已设置: %d", static_cast<int>(mode));
    return true;
}

bool HikCamera::setGain(float gain) {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开");
        return false;
    }

    int ret = MV_CC_SetFloatValue(handle_, "Gain", gain);
    if (MV_OK != ret) {
        printError("Set Gain", ret);
        return false;
    }

    LOG_INFO("[海康相机] 增益已设置: %f", gain);
    return true;
}

bool HikCamera::setTriggerMode(HikTriggerMode mode) {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开");
        return false;
    }

    unsigned int value = (mode == HikTriggerMode::ON) ? MV_TRIGGER_MODE_ON : MV_TRIGGER_MODE_OFF;

    int ret = MV_CC_SetEnumValue(handle_, "TriggerMode", value);
    if (MV_OK != ret) {
        printError("Set TriggerMode", ret);
        return false;
    }

    LOG_INFO("[海康相机] 触发模式已设置: %s", value == MV_TRIGGER_MODE_ON ? "ON" : "OFF");
    return true;
}

bool HikCamera::setTriggerSource(HikTriggerSource source) {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开");
        return false;
    }

    unsigned int value = MV_TRIGGER_SOURCE_SOFTWARE;
    switch (source) {
        case HikTriggerSource::LINE0:     value = MV_TRIGGER_SOURCE_LINE0;     break;
        case HikTriggerSource::LINE1:     value = MV_TRIGGER_SOURCE_LINE1;     break;
        case HikTriggerSource::LINE2:     value = MV_TRIGGER_SOURCE_LINE2;     break;
        case HikTriggerSource::SOFTWARE:  value = MV_TRIGGER_SOURCE_SOFTWARE;  break;
    }

    int ret = MV_CC_SetEnumValue(handle_, "TriggerSource", value);
    if (MV_OK != ret) {
        printError("Set TriggerSource", ret);
        return false;
    }

    LOG_INFO("[海康相机] 触发源已设置: %d", static_cast<int>(source));
    return true;
}

bool HikCamera::triggerSoftware() {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开");
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
        LOG_ERROR("[海康相机] 设备未打开");
        return false;
    }

    int ret = MV_CC_SetIntValueEx(handle_, "Width", width);
    if (MV_OK != ret) {
        printError("Set Width", ret);
        return false;
    }

    LOG_INFO("[海康相机] 图像宽度已设置: %d", width);
    return true;
}

bool HikCamera::setHeight(int height) {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开");
        return false;
    }

    int ret = MV_CC_SetIntValueEx(handle_, "Height", height);
    if (MV_OK != ret) {
        printError("Set Height", ret);
        return false;
    }

    LOG_INFO("[海康相机] 图像高度已设置: %d", height);
    return true;
}

bool HikCamera::setPixelFormat(const std::string& format) {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开");
        return false;
    }

    int ret = MV_CC_SetEnumValueByString(handle_, "PixelFormat", format.c_str());
    if (MV_OK != ret) {
        printError("Set PixelFormat", ret);
        return false;
    }

    LOG_INFO("[海康相机] 像素格式已设置: %s", format.c_str());
    return true;
}

bool HikCamera::setFrameRate(float fps) {
    if (!is_open_.load()) {
        LOG_ERROR("[海康相机] 设备未打开");
        return false;
    }

    int ret = MV_CC_SetFloatValue(handle_, "AcquisitionFrameRate", fps);
    if (MV_OK != ret) {
        printError("Set AcquisitionFrameRate", ret);
        return false;
    }

    LOG_INFO("[海康相机] 帧率已设置: %f fps", fps);
    return true;
}

// ========== 参数查询 ==========

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

// ========== 私有方法 ==========

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
        HikFrameInfo info;
        info.data = pData;
        info.dataLen = pstFrameInfo->nFrameLen;
        info.width = pstFrameInfo->nWidth;
        info.height = pstFrameInfo->nHeight;
        info.pixelType = static_cast<unsigned int>(pstFrameInfo->enPixelType);
        info.frameNum = pstFrameInfo->nFrameNum;
        info.exposureTime = pstFrameInfo->fExposureTime;
        info.gain = pstFrameInfo->fGain;

        image_callback_(info);
    }
}

bool HikCamera::saveImage(const std::string& filepath,
                          unsigned char* data, unsigned int dataLen,
                          unsigned short width, unsigned short height,
                          unsigned int pixelType, int format) {
    if (!is_open_.load() || handle_ == nullptr) {
        LOG_ERROR("[海康相机] 设备未打开，无法保存图像");
        return false;
    }

    // 分配输出缓冲区 (BMP最大约为 width * height * 3 + 1024)
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
    saveParam.iMethodValue  = 1;  // 双线性插值

    int ret = MV_CC_SaveImageEx2(handle_, &saveParam);
    if (MV_OK != ret) {
        printError("SaveImageEx2", ret);
        return false;
    }

    // 写入文件
    FILE* fp = fopen(filepath.c_str(), "wb");
    if (fp == nullptr) {
        LOG_ERROR("[海康相机] 无法创建文件: %s", filepath.c_str());
        return false;
    }

    fwrite(saveParam.pImageBuffer, 1, saveParam.nImageLen, fp);
    fclose(fp);

    LOG_INFO("[海康相机] 图像已保存: %s (%dx%d, %d bytes)", filepath.c_str(), width, height, saveParam.nImageLen);
    return true;
}

void HikCamera::printError(const std::string& operation, int errorCode) {
    LOG_ERROR("[海康相机] %s 失败, 错误码: 0x%x", operation.c_str(), errorCode);
}
