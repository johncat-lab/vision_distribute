#pragma once
#include "rpc/types.h"
#include <string>

class ConfigLoader {
public:
    static NodeConfig loadSystemConfig(const std::string& xml_path);
};