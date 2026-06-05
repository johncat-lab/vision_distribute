#include "main_window.h"

#include <QApplication>
#include <iostream>
#include <string>

static void printUsage(const char* prog) {
    std::cout << "用法: " << prog << " --config <system_config.xml>" << std::endl;
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
        std::cerr << "错误: 未指定系统配置文件" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    QApplication app(argc, argv);

    MainWindow window(config_path);
    window.show();

    return app.exec();
}
