#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="${SCRIPT_DIR}/../build/install"
BINS_DIR="${INSTALL_DIR}/bins"
LOG_DIR="${INSTALL_DIR}/log"
TRANSPORT="zeromq"

# ROS2 运行时 dlopen 需要找到自定义 typesupport 库
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${BINS_DIR}/lib:${LD_LIBRARY_PATH:-}"

# ===== 检测可用的终端模拟器 =====
detect_terminal() {
    if command -v gnome-terminal &>/dev/null; then
        echo "gnome-terminal"
    elif command -v konsole &>/dev/null; then
        echo "konsole"
    elif command -v xfce4-terminal &>/dev/null; then
        echo "xfce4-terminal"
    elif command -v xterm &>/dev/null; then
        echo "xterm"
    else
        echo "none"
    fi
}

TERMINAL="$(detect_terminal)"

# 支持 --transport 参数
while [[ $# -gt 0 ]]; do
    case "$1" in
        --transport)
            TRANSPORT="$2"
            shift 2
            ;;
        *)
            echo "未知参数: $1"
            echo "用法: $0 [--transport <zeromq|ros2|zenoh>]"
            exit 1
            ;;
    esac
done

# 系统配置文件名（根据 transport 选择）
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

# 每个节点目录结构: install/bins/<nodename>/<binary> + 配置文件
CAM_DIR="${BINS_DIR}/camera_node"
DET_DIR="${BINS_DIR}/detector_node"
COMM_DIR="${BINS_DIR}/comm_node"

# 各节点的系统配置（优先使用专用配置，回退到默认）
get_system_config() {
    local node_dir="$1"
    if [ -f "${node_dir}/${SYSTEM_CONFIG_NAME}" ]; then
        echo "${node_dir}/${SYSTEM_CONFIG_NAME}"
    elif [ -f "${node_dir}/system_config.xml" ]; then
        echo "${node_dir}/system_config.xml"
    else
        echo "错误: 未找到系统配置文件: ${node_dir}/${SYSTEM_CONFIG_NAME}" >&2
        exit 1
    fi
}

CAM_SYSTEM_CONFIG="$(get_system_config "${CAM_DIR}")"
DET_SYSTEM_CONFIG="$(get_system_config "${DET_DIR}")"
COMM_SYSTEM_CONFIG="$(get_system_config "${COMM_DIR}")"

cleanup() {
    echo ""
    echo "正在停止所有节点..."
    for proc in camera_node detector_node comm_node image_publisher_node; do
        pkill -f "${proc}" 2>/dev/null && echo "  已终止: ${proc}" || true
    done
    echo "所有节点已停止。"
    exit 0
}

trap cleanup SIGINT SIGTERM

# ===== 清理残留进程 =====
echo "正在清理已有进程..."
for proc in camera_node detector_node comm_node image_publisher_node manager; do
    pkill -9 -f "${proc}" 2>/dev/null && echo "  已终止: ${proc}" || true
done
sleep 0.5

echo ""
echo "========================================"
echo "  Vision Distribute - 无GUI模式启动"
echo "========================================"
echo "传输方式: ${TRANSPORT}"
echo "安装目录: ${BINS_DIR}"
echo "日志目录: ${LOG_DIR}"
echo ""

# 创建日志目录
mkdir -p "${LOG_DIR}"
LOG_TS=$(date +"%Y%m%d_%H%M%S")

# ROS2 模式下 DDS 发现需要更多时间，延长节点启动间隔
if [ "$TRANSPORT" = "ros2" ]; then
    NODE_DELAY=1.5
    echo "[ROS2] DDS 发现模式，节点间隔=${NODE_DELAY}s"
    echo ""
else
    NODE_DELAY=0.5
fi

# ===== 创建节点启动脚本（stdout+stderr 同时输出到终端和日志文件）=====
TMPDIR=$(mktemp -d)
make_script() {
    local path="$TMPDIR/$1"
    local logfile="$3"
    cat > "$path" << 'SCRIPTEOF'
#!/bin/bash
SCRIPTEOF
    echo "mkdir -p \"$(dirname "$logfile")\"" >> "$path"
    echo "exec > >(tee -a \"${logfile}\") 2>&1" >> "$path"
    echo "echo \"=== \$(date '+%Y-%m-%d %H:%M:%S') 启动 ===\"" >> "$path"
    echo "$2" >> "$path"
    echo 'echo; echo "[结束]"; read' >> "$path"
    chmod +x "$path"
    echo "$path"
}

CAM_LOG="${LOG_DIR}/camera_${LOG_TS}.log"
CAM_SCRIPT=$(make_script "camera.sh" \
    "${CAM_DIR}/camera_node --config '${CAM_SYSTEM_CONFIG}' --camera-config '${CAM_DIR}/camera_config.xml'" \
    "${CAM_LOG}")

DET_LOG="${LOG_DIR}/detector_${LOG_TS}.log"
DET_CMD="sleep ${NODE_DELAY}; ${DET_DIR}/detector_node --config '${DET_SYSTEM_CONFIG}' --detector-config '${DET_DIR}/detector.xml'"
DET_SCRIPT=$(make_script "detector.sh" "$DET_CMD" "${DET_LOG}")

COMM_SLEEP=$(awk "BEGIN {printf \"%.1f\", ${NODE_DELAY} * 2}")
COMM_LOG="${LOG_DIR}/communication_${LOG_TS}.log"
COMM_CMD="sleep ${COMM_SLEEP}; ${COMM_DIR}/comm_node --config '${COMM_SYSTEM_CONFIG}' --comm-config '${COMM_DIR}/communication.xml'"
COMM_SCRIPT=$(make_script "comm.sh" "$COMM_CMD" "${COMM_LOG}")

echo "[启动] 打开终端窗口，包含 3 个 Tab 页..."
echo "  camera       -> ${CAM_LOG}"
echo "  detector     -> ${DET_LOG}"
echo "  communication-> ${COMM_LOG}"
echo ""

# ===== 在单个终端窗口中使用多个 Tab 页启动所有节点 =====
case "$TERMINAL" in
    gnome-terminal)
        # 先创建主窗口
        gnome-terminal --window --title="VisionDistribute" &
        sleep 0.3
        # 逐个添加 tab
        gnome-terminal --tab --title="Camera" -- bash -c "$CAM_SCRIPT; exec bash" &
        sleep 0.2
        gnome-terminal --tab --title="Detector" -- bash -c "$DET_SCRIPT; exec bash" &
        sleep 0.2
        gnome-terminal --tab --title="Comm" -- bash -c "$COMM_SCRIPT; exec bash" &
        ;;
    konsole)
        konsole --new-tab -p tabtitle="Camera" -e "$CAM_SCRIPT" \
                --new-tab -p tabtitle="Detector" -e "$DET_SCRIPT" \
                --new-tab -p tabtitle="Comm" -e "$COMM_SCRIPT" &
        ;;
    xfce4-terminal)
        xfce4-terminal --tab --title="Camera" -e "$CAM_SCRIPT" \
                       --tab --title="Detector" -e "$DET_SCRIPT" \
                       --tab --title="Comm" -e "$COMM_SCRIPT" &
        ;;
    xterm|none)
        # xterm 不支持 tab，回退到多个窗口模式
        xterm -title "Camera" -e "$CAM_SCRIPT" 2>/dev/null &
        sleep 0.4
        xterm -title "Detector" -e "$DET_SCRIPT" 2>/dev/null &
        sleep 0.4
        xterm -title "Comm" -e "$COMM_SCRIPT" 2>/dev/null &
        ;;
esac

echo ""
echo "所有节点已在终端 Tab 页中启动。"
echo "  Tab: Camera | Detector | Comm"
echo ""
echo "按 Ctrl+C 停止所有节点..."

while true; do
    sleep 1
done
