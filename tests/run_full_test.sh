#!/bin/bash

# 完整多进程性能测试脚本
# 测试内容：
# 1. 小数据 Pub/Sub 性能（ZeroMQ vs ROS2）
# 2. 大数据（图像）Pub/Sub 性能（ZeroMQ vs ROS2）

TEST_DIR="/home/ch/johncat/vision_distribute/build/tests"
PUBLISHER="$TEST_DIR/perf_publisher"
SUBSCRIBER="$TEST_DIR/perf_subscriber"
IMAGE_TEST="$TEST_DIR/perf_test_image"

echo "=============================================="
echo "  ZeroMQ vs ROS2 多进程性能测试 - 完整报告"
echo "=============================================="
echo ""
echo "测试时间: $(date)"
echo ""

# ================ 测试1: 小数据 Pub/Sub ================
echo "=== 测试1: 小数据 Pub/Sub (640x480 灰度图像) ==="
echo ""

# ZeroMQ 小数据测试
echo "[ZeroMQ] 启动 Subscriber..."
$SUBSCRIBER --zmq --output zmq_small_latency.csv > zmq_small_sub.log 2>&1 &
SUB_PID=$!
sleep 2

echo "[ZeroMQ] 启动 Publisher..."
$PUBLISHER --zmq --duration 5 --fps 30 > zmq_small_pub.log 2>&1

echo "[ZeroMQ] 等待完成..."
sleep 3
kill $SUB_PID 2>/dev/null
wait $SUB_PID 2>/dev/null

# ROS2 小数据测试
echo "[ROS2] 启动 Subscriber..."
export ROS_LOG_DIR=/tmp/ros_logs
mkdir -p $ROS_LOG_DIR
$SUBSCRIBER --ros2 --output ros2_small_latency.csv > ros2_small_sub.log 2>&1 &
SUB_PID=$!
sleep 3

echo "[ROS2] 启动 Publisher..."
$PUBLISHER --ros2 --duration 5 --fps 30 > ros2_small_pub.log 2>&1

echo "[ROS2] 等待完成..."
sleep 3
kill $SUB_PID 2>/dev/null
wait $SUB_PID 2>/dev/null

# ================ 测试2: 大数据（图像）Pub/Sub ================
echo ""
echo "=== 测试2: 大数据 Pub/Sub (使用 test.png 真实图片) ==="
echo ""

TEST_IMAGE="/home/ch/johncat/vision_distribute/test.png"

# ZeroMQ 大数据测试
echo "[ZeroMQ] 启动图像 Subscriber..."
$IMAGE_TEST --zmq --subscriber --duration 5 --fps 15 > zmq_image_sub.log 2>&1 &
SUB_PID=$!
sleep 2

echo "[ZeroMQ] 启动图像 Publisher..."
$IMAGE_TEST --zmq --publisher --duration 5 --fps 15 --image $TEST_IMAGE > zmq_image_pub.log 2>&1

echo "[ZeroMQ] 等待完成..."
sleep 3
kill $SUB_PID 2>/dev/null
wait $SUB_PID 2>/dev/null

# ROS2 大数据测试
echo "[ROS2] 启动图像 Subscriber..."
$IMAGE_TEST --ros2 --subscriber --duration 5 --fps 15 > ros2_image_sub.log 2>&1 &
SUB_PID=$!
sleep 3

echo "[ROS2] 启动图像 Publisher..."
$IMAGE_TEST --ros2 --publisher --duration 5 --fps 15 --image $TEST_IMAGE > ros2_image_pub.log 2>&1

echo "[ROS2] 等待完成..."
sleep 3
kill $SUB_PID 2>/dev/null
wait $SUB_PID 2>/dev/null

# ================ 生成报告 ================
echo ""
echo "=============================================="
echo "                  测试结果报告"
echo "=============================================="

echo ""
echo "=== 小数据测试结果 ==="
echo "------------------------"
echo "ZeroMQ:"
grep -E "接收帧数|平均延迟|P99|吞吐量" zmq_small_sub.log
echo ""
echo "ROS2:"
grep -E "接收帧数|平均延迟|P99|吞吐量" ros2_small_sub.log

echo ""
echo "=== 大数据(图像)测试结果 ==="
echo "------------------------"
echo "ZeroMQ:"
grep -E "接收帧数|平均帧率|平均延迟|P99|最大延迟" zmq_image_sub.log
echo ""
echo "ROS2:"
grep -E "接收帧数|平均帧率|平均延迟|P99|最大延迟" ros2_image_sub.log

echo ""
echo "=============================================="
echo "文件输出:"
echo "  - 小数据延迟: zmq_small_latency.csv, ros2_small_latency.csv"
echo "  - 图像延迟: zmq_image_latency.csv, ros2_image_latency.csv"
echo "  - 日志文件: *pub.log, *sub.log"
echo "=============================================="