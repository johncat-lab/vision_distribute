#include "main_window.h"
#include "rpc/node_manifest.h"
#include "logger/logger.h"

#include <QApplication>
#include <string>
#include <iostream>

// ========== 构建 manifest ==========
static NodeManifest buildManifest() {
    NodeManifest m;
    m.name = "manager";
    m.binary = "manager";
    m.version = "1.0";
    m.config_file = "system_config.xml";
    m.inputs.push_back({"frame_input",      "FrameMsg",      "相机图像帧"});
    m.inputs.push_back({"detection_input",  "DetectionMsg",  "检测结果"});
    m.inputs.push_back({"annotation_input", "AnnotationMsg", "标注信息"});
    m.requires_services = {"camera", "detector", "comm"};
    return m;
}

static void printUsage(const char* prog) {
    LOG_INFO("用法: %s --config <system_config.xml>", prog);
}

int main(int argc, char* argv[])
{
    // 先检查 --describe（在 QApplication 之前）
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--describe") {
            std::cout << buildManifest().toJson() << std::endl;
            return 0;
        }
    }

    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (config_path.empty()) {
        LOG_ERROR("错误: 未指定系统配置文件");
        printUsage(argv[0]);
        return 1;
    }

    QApplication app(argc, argv);

    MainWindow window(config_path, argc, argv);
    window.show();

    return app.exec();
}
