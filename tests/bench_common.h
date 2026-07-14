#pragma once
/**
 * bench_common.h - 通信效率基准测试共享工具 (header-only)
 *
 * 提供 BenchConfig、LossDetector、BenchResult、CSV 输出、参数解析等功能，
 * 供 bench_inproc / bench_pub / bench_sub / bench_service 共用。
 */

#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "logger/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ROS2 原生端点注册需要这些头文件 (必须在 namespace bench 外部)
#ifdef HAS_ROS2
#include <rclcpp/rclcpp.hpp>
#include "vision_interfaces/srv/camera_soft_trigger.hpp"
#endif

namespace bench {

// ===================== 时间工具 =====================

inline int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

// ===================== 测试配置 =====================

struct BenchConfig {
    int         msg_count     = 1000;
    int         interval_us   = -1;      // 发送间隔 (微秒), -1=默认, 0=尽可能快
    int         warmup_count  = 50;
    int         wait_timeout_ms = 10000;
    std::string transport     = "zmq";   // "zmq" | "ros2"
    std::string output_dir    = ".";
    std::string image_path    = "";
    uint16_t    base_port     = 16000;
    int         repeat        = 1;
    std::string mode          = "small"; // "small" | "large"
    std::string role          = "client";// "server" | "client" (service 测试)
    int         expected      = -1;      // subscriber 预期消息数, -1=使用 msg_count
    int         timeout_ms    = 15000;
};

// ===================== 逐包记录 =====================

struct PacketRecord {
    uint32_t frame_num    = 0;
    double   latency_us   = 0.0;
    size_t   payload_size = 0;
    int64_t  recv_ts_us   = 0;
};

// ===================== 统计结果 =====================

struct BenchResult {
    std::string test_name;
    std::string transport_name;
    int    expected_count  = 0;
    int    received_count  = 0;
    int    lost_count      = 0;
    double loss_rate_pct   = 0.0;
    double avg_latency_us  = 0.0;
    double min_latency_us  = 0.0;
    double max_latency_us  = 0.0;
    double p50_latency_us  = 0.0;
    double p95_latency_us  = 0.0;
    double p99_latency_us  = 0.0;
    double throughput_msg_s = 0.0;
    double throughput_mbps  = 0.0;
    double total_elapsed_ms = 0.0;
    size_t payload_bytes    = 0;
    std::vector<double> all_latencies_us;
};

// ===================== 丢包检测器 =====================

class LossDetector {
public:
    void record(uint32_t frame_num) {
        received_.insert(frame_num);
    }

    int lost_count(int expected_count) const {
        int lost = 0;
        for (int i = 0; i < expected_count; ++i) {
            if (received_.find(static_cast<uint32_t>(i)) == received_.end())
                ++lost;
        }
        return lost;
    }

    double loss_rate(int expected_count) const {
        if (expected_count <= 0) return 0.0;
        return static_cast<double>(lost_count(expected_count)) / expected_count * 100.0;
    }

    std::vector<uint32_t> lost_frames(int expected_count) const {
        std::vector<uint32_t> lost;
        for (int i = 0; i < expected_count; ++i) {
            if (received_.find(static_cast<uint32_t>(i)) == received_.end())
                lost.push_back(static_cast<uint32_t>(i));
        }
        return lost;
    }

    size_t received_count() const { return received_.size(); }

private:
    std::set<uint32_t> received_;
};

// ===================== 统计计算 =====================

inline BenchResult compute_result(
    const std::string& test_name,
    const std::string& transport_name,
    int expected_count,
    const std::vector<PacketRecord>& records,
    const LossDetector& loss_detector,
    double total_elapsed_ms,
    size_t single_payload_bytes)
{
    BenchResult r;
    r.test_name      = test_name;
    r.transport_name = transport_name;
    r.expected_count = expected_count;
    r.received_count = static_cast<int>(records.size());
    r.lost_count     = loss_detector.lost_count(expected_count);
    r.loss_rate_pct  = loss_detector.loss_rate(expected_count);
    r.total_elapsed_ms = total_elapsed_ms;
    r.payload_bytes  = single_payload_bytes;

    if (records.empty()) return r;

    std::vector<double> lats;
    lats.reserve(records.size());
    double sum = 0.0;
    for (const auto& rec : records) {
        lats.push_back(rec.latency_us);
        sum += rec.latency_us;
    }
    std::sort(lats.begin(), lats.end());

    r.all_latencies_us = lats;
    r.avg_latency_us   = sum / lats.size();
    r.min_latency_us   = lats.front();
    r.max_latency_us   = lats.back();

    auto pct = [&](double p) -> double {
        size_t idx = static_cast<size_t>(lats.size() * p);
        if (idx >= lats.size()) idx = lats.size() - 1;
        return lats[idx];
    };
    r.p50_latency_us = pct(0.50);
    r.p95_latency_us = pct(0.95);
    r.p99_latency_us = pct(0.99);

    if (total_elapsed_ms > 0) {
        r.throughput_msg_s = r.received_count / (total_elapsed_ms / 1000.0);
        double total_bytes = static_cast<double>(single_payload_bytes) * r.received_count;
        r.throughput_mbps = (total_bytes * 8.0) / (total_elapsed_ms / 1000.0) / 1e6;
    }

    return r;
}

// ===================== CSV 输出 =====================

inline void write_summary_csv(const std::string& filepath,
                              const std::vector<BenchResult>& results)
{
    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
        std::cerr << "ERROR: 无法写入 " << filepath << std::endl;
        return;
    }
    ofs << "test_name,transport,expected,received,lost,loss_rate_pct,"
        << "avg_us,min_us,max_us,p50_us,p95_us,p99_us,"
        << "msg_s,mbps,elapsed_ms,payload_bytes\n";

    ofs << std::fixed << std::setprecision(2);
    for (const auto& r : results) {
        ofs << r.test_name << ","
            << r.transport_name << ","
            << r.expected_count << ","
            << r.received_count << ","
            << r.lost_count << ","
            << r.loss_rate_pct << ","
            << r.avg_latency_us << ","
            << r.min_latency_us << ","
            << r.max_latency_us << ","
            << r.p50_latency_us << ","
            << r.p95_latency_us << ","
            << r.p99_latency_us << ","
            << r.throughput_msg_s << ","
            << r.throughput_mbps << ","
            << r.total_elapsed_ms << ","
            << r.payload_bytes << "\n";
    }
}

inline void write_detail_csv(const std::string& filepath,
                             const std::vector<PacketRecord>& records)
{
    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
        std::cerr << "ERROR: 无法写入 " << filepath << std::endl;
        return;
    }
    ofs << "seq,frame_num,latency_us,payload_size,recv_timestamp_us\n";
    ofs << std::fixed << std::setprecision(2);
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        ofs << i << ","
            << r.frame_num << ","
            << r.latency_us << ","
            << r.payload_size << ","
            << r.recv_ts_us << "\n";
    }
}

// ===================== 控制台输出 =====================

inline void print_result(const BenchResult& r) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  ┌───────────────────────────────────────────────────────────┐\n";
    std::cout << "  │ [" << r.transport_name << "] " << std::left << std::setw(40) << r.test_name << "│\n";
    std::cout << "  ├───────────────────────────────────────────────────────────┤\n";
    std::cout << "  │ 发送/接收: " << r.expected_count << " / " << r.received_count
              << "  (丢失 " << r.lost_count << ", " << r.loss_rate_pct << "%) │\n";
    std::cout << "  │ 平均延迟:  " << std::setw(10) << std::right << r.avg_latency_us << " us  │\n";
    std::cout << "  │ 最小延迟:  " << std::setw(10) << std::right << r.min_latency_us << " us  │\n";
    std::cout << "  │ 最大延迟:  " << std::setw(10) << std::right << r.max_latency_us << " us  │\n";
    std::cout << "  │ P50 延迟:  " << std::setw(10) << std::right << r.p50_latency_us << " us  │\n";
    std::cout << "  │ P95 延迟:  " << std::setw(10) << std::right << r.p95_latency_us << " us  │\n";
    std::cout << "  │ P99 延迟:  " << std::setw(10) << std::right << r.p99_latency_us << " us  │\n";
    std::cout << "  │ 吞吐量:    " << std::setw(10) << std::right << r.throughput_msg_s << " msg/s │\n";
    if (r.payload_bytes > 1024) {
        std::cout << "  │ 带宽:      " << std::setw(10) << std::right << r.throughput_mbps << " Mbps  │\n";
    }
    std::cout << "  │ 总耗时:    " << std::setw(10) << std::right << r.total_elapsed_ms << " ms   │\n";
    std::cout << "  └───────────────────────────────────────────────────────────┘\n";
}

inline void print_comparison_table(const BenchResult& zmq, const BenchResult& ros2,
                                   const std::string& test_name) {
    std::cout << "\n" << std::string(72, '=') << "\n";
    std::cout << "           " << test_name << " - ZeroMQ vs ROS2\n";
    std::cout << std::string(72, '=') << "\n";
    std::cout << std::fixed << std::setprecision(2);

    auto row = [](const std::string& label, double v1, double v2, const std::string& unit) {
        std::cout << "│ " << std::setw(16) << std::left << label
                  << " │ " << std::setw(16) << std::right << v1
                  << " │ " << std::setw(16) << std::right << v2;
        if (v1 > 0) {
            double diff = (v2 - v1) / v1 * 100.0;
            std::cout << " │ " << std::setw(8) << std::right << (diff >= 0 ? "+" : "") << diff << "% │\n";
        } else {
            std::cout << " │ " << std::setw(9) << "---" << " │\n";
        }
    };

    std::cout << "┌──────────────────┬──────────────────┬──────────────────┬─────────────┐\n";
    std::cout << "│ 指标             │ ZeroMQ          │ ROS2            │ 差异        │\n";
    std::cout << "├──────────────────┼──────────────────┼──────────────────┼─────────────┤\n";
    row("平均延迟(us)",  zmq.avg_latency_us,  ros2.avg_latency_us, "us");
    row("P50 延迟(us)",  zmq.p50_latency_us,  ros2.p50_latency_us, "us");
    row("P95 延迟(us)",  zmq.p95_latency_us,  ros2.p95_latency_us, "us");
    row("P99 延迟(us)",  zmq.p99_latency_us,  ros2.p99_latency_us, "us");
    row("吞吐量(msg/s)", zmq.throughput_msg_s, ros2.throughput_msg_s, "");
    row("丢包率(%)",     zmq.loss_rate_pct,    ros2.loss_rate_pct, "%");
    std::cout << "└──────────────────┴──────────────────┴──────────────────┴─────────────┘\n";
}

// ===================== 命令行解析 =====================

inline BenchConfig parse_args(int argc, char* argv[]) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            return "";
        };
        if      (arg == "--zmq")        cfg.transport = "zmq";
        else if (arg == "--ros2")       cfg.transport = "ros2";
        else if (arg == "--all")        cfg.transport = "all";
        else if (arg == "--count")      cfg.msg_count = std::stoi(next());
        else if (arg == "--interval-us") cfg.interval_us = std::stoi(next());
        else if (arg == "--warmup")     cfg.warmup_count = std::stoi(next());
        else if (arg == "--image")      cfg.image_path = next();
        else if (arg == "--output-dir") cfg.output_dir = next();
        else if (arg == "--output")     cfg.output_dir = next();
        else if (arg == "--base-port")  cfg.base_port = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--repeat")     cfg.repeat = std::stoi(next());
        else if (arg == "--mode")       cfg.mode = next();
        else if (arg == "--role")       cfg.role = next();
        else if (arg == "--expected")   cfg.expected = std::stoi(next());
        else if (arg == "--timeout-ms") cfg.timeout_ms = std::stoi(next());
        else if (arg == "--total")      cfg.msg_count = std::stoi(next());
    }
    if (cfg.expected < 0) cfg.expected = cfg.msg_count;
    return cfg;
}

// ===================== NodeConfig 构建 =====================

inline NodeConfig create_node_config(const BenchConfig& cfg, const std::string& node_name,
                                     uint16_t port_offset = 0) {
    NodeConfig nc;
    nc.transport = (cfg.transport == "ros2") ? TransportType::ROS2 : TransportType::ZEROMQ;
    nc.node_name = node_name;
    nc.base_port = cfg.base_port + port_offset;
    nc.service_timeout_ms = 10000;
    nc.service_wait_ms    = 5000;
    nc.service_max_retries = 3;
    // 注: ROS2 intra-process 在 ros2_backend.cpp 中硬编码为 true,
    // 多进程测试中不影响 (不同进程无法走 intra-process 传输)
    return nc;
}

// ===================== ROS2 原生端点注册 =====================

template<typename Request, typename Response>
inline void register_ros2_echo_endpoint(std::shared_ptr<IService<Request, Response>>) {
}

// ===================== 图像加载 =====================

inline std::vector<uint8_t> load_image_or_generate(const std::string& image_path) {
    std::vector<uint8_t> data;
    if (!image_path.empty()) {
        // 直接读取文件原始字节
        std::ifstream ifs(image_path, std::ios::binary | std::ios::ate);
        if (ifs.is_open()) {
            auto sz = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            data.resize(static_cast<size_t>(sz));
            ifs.read(reinterpret_cast<char*>(data.data()), sz);
            std::cout << "  [INFO] 加载图片: " << image_path
                      << " (" << sz << " bytes)\n";
            return data;
        }
        std::cerr << "  [WARN] 无法打开图片 " << image_path << ", 使用生成数据\n";
    }
    // 生成 640x480 随机数据
    data.resize(640 * 480);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i & 0xFF);
    std::cout << "  [INFO] 使用生成数据: " << data.size() << " bytes\n";
    return data;
}

} // namespace bench
