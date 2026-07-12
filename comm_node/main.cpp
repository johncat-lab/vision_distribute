// comm_node: 使用 NodeContainer 框架的新入口
#include "comm_node.h"
#include "rpc/node_container.h"

int main(int argc, char* argv[]) {
    NodeContainer container(std::make_unique<CommNode>());
    return container.run(argc, argv);
}
