#include "inspector_window.h"
#include <QApplication>
#include <iostream>

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "用法: system_inspector_gui <pipeline.xml> [base_port]\n"
                  << "\n"
                  << "示例:\n"
                  << "  system_inspector_gui config/dag_camera_detector_comm.xml\n"
                  << "  system_inspector_gui config/dag_camera_detector_comm.xml 15550\n"
                  << std::endl;
        return 1;
    }

    std::string pipeline_xml = argv[1];
    uint16_t base_port = 15550;
    if (argc >= 3) {
        base_port = static_cast<uint16_t>(std::stoi(argv[2]));
    }

    QApplication app(argc, argv);
    app.setApplicationName("System Inspector");

    InspectorWindow window(pipeline_xml);
    window.show();

    return app.exec();
}
