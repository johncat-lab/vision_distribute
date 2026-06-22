// ============================================================================
// camera_node: 使用 NodeContainer 统一框架的新入口
// ----------------------------------------------------------------------------
// 这是一个简化的入口文件，展示如何使用 NodeBase + NodeContainer 模式：
//   1. 继承 NodeBase，实现 describe/initDataflow/initServices/start/stop/tick
//   2. 在 main() 中创建 NodeContainer，一行 run() 完成所有生命周期
// ============================================================================

#include "camera_node.h"
#include "rpc/node_container.h"
#include <iostream>
#include <csignal>
#include <atomic>

int main(int argc, char* argv[]) {
    std::cout << "[camera_node] 使用 NodeContainer 框架启动" << std::endl;

    NodeContainer container(std::make_unique<CameraNode>());
    return container.run(argc, argv);
}
