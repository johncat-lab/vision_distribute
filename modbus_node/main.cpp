#include "modbus_node.h"
#include "rpc/node_container.h"

int main(int argc, char* argv[]) {
    auto container = std::make_unique<NodeContainer>(std::make_unique<ModbusNode>());
    return container->run(argc, argv);
}