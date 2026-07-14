// 兼容头文件：将 <modbus.h> 重定向到 <modbus/modbus.h>
// 某些系统（如 Ubuntu）将 libmodbus 头文件安装在 modbus/ 子目录下
#pragma once
#include <modbus/modbus.h>
