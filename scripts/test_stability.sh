#!/bin/bash
set -e

# 设置 ROS2 环境
source /opt/ros/humble/setup.bash
cd /home/ch/johncat/vision_distribute

# 设置日志目录到可写位置
export ROS_LOG_DIR="${PWD}/build/install/log"
export ROS_HOME="${ROS_LOG_DIR}/ros_home"
mkdir -p "${ROS_HOME}"

echo "=== 稳定性测试 (3分钟) ==="
echo "开始时间: $(date)"
echo ""

# 清理旧进程
pkill -f detector_node 2>/dev/null || true
pkill -f camera_node 2>/dev/null || true
pkill -f comm_node 2>/dev/null || true
sleep 1

# 启动节点函数
start_node() {
    local name="$1"
    local cmd="$2"
    local logfile="$3"
    echo "启动 $name..."
    nohup $cmd > "$logfile" 2>&1 &
    echo $! > "/tmp/${name}_pid"
    echo "   PID: $(cat /tmp/${name}_pid)"
}

# 检查节点状态
check_node() {
    local name="$1"
    local pidfile="/tmp/${name}_pid"
    if [ -f "$pidfile" ]; then
        local pid=$(cat "$pidfile")
        if kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
    fi
    return 1
}

# 重启节点
restart_node() {
    local name="$1"
    local cmd="$2"
    local logfile="$3"
    echo "!!! $name 已退出，正在重启..."
    pkill -f "$name" 2>/dev/null || true
    sleep 0.5
    start_node "$name" "$cmd" "$logfile"
}

# 启动所有节点
CAM_LOG="${ROS_LOG_DIR}/camera_test.log"
DET_LOG="${ROS_LOG_DIR}/detector_test.log"
COMM_LOG="${ROS_LOG_DIR}/comm_test.log"

start_node "camera_node" "./build/install/bin/camera_node --config ./build/install/config/system_config_ros2.xml --camera-config ./build/install/config/camera_config.xml" "$CAM_LOG"
sleep 3

start_node "detector_node" "./build/install/bin/detector_node --config ./build/install/config/system_config_ros2.xml --detector-config ./build/install/config/detector.xml" "$DET_LOG"
sleep 3

start_node "comm_node" "./build/install/bin/comm_node --config ./build/install/config/system_config_ros2.xml --comm-config ./build/install/config/communication.xml" "$COMM_LOG"
sleep 3

# 初始化统计
SUCCESS_COUNT=0
FAILED_COUNT=0
RESTART_COUNT=0

echo ""
echo "5. 运行中... (3分钟)"
for i in {1..180}; do
    sleep 1
    
    # 检查并重启崩溃的节点
    if ! check_node "camera_node"; then
        restart_node "camera_node" "./build/install/bin/camera_node --config ./build/install/config/system_config_ros2.xml --camera-config ./build/install/config/camera_config.xml" "$CAM_LOG"
        RESTART_COUNT=$((RESTART_COUNT+1))
    fi
    if ! check_node "detector_node"; then
        restart_node "detector_node" "./build/install/bin/detector_node --config ./build/install/config/system_config_ros2.xml --detector-config ./build/install/config/detector.xml" "$DET_LOG"
        RESTART_COUNT=$((RESTART_COUNT+1))
    fi
    if ! check_node "comm_node"; then
        restart_node "comm_node" "./build/install/bin/comm_node --config ./build/install/config/system_config_ros2.xml --comm-config ./build/install/config/communication.xml" "$COMM_LOG"
        RESTART_COUNT=$((RESTART_COUNT+1))
    fi
    
    # 每2秒执行一次服务调用测试
    if [ $((i % 2)) -eq 0 ]; then
        # 使用 manager 测试服务调用
        export ROS_LOG_DIR="${PWD}/build/install/log"
        export ROS_HOME="${ROS_LOG_DIR}/ros_home"
        
        # 运行 manager 5秒，检查输出中的成功次数
        timeout 5 ./build/install/bin/manager --config ./build/install/config/system_config_ros2.xml 2>&1 | tail -20 | grep -E "SUCCESS|FAILED" > /tmp/service_test_result.txt
        
        SUCC=$(grep -c "SUCCESS" /tmp/service_test_result.txt)
        FAIL=$(grep -c "FAILED" /tmp/service_test_result.txt)
        
        SUCCESS_COUNT=$((SUCCESS_COUNT + SUCC))
        FAILED_COUNT=$((FAILED_COUNT + FAIL))
    fi
    
    # 每30秒打印一次进度和统计
    if [ $((i % 30)) -eq 0 ]; then
        echo "   已运行 ${i}s | 成功: $SUCCESS_COUNT | 失败: $FAILED_COUNT | 重启次数: $RESTART_COUNT"
    fi
done

echo ""
echo "6. 清理..."
pkill -f camera_node 2>/dev/null || true
pkill -f detector_node 2>/dev/null || true
pkill -f comm_node 2>/dev/null || true
sleep 1

# 删除临时PID文件
rm -f /tmp/*_pid

echo ""
echo "=== 测试完成 ==="
echo "结束时间: $(date)"
echo "总成功次数: $SUCCESS_COUNT"
echo "总失败次数: $FAILED_COUNT"
echo "重启次数: $RESTART_COUNT"
echo "成功率: $((SUCCESS_COUNT * 100 / (SUCCESS_COUNT + FAILED_COUNT)))%"
