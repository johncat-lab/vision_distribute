/**
 * bench_inproc.cpp - 单进程快速冒烟基准测试
 *
 * 注意: 单进程中 ZMQ 可能走 inproc 传输、ROS2 可能走 intra-process，
 * 结果不代表真实跨进程性能。主要用于开发阶段快速验证连通性。
 */

#include "bench_common.h"
#include <thread>
#include <csignal>

// ===================== Pub-Sub 小数据测试 =====================

static bench::BenchResult test_pubsub_small(TransportType transport,
                                            const std::string& tname,
                                            const bench::BenchConfig& cfg)
{
    const std::string test_name = "pubsub_small";
    std::cout << "\n=== [" << tname << "] Pub-Sub 小数据 (" << cfg.msg_count << " msg) ===\n";

    auto nc = bench::create_node_config(cfg, "bench_inproc_small", 0);
    NodeFactory factory(nc);

    auto pub = factory.createPublisher<DetectionMsg>("bench/small");
    auto sub = factory.createSubscriber<DetectionMsg>("bench/small");

    std::vector<bench::PacketRecord> records;
    bench::LossDetector loss;
    std::mutex mtx;
    std::atomic<bool> warmup_done{false};
    int warmup_received = 0;

    sub->subscribe([&](const DetectionMsg& msg) {
        int64_t recv_us = bench::now_us();
        if (!warmup_done.load()) {
            // 预热阶段: 不计入统计
            warmup_received++;
            return;
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
    });

    // 等待订阅连接建立
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 预热
    for (int i = 0; i < cfg.warmup_count; ++i) {
        DetectionMsg msg;
        msg.frame_num  = static_cast<uint32_t>(i);
        msg.timestamp  = bench::now_us();
        msg.object_count = 2;
        msg.protocol_string = "TA,100,200,30,40,0.95;TA,300,400,50,60,0.88;NG";
        pub->publish(msg);
        std::this_thread::sleep_for(std::chrono::microseconds(cfg.interval_us));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    warmup_done.store(true);

    // 正式发送
    int64_t t_start = bench::now_us();
    for (int i = 0; i < cfg.msg_count; ++i) {
        DetectionMsg msg;
        msg.frame_num  = static_cast<uint32_t>(i);
        msg.timestamp  = bench::now_us();
        msg.object_count = 2;
        msg.protocol_string = "TA,100,200,30,40,0.95;TA,300,400,50,60,0.88;NG";
        pub->publish(msg);
        if (cfg.interval_us > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(cfg.interval_us));
    }
    int64_t t_end = bench::now_us();

    // 等待接收
    int waited = 0;
    while (static_cast<int>(records.size()) < cfg.msg_count &&
           waited < cfg.wait_timeout_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waited += 10;
    }

    double elapsed_ms = (t_end - t_start) / 1000.0;
    size_t payload_sz = 68; // 约 68 bytes
    auto result = bench::compute_result(test_name, tname, cfg.msg_count,
                                        records, loss, elapsed_ms, payload_sz);
    bench::print_result(result);

    if (!cfg.output_dir.empty() && cfg.output_dir != ".") {
        bench::write_detail_csv(cfg.output_dir + "/" + tname + "_inproc_pubsub_small_detail.csv",
                                records);
    }
    return result;
}

// ===================== Pub-Sub 大数据测试 =====================

static bench::BenchResult test_pubsub_large(TransportType transport,
                                            const std::string& tname,
                                            const bench::BenchConfig& cfg)
{
    const std::string test_name = "pubsub_large";
    std::cout << "\n=== [" << tname << "] Pub-Sub 大数据 (" << cfg.msg_count << " msg) ===\n";

    auto nc = bench::create_node_config(cfg, "bench_inproc_large", 10);
    NodeFactory factory(nc);

    auto pub = factory.createPublisher<FrameMsg>("bench/large");
    auto sub = factory.createSubscriber<FrameMsg>("bench/large");

    std::vector<uint8_t> image_data = bench::load_image_or_generate(cfg.image_path);

    std::vector<bench::PacketRecord> records;
    bench::LossDetector loss;
    std::mutex mtx;
    std::atomic<bool> warmup_done{false};

    sub->subscribe([&](const FrameMsg& msg) {
        int64_t recv_us = bench::now_us();
        if (!warmup_done.load()) return;
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
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 预热
    for (int i = 0; i < cfg.warmup_count; ++i) {
        FrameMsg msg;
        msg.camera_id = 0;
        msg.timestamp = bench::now_us();
        msg.width     = 640;
        msg.height    = 480;
        msg.pixel_type = 1;
        msg.frame_num = static_cast<uint32_t>(i);
        msg.data      = image_data;
        pub->publish(msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    warmup_done.store(true);

    int large_interval = (cfg.interval_us > 0) ? cfg.interval_us : 50000;

    int64_t t_start = bench::now_us();
    for (int i = 0; i < cfg.msg_count; ++i) {
        FrameMsg msg;
        msg.camera_id = 0;
        msg.timestamp = bench::now_us();
        msg.width     = 640;
        msg.height    = 480;
        msg.pixel_type = 1;
        msg.frame_num = static_cast<uint32_t>(i);
        msg.data      = image_data;
        pub->publish(msg);
        std::this_thread::sleep_for(std::chrono::microseconds(large_interval));
    }
    int64_t t_end = bench::now_us();

    int waited = 0;
    while (static_cast<int>(records.size()) < cfg.msg_count &&
           waited < cfg.wait_timeout_ms * 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waited += 10;
    }

    double elapsed_ms = (t_end - t_start) / 1000.0;
    auto result = bench::compute_result(test_name, tname, cfg.msg_count,
                                        records, loss, elapsed_ms, image_data.size());
    bench::print_result(result);

    if (!cfg.output_dir.empty() && cfg.output_dir != ".") {
        bench::write_detail_csv(cfg.output_dir + "/" + tname + "_inproc_pubsub_large_detail.csv",
                                records);
    }
    return result;
}

// ===================== Service 调用测试 =====================

static bench::BenchResult test_service(TransportType transport,
                                       const std::string& tname,
                                       const bench::BenchConfig& cfg)
{
    const std::string test_name = "service";
    std::cout << "\n=== [" << tname << "] Service 调用 (" << cfg.msg_count << " 次) ===\n";

    auto nc = bench::create_node_config(cfg, "bench_inproc_svc", 20);
    NodeFactory factory(nc);

    auto svc = factory.createService<ServiceRequest, ServiceResponse>("bench_svc");

    // ROS2 后端需要注册原生端点
    bench::register_ros2_echo_endpoint(svc);

    svc->serve("echo", [](const ServiceRequest& req) -> ServiceResponse {
        ServiceResponse resp;
        resp.success = true;
        resp.data = req.payload;
        return resp;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    svc->preconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 预热
    for (int i = 0; i < cfg.warmup_count; ++i) {
        try {
            ServiceRequest req;
            req.endpoint = "echo";
            req.payload  = "warmup";
            svc->call("echo", req);
        } catch (...) {}
    }

    std::vector<bench::PacketRecord> records;
    bench::LossDetector loss;
    int fail_count = 0;

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
        }
    }
    int64_t t_end = bench::now_us();

    double elapsed_ms = (t_end - t_start) / 1000.0;
    size_t payload_sz = 30;
    auto result = bench::compute_result(test_name, tname, cfg.msg_count,
                                        records, loss, elapsed_ms, payload_sz);
    bench::print_result(result);

    if (!cfg.output_dir.empty() && cfg.output_dir != ".") {
        bench::write_detail_csv(cfg.output_dir + "/" + tname + "_inproc_service_detail.csv",
                                records);
    }
    return result;
}

// ===================== main =====================

int main(int argc, char* argv[]) {
    auto cfg = bench::parse_args(argc, argv);

    std::cout << "==============================================\n";
    std::cout << "  单进程基准测试 (冒烟)\n";
    std::cout << "==============================================\n";
    std::cout << "  传输层: " << cfg.transport << "\n";
    std::cout << "  消息数: " << cfg.msg_count << "\n";
    std::cout << "  预热数: " << cfg.warmup_count << "\n";
    if (!cfg.image_path.empty())
        std::cout << "  图片:   " << cfg.image_path << "\n";
    std::cout << "==============================================\n";
    std::cout << "  注意: 单进程测试结果不代表跨进程性能\n";
    std::cout << "==============================================\n";

    std::vector<bench::BenchResult> all_results;

    auto run_all = [&](TransportType tt, const std::string& tname) {
        all_results.push_back(test_pubsub_small(tt, tname, cfg));
        all_results.push_back(test_pubsub_large(tt, tname, cfg));
        all_results.push_back(test_service(tt, tname, cfg));
    };

    bool run_zmq  = (cfg.transport == "zmq"  || cfg.transport == "all");
    bool run_ros2 = (cfg.transport == "ros2" || cfg.transport == "all");

    if (run_zmq)  run_all(TransportType::ZEROMQ, "ZeroMQ");
#ifdef HAS_ROS2
    if (run_ros2) run_all(TransportType::ROS2,   "ROS2");
#else
    if (run_ros2) {
        std::cerr << "ERROR: ROS2 不可用 (未编译 HAS_ROS2)\n";
        return 1;
    }
#endif

    // 对比表
    if (run_zmq && run_ros2) {
        for (size_t i = 0; i + 1 < all_results.size(); i += 2) {
            bench::print_comparison_table(all_results[i], all_results[i+1],
                                          all_results[i].test_name);
        }
    }

    // 写 summary CSV
    if (!cfg.output_dir.empty() && cfg.output_dir != ".") {
        bench::write_summary_csv(cfg.output_dir + "/inproc_summary.csv", all_results);
        std::cout << "\nCSV 已写入: " << cfg.output_dir << "/\n";
    }

    std::cout << "\n==============================================\n";
    std::cout << "  单进程冒烟测试完成\n";
    std::cout << "==============================================\n";
    return 0;
}
