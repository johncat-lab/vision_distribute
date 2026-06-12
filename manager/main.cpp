#include "main_window.h"
#include "logger/logger.h"

#include <QApplication>
#include <string>

static void printUsage(const char* prog) {
    LOG_INFO("用法: %s --config <system_config.xml>", prog);
}

int main(int argc, char* argv[])
{
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

    MainWindow window(config_path);
    window.show();

    return app.exec();
}
