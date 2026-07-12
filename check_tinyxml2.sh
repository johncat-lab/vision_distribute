#!/bin/bash
# tinyxml2环境诊断脚本

echo "=========================================="
echo "  tinyxml2 环境诊断"
echo "=========================================="
echo ""

# 1. 检查conda环境
echo "[1] 检查conda环境..."
CONDA_ENV="vision_x86"
if conda env list | grep -q "$CONDA_ENV"; then
    echo "   ✅ Conda环境 '$CONDA_ENV' 存在"
    CONDA_PREFIX_PATH=$(conda run -n "$CONDA_ENV" bash -c 'echo $CONDA_PREFIX' 2>/dev/null)
    echo "   路径: $CONDA_PREFIX_PATH"
else
    echo "   ❌ Conda环境 '$CONDA_ENV' 不存在"
    echo "   安装命令: conda create -n vision_x86 python=3.10"
    exit 1
fi

# 2. 检查tinyxml2包
echo ""
echo "[2] 检查tinyxml2包..."
if conda run -n "$CONDA_ENV" conda list tinyxml2 2>/dev/null | grep -q tinyxml2; then
    echo "   ✅ tinyxml2已安装"
    conda run -n "$CONDA_ENV" conda list tinyxml2 2>/dev/null | grep tinyxml2
else
    echo "   ❌ tinyxml2未安装"
    echo "   安装命令: conda install -n $CONDA_ENV -c conda-forge tinyxml2"
    exit 1
fi

# 3. 检查头文件和库文件
echo ""
echo "[3] 检查文件..."
if [[ -f "$CONDA_PREFIX_PATH/include/tinyxml2.h" ]]; then
    echo "   ✅ 头文件: $CONDA_PREFIX_PATH/include/tinyxml2.h"
else
    echo "   ❌ 头文件缺失"
    exit 1
fi

if [[ -f "$CONDA_PREFIX_PATH/lib/libtinyxml2.dylib" ]]; then
    echo "   ✅ 库文件: $CONDA_PREFIX_PATH/lib/libtinyxml2.dylib"
else
    echo "   ❌ 库文件缺失"
    exit 1
fi

# 4. 检查CMake缓存
echo ""
echo "[4] 检查CMake配置..."
if [[ -f "build/CMakeCache.txt" ]]; then
    CMAKE_PREFIX=$(grep "CMAKE_PREFIX_PATH" build/CMakeCache.txt | cut -d= -f2)
    echo "   CMAKE_PREFIX_PATH: $CMAKE_PREFIX"
    
    if [[ "$CMAKE_PREFIX" == "$CONDA_PREFIX_PATH" ]]; then
        echo "   ✅ CMake路径正确"
    else
        echo "   ⚠️  CMake路径不匹配!"
        echo "   期望: $CONDA_PREFIX_PATH"
        echo "   实际: $CMAKE_PREFIX"
        echo ""
        echo "   解决方案: ./build.sh --clean"
    fi
    
    if grep -q "TINYXML2_LIBRARY" build/CMakeCache.txt; then
        TINYXML2_LIB=$(grep "TINYXML2_LIBRARY" build/CMakeCache.txt | cut -d= -f2)
        echo "   ✅ tinyxml2库已找到: $TINYXML2_LIB"
    else
        echo "   ❌ CMake未找到tinyxml2"
    fi
else
    echo "   ⚠️  CMakeCache不存在,还未配置构建"
fi

# 5. 检查编译产物
echo ""
echo "[5] 检查编译产物..."
if [[ -f "build/install/bins/dag_launcher/dag_launcher" ]]; then
    echo "   ✅ dag_launcher已编译"
    echo "   tinyxml2依赖:"
    otool -L build/install/bins/dag_launcher/dag_launcher | grep tinyxml2 | sed 's/^/      /'
else
    echo "   ⚠️  dag_launcher未编译"
fi

echo ""
echo "=========================================="
echo "  诊断完成"
echo "=========================================="
echo ""
echo "快速修复命令:"
echo "  ./build.sh --clean          # 清理并重新配置"
echo "  ./build.sh dag-launcher     # 仅编译dag_launcher"
echo ""
