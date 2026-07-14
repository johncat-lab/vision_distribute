/**
 * bench_pub.cpp - 多进程基准测试 Publisher
 *
 * 独立进程运行，模拟 camera_node 等发布端。
 * 支持 small (DetectionMsg) 和 large (FrameMsg) 两种模式。
 *
 * 用法:
 *   bench_pub --zmq --mode small --total 5000 --interval-us 1000
 *   bench_pub --ros2 --mode large --total 200 --image /path/to/test.png
 */

#include "bench_common.h"
#include <thread>
#include <fstream>
#include <cstring>

int main(int argc, char* argv[]) {
    auto cfg = bench::parse_args(argc, argv);

    // 日志初始化
    vision::LogConfig log_cfg;
    log_cfg.level  = vision::LogLevel::INFO;
    log_cfg.output = vision::LogOutput::CONSOLE;
    log_cfg.node_name = "bench_pub";
    vision::Logger::init(log_cfg);

    std::cout << "=== bench_pub ===\n";
    std::cout << "  transport: " << cfg.transport << "\n";
    std::cout << "  mode:      " << cfg.mode << "\n";
    std::cout << "  total:     " << cfg.msg_count << "\n";
    std::cout << "  interval:  " << cfg.interval_us << " us\n";
    std::cout << "  warmup:    " << cfg.warmup_count << "\n";

    bool is_small = (cfg.mode == "small");
    uint16_t port_offset = is_small ? 0 : 10;
    auto nc = bench::create_node_config(cfg, "bench_pub", port_offset);

    NodeFactory factory(nc);

    if (is_small) {
        // ===================== 小数据发布 =====================
        auto pub = factory.createPublisher<DetectionMsg>("bench/small");

        // 等待 subscriber 连接
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int small_interval = (cfg.interval_us >= 0) ? cfg.interval_us : 1000;

        // 预热
        for (int i = 0; i < cfg.warmup_count; ++i) {
            DetectionMsg msg;
            msg.frame_num  = static_cast<uint32_t>(0xFFFF0000 | i); // 预热用特殊序号
            msg.timestamp  = bench::now_us();
            msg.object_count = 2;
            msg.protocol_string = "TA,100,200,30,40,0.95;TA,300,400,50,60,0.88;NG";
            pub->publish(msg);
            if (small_interval > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(small_interval));
        }

        // 给 subscriber 时间处理完预热
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        LOG_INFO("开始发布小数据消息, 总数=%d, 间隔=%d us", cfg.msg_count, small_interval);

        int64_t t_start = bench::now_us();
        for (int i = 0; i < cfg.msg_count; ++i) {
            DetectionMsg msg;
            msg.frame_num  = static_cast<uint32_t>(i);
            msg.timestamp  = bench::now_us();
            msg.object_count = 2;
            msg.protocol_string = "TA,100,200,30,40,0.95;TA,300,400,50,60,0.88;NG";
            pub->publish(msg);

            if (small_interval > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(small_interval));
        }
        int64_t t_end = bench::now_us();

        double elapsed_ms = (t_end - t_start) / 1000.0;
        LOG_INFO("小数据发布完成: %d 帧, 耗时 %.1f ms, 平均 %.1f msg/s",
                 cfg.msg_count, elapsed_ms,
                 cfg.msg_count / (elapsed_ms / 1000.0));

    } else {
        // ===================== 大数据发布 =====================
        auto pub = factory.createPublisher<FrameMsg>("bench/large");

        std::vector<uint8_t> image_data = bench::load_image_or_generate(cfg.image_path);
        LOG_INFO("图像数据大小: %zu bytes", image_data.size());

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int large_interval = (cfg.interval_us >= 0) ? cfg.interval_us : 50000;

        // 预热
        for (int i = 0; i < cfg.warmup_count; ++i) {
            FrameMsg msg;
            msg.camera_id  = 0;
            msg.timestamp  = bench::now_us();
            msg.width      = 640;
            msg.height     = 480;
            msg.pixel_type = 1;
            msg.frame_num  = static_cast<uint32_t>(0xFFFF0000 | i);
            msg.data       = image_data;
            pub->publish(msg);
            if (large_interval > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(large_interval));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        LOG_INFO("开始发布大数据消息, 总数=%d, 单帧=%zu bytes, 间隔=%d us",
                 cfg.msg_count, image_data.size(), large_interval);

        int64_t t_start = bench::now_us();
        for (int i = 0; i < cfg.msg_count; ++i) {
            FrameMsg msg;
            msg.camera_id  = 0;
            msg.timestamp  = bench::now_us();
            msg.width      = 640;
            msg.height     = 480;
            msg.pixel_type = 1;
            msg.frame_num  = static_cast<uint32_t>(i);
            msg.data       = image_data;
            pub->publish(msg);

            if (large_interval > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(large_interval));
        }
        int64_t t_end = bench::now_us();

        double elapsed_ms = (t_end - t_start) / 1000.0;
        double total_mb = (static_cast<double>(image_data.size()) * cfg.msg_count) / 1024.0 / 1024.0;
        LOG_INFO("大数据发布完成: %d 帧, 耗时 %.1f ms, 总传输 %.1f MB, 平均 %.2f msg/s, %.2f Mbps",
                 cfg.msg_count, elapsed_ms, total_mb,
                 cfg.msg_count / (elapsed_ms / 1000.0),
                 total_mb * 8.0 / (elapsed_ms / 1000.0));
    }

    // 保持进程存活一段时间，让最后的消息送达
    std::this_thread::sleep_for(std::chrono::seconds(2));

    return 0;
}
