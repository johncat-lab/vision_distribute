#ifndef DETECTOR_H
#define DETECTOR_H

#include "object_info.h"
#include "frame_queue.h"
#include <string>

// 检测器抽象基类
// YOLO 检测器和 OpenCV 模版匹配检测器均继承此接口
class Detector {
public:
    virtual ~Detector() = default;

    // 初始化检测器 (加载模型/模版等资源)
    virtual bool init() = 0;

    // 检测器是否已就绪
    virtual bool isReady() const = 0;

    // 对一帧图像执行检测，返回检测结果列表
    virtual ObjectInfoList detect(const Frame& frame) = 0;

    // 将检测结果绘制到帧图像上并保存为文件
    virtual bool saveAnnotated(const Frame& frame, const std::string& path) = 0;
};

#endif // DETECTOR_H
