#include <opencv2/opencv.hpp>
#include "logger/logger.h"

int main() {
    // 写入测试配置
    cv::FileStorage fs("test_comm.xml", cv::FileStorage::WRITE);
    fs << "mode" << "server";
    fs << "host" << "192.168.1.211";
    fs << "port" << 7930;
    fs << "server_mode" << 2;
    fs << "interval_ms" << 100;
    fs.release();
    LOG_INFO("已生成 test_comm.xml");
    
    // 读取测试配置
    cv::FileStorage fs2("test_comm.xml", cv::FileStorage::READ);
    if (!fs2.isOpened()) {
        LOG_ERROR("无法打开文件");
        return 1;
    }
    std::string mode = (std::string)fs2["mode"];
    std::string host = (std::string)fs2["host"];
    int port = (int)fs2["port"];
    LOG_INFO("mode: %s", mode.c_str());
    LOG_INFO("host: %s", host.c_str());
    LOG_INFO("port: %d", port);
    fs2.release();
    return 0;
}
