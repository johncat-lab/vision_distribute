#!/bin/bash
set -e

# 设置 ROS2 环境
source /opt/ros/humble/setup.bash
cd /home/ch/johncat/vision_distribute

# 设置日志目录到可写位置
export ROS_LOG_DIR="${PWD}/build/install/log"
export ROS_HOME="${ROS_LOG_DIR}/ros_home"
mkdir -p "${ROS_HOME}"

echo "=== 测试所有 ROS2 服务 ==="
echo "ROS_LOG_DIR: ${ROS_LOG_DIR}"
echo "ROS_HOME: ${ROS_HOME}"
echo ""

# 清理旧进程
pkill -f detector_node 2>/dev/null || true
pkill -f camera_node 2>/dev/null || true
pkill -f comm_node 2>/dev/null || true
pkill -f manager 2>/dev/null || true
sleep 1

echo "1. 启动 camera 节点..."
./build/install/bin/camera_node --config ./build/install/config/system_config_ros2.xml --camera-config ./build/install/config/camera_config.xml &
CAM_PID=$!
echo "   camera PID: $CAM_PID"
sleep 3

echo ""
echo "2. 启动 detector 节点..."
./build/install/bin/detector_node --config ./build/install/config/system_config_ros2.xml --detector-config ./build/install/config/detector.xml &
DET_PID=$!
echo "   detector PID: $DET_PID"
sleep 3

echo ""
echo "3. 启动 comm 节点..."
./build/install/bin/comm_node --config ./build/install/config/system_config_ros2.xml --comm-config ./build/install/config/communication.xml &
COMM_PID=$!
echo "   comm PID: $COMM_PID"
sleep 3

echo ""
echo "4. 检查服务列表..."
ros2 service list

echo ""
echo "5. 启动 manager..."
./build/install/bin/manager --config ./build/install/config/system_config_ros2.xml &
MAN_PID=$!
echo "   manager PID: $MAN_PID"

# 等待 10 秒让服务调用完成
echo ""
echo "6. 运行中... (15秒后停止)"
sleep 15

echo ""
echo "7. 清理..."
kill $MAN_PID 2>/dev/null || true
sleep 1
kill $COMM_PID 2>/dev/null || true
kill $DET_PID 2>/dev/null || true
kill $CAM_PID 2>/dev/null || true
sleep 1

echo ""
echo "=== 测试完成 ==="
