#!/bin/bash
set -e

echo "=========================================="
echo "  ROS2 完整链路测试: camera→detector→comm"
echo "=========================================="
echo ""

# 检查ROS2环境
if [ ! -f /opt/ros/humble/setup.bash ]; then
    echo "❌ 错误: 未找到ROS2 Humble安装"
    exit 1
fi

source /opt/ros/humble/setup.bash

# 切换到项目目录
cd /home/ch/johncat/vision_distribute

# 设置日志目录
export ROS_LOG_DIR="${PWD}/build/install/log"
export ROS_HOME="${ROS_LOG_DIR}/ros_home"
mkdir -p "${ROS_HOME}"

echo "✅ [环境] ROS2环境已加载"
echo "   ROS_DOMAIN_ID: ${ROS_DOMAIN_ID:-0}"
echo "   ROS_LOG_DIR: ${ROS_LOG_DIR}"
echo ""

# 清理旧进程
echo "🧹 [清理] 停止可能存在的节点..."
pkill -f detector_node 2>/dev/null || true
pkill -f camera_node 2>/dev/null || true
pkill -f comm_node 2>/dev/null || true
pkill -f manager 2>/dev/null || true
sleep 2

# 定义日志文件
CAM_LOG="${ROS_LOG_DIR}/camera_test.log"
DET_LOG="${ROS_LOG_DIR}/detector_test.log"
COMM_LOG="${ROS_LOG_DIR}/comm_test.log"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[1/7] 启动 camera 节点..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
nohup ./build/install/bin/camera_node \
    --config ./build/install/config/system_config_ros2.xml \
    --camera-config ./build/install/config/camera_config.xml \
    > "${CAM_LOG}" 2>&1 &
CAM_PID=$!
echo "   PID: $CAM_PID"
echo "   日志: ${CAM_LOG}"
sleep 4

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[2/7] 启动 detector 节点..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
nohup ./build/install/bin/detector_node \
    --config ./build/install/config/system_config_ros2.xml \
    --detector-config ./build/install/config/detector.xml \
    > "${DET_LOG}" 2>&1 &
DET_PID=$!
echo "   PID: $DET_PID"
echo "   日志: ${DET_LOG}"
sleep 4

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[3/7] 启动 comm 节点..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
nohup ./build/install/bin/comm_node \
    --config ./build/install/config/system_config_ros2.xml \
    --comm-config ./build/install/config/communication.xml \
    > "${COMM_LOG}" 2>&1 &
COMM_PID=$!
echo "   PID: $COMM_PID"
echo "   日志: ${COMM_LOG}"
sleep 3

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[4/7] 检查ROS2服务注册情况..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📋 所有服务列表:"
ros2 service list | sort

echo ""
echo "🔍 验证关键服务类型:"
for svc in $(ros2 service list); do
    TYPE=$(ros2 service type $svc 2>/dev/null || echo "未知")
    echo "   $svc → $TYPE"
done

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[5/7] 检查Topic订阅情况..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📡 活跃的Topics:"
ros2 topic list | grep -E "vision|frame|detection" || echo "   (未发现vision相关topics)"

echo ""
echo "📊 Topic详细信息:"
for topic in $(ros2 topic list | grep -E "vision|frame|detection"); do
    TYPE=$(ros2 topic type $topic 2>/dev/null || echo "未知")
    echo "   $topic → $TYPE"
done

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[6/7] 测试服务调用连通性..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo ""
echo "🔌 测试 /detector/get_config 服务..."
timeout 3 ros2 service call /detector/get_config vision_interfaces/srv/DetectorGetConfig "{}" 2>&1 | head -10 || echo "   ⚠️  超时或无响应"

echo ""
echo "🔌 测试 /camera/get_config 服务..."
timeout 3 ros2 service call /camera/get_config vision_interfaces/srv/CameraGetConfig "{}" 2>&1 | head -10 || echo "   ⚠️  超时或无响应"

echo ""
echo "🔌 测试 /comm/get_status 服务..."
timeout 3 ros2 service call /comm/get_status vision_interfaces/srv/CommGetStatus "{}" 2>&1 | head -10 || echo "   ⚠️  超时或无响应"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[7/7] 检查节点日志(最近20行)..."
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
echo "📈 检查Detector是否收到帧:"
grep -i "receive frame\|processFrame\|frame" "${DET_LOG}" 2>/dev/null | tail -5 || echo "   (未找到帧处理日志)"

echo ""
echo "📈 检查Comm是否收到检测结果:"
grep -i "detection\|result\|object" "${COMM_LOG}" 2>/dev/null | tail -5 || echo "   (未找到检测结果日志)"

echo ""
echo "=========================================="
echo "  链路测试完成"
echo "=========================================="
echo ""
echo "✅ 测试摘要:"
echo "   • ROS2环境: 正常"
echo "   • 节点启动: camera(PID:$CAM_PID), detector(PID:$DET_PID), comm(PID:$COMM_PID)"
echo "   • 服务注册: 见上方输出"
echo "   • Topic通信: 见上方输出"
echo "   • 数据流: camera → detector → comm"
echo ""
echo "📝 日志文件位置:"
echo "   Camera:  ${CAM_LOG}"
echo "   Detector: ${DET_LOG}"
echo "   Comm:    ${COMM_LOG}"
echo ""
echo "⚠️  如需停止节点，执行:"
echo "   kill $CAM_PID $DET_PID $COMM_PID"
echo ""

# 可选:自动清理(取消注释以下行以自动停止节点)
# echo "🧹 自动清理节点..."
# kill $CAM_PID $DET_PID $COMM_PID 2>/dev/null || true
# wait $CAM_PID $DET_PID $COMM_PID 2>/dev/null || true
# echo "✅ 所有节点已停止"
