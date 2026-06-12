#include "logger/logger.h"
#include <opencv2/core.hpp>
#include <filesystem>
#include <thread>
#include <cstdio>
#include <iostream>

namespace vision {

// 静态成员初始化
std::unique_ptr<LogConfig> Logger::config_;
std::ofstream Logger::log_file_;
std::mutex Logger::mutex_;
std::atomic<bool> Logger::initialized_{false};

Logger::~Logger() {
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::OFF:   return "OFF";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        default:              return "UNKNOWN";
    }
}

std::string Logger::getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

void Logger::rotateLogFile() {
    if (!config_ || config_->log_file.empty()) return;
    
    log_file_.close();
    
    std::filesystem::path log_path(config_->log_file);
    std::string base_name = log_path.stem().string();
    std::string extension = log_path.extension().string();
    std::string dir = log_path.parent_path().string();
    
    // 删除最旧的文件
    std::string oldest_file = dir + "/" + base_name + "_" + 
                              std::to_string(config_->max_files) + extension;
    std::filesystem::remove(oldest_file);
    
    // 重命名其他文件
    for (int i = config_->max_files - 1; i > 0; --i) {
        std::string old_name = dir + "/" + base_name + "_" + std::to_string(i) + extension;
        std::string new_name = dir + "/" + base_name + "_" + std::to_string(i + 1) + extension;
        if (std::filesystem::exists(old_name)) {
            std::filesystem::rename(old_name, new_name);
        }
    }
    
    // 重命名当前日志文件
    std::string backup_name = dir + "/" + base_name + "_1" + extension;
    std::filesystem::rename(config_->log_file, backup_name);
    
    // 重新打开新文件
    log_file_.open(config_->log_file, std::ios::out | std::ios::app);
}

void Logger::writeToConsole(const std::string& msg, LogLevel level) {
    // 根据日志级别设置颜色
    const char* color_code = "";
    const char* reset_code = "\033[0m";
    
    switch (level) {
        case LogLevel::DEBUG: color_code = "\033[36m"; break;  // 青色
        case LogLevel::INFO:  color_code = "\033[32m"; break;  // 绿色
        case LogLevel::WARN:  color_code = "\033[33m"; break;  // 黄色
        case LogLevel::ERROR: color_code = "\033[31m"; break;  // 红色
        default: break;
    }
    
    std::cout << color_code << msg << reset_code << std::endl;
}

void Logger::writeToFile(const std::string& msg) {
    if (!log_file_.is_open()) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查文件大小是否需要滚动
    log_file_.seekp(0, std::ios::end);
    if (log_file_.tellp() > static_cast<std::streampos>(config_->max_file_size)) {
        rotateLogFile();
    }
    
    log_file_ << msg << std::endl;
    log_file_.flush();
}

void Logger::init(const LogConfig& config) {
    init(config, "");
}

void Logger::init(const LogConfig& config, const std::string& node_name) {
    std::string log_file_path;
    LogLevel log_level = config.level;
    LogOutput log_output = config.output;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (initialized_) {
            // 如果已初始化，关闭旧文件
            if (log_file_.is_open()) {
                log_file_.close();
            }
        }
        
        config_ = std::make_unique<LogConfig>(config);
        
        // 确定实际使用的 node 名称（优先使用参数，其次使用配置）
        std::string effective_node_name = node_name.empty() ? config.node_name : node_name;
        
        // 构建日志目录路径
        std::string actual_log_dir = config.log_dir;
        if (config.enable_subdir_by_node && !effective_node_name.empty()) {
            // 如果启用按 node 名称创建子目录
            if (actual_log_dir.empty()) {
                actual_log_dir = "./logs/" + effective_node_name;
            } else {
                // 移除末尾的斜杠（如果有）
                if (actual_log_dir.back() == '/') {
                    actual_log_dir.pop_back();
                }
                actual_log_dir = actual_log_dir + "/" + effective_node_name;
            }
        }
        
        // 创建日志目录
        if (!actual_log_dir.empty()) {
            std::filesystem::create_directories(actual_log_dir);
        }
        
        // 如果需要文件输出，打开日志文件
        if (config.output == LogOutput::FILE || config.output == LogOutput::BOTH) {
            if (config.log_file.empty()) {
                // 生成默认日志文件名
                auto now = std::chrono::system_clock::now();
                auto time = std::chrono::system_clock::to_time_t(now);
                char filename[64];
                std::strftime(filename, sizeof(filename), "%Y%m%d_%H%M%S.log", std::localtime(&time));
                
                // 如果有 node 名称，在文件名中加入
                if (!effective_node_name.empty()) {
                    std::string name_with_node = effective_node_name + "_" + filename;
                    config_->log_file = actual_log_dir + "/" + name_with_node;
                } else {
                    config_->log_file = actual_log_dir + "/" + filename;
                }
            } else {
                // 用户指定了完整路径
                config_->log_file = actual_log_dir + "/" + config.log_file;
            }
            
            log_file_.open(config_->log_file, std::ios::out | std::ios::app);
            if (!log_file_.is_open()) {
                std::cerr << "[Logger] 无法打开日志文件: " << config_->log_file << std::endl;
                config_->output = LogOutput::CONSOLE;
                log_output = LogOutput::CONSOLE;
            }
        }
        
        log_file_path = config_->log_file;
        initialized_ = true;
    }
    
    // 在锁外输出初始化信息，避免死锁
    if (log_level != LogLevel::OFF) {
        LOG_INFO("Logger initialized. Level: %s, Output: %s, Log file: %s", 
                 levelToString(log_level).c_str(), 
                 (log_output == LogOutput::CONSOLE ? "console" : 
                  log_output == LogOutput::FILE ? "file" : "both"),
                 log_file_path.empty() ? "(none)" : log_file_path.c_str());
    }
}

bool Logger::initFromXml(const std::string& xml_path) {
    cv::FileStorage fs(xml_path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        std::cerr << "[Logger] 无法打开配置文件: " << xml_path << std::endl;
        return false;
    }
    
    LogConfig config;
    
    // 日志级别（支持 off, debug, info, warn, error）
    if (!fs["log_level"].empty()) {
        std::string level_str = (std::string)fs["log_level"];
        if (level_str == "off") config.level = LogLevel::OFF;
        else if (level_str == "debug") config.level = LogLevel::DEBUG;
        else if (level_str == "info") config.level = LogLevel::INFO;
        else if (level_str == "warn") config.level = LogLevel::WARN;
        else if (level_str == "error") config.level = LogLevel::ERROR;
    }
    
    // 输出方式
    if (!fs["log_output"].empty()) {
        std::string output_str = (std::string)fs["log_output"];
        if (output_str == "console") config.output = LogOutput::CONSOLE;
        else if (output_str == "file") config.output = LogOutput::FILE;
        else if (output_str == "both") config.output = LogOutput::BOTH;
    }
    
    // 日志目录
    if (!fs["log_dir"].empty()) {
        config.log_dir = (std::string)fs["log_dir"];
    }
    
    // 日志文件名
    if (!fs["log_file"].empty()) {
        config.log_file = (std::string)fs["log_file"];
    }
    
    // 是否根据 node 名称创建子目录
    if (!fs["log_subdir_by_node"].empty()) {
        config.enable_subdir_by_node = (int)fs["log_subdir_by_node"] != 0;
    }
    
    // 是否启用时间戳
    if (!fs["enable_timestamp"].empty()) {
        config.enable_timestamp = (int)fs["enable_timestamp"] != 0;
    }
    
    // 是否启用线程ID
    if (!fs["enable_thread_id"].empty()) {
        config.enable_thread_id = (int)fs["enable_thread_id"] != 0;
    }
    
    // 最大文件大小（MB）
    if (!fs["max_file_size_mb"].empty()) {
        config.max_file_size = (size_t)(int)fs["max_file_size_mb"] * 1024 * 1024;
    }
    
    // 最大文件数
    if (!fs["max_files"].empty()) {
        config.max_files = (int)fs["max_files"];
    }
    
    fs.release();
    init(config);
    return true;
}

void Logger::log(LogLevel level, const std::string& msg) {
    if (!initialized_) {
        // 如果未初始化，使用默认配置
        LogConfig default_config;
        init(default_config);
    }
    
    // 如果日志级别为 OFF，不输出任何内容
    if (config_->level == LogLevel::OFF) {
        return;
    }
    
    // 如果当前级别低于配置的级别，不输出
    if (level < config_->level) {
        return;
    }
    
    std::stringstream ss;
    
    // 时间戳
    if (config_->enable_timestamp) {
        ss << "[" << getCurrentTime() << "] ";
    }
    
    // 日志级别
    ss << "[" << levelToString(level) << "] ";
    
    // 线程ID
    if (config_->enable_thread_id) {
        ss << "[T" << std::this_thread::get_id() << "] ";
    }
    
    // 日志消息
    ss << msg;
    
    std::string formatted_msg = ss.str();
    
    // 输出到控制台
    if (config_->output == LogOutput::CONSOLE || config_->output == LogOutput::BOTH) {
        writeToConsole(formatted_msg, level);
    }
    
    // 输出到文件
    if (config_->output == LogOutput::FILE || config_->output == LogOutput::BOTH) {
        writeToFile(formatted_msg);
    }
}

void Logger::setLevel(LogLevel level) {
    if (config_) {
        LogLevel old_level = config_->level;
        config_->level = level;
        
        // 如果新级别不是 OFF，输出级别变更信息
        if (level != LogLevel::OFF) {
            LOG_INFO("Log level changed from: %s to: %s", 
                     levelToString(old_level).c_str(),
                     levelToString(level).c_str());
        }
    }
}

void Logger::setOutput(LogOutput output) {
    if (config_) {
        config_->output = output;
        if (config_->level != LogLevel::OFF) {
            LOG_INFO("Log output changed to: %d", static_cast<int>(output));
        }
    }
}

void Logger::setNodeName(const std::string& node_name) {
    if (config_) {
        config_->node_name = node_name;
    }
}

// 便捷函数实现
void Logger::debug(const std::string& msg) { log(LogLevel::DEBUG, msg); }
void Logger::info(const std::string& msg) { log(LogLevel::INFO, msg); }
void Logger::warn(const std::string& msg) { log(LogLevel::WARN, msg); }
void Logger::error(const std::string& msg) { log(LogLevel::ERROR, msg); }

} // namespace vision