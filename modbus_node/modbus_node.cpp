#include "modbus_node.h"
#include "rpc/node_factory.h"
#include "rpc/edge_manager.h"
#include "rpc/service_endpoint_registry.h"
#include "logger/logger.h"

#ifdef HAS_ROS2
#include "vision_interfaces/srv/comm_set_config.hpp"
#include "vision_interfaces/srv/comm_get_config.hpp"
#include "vision_interfaces/srv/comm_get_status.hpp"
#endif

#include <opencv2/core.hpp>
#include <filesystem>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>

ModbusNode::~ModbusNode() {
    stop();
}

ModbusNode::ModbusConfig ModbusNode::loadConfig(const std::string& path) {
    ModbusConfig cfg;
    if (path.empty()) return cfg;
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        LOG_WARN("[ModbusNode] 无法打开Modbus配置文件: %s", path.c_str());
        return cfg;
    }
    if (!fs["type"].empty())           cfg.type = (int)fs["type"];
    if (!fs["addr"].empty())           cfg.addr = (std::string)fs["addr"];
    if (!fs["port"].empty())           cfg.port = (int)fs["port"];
    if (!fs["baud_rate"].empty())      cfg.baud_rate = (int)fs["baud_rate"];
    if (!fs["data_bits"].empty())      cfg.data_bits = (int)fs["data_bits"];
    if (!fs["parity"].empty())         cfg.parity = ((std::string)fs["parity"])[0];
    if (!fs["stop_bits"].empty())      cfg.stop_bits = (int)fs["stop_bits"];
    if (!fs["slave_id"].empty())       cfg.slave_id = (int)fs["slave_id"];
    if (!fs["interval_ms"].empty())    cfg.interval_ms = (int)fs["interval_ms"];
    fs.release();
    return cfg;
}

bool ModbusNode::saveConfig(const std::string& path, const ModbusConfig& cfg) {
    cv::FileStorage fs(path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) {
        LOG_ERROR("[ModbusNode] 无法写入配置: %s", path.c_str());
        return false;
    }
    fs << "type" << cfg.type;
    fs << "addr" << cfg.addr;
    fs << "port" << cfg.port;
    fs << "baud_rate" << cfg.baud_rate;
    fs << "data_bits" << cfg.data_bits;
    fs << "parity" << std::string(1, cfg.parity);
    fs << "stop_bits" << cfg.stop_bits;
    fs << "slave_id" << cfg.slave_id;
    fs << "interval_ms" << cfg.interval_ms;
    fs.release();
    return true;
}

NodeManifest ModbusNode::describe() const {
    NodeManifest m;
    m.name = "modbus_node";
    m.binary = "modbus_node";
    m.version = "1.0";
    m.config_file = "modbus.xml";
    m.inputs.push_back({"detection_input", "DetectionMsg", "检测结果"});
    m.provides_services.push_back({"modbus", {"read_coils", "write_coils", "read_holding", "write_holding", "get_config", "get_status"}});
    return m;
}

void ModbusNode::initDataflow(NodeEdgeManager& edges,
                               const std::string& config_file) {
    edges.setDefaultTopic("detection_input", "vision/detection");
    detection_sub_ = edges.subscribe<DetectionMsg>("detection_input", "vision/detection");

    detection_sub_->subscribe([this](const DetectionMsg& msg) {
        std::lock_guard<std::mutex> lock(detection_mutex_);
        latest_detection_ = msg;
        (void)msg;
    });

    modbus_config_file_ = config_file;

    if (!modbus_config_file_.empty()) {
        LOG_INFO("[ModbusNode] Modbus配置文件: %s", modbus_config_file_.c_str());
    } else {
        LOG_INFO("[ModbusNode] 未指定Modbus配置文件，将使用默认值");
    }

    LOG_INFO("[ModbusNode] 数据流通道已初始化");
}

void ModbusNode::initServices(ServiceEndpointRegistry& services) {
    services.registerEndpoint({"read_coils", "读取线圈寄存器", false, 0},
        [this](const ServiceRequest& req) { return handleReadCoils(req); });

    services.registerEndpoint({"write_coils", "写入线圈寄存器", false, 0},
        [this](const ServiceRequest& req) { return handleWriteCoils(req); });

    services.registerEndpoint({"read_holding", "读取保持寄存器", false, 0},
        [this](const ServiceRequest& req) { return handleReadHolding(req); });

    services.registerEndpoint({"write_holding", "写入保持寄存器", false, 0},
        [this](const ServiceRequest& req) { return handleWriteHolding(req); });

    services.registerEndpoint({"get_config", "获取当前Modbus配置", false, 0},
        [this](const ServiceRequest& req) { return handleGetConfig(req); });

    services.registerEndpoint({"get_status", "获取连接状态", false, 0},
        [this](const ServiceRequest& req) { return handleGetStatus(req); });

    LOG_INFO("[ModbusNode] 已注册 6 个服务端点");
}

void ModbusNode::initServices(ServiceEndpointRegistry& services, NodeContainer& container) {
    initServices(services);

#ifdef HAS_ROS2
    container.registerRos2NativeEndpoint<vision_interfaces::srv::CommGetConfig>(
        "get_config",
        [](auto) -> ServiceRequest { return ServiceRequest{}; },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->config_data = sr.data();
        },
        [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CommGetConfig::Request> {
            return std::make_shared<vision_interfaces::srv::CommGetConfig::Request>();
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->config_data);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::CommGetStatus>(
        "get_status",
        [](auto) -> ServiceRequest { return ServiceRequest{}; },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->status_data = sr.data();
        },
        [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CommGetStatus::Request> {
            return std::make_shared<vision_interfaces::srv::CommGetStatus::Request>();
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->status_data);
            return sr;
        });

    LOG_INFO("[ModbusNode] 已注册 ROS2 原生 service 类型映射");
#endif
}

bool ModbusNode::connectRTU() {
    disconnect();

    modbus_ctx_ = modbus_new_rtu(cfg_.addr.c_str(), cfg_.baud_rate, cfg_.parity,
                                  cfg_.data_bits, cfg_.stop_bits);
    if (!modbus_ctx_) {
        LOG_ERROR("[ModbusNode] 创建RTU上下文失败: %s", modbus_strerror(errno));
        return false;
    }

    modbus_set_slave(modbus_ctx_, cfg_.slave_id);
    modbus_set_response_timeout(modbus_ctx_, 2, 0);

    if (modbus_connect(modbus_ctx_) == -1) {
        LOG_ERROR("[ModbusNode] RTU连接失败: %s", modbus_strerror(errno));
        modbus_free(modbus_ctx_);
        modbus_ctx_ = nullptr;
        return false;
    }

    LOG_INFO("[ModbusNode] RTU已连接: %s @ %d bps", cfg_.addr.c_str(), cfg_.baud_rate);
    is_connected_ = true;
    return true;
}

bool ModbusNode::connectTCP() {
    disconnect();

    modbus_ctx_ = modbus_new_tcp(cfg_.addr.c_str(), cfg_.port);
    if (!modbus_ctx_) {
        LOG_ERROR("[ModbusNode] 创建TCP上下文失败: %s", modbus_strerror(errno));
        return false;
    }

    modbus_set_slave(modbus_ctx_, cfg_.slave_id);
    modbus_set_response_timeout(modbus_ctx_, 2, 0);

    if (modbus_connect(modbus_ctx_) == -1) {
        LOG_ERROR("[ModbusNode] TCP连接失败: %s", modbus_strerror(errno));
        modbus_free(modbus_ctx_);
        modbus_ctx_ = nullptr;
        return false;
    }

    LOG_INFO("[ModbusNode] TCP已连接: %s:%d", cfg_.addr.c_str(), cfg_.port);
    is_connected_ = true;
    return true;
}

void ModbusNode::disconnect() {
    if (modbus_ctx_) {
        modbus_close(modbus_ctx_);
        modbus_free(modbus_ctx_);
        modbus_ctx_ = nullptr;
    }
    is_connected_ = false;
}

bool ModbusNode::start() {
    cfg_ = loadConfig(modbus_config_file_);

    LOG_INFO("[ModbusNode] 配置: type=%d addr=%s port=%d slave_id=%d",
             cfg_.type, cfg_.addr.c_str(), cfg_.port, cfg_.slave_id);

    try {
        if (cfg_.type == 0) {
            return connectRTU();
        } else {
            return connectTCP();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[ModbusNode] 启动失败: %s", e.what());
        return false;
    }
}

void ModbusNode::stop() {
    disconnect();
    LOG_INFO("[ModbusNode] 已停止");
}

void ModbusNode::tick(std::atomic<bool>& running) {
    int interval = std::max(10, cfg_.interval_ms);
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));

        if (!is_connected_) {
            LOG_WARN("[ModbusNode] 未连接，尝试重连...");
            if (cfg_.type == 0) {
                connectRTU();
            } else {
                connectTCP();
            }
            continue;
        }

        if (latest_detection_.object_count() > 0) {
            std::lock_guard<std::mutex> lock(detection_mutex_);
            int obj_count = latest_detection_.object_count();
            std::lock_guard<std::mutex> modbus_lock(modbus_mutex_);
            if (modbus_ctx_) {
                std::vector<uint16_t> values = {static_cast<uint16_t>(obj_count)};
                writeHoldingRegisters(0, values);
            }
        }
    }
}

bool ModbusNode::readCoils(uint16_t start_addr, uint16_t count, std::vector<bool>& result) {
    if (!modbus_ctx_) return false;

    result.resize(count);
    std::vector<uint8_t> dest(count / 8 + 1, 0);

    std::lock_guard<std::mutex> lock(modbus_mutex_);
    int rc = modbus_read_bits(modbus_ctx_, start_addr, count, dest.data());
    if (rc == -1) {
        LOG_ERROR("[ModbusNode] read_coils失败: %s", modbus_strerror(errno));
        is_connected_ = false;
        return false;
    }

    for (uint16_t i = 0; i < count; ++i) {
        result[i] = (dest[i / 8] & (1 << (i % 8))) != 0;
    }
    return true;
}

bool ModbusNode::writeCoils(uint16_t start_addr, const std::vector<bool>& values) {
    if (!modbus_ctx_) return false;

    std::vector<uint8_t> dest(values.size() / 8 + 1, 0);
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i]) {
            dest[i / 8] |= (1 << (i % 8));
        }
    }

    std::lock_guard<std::mutex> lock(modbus_mutex_);
    int rc = modbus_write_bits(modbus_ctx_, start_addr, static_cast<int>(values.size()), dest.data());
    if (rc == -1) {
        LOG_ERROR("[ModbusNode] write_coils失败: %s", modbus_strerror(errno));
        is_connected_ = false;
        return false;
    }
    return true;
}

bool ModbusNode::readHoldingRegisters(uint16_t start_addr, uint16_t count, std::vector<uint16_t>& result) {
    if (!modbus_ctx_) return false;

    result.resize(count);

    std::lock_guard<std::mutex> lock(modbus_mutex_);
    int rc = modbus_read_registers(modbus_ctx_, start_addr, count, result.data());
    if (rc == -1) {
        LOG_ERROR("[ModbusNode] read_holding失败: %s", modbus_strerror(errno));
        is_connected_ = false;
        return false;
    }
    return true;
}

bool ModbusNode::writeHoldingRegisters(uint16_t start_addr, const std::vector<uint16_t>& values) {
    if (!modbus_ctx_) return false;

    std::lock_guard<std::mutex> lock(modbus_mutex_);
    int rc = modbus_write_registers(modbus_ctx_, start_addr, static_cast<int>(values.size()), values.data());
    if (rc == -1) {
        LOG_ERROR("[ModbusNode] write_holding失败: %s", modbus_strerror(errno));
        is_connected_ = false;
        return false;
    }
    return true;
}

ServiceResponse ModbusNode::handleReadCoils(const ServiceRequest& req) {
    ServiceResponse resp;
    try {
        uint16_t start_addr = 0, count = 1;
        std::istringstream iss(req.payload());
        iss >> start_addr >> count;

        std::vector<bool> result;
        if (readCoils(start_addr, count, result)) {
            std::ostringstream oss;
            oss << "addr=" << start_addr << " count=" << count << " values=";
            for (size_t i = 0; i < result.size(); ++i) {
                if (i > 0) oss << ",";
                oss << (result[i] ? "1" : "0");
            }
            resp.set_success(true);
            resp.set_data(oss.str());
        } else {
            resp.set_success(false);
            resp.set_data("读取失败");
        }
    } catch (...) {
        resp.set_success(false);
        resp.set_data("参数错误");
    }
    return resp;
}

ServiceResponse ModbusNode::handleWriteCoils(const ServiceRequest& req) {
    ServiceResponse resp;
    try {
        uint16_t start_addr = 0;
        std::string values_str;
        std::istringstream iss(req.payload());
        iss >> start_addr >> values_str;

        std::vector<bool> values;
        for (char c : values_str) {
            if (c == '0' || c == '1') {
                values.push_back(c == '1');
            }
        }

        if (writeCoils(start_addr, values)) {
            resp.set_success(true);
            resp.set_data("写入成功");
        } else {
            resp.set_success(false);
            resp.set_data("写入失败");
        }
    } catch (...) {
        resp.set_success(false);
        resp.set_data("参数错误");
    }
    return resp;
}

ServiceResponse ModbusNode::handleReadHolding(const ServiceRequest& req) {
    ServiceResponse resp;
    try {
        uint16_t start_addr = 0, count = 1;
        std::istringstream iss(req.payload());
        iss >> start_addr >> count;

        std::vector<uint16_t> result;
        if (readHoldingRegisters(start_addr, count, result)) {
            std::ostringstream oss;
            oss << "addr=" << start_addr << " count=" << count << " values=";
            for (size_t i = 0; i < result.size(); ++i) {
                if (i > 0) oss << ",";
                oss << result[i];
            }
            resp.set_success(true);
            resp.set_data(oss.str());
        } else {
            resp.set_success(false);
            resp.set_data("读取失败");
        }
    } catch (...) {
        resp.set_success(false);
        resp.set_data("参数错误");
    }
    return resp;
}

ServiceResponse ModbusNode::handleWriteHolding(const ServiceRequest& req) {
    ServiceResponse resp;
    try {
        uint16_t start_addr = 0;
        std::istringstream iss(req.payload());
        iss >> start_addr;

        std::vector<uint16_t> values;
        uint16_t val;
        while (iss >> val) {
            values.push_back(val);
        }

        if (writeHoldingRegisters(start_addr, values)) {
            resp.set_success(true);
            resp.set_data("写入成功");
        } else {
            resp.set_success(false);
            resp.set_data("写入失败");
        }
    } catch (...) {
        resp.set_success(false);
        resp.set_data("参数错误");
    }
    return resp;
}

ServiceResponse ModbusNode::handleGetConfig(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    std::ostringstream oss;
    oss << "type=" << cfg_.type
        << " addr=" << cfg_.addr
        << " port=" << cfg_.port
        << " baud_rate=" << cfg_.baud_rate
        << " data_bits=" << cfg_.data_bits
        << " parity=" << cfg_.parity
        << " stop_bits=" << cfg_.stop_bits
        << " slave_id=" << cfg_.slave_id
        << " interval_ms=" << cfg_.interval_ms;
    resp.set_success(true);
    resp.set_data(oss.str());
    return resp;
}

ServiceResponse ModbusNode::handleGetStatus(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    resp.set_success(true);
    std::ostringstream oss;
    oss << "connected=" << (is_connected_ ? "1" : "0");
    oss << " type=" << (cfg_.type == 0 ? "RTU" : "TCP");
    oss << " addr=" << cfg_.addr;
    oss << " slave_id=" << cfg_.slave_id;
    resp.set_data(oss.str());
    return resp;
}