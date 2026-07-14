/**
 * bench_sub.cpp - 多进程基准测试 Subscriber
 *
 * 独立进程运行，模拟 detector_node/comm_node 等订阅端。
 * 接收消息并统计延迟、丢包率，输出 CSV。
 *
 * 用法:
 *   bench_sub --zmq --mode small --expected 5000 --output-dir ./results
 *   bench_sub --ros2 --mode large --expected 200 --output-dir ./results
 */

#include "bench_common.h"
#include <thread>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running.store(false); }

int main(int argc, char* argv[]) {
    auto cfg = bench::parse_args(argc, argv);

    // 日志初始化
    vision::LogConfig log_cfg;
    log_cfg.level  = vision::LogLevel::INFO;
    log_cfg.output = vision::LogOutput::CONSOLE;
    log_cfg.node_name = "bench_sub";
    vision::Logger::init(log_cfg);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "=== bench_sub ===\n";
    std::cout << "  transport: " << cfg.transport << "\n";
    std::cout << "  mode:      " << cfg.mode << "\n";
    std::cout << "  expected:  " << cfg.expected << "\n";
    std::cout << "  timeout:   " << cfg.timeout_ms << " ms\n";

    bool is_small = (cfg.mode == "small");
    uint16_t port_offset = is_small ? 0 : 10;
    auto nc = bench::create_node_config(cfg, "bench_sub", port_offset);

    NodeFactory factory(nc);

    std::vector<bench::PacketRecord> records;
    bench::LossDetector loss;
    std::mutex mtx;
    std::atomic<int> total_received{0};
    std::atomic<bool> warmup_phase{true};

    // 预热阶段: 丢弃序号 >= 0xFFFF0000 的预热消息
    auto is_warmup_frame = [](uint32_t fn) { return (fn & 0xFFFF0000) == 0xFFFF0000; };

    // 必须在 if/else 外部声明，保证整个等待循环期间 subscriber 存活
    std::shared_ptr<ISubscriber<DetectionMsg>> sub_small;
    std::shared_ptr<ISubscriber<FrameMsg>>     sub_large;

    if (is_small) {
        // ===================== 小数据订阅 =====================
        sub_small = factory.createSubscriber<DetectionMsg>("bench/small");

        sub_small->subscribe([&](const DetectionMsg& msg) {
            int64_t recv_us = bench::now_us();
            if (!g_running.load()) return;

            // 过滤预热消息
            if (is_warmup_frame(msg.frame_num)) return;

            if (warmup_phase.load()) {
                warmup_phase.store(false);
                LOG_INFO("第一条正式消息到达, frame_num=%u", msg.frame_num);
            }

            double latency_us = static_cast<double>(recv_us - msg.timestamp);
            bench::PacketRecord rec;
            rec.frame_num    = msg.frame_num;
            rec.latency_us   = latency_us;
            rec.payload_size = msg.serialize().size();
            rec.recv_ts_us   = recv_us;

            {
                std::lock_guard<std::mutex> lock(mtx);
                records.push_back(rec);
                loss.record(msg.frame_num);
            }
            int cnt = total_received.fetch_add(1) + 1;
            if (cnt % 500 == 0) {
                LOG_INFO("[small] 已接收 %d 条, 最新延迟=%.1f us", cnt, latency_us);
            }
        });

    } else {
        // ===================== 大数据订阅 =====================
        sub_large = factory.createSubscriber<FrameMsg>("bench/large");

        sub_large->subscribe([&](const FrameMsg& msg) {
            int64_t recv_us = bench::now_us();
            if (!g_running.load()) return;

            if (is_warmup_frame(msg.frame_num)) return;

            if (warmup_phase.load()) {
                warmup_phase.store(false);
                LOG_INFO("第一条正式消息到达, frame_num=%u, size=%zu bytes",
                         msg.frame_num, msg.data.size());
            }

            double latency_us = static_cast<double>(recv_us - msg.timestamp);
            bench::PacketRecord rec;
            rec.frame_num    = msg.frame_num;
            rec.latency_us   = latency_us;
            rec.payload_size = msg.data.size();
            rec.recv_ts_us   = recv_us;

            {
                std::lock_guard<std::mutex> lock(mtx);
                records.push_back(rec);
                loss.record(msg.frame_num);
            }
            int cnt = total_received.fetch_add(1) + 1;
            if (cnt % 10 == 0) {
                LOG_INFO("[large] 已接收 %d 条, 最新延迟=%.1f us (%.2f ms)",
                         cnt, latency_us, latency_us / 1000.0);
            }
        });
    }

    LOG_INFO("Subscriber 已启动, 等待数据...");

    // 等待直到接收够数或超时
    int64_t wait_start = bench::now_us();
    while (g_running.load() &&
           total_received.load() < cfg.expected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int64_t waited_ms = (bench::now_us() - wait_start) / 1000;
        if (waited_ms >= cfg.timeout_ms) {
            LOG_WARN("等待超时: 已接收 %d / %d, 耗时 %.1f s",
                     total_received.load(), cfg.expected, waited_ms / 1000.0);
            break;
        }
    }

    // 再等一小段时间，收取可能延迟到达的最后几条
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int64_t total_elapsed_us = bench::now_us() - wait_start;
    double total_elapsed_ms  = total_elapsed_us / 1000.0;

    // 计算统计
    size_t payload_sz = is_small ? 68 : 0;
    if (!records.empty() && !is_small) {
        payload_sz = records[0].payload_size;
    }

    auto result = bench::compute_result(
        is_small ? "pubsub_small" : "pubsub_large",
        cfg.transport == "ros2" ? "ROS2" : "ZeroMQ",
        cfg.expected,
        records, loss, total_elapsed_ms, payload_sz);

    bench::print_result(result);

    // 写 CSV
    std::string out_dir = cfg.output_dir;
    if (out_dir.empty()) out_dir = ".";

    std::string prefix = (cfg.transport == "ros2" ? "ros2" : "zmq");
    std::string mode_str = is_small ? "small" : "large";

    bench::write_detail_csv(out_dir + "/" + prefix + "_multiproc_pubsub_" + mode_str + "_detail.csv",
                            records);

    std::vector<bench::BenchResult> summary_vec = {result};
    bench::write_summary_csv(out_dir + "/" + prefix + "_multiproc_pubsub_" + mode_str + "_summary.csv",
                             summary_vec);

    // JSON 摘要输出到 stdout (供脚本解析)
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n__RESULT_JSON__{"
              << "\"test\":\"pubsub_" << mode_str << "\","
              << "\"transport\":\"" << cfg.transport << "\","
              << "\"expected\":" << cfg.expected << ","
              << "\"received\":" << result.received_count << ","
              << "\"lost\":" << result.lost_count << ","
              << "\"loss_rate_pct\":" << result.loss_rate_pct << ","
              << "\"avg_us\":" << result.avg_latency_us << ","
              << "\"p50_us\":" << result.p50_latency_us << ","
              << "\"p95_us\":" << result.p95_latency_us << ","
              << "\"p99_us\":" << result.p99_latency_us << ","
              << "\"msg_s\":" << result.throughput_msg_s << ","
              << "\"mbps\":" << result.throughput_mbps
              << "}__END__\n";

    LOG_INFO("CSV 已写入: %s/", out_dir.c_str());
    return 0;
}
