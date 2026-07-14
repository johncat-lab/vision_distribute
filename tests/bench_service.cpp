/**
 * bench_service.cpp - 多进程基准测试 Service (server + client)
 *
 * 独立进程运行，--role 控制角色:
 *   server: 注册 echo 服务，阻塞等待直到 SIGINT
 *   client: 执行 N 次同步调用，统计 RTT 和失败率
 *
 * 用法:
 *   bench_service --zmq --role server
 *   bench_service --zmq --role client --count 1000 --output-dir ./results
 */

#include "bench_common.h"
#include <thread>
#include <csignal>
#include <atomic>
#include <chrono>

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running.store(false); }

// ===================== Server 模式 =====================

static int run_server(const bench::BenchConfig& cfg) {
    vision::LogConfig log_cfg;
    log_cfg.level  = vision::LogLevel::INFO;
    log_cfg.output = vision::LogOutput::CONSOLE;
    log_cfg.node_name = "bench_svc_server";
    vision::Logger::init(log_cfg);

    std::cout << "=== bench_service server ===\n";
    std::cout << "  transport: " << cfg.transport << "\n";

    auto nc = bench::create_node_config(cfg, "bench_svc_server", 20);
    NodeFactory factory(nc);

    auto svc = factory.createService<ServiceRequest, ServiceResponse>("bench_svc");

    svc->serve("echo", [](const ServiceRequest& req) -> ServiceResponse {
        ServiceResponse resp;
        resp.success = true;
        resp.data = req.payload;
        return resp;
    });

    LOG_INFO("Service server 已启动, 等待请求... (Ctrl+C 退出)");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("Service server 收到退出信号, 正在关闭...");
    return 0;
}

// ===================== Client 模式 =====================

static int run_client(const bench::BenchConfig& cfg) {
    vision::LogConfig log_cfg;
    log_cfg.level  = vision::LogLevel::INFO;
    log_cfg.output = vision::LogOutput::CONSOLE;
    log_cfg.node_name = "bench_svc_client";
    vision::Logger::init(log_cfg);

    std::cout << "=== bench_service client ===\n";
    std::cout << "  transport: " << cfg.transport << "\n";
    std::cout << "  count:     " << cfg.msg_count << "\n";
    std::cout << "  warmup:    " << cfg.warmup_count << "\n";

    auto nc = bench::create_node_config(cfg, "bench_svc_client", 20);
    NodeFactory factory(nc);

    auto svc = factory.createService<ServiceRequest, ServiceResponse>("bench_svc");

    // 预连接
    svc->preconnect();
    LOG_INFO("正在等待 server 就绪...");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // 探测 server 是否就绪
    int probe_attempts = 0;
    bool server_ready = false;
    while (probe_attempts < 20) {
        try {
            ServiceRequest probe_req;
            probe_req.endpoint = "echo";
            probe_req.payload  = "probe";
            ServiceResponse probe_resp = svc->call("echo", probe_req);
            if (probe_resp.success) {
                server_ready = true;
                break;
            }
        } catch (...) {
            // server 未就绪, 继续等待
        }
        ++probe_attempts;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!server_ready) {
        LOG_ERROR("Service server 未能就绪, 退出");
        return 1;
    }
    LOG_INFO("Service server 已就绪");

    // 预热
    LOG_INFO("开始预热 (%d 次)...", cfg.warmup_count);
    for (int i = 0; i < cfg.warmup_count; ++i) {
        try {
            ServiceRequest req;
            req.endpoint = "echo";
            req.payload  = "warmup_" + std::to_string(i);
            svc->call("echo", req);
        } catch (...) {}
    }
    LOG_INFO("预热完成");

    // 正式测试
    std::vector<bench::PacketRecord> records;
    bench::LossDetector loss;
    int fail_count = 0;

    LOG_INFO("开始 Service 调用测试 (%d 次)...", cfg.msg_count);
    int64_t t_start = bench::now_us();

    for (int i = 0; i < cfg.msg_count; ++i) {
        int64_t call_start = bench::now_us();
        try {
            ServiceRequest req;
            req.endpoint = "echo";
            req.payload  = "bench_payload_" + std::to_string(i);

            ServiceResponse resp = svc->call("echo", req);

            int64_t call_end = bench::now_us();
            if (resp.success) {
                bench::PacketRecord rec;
                rec.frame_num    = static_cast<uint32_t>(i);
                rec.latency_us   = static_cast<double>(call_end - call_start);
                rec.payload_size = req.serialize().size();
                rec.recv_ts_us   = call_end;
                records.push_back(rec);
                loss.record(static_cast<uint32_t>(i));
            } else {
                ++fail_count;
            }
        } catch (const std::exception& e) {
            ++fail_count;
            if (fail_count <= 5) {
                LOG_WARN("调用失败 #%d: %s", i, e.what());
            }
        }

        // 每100次输出进度
        if ((i + 1) % 100 == 0) {
            LOG_INFO("进度: %d / %d (失败: %d)", i + 1, cfg.msg_count, fail_count);
        }
    }

    int64_t t_end = bench::now_us();
    double elapsed_ms = (t_end - t_start) / 1000.0;

    size_t payload_sz = 30;
    auto result = bench::compute_result(
        "service",
        cfg.transport == "ros2" ? "ROS2" : "ZeroMQ",
        cfg.msg_count,
        records, loss, elapsed_ms, payload_sz);

    bench::print_result(result);

    // 写 CSV
    std::string out_dir = cfg.output_dir;
    if (out_dir.empty()) out_dir = ".";

    std::string prefix = (cfg.transport == "ros2" ? "ros2" : "zmq");

    bench::write_detail_csv(out_dir + "/" + prefix + "_multiproc_service_detail.csv", records);

    std::vector<bench::BenchResult> summary_vec = {result};
    bench::write_summary_csv(out_dir + "/" + prefix + "_multiproc_service_summary.csv", summary_vec);

    // JSON 摘要
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n__RESULT_JSON__{"
              << "\"test\":\"service\","
              << "\"transport\":\"" << cfg.transport << "\","
              << "\"expected\":" << cfg.msg_count << ","
              << "\"received\":" << result.received_count << ","
              << "\"lost\":" << result.lost_count << ","
              << "\"loss_rate_pct\":" << result.loss_rate_pct << ","
              << "\"avg_us\":" << result.avg_latency_us << ","
              << "\"p50_us\":" << result.p50_latency_us << ","
              << "\"p95_us\":" << result.p95_latency_us << ","
              << "\"p99_us\":" << result.p99_latency_us << ","
              << "\"msg_s\":" << result.throughput_msg_s
              << "}__END__\n";

    LOG_INFO("CSV 已写入: %s/", out_dir.c_str());
    return 0;
}

// ===================== main =====================

int main(int argc, char* argv[]) {
    auto cfg = bench::parse_args(argc, argv);

    if (cfg.role == "server") {
        return run_server(cfg);
    } else {
        return run_client(cfg);
    }
}
