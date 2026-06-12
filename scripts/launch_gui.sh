#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="${SCRIPT_DIR}/../build/install"
BIN_DIR="${INSTALL_DIR}/bin"
CONFIG_DIR="${INSTALL_DIR}/config"
LOG_DIR="${INSTALL_DIR}/log"
TRANSPORT="zeromq"

# ROS2 运行时 dlopen 需要找到自定义 typesupport 库
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH:-}"

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
    for proc in camera_node detector_node comm_node manager; do
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

echo "========================================"
echo "  Vision Distribute - GUI模式启动"
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

# ROS2 模式下的特殊配置
if [ "$TRANSPORT" = "ros2" ]; then
    NODE_DELAY=1.5
    MANAGER_DELAY=3.0
    echo "[ROS2] DDS 发现模式，节点间隔=${NODE_DELAY}s，Manager 额外等待=${MANAGER_DELAY}s"
    # 设置 ROS2 日志目录到可写位置（避免只读文件系统问题）
    export ROS_LOG_DIR="${LOG_DIR}"
    export ROS_HOME="${LOG_DIR}/ros_home"
    mkdir -p "${ROS_HOME}"
    echo "[ROS2] 设置日志目录: ${ROS_LOG_DIR}"
    echo ""
else
    NODE_DELAY=0.5
    MANAGER_DELAY=0
fi

# ===== 创建节点启动脚本（stdout+stderr 同时输出到终端和日志文件）=====
TMPDIR=$(mktemp -d)
make_script() {
    local path="$TMPDIR/$1"
    local logfile="$3"
    # 使用非引号的 here-doc 允许变量展开
    cat > "$path" << SCRIPTEOF
#!/bin/bash
# 导出 ROS2 环境变量
export ROS_LOG_DIR="${LOG_DIR}"
export ROS_HOME="${LOG_DIR}/ros_home"
mkdir -p "\$ROS_HOME"
mkdir -p "$(dirname "$logfile")"
exec > >(tee -a "${logfile}") 2>&1
echo "=== \$(date '+%Y-%m-%d %H:%M:%S') 启动 ==="
$2
echo; echo "[结束]"; read
SCRIPTEOF
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

MGR_SLEEP=$(awk "BEGIN {printf \"%.1f\", ${NODE_DELAY} * 3 + ${MANAGER_DELAY}}")
MGR_LOG="${LOG_DIR}/manager_${LOG_TS}.log"
if [ -z "${DISPLAY:-}" ]; then
    echo "  ⚠ 未检测到显示器，Manager 使用 offscreen 模式"
    MGR_CMD="sleep ${MGR_SLEEP}; QT_QPA_PLATFORM=offscreen ${BIN_DIR}/manager --config '${SYSTEM_CONFIG}'"
else
    MGR_CMD="sleep ${MGR_SLEEP}; ${BIN_DIR}/manager --config '${SYSTEM_CONFIG}'"
fi
MGR_SCRIPT=$(make_script "manager.sh" "$MGR_CMD" "${MGR_LOG}")

echo "[启动] 打开终端窗口，包含 4 个 Tab 页..."
echo "  camera       -> ${CAM_LOG}"
echo "  detector     -> ${DET_LOG}"
echo "  communication-> ${COMM_LOG}"
echo "  manager      -> ${MGR_LOG}"
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
        sleep 0.2
        gnome-terminal --tab --title="Manager" -- bash -c "$MGR_SCRIPT; exec bash" &
        ;;
    konsole)
        konsole --new-tab -p tabtitle="Camera" -e "$CAM_SCRIPT" \
                --new-tab -p tabtitle="Detector" -e "$DET_SCRIPT" \
                --new-tab -p tabtitle="Comm" -e "$COMM_SCRIPT" \
                --new-tab -p tabtitle="Manager" -e "$MGR_SCRIPT" &
        ;;
    xfce4-terminal)
        xfce4-terminal --tab --title="Camera" -e "$CAM_SCRIPT" \
                       --tab --title="Detector" -e "$DET_SCRIPT" \
                       --tab --title="Comm" -e "$COMM_SCRIPT" \
                       --tab --title="Manager" -e "$MGR_SCRIPT" &
        ;;
    xterm|none)
        # xterm 不支持 tab，回退到多个窗口模式
        xterm -title "Camera" -e "$CAM_SCRIPT" 2>/dev/null &
        sleep 0.4
        xterm -title "Detector" -e "$DET_SCRIPT" 2>/dev/null &
        sleep 0.4
        xterm -title "Comm" -e "$COMM_SCRIPT" 2>/dev/null &
        sleep 0.4
        xterm -title "Manager" -e "$MGR_SCRIPT" 2>/dev/null &
        ;;
esac

echo ""
echo "所有节点已在终端 Tab 页中启动。"
echo "  Tab: Camera | Detector | Comm | Manager"
echo ""
echo "按 Ctrl+C 停止所有节点..."

while true; do
    sleep 1
done
