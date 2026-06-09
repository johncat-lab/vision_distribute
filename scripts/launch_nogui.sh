#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="${SCRIPT_DIR}/../build/install"
BIN_DIR="${INSTALL_DIR}/bin"
CONFIG_DIR="${INSTALL_DIR}/config"
LOG_DIR="${INSTALL_DIR}/log"
TRANSPORT="zeromq"

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

# 在独立终端窗口中分别启动每个节点
launch_term() {
    local title="$1"
    local script_path="$2"
    case "$TERMINAL" in
        gnome-terminal)
            gnome-terminal --title="$title" -- "$script_path" &
            ;;
        konsole)
            konsole -p tabtitle="$title" -e "$script_path" &
            ;;
        xfce4-terminal)
            xfce4-terminal --title="$title" -e "$script_path" &
            ;;
        xterm|none)
            xterm -title "$title" -e "$script_path" 2>/dev/null || bash "$script_path" &
            ;;
    esac
    sleep 0.4
}

TERMINAL="$(detect_terminal)"

# 支持 --config-dir 和 --transport 参数
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config-dir)
            CONFIG_DIR="$2"
            shift 2
            ;;
        --transport)
            TRANSPORT="$2"
            shift 2
            ;;
        *)
            echo "未知参数: $1"
            echo "用法: $0 [--config-dir <配置目录>] [--transport <zeromq|ros2|zenoh>]"
            exit 1
            ;;
    esac
done

# 根据 transport 选择对应的系统配置文件
case "$TRANSPORT" in
    zeromq|zmq)
        SYSTEM_CONFIG="${CONFIG_DIR}/system_config_zeromq.xml"
        ;;
    ros2)
        SYSTEM_CONFIG="${CONFIG_DIR}/system_config_ros2.xml"
        ;;
    zenoh)
        SYSTEM_CONFIG="${CONFIG_DIR}/system_config_zenoh.xml"
        ;;
    *)
        echo "错误: 不支持的传输方式: $TRANSPORT"
        echo "可选值: zeromq, ros2, zenoh"
        exit 1
        ;;
esac

# 如果专用配置文件不存在，回退到默认配置
if [ ! -f "$SYSTEM_CONFIG" ]; then
    echo "警告: 未找到专用配置 '$SYSTEM_CONFIG'，回退到默认配置"
    SYSTEM_CONFIG="${CONFIG_DIR}/system_config.xml"
fi

cleanup() {
    echo ""
    echo "正在停止所有节点..."
    for proc in camera_node detector_node comm_node; do
        pkill -f "${proc}" 2>/dev/null && echo "  已终止: ${proc}" || true
    done
    echo "所有节点已停止。"
    exit 0
}

trap cleanup SIGINT SIGTERM

# ===== 清理残留进程 =====
echo "正在清理已有进程..."
for proc in camera_node detector_node comm_node manager; do
    pkill -9 -f "${proc}" 2>/dev/null && echo "  已终止: ${proc}" || true
done
sleep 0.5

echo ""
echo "========================================"
echo "  Vision Distribute - 无GUI模式启动"
echo "========================================"
echo "传输方式: ${TRANSPORT}"
echo "系统配置: ${SYSTEM_CONFIG}"
echo "配置目录: ${CONFIG_DIR}"
echo "可执行目录: ${BIN_DIR}"
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
    "${BIN_DIR}/camera_node --config '${SYSTEM_CONFIG}' --camera-config '${CONFIG_DIR}/camera_config.xml'" \
    "${CAM_LOG}")

DET_LOG="${LOG_DIR}/detector_${LOG_TS}.log"
DET_CMD="sleep ${NODE_DELAY}; ${BIN_DIR}/detector_node --config '${SYSTEM_CONFIG}' --detector-config '${CONFIG_DIR}/detector.xml'"
DET_SCRIPT=$(make_script "detector.sh" "$DET_CMD" "${DET_LOG}")

COMM_SLEEP=$(awk "BEGIN {printf \"%.1f\", ${NODE_DELAY} * 2}")
COMM_LOG="${LOG_DIR}/communication_${LOG_TS}.log"
COMM_CMD="sleep ${COMM_SLEEP}; ${BIN_DIR}/comm_node --config '${SYSTEM_CONFIG}' --comm-config '${CONFIG_DIR}/communication.xml'"
COMM_SCRIPT=$(make_script "comm.sh" "$COMM_CMD" "${COMM_LOG}")

echo "[启动] 打开 3 个独立终端窗口..."
echo "  camera       -> ${CAM_LOG}"
echo "  detector     -> ${DET_LOG}"
echo "  communication-> ${COMM_LOG}"
echo ""
launch_term "Camera"   "$CAM_SCRIPT"
launch_term "Detector" "$DET_SCRIPT"
launch_term "Comm"     "$COMM_SCRIPT"

echo ""
echo "所有节点已在新终端窗口中启动。"
echo "  窗口: Camera | Detector | Comm"
echo ""
echo "按 Ctrl+C 停止所有节点..."

while true; do
    sleep 1
done
