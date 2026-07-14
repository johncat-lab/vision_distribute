#!/bin/bash

# 大数据性能测试脚本 - 运行100次测试
# 测试图片: test.png (4096×3000, ~35MB)

TEST_DIR="/home/ch/johncat/vision_distribute/build/tests"
IMAGE_TEST="$TEST_DIR/perf_test_image"
TEST_IMAGE="/home/ch/johncat/vision_distribute/test.png"

echo "=============================================="
echo "  大数据性能测试 - 运行100次"
echo "=============================================="
echo ""
echo "测试时间: $(date)"
echo "测试图片: $TEST_IMAGE"
echo "图片信息: 4096×3000 像素, 约 35MB"
echo "测试次数: 100"
echo ""

# 初始化结果数组
declare -a zmq_frames=()
declare -a zmq_latencies=()
declare -a ros2_frames=()
declare -a ros2_latencies=()

# 创建临时目录
mkdir -p /tmp/perf_test_results

# 运行100次测试
for i in $(seq 1 100); do
    echo -ne "测试进度: $i/100\r"
    
    # ZeroMQ 测试
    $IMAGE_TEST --zmq --subscriber --duration 1 --fps 10 > /tmp/perf_test_results/zmq_sub_$i.log 2>&1 &
    SUB_PID=$!
    sleep 1
    $IMAGE_TEST --zmq --publisher --duration 1 --fps 10 --image $TEST_IMAGE > /tmp/perf_test_results/zmq_pub_$i.log 2>&1
    sleep 2
    kill $SUB_PID 2>/dev/null
    wait $SUB_PID 2>/dev/null
    
    # 提取结果
    zmq_frame=$(grep "接收帧数:" /tmp/perf_test_results/zmq_sub_$i.log | awk '{print $2}')
    zmq_lat=$(grep "平均延迟:" /tmp/perf_test_results/zmq_sub_$i.log | awk '{print $2}')
    zmq_frames+=($zmq_frame)
    zmq_latencies+=($zmq_lat)
    
    # ROS2 测试
    export ROS_LOG_DIR=/tmp/ros_logs
    mkdir -p $ROS_LOG_DIR
    
    $IMAGE_TEST --ros2 --subscriber --duration 1 --fps 10 > /tmp/perf_test_results/ros2_sub_$i.log 2>&1 &
    SUB_PID=$!
    sleep 2
    $IMAGE_TEST --ros2 --publisher --duration 1 --fps 10 --image $TEST_IMAGE > /tmp/perf_test_results/ros2_pub_$i.log 2>&1
    sleep 3
    kill $SUB_PID 2>/dev/null
    wait $SUB_PID 2>/dev/null
    
    # 提取结果
    ros2_frame=$(grep "接收帧数:" /tmp/perf_test_results/ros2_sub_$i.log | awk '{print $2}')
    ros2_lat=$(grep "平均延迟:" /tmp/perf_test_results/ros2_sub_$i.log | awk '{print $2}')
    ros2_frames+=($ros2_frame)
    ros2_latencies+=($ros2_frame)
done

echo ""
echo ""
echo "=============================================="
echo "              100次测试结果汇总"
echo "=============================================="
echo ""

# 计算 ZeroMQ 统计
zmq_total=0
zmq_success=0
zmq_lat_sum=0
for f in "${zmq_frames[@]}"; do
    zmq_total=$((zmq_total + f))
    if [ $f -gt 0 ]; then zmq_success=$((zmq_success + 1)); fi
done
zmq_avg_frames=$(echo "scale=2; $zmq_total / 100" | bc)
zmq_success_rate=$(echo "scale=2; $zmq_success / 100 * 100" | bc)

echo "ZeroMQ 100次测试统计:"
echo "  总接收帧数: $zmq_total"
echo "  平均每次帧数: $zmq_avg_frames"
echo "  成功次数(接收到数据): $zmq_success/100"
echo "  成功率: ${zmq_success_rate}%"

# 计算 ROS2 统计
ros2_total=0
ros2_success=0
for f in "${ros2_frames[@]}"; do
    ros2_total=$((ros2_total + f))
    if [ $f -gt 0 ]; then ros2_success=$((ros2_success + 1)); fi
done
ros2_avg_frames=$(echo "scale=2; $ros2_total / 100" | bc)
ros2_success_rate=$(echo "scale=2; $ros2_success / 100 * 100" | bc)

echo ""
echo "ROS2 100次测试统计:"
echo "  总接收帧数: $ros2_total"
echo "  平均每次帧数: $ros2_avg_frames"
echo "  成功次数(接收到数据): $ros2_success/100"
echo "  成功率: ${ros2_success_rate}%"

# 保存详细结果到CSV
echo ""
echo "保存详细结果到 bigdata_100x_results.csv..."
echo "测试次数,ZeroMQ帧数,ROS2帧数" > bigdata_100x_results.csv
for i in $(seq 0 99); do
    echo "$((i+1)),${zmq_frames[$i]},${ros2_frames[$i]}" >> bigdata_100x_results.csv
done

echo ""
echo "=============================================="
echo "测试完成！结果已保存到 bigdata_100x_results.csv"
echo "=============================================="