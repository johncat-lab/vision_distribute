#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="${SCRIPT_DIR}/../build/install"
BINS_DIR="${INSTALL_DIR}/bins"
TRANSPORT="zeromq"

# ROS2 运行时 dlopen 需要找到自定义 typesupport 库
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${BINS_DIR}/lib:${LD_LIBRARY_PATH:-}"

NODE_DIR="${BINS_DIR}/image_publisher_node"
NODE_BIN="${NODE_DIR}/image_publisher_node"

# ===== 参数解析 =====
while [[ $# -gt 0 ]]; do
    case "$1" in
        --transport)
            TRANSPORT="$2"
            shift 2
            ;;
        --images)
            IMAGE_DIR="$2"
            shift 2
            ;;
        *)
            echo "未知参数: $1"
            echo "用法: $0 [--transport <zeromq|ros2|zenoh>] [--images <图像目录>]"
            exit 1
            ;;
    esac
done

# 根据 transport 选择对应的系统配置文件名
case "$TRANSPORT" in
    zeromq|zmq)
        SYSTEM_CONFIG_NAME="system_config_zeromq.xml"
        ;;
    ros2)
        SYSTEM_CONFIG_NAME="system_config_ros2.xml"
        ;;
    zenoh)
        SYSTEM_CONFIG_NAME="system_config_zenoh.xml"
        ;;
    *)
        echo "错误: 不支持的传输方式: $TRANSPORT"
        echo "可选值: zeromq, ros2, zenoh"
        exit 1
        ;;
esac

# 在节点目录中查找系统配置文件
if [ -f "${NODE_DIR}/${SYSTEM_CONFIG_NAME}" ]; then
    SYSTEM_CONFIG="${NODE_DIR}/${SYSTEM_CONFIG_NAME}"
elif [ -f "${NODE_DIR}/system_config.xml" ]; then
    SYSTEM_CONFIG="${NODE_DIR}/system_config.xml"
else
    echo "错误: 未找到系统配置文件: ${NODE_DIR}/${SYSTEM_CONFIG_NAME}"
    exit 1
fi

PUB_CONFIG="${NODE_DIR}/image_publisher_config.xml"

# 如果通过 --images 指定了图像目录，则临时修改配置
if [ -n "${IMAGE_DIR:-}" ]; then
    if [ ! -d "$IMAGE_DIR" ]; then
        echo "错误: 图像目录不存在: $IMAGE_DIR"
        exit 1
    fi
    # 使用 sed 替换配置中的 image_dir
    TMP_PUB_CONFIG=$(mktemp /tmp/image_pub_config_XXXXXX.xml)
    sed "s|<image_dir>.*</image_dir>|<image_dir>${IMAGE_DIR}</image_dir>|" \
        "$PUB_CONFIG" > "$TMP_PUB_CONFIG"
    PUB_CONFIG="$TMP_PUB_CONFIG"
    echo "使用指定图像目录: $IMAGE_DIR"
fi

cleanup() {
    echo ""
    echo "正在停止 image_publisher_node..."
    pkill -f "image_publisher_node" 2>/dev/null && echo "  已终止: image_publisher_node" || true
    echo "已停止。"
    # 清理临时配置文件
    if [ -n "${TMP_PUB_CONFIG:-}" ] && [ -f "$TMP_PUB_CONFIG" ]; then
        rm -f "$TMP_PUB_CONFIG"
    fi
    exit 0
}

trap cleanup SIGINT SIGTERM

echo ""
echo "========================================"
echo "  Vision Distribute - 图像发布器"
echo "========================================"
echo "传输方式:   ${TRANSPORT}"
echo "节点目录:   ${NODE_DIR}"
echo "系统配置:   ${SYSTEM_CONFIG}"
echo "发布器配置: ${PUB_CONFIG}"
echo ""

if [ ! -f "${NODE_BIN}" ]; then
    echo "错误: 可执行文件不存在: ${NODE_BIN}"
    echo "请先编译项目: ./build.sh image-publisher"
    exit 1
fi

echo "启动 image_publisher_node..."
echo "按 Ctrl+C 停止..."
echo ""

"${NODE_BIN}" \
    --config "${SYSTEM_CONFIG}" \
    --pub-config "${PUB_CONFIG}"
