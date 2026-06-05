#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="${SCRIPT_DIR}/../build/install/bin"
CONFIG_DIR="${SCRIPT_DIR}/../build/install/config"

# 支持 --config-dir 参数覆盖配置目录
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config-dir)
            CONFIG_DIR="$2"
            shift 2
            ;;
        *)
            echo "未知参数: $1"
            echo "用法: $0 [--config-dir <配置目录>]"
            exit 1
            ;;
    esac
done

PIDS=()

cleanup() {
    echo ""
    echo "正在停止所有节点..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null
    echo "所有节点已停止。"
    exit 0
}

trap cleanup SIGINT SIGTERM

echo "========================================"
echo "  Vision Distribute - GUI模式启动"
echo "========================================"
echo "配置目录: ${CONFIG_DIR}"
echo "可执行目录: ${BIN_DIR}"
echo ""

# 启动 Camera 节点
echo "[1/4] 启动 Camera 节点..."
"${BIN_DIR}/camera_node" \
    --config "${CONFIG_DIR}/system_config.xml" \
    --camera-config "${CONFIG_DIR}/camera_config.xml" &
PIDS+=($!)
sleep 0.5

# 启动 Detector 节点
echo "[2/4] 启动 Detector 节点..."
"${BIN_DIR}/detector_node" \
    --config "${CONFIG_DIR}/system_config.xml" \
    --detector-config "${CONFIG_DIR}/detector.xml" &
PIDS+=($!)
sleep 0.5

# 启动 Communication 节点
echo "[3/4] 启动 Communication 节点..."
"${BIN_DIR}/comm_node" \
    --config "${CONFIG_DIR}/system_config.xml" \
    --comm-config "${CONFIG_DIR}/communication.xml" &
PIDS+=($!)
sleep 0.5

# 启动 Manager 节点 (GUI)
echo "[4/4] 启动 Manager 节点..."
"${BIN_DIR}/manager" \
    --config "${CONFIG_DIR}/system_config.xml" &
PIDS+=($!)

echo ""
echo "所有节点已启动。PID 列表:"
echo "  Camera:   ${PIDS[0]}"
echo "  Detector: ${PIDS[1]}"
echo "  Comm:     ${PIDS[2]}"
echo "  Manager:  ${PIDS[3]}"
echo ""
echo "按 Ctrl+C 停止所有节点..."

wait
