#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include <vector>

// 帧数据（深拷贝，生命周期独立于 SDK 回调）
struct Frame {
    std::vector<unsigned char> data;
    unsigned short width = 0;
    unsigned short height = 0;
    unsigned int pixelType = 0;
    unsigned int frameNum = 0;
};

#endif // FRAME_QUEUE_H
