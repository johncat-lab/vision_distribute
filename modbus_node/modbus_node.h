#pragma once
#include "rpc/node_base.h"
#include "rpc/node_container.h"
#include "rpc/message_types.h"
#ifdef _WIN32
#include <modbus/modbus.h>
#else
#include <modbus.h>
#endif
#include <mutex>
#include <atomic>
#include <string>
#include <vector>
#include <cstdint>

/// @brief Modbus 节点：
/// - 支持 RTU (type=0) 和 TCP (type=1) 两种模式
/// - RTU 模式：addr 为 COM 端口号（如 "COM3" 或 "/dev/ttyUSB0"）
/// - TCP 模式：addr 为 IP 地址（如 "192.168.1.100"）
/// - 数据流通道：输入 DetectionMsg，可写入 Modbus 寄存器
/// - 服务通道：提供 modbus 角色（read_coils/write_coils/read_holding/write_holding/get_config/get_status）
class ModbusNode : public NodeBase {
public:
    ModbusNode() = default;
    ~ModbusNode() override;

    NodeManifest describe() const override;
    void initDataflow(NodeEdgeManager& edges,
                       const std::string& config_file) override;
    void initServices(ServiceEndpointRegistry& services) override;
    void initServices(ServiceEndpointRegistry& services, NodeContainer& container) override;

    bool start() override;
    void stop() override;
    void tick(std::atomic<bool>& running) override;

private:
    struct ModbusConfig {
        int type = 0;
        std::string addr = "COM3";
        int port = 502;
        int baud_rate = 9600;
        int data_bits = 8;
        char parity = 'N';
        int stop_bits = 1;
        int slave_id = 1;
        int interval_ms = 100;
    };

    static ModbusConfig loadConfig(const std::string& path);
    static bool saveConfig(const std::string& path, const ModbusConfig& cfg);

    bool connectRTU();
    bool connectTCP();
    void disconnect();

    bool readCoils(uint16_t start_addr, uint16_t count, std::vector<bool>& result);
    bool writeCoils(uint16_t start_addr, const std::vector<bool>& values);
    bool readHoldingRegisters(uint16_t start_addr, uint16_t count, std::vector<uint16_t>& result);
    bool writeHoldingRegisters(uint16_t start_addr, const std::vector<uint16_t>& values);

    modbus_t* modbus_ctx_ = nullptr;
    std::mutex modbus_mutex_;
    ModbusConfig cfg_;

    std::string modbus_config_file_;

    DetectionMsg latest_detection_;
    std::mutex detection_mutex_;

    std::shared_ptr<ISubscriber<DetectionMsg>> detection_sub_;

    bool is_connected_ = false;

    ServiceResponse handleReadCoils(const ServiceRequest& req);
    ServiceResponse handleWriteCoils(const ServiceRequest& req);
    ServiceResponse handleReadHolding(const ServiceRequest& req);
    ServiceResponse handleWriteHolding(const ServiceRequest& req);
    ServiceResponse handleGetConfig(const ServiceRequest& req);
    ServiceResponse handleGetStatus(const ServiceRequest& req);
};