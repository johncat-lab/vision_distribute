#include "rpc/node_factory.h"
#include "rpc/config_loader.h"
#include "rpc/message_types.h"
#include "rpc/node_manifest.h"
#include "rpc/edge_manager.h"
#include "logger/logger.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// ========== 全局运行标志 ==========
static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

// ========== 图像发布器配置 ==========
struct ImagePublisherConfig {
    std::string image_dir;           // 图像目录路径
    std::vector<std::string> image_files;  // 指定图像文件列表（优先使用）
    float fps = 10.0f;               // 发布帧率
    bool loop = true;                // 是否循环发布
    int camera_id = 0;               // 模拟的相机 ID
    std::string pixel_format = "Mono8";  // 像素格式（仅用于 pixel_type 字段）
};

// ========== 支持的图像扩展名 ==========
static bool isImageFile(const std::string& filename) {
    static const std::vector<std::string> exts = {
        ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".pgm", ".ppm"
    };
    std::string ext = fs::path(filename).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

// ========== 从目录扫描图像文件 ==========
static std::vector<std::string> scanImageDir(const std::string& dir_path) {
    std::vector<std::string> files;
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        LOG_ERROR("[ImagePublisher] 目录不存在: %s", dir_path.c_str());
        return files;
    }

    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (entry.is_regular_file() && isImageFile(entry.path().string())) {
            files.push_back(entry.path().string());
        }
    }

    std::sort(files.begin(), files.end());
    LOG_INFO("[ImagePublisher] 从目录加载 %zu 个图像文件: %s", files.size(), dir_path.c_str());
    return files;
}

// ========== 从 image_publisher_config.xml 加载配置 ==========
static ImagePublisherConfig loadPublisherConfig(const std::string& path) {
    ImagePublisherConfig cfg;
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        LOG_WARN("[ImagePublisher] 无法打开配置文件: %s，使用默认配置", path.c_str());
        return cfg;
    }

    if (!fs["image_dir"].empty()) {
        fs["image_dir"] >> cfg.image_dir;
        LOG_INFO("[ImagePublisher] image_dir: %s", cfg.image_dir.c_str());
    }
    
    if (!fs["fps"].empty()) {
        fs["fps"] >> cfg.fps;
    }
    
    if (!fs["loop"].empty()) {
        fs["loop"] >> cfg.loop;
    }
    
    if (!fs["camera_id"].empty()) {
        fs["camera_id"] >> cfg.camera_id;
    }
    
    if (!fs["pixel_format"].empty()) {
        fs["pixel_format"] >> cfg.pixel_format;
    }

    cv::FileNode file_list_node = fs["image_files"];
    if (file_list_node.type() == cv::FileNode::SEQ) {
        LOG_INFO("[ImagePublisher] 找到 image_files 序列");
        for (const auto& node : file_list_node) {
            std::string file_path;
            node >> file_path;
            LOG_INFO("[ImagePublisher] 文件: '%s'", file_path.c_str());
            if (!file_path.empty()) {
                cfg.image_files.push_back(file_path);
            }
        }
        LOG_INFO("[ImagePublisher] 共加载 %zu 个文件", cfg.image_files.size());
    } else if (!file_list_node.empty()) {
        LOG_INFO("[ImagePublisher] image_files 不是序列类型, type=%d", file_list_node.type());
    } else {
        LOG_INFO("[ImagePublisher] image_files 为空");
    }

    fs.release();
    return cfg;
}

// ========== 像素格式字符串转整型（用于 FrameMsg::pixel_type）==========
static uint32_t pixelFormatFromString(const std::string& fmt) {
    // 常用像素格式映射（与 HikCamera MvGvspPixelType 部分兼容）
    if (fmt == "Mono8")       return 0x01080001;
    if (fmt == "Mono10")      return 0x01100003;
    if (fmt == "Mono12")      return 0x01100005;
    if (fmt == "BayerRG8")    return 0x01080009;
    if (fmt == "BayerGB8")    return 0x0108000A;
    if (fmt == "RGB8")        return 0x02180014;
    if (fmt == "BGR8")        return 0x02180015;
    // 默认 Mono8
    return 0x01080001;
}

// ========== 构建 manifest ==========
static NodeManifest buildManifest() {
    NodeManifest m;
    m.name = "image_publisher_node";
    m.binary = "image_publisher_node";
    m.version = "1.0";
    m.config_file = "image_publisher_config.xml";
    m.outputs.push_back({"frame_output", "FrameMsg", "从图像文件发布的图像帧"});
    return m;
}

// ========== 主函数 ==========
int main(int argc, char* argv[]) {
    std::string config_path;
    std::string system_config_path;
    std::string pub_config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--describe") {
            std::cout << buildManifest().toJson() << std::endl;
            return 0;
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            pub_config_path = config_path;
        } else if (arg == "--system-config" && i + 1 < argc) {
            system_config_path = argv[++i];
        } else if (arg == "--pub-config" && i + 1 < argc) {
            pub_config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "用法: " << argv[0]
                      << " --config <image_publisher_config.xml>" << std::endl;
            return 0;
        }
    }

    if (pub_config_path.empty()) {
        LOG_ERROR("[ImagePublisher] 未指定配置文件");
        std::cerr << "用法: " << argv[0]
                  << " --config <image_publisher_config.xml>" << std::endl;
        return 1;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    NodeConfig node_config;
    try {
        if (!system_config_path.empty()) {
            node_config = ConfigLoader::loadSystemConfig(system_config_path);
        } else {
            node_config.transport = TransportType::ZEROMQ;
            LOG_INFO("[ImagePublisher] 未指定系统配置，使用默认 ZeroMQ");
        }
        node_config.node_name = "image_publisher_node";
    } catch (const std::exception& e) {
        LOG_ERROR("[ImagePublisher] 加载系统配置失败: %s，使用默认配置", e.what());
        node_config.transport = TransportType::ZEROMQ;
        node_config.node_name = "image_publisher_node";
    }

    std::string transport_name;
    switch (node_config.transport) {
    case TransportType::ZEROMQ: transport_name = "ZeroMQ"; break;
    case TransportType::ZENOH:  transport_name = "Zenoh";  break;
    case TransportType::ROS2:   transport_name = "ROS2";   break;
    }
    LOG_INFO("[ImagePublisher] 传输方式: %s", transport_name.c_str());

    ImagePublisherConfig pub_cfg = loadPublisherConfig(pub_config_path);
    LOG_INFO("[ImagePublisher] 发布器配置已加载: %s", pub_config_path.c_str());

    // ---- 收集图像文件 ----
    std::vector<std::string> image_paths;
    if (!pub_cfg.image_files.empty()) {
        // 优先使用文件列表
        image_paths = pub_cfg.image_files;
        LOG_INFO("[ImagePublisher] 使用指定文件列表，共 %zu 个文件", image_paths.size());
    } else if (!pub_cfg.image_dir.empty()) {
        image_paths = scanImageDir(pub_cfg.image_dir);
    } else {
        LOG_ERROR("[ImagePublisher] 未配置 image_dir 或 image_files，退出");
        return 1;
    }

    if (image_paths.empty()) {
        LOG_ERROR("[ImagePublisher] 未找到任何图像文件，退出");
        return 1;
    }

    LOG_INFO("[ImagePublisher] 共加载 %zu 张图像，发布帧率: %.1f fps，循环: %s",
             image_paths.size(), pub_cfg.fps, pub_cfg.loop ? "是" : "否");

    // ---- 创建 NodeFactory + EdgeManager ----
    NodeFactory factory(node_config);
    NodeEdgeManager edges(factory, "image_publisher_node");
    edges.setDefaultTopic("frame_output", "vision/frame");
    edges.parseArgs(argc, argv);

    auto frame_pub = edges.publish<FrameMsg>("frame_output", "vision/frame");
    LOG_INFO("[ImagePublisher] 帧发布者已创建，topic: %s", frame_pub->getTopic().c_str());

    // ---- 开始发布 ----
    uint32_t frame_num = 0;
    size_t image_idx = 0;
    uint32_t pixel_type = pixelFormatFromString(pub_cfg.pixel_format);

    auto frame_interval = std::chrono::microseconds(
        static_cast<int64_t>(1000000.0f / pub_cfg.fps));

    LOG_INFO("[ImagePublisher] 开始发布图像帧...");

    while (g_running) {
        if (image_idx >= image_paths.size()) {
            if (pub_cfg.loop) {
                image_idx = 0;
                LOG_INFO("[ImagePublisher] 循环重新开始，共 %zu 张图像", image_paths.size());
            } else {
                LOG_INFO("[ImagePublisher] 所有图像已发布完毕，退出");
                break;
            }
        }

        const std::string& img_path = image_paths[image_idx];

        // 使用 OpenCV 读取图像（以灰度模式，保持与 Mono8 一致）
        cv::Mat img = cv::imread(img_path, cv::IMREAD_UNCHANGED);
        if (img.empty()) {
            LOG_WARN("[ImagePublisher] 无法读取图像，跳过: %s", img_path.c_str());
            ++image_idx;
            continue;
        }

        // 如果是多通道图像，转换为单通道（Mono8）
        cv::Mat gray;
        bool converted_to_gray = false;
        if (img.channels() == 3 || img.channels() == 4) {
            cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
            converted_to_gray = true;
        } else {
            gray = img;
        }

        // 确保数据类型为 CV_8U
        if (gray.type() != CV_8UC1) {
            gray.convertTo(gray, CV_8UC1);
        }

        // 构造 FrameMsg
        FrameMsg msg;
        msg.set_camera_id(pub_cfg.camera_id);
        msg.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        msg.set_width(static_cast<uint16_t>(gray.cols));
        msg.set_height(static_cast<uint16_t>(gray.rows));
        // 发送灰度图时，强制使用 Mono8 像素格式
        msg.set_pixel_type(0x01080001);
        msg.set_frame_num(frame_num++);
        msg.set_exposure_time(0.0f);
        msg.set_gain(0.0f);

        // 压缩像素数据为 PNG（大幅减小消息体积，避免 DDS UDP 大消息丢失）
        std::vector<uint8_t> compressed;
        cv::imencode(".png", gray, compressed);
        std::string data_str(compressed.begin(), compressed.end());
        msg.set_pixel_type(0);  // pixel_type=0 表示 PNG 压缩数据
        msg.set_data(data_str);

        frame_pub->publish(msg);

        LOG_DEBUG("[ImagePublisher] 发布帧 #%u: %s (%ux%u, raw=%zu, compressed=%zu bytes)",
                  msg.frame_num(), fs::path(img_path).filename().c_str(),
                  msg.width(), msg.height(),
                  static_cast<size_t>(gray.rows) * gray.cols,
                  msg.data().size());

        ++image_idx;

        // 等待下一帧时间
        std::this_thread::sleep_for(frame_interval);
    }

    LOG_INFO("[ImagePublisher] 已停止，共发布 %u 帧", frame_num);
    return 0;
}
