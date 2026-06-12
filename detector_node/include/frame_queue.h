#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

// 帧数据（深拷贝，生命周期独立于 SDK 回调）
struct Frame {
    std::vector<unsigned char> data;
    unsigned short width = 0;
    unsigned short height = 0;
    unsigned int pixelType = 0;
    unsigned int frameNum = 0;
};

// 线程安全的固定长度帧队列
template<typename T>
class FrameQueue {
public:
    explicit FrameQueue(size_t max_size = 5)
        : max_size_(max_size), shutdown_(false) {}

    ~FrameQueue() {
        shutdown();
    }

    // 非阻塞入队，队列满时返回 false
    bool try_enqueue(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= max_size_) {
            return false;
        }
        queue_.push_back(item);
        cv_.notify_one();
        return true;
    }

    // 阻塞出队，返回是否成功（false 表示队列已关闭）
    bool try_dequeue(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        
        if (shutdown_ && queue_.empty()) {
            return false;
        }
        
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // 关闭队列，唤醒所有等待的线程
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    // 获取队列当前大小
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::deque<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    size_t max_size_;
    std::atomic<bool> shutdown_{false};
};

#endif // FRAME_QUEUE_H
