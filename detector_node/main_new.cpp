// detector_node: 使用 NodeContainer 框架的新入口
#include "detector_node.h"
#include "rpc/node_container.h"

int main(int argc, char* argv[]) {
    NodeContainer container(std::make_unique<DetectorNode>());
    return container.run(argc, argv);
}
