/**
 * Minimal ZMQ pub/sub diagnostic - supports both single and cross-process
 */
#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstring>
#include <signal.h>

static std::atomic<bool> g_running{true};
void sig_handler(int) { g_running = false; }

int main(int argc, char* argv[]) {
    std::string mode = "both"; // "both", "pub", "sub"
    uint16_t base_port = 19000;
    std::string topic = "diag/topic";
    int count = 5;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--pub") == 0) mode = "pub";
        else if (strcmp(argv[i], "--sub") == 0) mode = "sub";
        else if (strcmp(argv[i], "--both") == 0) mode = "both";
        else if (strcmp(argv[i], "--port") == 0 && i+1 < argc) base_port = std::stoi(argv[++i]);
        else if (strcmp(argv[i], "--topic") == 0 && i+1 < argc) topic = argv[++i];
        else if (strcmp(argv[i], "--count") == 0 && i+1 < argc) count = std::stoi(argv[++i]);
    }

    std::cout << "=== ZMQ Diagnostic: mode=" << mode << " port=" << base_port
              << " topic=" << topic << " ===" << std::endl;

    // Print the computed port
    size_t h = 0;
    for (char c : topic) h = h * 31 + (unsigned char)c;
    uint16_t computed_port = base_port + (uint16_t)(h % 10);
    std::cout << "Hash=" << h << " PortOffset=" << (h%10)
              << " ComputedPort=" << computed_port << std::endl;

    NodeConfig cfg;
    cfg.transport = TransportType::ZEROMQ;
    cfg.node_name = std::string("diag_") + mode;
    cfg.base_port = base_port;

    NodeFactory factory(cfg);
    signal(SIGINT, sig_handler);

    if (mode == "pub" || mode == "both") {
        auto pub = factory.createPublisher<DetectionMsg>(topic);

        if (mode == "both") {
            // Single-process: also create subscriber
            auto sub = factory.createSubscriber<DetectionMsg>(topic);
            std::atomic<int> received{0};
            std::mutex mtx;
            sub->subscribe([&](const DetectionMsg& msg) {
                std::lock_guard<std::mutex> lock(mtx);
                received++;
                std::cout << "  [RECV] frame=" << msg.frame_num << " count=" << received.load() << std::endl;
            });
            std::this_thread::sleep_for(std::chrono::seconds(1));
            for (int i = 0; i < count; ++i) {
                DetectionMsg msg;
                msg.frame_num = i;
                msg.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                msg.protocol_string = "test";
                pub->publish(msg);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::cout << "Received: " << received.load() << "/" << count << std::endl;
        } else {
            // Publisher-only: send messages
            std::cout << "Publisher sending " << count << " messages..." << std::endl;
            for (int i = 0; i < count; ++i) {
                DetectionMsg msg;
                msg.frame_num = i;
                msg.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                msg.protocol_string = "cross_process_test";
                bool ok = pub->publish(msg);
                std::cout << "  [SEND] frame=" << i << " ok=" << ok << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    } else if (mode == "sub") {
        // Subscriber-only
        auto sub = factory.createSubscriber<DetectionMsg>(topic);
        std::atomic<int> received{0};
        std::mutex mtx;
        sub->subscribe([&](const DetectionMsg& msg) {
            std::lock_guard<std::mutex> lock(mtx);
            int r = received.fetch_add(1) + 1;
            std::cout << "  [RECV] frame=" << msg.frame_num
                      << " count=" << r
                      << " payload=" << msg.protocol_string << std::endl;
        });
        std::cout << "Subscriber waiting for messages..." << std::endl;
        while (g_running.load() && received.load() < count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "Subscriber received: " << received.load() << "/" << count << std::endl;
    }

    return 0;
}
