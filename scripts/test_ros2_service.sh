#!/bin/bash
set -e

echo "=== ROS2 Service Test ==="
echo ""

# 先清理可能存在的节点
pkill -f detector_node 2>/dev/null || true
pkill -f manager 2>/dev/null || true
sleep 1

echo "1. 启动 detector 节点..."
source /opt/ros/humble/setup.bash
cd /home/ch/johncat/vision_distribute

# 设置 ROS2 日志目录到可写位置
export ROS_LOG_DIR="${PWD}/build/install/log"
export ROS_HOME="${ROS_LOG_DIR}/ros_home"
mkdir -p "${ROS_HOME}"
echo "[ROS2] 设置日志目录: ${ROS_LOG_DIR}"

# 在后台启动 detector
./build/install/bin/detector_node --config ./build/install/config/system_config_ros2.xml --detector-config ./build/install/config/detector.xml &
DET_PID=$!

echo "   detector PID: $DET_PID"
sleep 5

echo ""
echo "2. 检查服务列表..."
ros2 service list

echo ""
echo "3. 检查服务类型..."
ros2 service type /detector

echo ""
echo "4. 尝试调用服务..."
ros2 service call /detector rcl_interfaces/srv/GetParameterTypes "{names: ['test']}"

echo ""
echo "5. 清理..."
kill $DET_PID
wait $DET_PID 2>/dev/null || true

echo ""
echo "=== Test completed ==="