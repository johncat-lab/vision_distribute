#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <atomic>
#include <memory>
#include <sstream>
#include <chrono>
#include <iomanip>

namespace vision {

// 日志级别（移除 trace 和 fatal，添加 off）
enum class LogLevel {
    OFF = 0,     // 关闭所有日志
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4
};

// 日志输出方式
enum class LogOutput {
    CONSOLE = 1,    // 仅控制台
    FILE = 2,       // 仅文件
    BOTH = 3        // 控制台+文件
};

// 日志配置
struct LogConfig {
    LogLevel level = LogLevel::DEBUG;
    LogOutput output = LogOutput::CONSOLE;
    std::string log_file;
    std::string log_dir = "./logs";
    std::string node_name;                     // node 名称，用于创建子目录
    bool enable_subdir_by_node = true;         // 是否根据 node 名称创建子目录
    bool enable_timestamp = true;
    bool enable_thread_id = false;
    size_t max_file_size = 10 * 1024 * 1024;  // 10MB
    int max_files = 5;                        // 最大保留文件数
};

// Logger 类
class Logger {
public:
    ~Logger();
    
    // 初始化日志系统
    static void init(const LogConfig& config);
    
    // 初始化日志系统（带 node 名称）
    static void init(const LogConfig& config, const std::string& node_name);
    
    // 从 XML 文件加载配置
    static bool initFromXml(const std::string& xml_path);
    
    // 获取日志级别名称
    static std::string levelToString(LogLevel level);
    
    // 日志输出函数
    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void warn(const std::string& msg);
    static void error(const std::string& msg);
    
    // 带格式化的日志输出
    template<typename... Args>
    static void debug(const char* format, Args&&... args) {
        log(LogLevel::DEBUG, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void info(const char* format, Args&&... args) {
        log(LogLevel::INFO, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void warn(const char* format, Args&&... args) {
        log(LogLevel::WARN, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void error(const char* format, Args&&... args) {
        log(LogLevel::ERROR, format, std::forward<Args>(args)...);
    }
    
    // 设置日志级别
    static void setLevel(LogLevel level);
    
    // 设置输出方式
    static void setOutput(LogOutput output);
    
    // 设置 node 名称（用于创建子目录）
    static void setNodeName(const std::string& node_name);
    
private:
    Logger() = delete;
    
    static void log(LogLevel level, const std::string& msg);
    
    static void log(LogLevel level, const char* format) {
        log(level, std::string(format));
    }
    
    template<typename... Args>
    static void log(LogLevel level, const char* format, Args&&... args) {
        char buffer[4096];
        snprintf(buffer, sizeof(buffer), format, std::forward<Args>(args)...);
        log(level, std::string(buffer));
    }
    
    static void writeToConsole(const std::string& msg, LogLevel level);
    static void writeToFile(const std::string& msg);
    static std::string getCurrentTime();
    static void rotateLogFile();
    
    // 静态成员
    static std::unique_ptr<LogConfig> config_;
    static std::ofstream log_file_;
    static std::mutex mutex_;
    static std::atomic<bool> initialized_;
};

} // namespace vision

// 便捷宏定义
#define LOG_DEBUG(...)    vision::Logger::debug(__VA_ARGS__)
#define LOG_INFO(...)     vision::Logger::info(__VA_ARGS__)
#define LOG_WARN(...)     vision::Logger::warn(__VA_ARGS__)
#define LOG_ERROR(...)    vision::Logger::error(__VA_ARGS__)