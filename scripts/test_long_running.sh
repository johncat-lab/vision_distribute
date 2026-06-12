#!/bin/bash
set -e

# 设置 ROS2 环境
source /opt/ros/humble/setup.bash
cd /home/ch/johncat/vision_distribute

# 设置日志目录到可写位置
export ROS_LOG_DIR="${PWD}/build/install/log"
export ROS_HOME="${ROS_LOG_DIR}/ros_home"
mkdir -p "${ROS_HOME}"

echo "=== 长时间运行测试 (30秒) ==="
echo "开始时间: $(date)"
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
sleep 2

echo ""
echo "2. 启动 detector 节点..."
./build/install/bin/detector_node --config ./build/install/config/system_config_ros2.xml --detector-config ./build/install/config/detector.xml &
DET_PID=$!
echo "   detector PID: $DET_PID"
sleep 2

echo ""
echo "3. 启动 comm 节点..."
./build/install/bin/comm_node --config ./build/install/config/system_config_ros2.xml --comm-config ./build/install/config/communication.xml &
COMM_PID=$!
echo "   comm PID: $COMM_PID"
sleep 2

echo ""
echo "4. 启动 manager..."
./build/install/bin/manager --config ./build/install/config/system_config_ros2.xml &
MAN_PID=$!
echo "   manager PID: $MAN_PID"

# 运行 30 秒，记录每次服务调用的结果
echo ""
echo "5. 运行中... (30秒)"
for i in {1..30}; do
    sleep 1
    # 检查是否所有进程都还在运行
    if ! kill -0 $MAN_PID 2>/dev/null; then
        echo "!!! manager 进程已退出"
        break
    fi
    if ! kill -0 $CAM_PID 2>/dev/null; then
        echo "!!! camera 进程已退出"
    fi
    if ! kill -0 $DET_PID 2>/dev/null; then
        echo "!!! detector 进程已退出"
    fi
    if ! kill -0 $COMM_PID 2>/dev/null; then
        echo "!!! comm 进程已退出"
    fi
    # 每5秒打印一次进度
    if [ $((i % 5)) -eq 0 ]; then
        echo "   已运行 ${i}s"
    fi
done

echo ""
echo "6. 清理..."
kill $MAN_PID 2>/dev/null || true
sleep 1
kill $COMM_PID 2>/dev/null || true
kill $DET_PID 2>/dev/null || true
kill $CAM_PID 2>/dev/null || true
sleep 1

echo ""
echo "=== 测试完成 ==="
echo "结束时间: $(date)"
