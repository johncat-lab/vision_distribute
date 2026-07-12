#!/bin/bash
set -e

echo "=========================================="
echo "  ZMQ 完整链路测试: camera→detector→comm"
echo "=========================================="
echo ""

# 切换到项目目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="${SCRIPT_DIR}/../build/install"
BINS_DIR="${INSTALL_DIR}/bins"

# 检查安装目录
if [ ! -d "${INSTALL_DIR}" ]; then
    echo "❌ 错误: 未找到安装目录 ${INSTALL_DIR}"
    echo "   请先运行 ./build.sh 进行编译安装"
    exit 1
fi

# 设置动态库路径(ZMQ模式不需要ROS2库)
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${BINS_DIR}/lib:${LD_LIBRARY_PATH:-}"

echo "✅ [环境] 安装目录: ${INSTALL_DIR}"
echo "   动态库路径: ${LD_LIBRARY_PATH}"
echo ""

# 清理旧进程
echo "🧹 [清理] 停止可能存在的节点..."
pkill -f detector_node 2>/dev/null || true
pkill -f camera_node 2>/dev/null || true
pkill -f comm_node 2>/dev/null || true
pkill -f manager 2>/dev/null || true
sleep 2

# 检查端口占用(ZMQ使用15550-15560范围)
echo "🔍 [检查] 端口占用情况..."
PORTS_IN_USE=$(netstat -tlnp 2>/dev/null | grep -E "1555[0-9]|1556[0-9]" || ss -tlnp | grep -E "1555[0-9]|1556[0-9]" || echo "")
if [ -n "$PORTS_IN_USE" ]; then
    echo "   ⚠️  发现端口被占用:"
    echo "$PORTS_IN_USE"
    echo "   建议: 停止占用端口的进程或修改system_config_zeromq.xml中的base_port"
else
    echo "   ✅ 端口15550-15560范围空闲"
fi

# 定义日志文件
LOG_DIR="${INSTALL_DIR}/log"
mkdir -p "${LOG_DIR}"

CAM_LOG="${LOG_DIR}/camera_zmq_test.log"
DET_LOG="${LOG_DIR}/detector_zmq_test.log"
COMM_LOG="${LOG_DIR}/comm_zmq_test.log"

# 配置文件路径
CAM_DIR="${BINS_DIR}/camera_node"
DET_DIR="${BINS_DIR}/detector_node"
COMM_DIR="${BINS_DIR}/comm_node"

# 检查节点二进制文件
for node_bin in "${CAM_DIR}/camera_node" "${DET_DIR}/detector_node" "${COMM_DIR}/comm_node"; do
    if [ ! -f "$node_bin" ]; then
        echo "❌ 错误: 未找到节点二进制文件 $node_bin"
        exit 1
    fi
done

# 检查配置文件
CAM_CONFIG="${CAM_DIR}/camera_config.xml"
DET_CONFIG="${DET_DIR}/detector.xml"
COMM_CONFIG="${COMM_DIR}/communication.xml"
SYSTEM_CONFIG_ZMQ="${CAM_DIR}/system_config_zeromq.xml"

for config in "$CAM_CONFIG" "$DET_CONFIG" "$COMM_CONFIG"; do
    if [ ! -f "$config" ]; then
        echo "⚠️  警告: 未找到配置文件 $config"
    fi
done

# 选择系统配置(优先使用节点目录下的,回退到install/config)
if [ -f "$SYSTEM_CONFIG_ZMQ" ]; then
    SYS_CONFIG="$SYSTEM_CONFIG_ZMQ"
elif [ -f "${INSTALL_DIR}/config/system_config_zeromq.xml" ]; then
    SYS_CONFIG="${INSTALL_DIR}/config/system_config_zeromq.xml"
else
    echo "❌ 错误: 未找到ZMQ系统配置文件"
    exit 1
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[1/7] 启动 camera 节点 (ZMQ模式)..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "   配置: $SYS_CONFIG"
echo "   相机配置: $CAM_CONFIG"
nohup "${CAM_DIR}/camera_node" \
    --config "$SYS_CONFIG" \
    --camera-config "$CAM_CONFIG" \
    > "${CAM_LOG}" 2>&1 &
CAM_PID=$!
echo "   PID: $CAM_PID"
echo "   日志: ${CAM_LOG}"
sleep 3

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[2/7] 启动 detector 节点 (ZMQ模式)..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "   配置: $SYS_CONFIG"
echo "   检测器配置: $DET_CONFIG"
nohup "${DET_DIR}/detector_node" \
    --config "$SYS_CONFIG" \
    --detector-config "$DET_CONFIG" \
    > "${DET_LOG}" 2>&1 &
DET_PID=$!
echo "   PID: $DET_PID"
echo "   日志: ${DET_LOG}"
sleep 3

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[3/7] 启动 comm 节点 (ZMQ模式)..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "   配置: $SYS_CONFIG"
echo "   通信配置: $COMM_CONFIG"
nohup "${COMM_DIR}/comm_node" \
    --config "$SYS_CONFIG" \
    --comm-config "$COMM_CONFIG" \
    > "${COMM_LOG}" 2>&1 &
COMM_PID=$!
echo "   PID: $COMM_PID"
echo "   日志: ${COMM_LOG}"
sleep 3

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[4/7] 检查节点运行状态..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

check_process() {
    local pid=$1
    local name=$2
    if kill -0 "$pid" 2>/dev/null; then
        echo "   ✅ $name (PID:$pid) - 运行中"
        return 0
    else
        echo "   ❌ $name (PID:$pid) - 已退出"
        return 1
    fi
}

ALL_RUNNING=true
check_process $CAM_PID "Camera节点" || ALL_RUNNING=false
check_process $DET_PID "Detector节点" || ALL_RUNNING=false
check_process $COMM_PID "Comm节点" || ALL_RUNNING=false

if [ "$ALL_RUNNING" = false ]; then
    echo ""
    echo "⚠️  部分节点启动失败,请检查日志:"
    echo "   Camera:  tail -50 ${CAM_LOG}"
    echo "   Detector: tail -50 ${DET_LOG}"
    echo "   Comm:    tail -50 ${COMM_LOG}"
    exit 1
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[5/7] 检查ZMQ端口绑定情况..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📡 活跃的ZMQ端口:"
PORT_INFO=$(netstat -tlnp 2>/dev/null | grep -E "1555[0-9]|1556[0-9]" || ss -tlnp | grep -E "1555[0-9]|1556[0-9]" || echo "")
if [ -n "$PORT_INFO" ]; then
    echo "$PORT_INFO" | while read line; do
        echo "   $line"
    done
else
    echo "   ⚠️  未发现ZMQ端口(可能节点未完全启动)"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[6/7] 检查节点日志(最近20行)..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo ""
echo "📄 Camera 节点日志:"
tail -20 "${CAM_LOG}" 2>/dev/null || echo "   (无日志)"

echo ""
echo "📄 Detector 节点日志:"
tail -20 "${DET_LOG}" 2>/dev/null || echo "   (无日志)"

echo ""
echo "📄 Comm 节点日志:"
tail -20 "${COMM_LOG}" 2>/dev/null || echo "   (无日志)"

# 等待数据采集
echo ""
echo "⏳ 等待5秒采集数据..."
sleep 5

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[7/7] 验证数据流贯通..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo ""
echo "📈 检查Camera是否发布帧:"
FRAME_COUNT=$(grep -c "publish\|帧\|frame" "${CAM_LOG}" 2>/dev/null || echo "0")
echo "   找到 ${FRAME_COUNT} 条帧相关日志"
grep -i "publish\|帧发布者\|frame.*topic" "${CAM_LOG}" 2>/dev/null | tail -3 || echo "   (未找到发布日志)"

echo ""
echo "📈 检查Detector是否收到并处理帧:"
RECV_COUNT=$(grep -c "receive\|processFrame\|detect" "${DET_LOG}" 2>/dev/null || echo "0")
echo "   找到 ${RECV_COUNT} 条处理相关日志"
grep -i "receive frame\|processFrame\|detect\|publish.*detection" "${DET_LOG}" 2>/dev/null | tail -5 || echo "   (未找到处理日志)"

echo ""
echo "📈 检查Comm是否收到检测结果:"
COMM_COUNT=$(grep -c "detection\|result\|object" "${COMM_LOG}" 2>/dev/null || echo "0")
echo "   找到 ${COMM_COUNT} 条通信相关日志"
grep -i "detection\|result\|object\|protocol" "${COMM_LOG}" 2>/dev/null | tail -5 || echo "   (未找到结果日志)"

echo ""
echo "=========================================="
echo "  链路测试完成"
echo "=========================================="
echo ""
echo "✅ 测试摘要:"
echo "   • 传输模式: ZeroMQ"
echo "   • 节点状态: camera(PID:$CAM_PID), detector(PID:$DET_PID), comm(PID:$COMM_PID)"
echo "   • 数据流: camera →(vision/frame)→ detector →(vision/detection)→ comm"
echo "   • 端口范围: 15550-15560 (base_port + hash)"
echo ""
echo "📝 日志文件位置:"
echo "   Camera:  ${CAM_LOG}"
echo "   Detector: ${DET_LOG}"
echo "   Comm:    ${COMM_LOG}"
echo ""
echo "🔧 ZMQ架构特点:"
echo "   • 无中心化Broker,节点间直接TCP连接"
echo "   • Publisher绑定端口,Subscriber主动连接"
echo "   • 端口计算: base_port(15550) + hash(topic) % 10"
echo ""
echo "⚠️  如需停止节点,执行:"
echo "   kill $CAM_PID $DET_PID $COMM_PID"
echo ""

# 可选:自动清理(取消注释以下行以自动停止节点)
# echo "🧹 自动清理节点..."
# kill $CAM_PID $DET_PID $COMM_PID 2>/dev/null || true
# wait $CAM_PID $DET_PID $COMM_PID 2>/dev/null || true
# echo "✅ 所有节点已停止"
