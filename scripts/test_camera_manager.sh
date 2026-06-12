#!/bin/bash
set -e

# 设置 ROS2 环境
source /opt/ros/humble/setup.bash
cd /home/ch/johncat/vision_distribute

# 设置日志目录到可写位置
export ROS_LOG_DIR="${PWD}/build/install/log"
export ROS_HOME="${ROS_LOG_DIR}/ros_home"
mkdir -p "${ROS_HOME}"

echo "=== 单独测试 Camera + Manager ==="
echo "ROS_LOG_DIR: ${ROS_LOG_DIR}"
echo ""

# 清理旧进程
pkill -f camera_node 2>/dev/null || true
pkill -f manager 2>/dev/null || true
sleep 1

echo "1. 启动 camera 节点..."
./build/install/bin/camera_node --config ./build/install/config/system_config_ros2.xml --camera-config ./build/install/config/camera_config.xml &
CAM_PID=$!
echo "   camera PID: $CAM_PID"

# 等待 camera 启动
sleep 5

echo ""
echo "2. 检查服务列表..."
ros2 service list

echo ""
echo "3. 启动 manager..."
./build/install/bin/manager --config ./build/install/config/system_config_ros2.xml &
MAN_PID=$!
echo "   manager PID: $MAN_PID"

# 等待 10 秒让服务调用完成
echo ""
echo "4. 运行中... (10秒后停止)"
sleep 10

echo ""
echo "5. 清理..."
kill $MAN_PID 2>/dev/null || true
sleep 1
kill $CAM_PID 2>/dev/null || true
sleep 1

echo ""
echo "=== 测试完成 ==="
