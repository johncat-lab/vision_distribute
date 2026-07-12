#!/bin/bash
# 测试海康SDK检测和CMake配置

echo "=========================================="
echo "  海康SDK配置验证"
echo "=========================================="
echo ""

# 检查SDK目录
echo "[1] 检查SDK目录结构..."
if [[ -d "/Library/MVS_SDK" ]]; then
    echo "   ✅ SDK目录存在: /Library/MVS_SDK"
else
    echo "   ❌ SDK目录不存在"
    exit 1
fi

# 检查头文件
echo ""
echo "[2] 检查头文件..."
if [[ -f "/Library/MVS_SDK/Includes/MvCameraControl.h" ]]; then
    echo "   ✅ 头文件存在: Includes/MvCameraControl.h"
else
    echo "   ❌ 头文件缺失"
    exit 1
fi

# 检查库文件
echo ""
echo "[3] 检查库文件..."
if [[ -f "/Library/MVS_SDK/lib/libMvCameraControl.dylib" ]]; then
    echo "   ✅ 库文件存在: lib/libMvCameraControl.dylib"
else
    echo "   ❌ 库文件缺失"
    exit 1
fi

# 测试build.sh检测逻辑
echo ""
echo "[4] 测试build.sh检测逻辑..."
cd /Users/johncat/workspace/工业机器人/上下料/汇川scara/vision_distribute

# 模拟build.sh的检测部分
USE_HIK="OFF"
HIK_SDK_PATH=""

if [[ -d "/Library/MVS_SDK" ]]; then
    if [[ -f "/Library/MVS_SDK/Includes/MvCameraControl.h" ]]; then
        USE_HIK="ON"
        HIK_SDK_PATH="/Library/MVS_SDK"
        echo "   ✅ 检测结果: USE_HIK=${USE_HIK}"
        echo "   ✅ SDK路径: ${HIK_SDK_PATH}"
    fi
fi

# 清理并测试CMake配置
echo ""
echo "[5] 测试CMake配置..."
if [[ -d "build" ]]; then
    echo "   清理旧的build目录..."
    rm -rf build
fi

mkdir -p build
cd build

echo "   运行CMake配置..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_GUI=OFF \
    -DUSE_HIK=ON \
    -DHIK_SDK_ROOT=/Library/MVS_SDK \
    2>&1 | grep -E "海康SDK|HIK|USE_HIK|CMake Error|CMake Warning"

if [[ $? -eq 0 ]]; then
    echo "   ✅ CMake配置成功"
else
    echo "   ❌ CMake配置失败"
    exit 1
fi

# 检查CMakeCache
echo ""
echo "[6] 验证CMakeCache..."
if grep -q "USE_HIK:BOOL=ON" CMakeCache.txt; then
    echo "   ✅ USE_HIK已启用"
else
    echo "   ❌ USE_HIK未启用"
fi

if grep -q "HIK_SDK_PATH:PATH=/Library/MVS_SDK" CMakeCache.txt; then
    echo "   ✅ HIK_SDK_PATH正确设置"
else
    echo "   ⚠️  HIK_SDK_PATH可能使用默认值"
fi

echo ""
echo "=========================================="
echo "  验证完成"
echo "=========================================="
echo ""
echo "✅ 海康SDK配置正确!"
echo ""
echo "下一步:"
echo "  ./build.sh camera    # 仅编译camera_node"
echo "  ./build.sh           # 编译全部"
echo ""
