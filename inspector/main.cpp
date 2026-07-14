#include "inspector_window.h"
#include <QApplication>
#include <iostream>
#include <cstring>

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "用法: system_inspector_gui <pipeline.xml> [--transport zeromq|ros2] [base_port]\n"
                  << "\n"
                  << "示例:\n"
                  << "  system_inspector_gui config/dag_camera_detector_comm.xml\n"
                  << "  system_inspector_gui config/dag_camera_detector_comm.xml --transport ros2\n"
                  << "  system_inspector_gui config/dag_camera_detector_comm.xml 15550\n"
                  << "  system_inspector_gui config/dag_camera_detector_comm.xml --transport ros2 15550\n"
                  << std::endl;
        return 1;
    }

    std::string pipeline_xml = argv[1];
    TransportType transport = TransportType::ZEROMQ;
    uint16_t base_port = 15550;

    // 解析可选参数
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
            std::string t = argv[++i];
            if (t == "ros2") {
                transport = TransportType::ROS2;
            } else if (t == "zeromq" || t == "zmq") {
                transport = TransportType::ZEROMQ;
            } else if (t == "zenoh") {
                transport = TransportType::ZENOH;
            } else {
                std::cerr << "未知传输类型: " << t << " (支持: zeromq, ros2, zenoh)" << std::endl;
                return 1;
            }
        } else {
            base_port = static_cast<uint16_t>(std::stoi(argv[i]));
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName("System Inspector");

    InspectorWindow window(pipeline_xml, transport, base_port);
    window.show();

    return app.exec();
}
