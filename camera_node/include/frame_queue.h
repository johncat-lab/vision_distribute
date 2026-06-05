#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

// 海康相机头文件（用于 HikFrameInfo）
#include "hik_camera.h"

// 帧数据（深拷贝，生命周期独立于 SDK 回调）
struct Frame {
    std::vector<unsigned char> data;
    unsigned short width = 0;
    unsigned short height = 0;
    unsigned int pixelType = 0;
    unsigned int frameNum = 0;
};

// 线程安全的固定长度帧队列
// - push: 相机回调线程调用，满时丢弃最旧帧
// - pop: 推理线程调用，阻塞等待新帧
class FrameQueue {
public:
    explicit FrameQueue(size_t max_size = 5)
        : max_size_(max_size), shutdown_(false) {}

    // 从相机帧信息深拷贝入队（SDK回调中调用，需快速返回）
    void push(const HikFrameInfo& frameInfo) {
        Frame frame;
        frame.width = frameInfo.width;
        frame.height = frameInfo.height;
        frame.pixelType = frameInfo.pixelType;
        frame.frameNum = frameInfo.frameNum;

        // 深拷贝像素数据
        if (frameInfo.data != nullptr && frameInfo.dataLen > 0) {
            frame.data.assign(frameInfo.data, frameInfo.data + frameInfo.dataLen);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 满时丢弃最旧帧
            while (queue_.size() >= max_size_) {
                queue_.pop_front();
            }

            queue_.push_back(std::move(frame));
        }

        cond_.notify_one();
    }

    // 阻塞等待取帧，返回 false 表示队列已关闭或超时
    bool pop(Frame& out, int timeout_ms = 1000) {
        std::unique_lock<std::mutex> lock(mutex_);

        bool got = cond_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
            return !queue_.empty() || shutdown_.load();
        });

        if (!got || queue_.empty()) {
            return false;
        }

        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // 关闭队列，唤醒所有等待线程
    void shutdown() {
        shutdown_.store(true);
        cond_.notify_all();
    }

    // 当前队列长度
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    size_t max_size_;
    std::deque<Frame> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::atomic<bool> shutdown_;
};

#endif // FRAME_QUEUE_H
