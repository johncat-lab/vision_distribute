#!/usr/bin/env bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
INSTALL_DIR="${BUILD_DIR}/install"

# ========== 默认参数 ==========
BUILD_TYPE="Release"
BUILD_GUI="OFF"
TARGET=""
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN_FIRST="false"
ROS2_SETUP="/opt/ros/humble/setup.bash"

# ========== 帮助信息 ==========
usage() {
    cat << EOF
用法: $0 [选项] [目标]

选项:
    --release               Release 构建 (默认)
    --debug                 Debug 构建
    --gui                   启用 GUI (manager) 构建
    --clean                 构建前清理 build 目录
    -j<N>                   并行编译线程数 (默认: ${JOBS})

目标 (可选，不指定则构建全部):
    all                     构建全部可执行文件
    camera                  仅构建 camera_node
    detector                仅构建 detector_node
    comm                    仅构建 comm_node
    manager                 仅构建 manager (需 --gui)
    rpc                     仅构建 vision_rpc 库
    core                    仅构建 vision_core 库
    clean                   清理 build 目录并退出

示例:
    $0                                  # 构建全部 (Release, 无GUI)
    $0 camera                           # 仅构建 camera_node
    $0 --debug --gui manager            # Debug模式构建 manager
    $0 --clean --gui                    # 清理后构建全部 (含GUI)
    $0 clean                            # 仅清理
    $0 -j8 detector                     # 8线程构建 detector_node
EOF
}

# ========== 错误处理 ==========
die() {
    echo ""
    echo -e "✗ 错误: $*" >&2
    exit 1
}

# ========== 参数解析 ==========
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)  BUILD_TYPE="Release"; shift ;;
        --debug)    BUILD_TYPE="Debug";   shift ;;
        --gui)      BUILD_GUI="ON";       shift ;;
        --clean)    CLEAN_FIRST="true";   shift ;;
        -j*)        JOBS="${1#-j}";       shift ;;
        -h|--help)  usage; exit 0 ;;
        clean)      TARGET="clean";       shift ;;
        all|camera|detector|comm|manager|rpc|core)
                    TARGET="$1";          shift ;;
        *)          die "未知参数 '$1'，使用 --help 查看帮助" ;;
    esac
done

# ========== clean 命令 ==========
if [[ "$TARGET" == "clean" ]]; then
    echo "清理 build 目录: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
    echo "清理完成。"
    exit 0
fi

# ========== 清理 ==========
if [[ "$CLEAN_FIRST" == "true" ]]; then
    echo "清理 build 目录..."
    rm -rf "${BUILD_DIR}"
fi

# ========== ROS2 环境 ==========
if [[ -f "$ROS2_SETUP" ]]; then
    source "$ROS2_SETUP" 2>/dev/null || true
    echo "[环境] 已加载 ROS2 Humble"
fi

# ========== 准备构建目录 ==========
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo "=========================================="
echo "  Vision Distribute - 构建"
echo "=========================================="
echo "构建类型: ${BUILD_TYPE}"
echo "启用 GUI:  ${BUILD_GUI}"
echo "并行线程: ${JOBS}"
echo "构建目标: ${TARGET:-全部}"
echo "构建目录: ${BUILD_DIR}"
echo ""

# ========== 检测可选依赖 ==========
USE_HIK="OFF"
if [[ -f /opt/MVS/include/MvCameraControl.h ]] && [[ -f /opt/MVS/lib/64/libMvCameraControl.so ]]; then
    USE_HIK="ON"
    echo "[检测] 海康 MVS SDK: /opt/MVS"
else
    echo "[检测] 海康 MVS SDK: 未找到 (camera_node 将被跳过)"
fi

USE_ONNX="OFF"
if [[ -d "${SCRIPT_DIR}/deps/onnxruntime-linux-x64-1.17.3" ]]; then
    USE_ONNX="ON"
    echo "[检测] ONNX Runtime: deps/onnxruntime-linux-x64-1.17.3"
else
    echo "[检测] ONNX Runtime: 未找到 (YOLO 检测器将被跳过)"
fi

USE_NCNN="OFF"
if [[ -d "${SCRIPT_DIR}/deps/ncnn-linux-x64" ]]; then
    USE_NCNN="ON"
    echo "[检测] NCNN: deps/ncnn-linux-x64"
else
    echo "[检测] NCNN: 未找到"
fi
echo ""

# ========== CMake 配置 ==========
NEED_CONFIGURE="false"
if [[ ! -f CMakeCache.txt ]]; then
    NEED_CONFIGURE="true"
fi
if [[ "$CLEAN_FIRST" == "true" ]]; then
    NEED_CONFIGURE="true"
fi
# 检查上次配置的参数是否变化
if [[ -f CMakeCache.txt ]]; then
    LAST_TYPE=$(grep 'CMAKE_BUILD_TYPE:STRING=' CMakeCache.txt 2>/dev/null | cut -d= -f2)
    if [[ "$LAST_TYPE" != "$BUILD_TYPE" ]]; then
        NEED_CONFIGURE="true"
    fi
fi

if [[ "$NEED_CONFIGURE" == "true" ]]; then
    echo ">>> CMake 配置..."
    cmake "${SCRIPT_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DBUILD_GUI="${BUILD_GUI}" \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
        -DUSE_HIK="${USE_HIK}" \
        -DUSE_ONNX="${USE_ONNX}" \
        -DUSE_NCNN="${USE_NCNN}"
    if [[ $? -ne 0 ]]; then
        die "CMake 配置失败，请检查依赖是否安装。可尝试: -DUSE_HIK=OFF -DUSE_ONNX=OFF -DUSE_NCNN=OFF"
    fi
    echo ""
fi

# ========== 编译函数 ==========
build_target() {
    local name="$1"
    echo ">>> 编译 ${name}..."
    cmake --build . --target "${name}" --config "${BUILD_TYPE}" -j "${JOBS}"
    if [[ $? -ne 0 ]]; then
        die "${name} 编译失败"
    fi
    echo "    ${name} 编译完成。"
    echo ""
}

# 检查 CMake 目标是否存在
check_target() {
    local name="$1"
    if ! cmake --build . --target help 2>/dev/null | grep -q "${name}"; then
        return 1
    fi
    return 0
}

# ========== 按目标编译 ==========
if [[ -z "$TARGET" ]] || [[ "$TARGET" == "all" ]]; then
    echo ">>> 编译全部目标..."
    cmake --build . --config "${BUILD_TYPE}" -j "${JOBS}" || die "编译失败"
    echo ""
    echo ">>> 安装..."
    cmake --install . --config "${BUILD_TYPE}" || die "安装失败"
    echo ""
    echo "全部编译完成。"

elif [[ "$TARGET" == "rpc" ]]; then
    build_target "vision_rpc"

elif [[ "$TARGET" == "core" ]]; then
    echo ">>> vision_core 是仅头文件(INTERFACE)库，无需编译。"
    echo "    依赖它的节点会自动包含其头文件路径。"

elif [[ "$TARGET" == "camera" ]]; then
    if ! check_target "camera_node"; then
        die "camera_node 目标不存在，需要海康 MVS SDK。"$'\n'"    - 安装 SDK 后重新配置: cmake .. -DUSE_HIK=ON"$'\n'"    - 或在无海康摄像头环境下跳过此节点"
    fi
    build_target "vision_rpc"
    build_target "camera_node"

elif [[ "$TARGET" == "detector" ]]; then
    build_target "vision_rpc"
    build_target "detector_node"

elif [[ "$TARGET" == "comm" ]]; then
    build_target "vision_rpc"
    build_target "comm_node"

elif [[ "$TARGET" == "manager" ]]; then
    if [[ "$BUILD_GUI" != "ON" ]]; then
        die "manager 需要 --gui 选项启用 Qt6 支持"
    fi
    if ! check_target "manager"; then
        die "manager 目标不存在，请确认 Qt6 已安装并重新配置。"
    fi
    build_target "vision_rpc"
    build_target "manager"
fi

# ========== 结果 ==========
echo "=========================================="
echo "  构建完成"
echo "=========================================="

# 找到编译产物
if [[ -z "$TARGET" ]] || [[ "$TARGET" == "all" ]]; then
    echo "安装目录: ${INSTALL_DIR}/bin/"
    if [[ -d "${INSTALL_DIR}/bin" ]]; then
        ls -la "${INSTALL_DIR}/bin/" 2>/dev/null
    fi
else
    case "$TARGET" in
        camera)   BIN_DIR="${BUILD_DIR}/camera_node" ;;
        detector) BIN_DIR="${BUILD_DIR}/detector_node" ;;
        comm)     BIN_DIR="${BUILD_DIR}/comm_node" ;;
        manager)  BIN_DIR="${BUILD_DIR}/manager" ;;
        *)        BIN_DIR="" ;;
    esac
    if [[ -n "$BIN_DIR" ]] && [[ -f "${BIN_DIR}/${TARGET}_node" ]]; then
        echo "可执行文件: ${BIN_DIR}/${TARGET}_node"
        ls -la "${BIN_DIR}/${TARGET}_node"
    fi
fi

echo ""
echo "启动方式:"
echo "  ./scripts/launch_nogui.sh"
if [[ "$BUILD_GUI" == "ON" ]]; then
    echo "  ./scripts/launch_gui.sh"
fi
