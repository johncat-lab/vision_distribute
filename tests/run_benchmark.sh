#!/bin/bash
# ============================================================
# run_benchmark.sh - ROS2 vs ZeroMQ 通信效率基准测试一键脚本
#
# 用法:
#   ./run_benchmark.sh [multiproc|inproc|all] [--count N] [--image PATH]
#
# 默认:
#   ./run_benchmark.sh multiproc
#
# 示例:
#   ./run_benchmark.sh multiproc --count 3000 --image /path/to/test.png
#   ./run_benchmark.sh inproc --count 200
#   ./run_benchmark.sh all --image ../test.png
# ============================================================

set -euo pipefail

# ============ 路径配置 ============
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build/tests"
BENCH_INPROC="$BUILD_DIR/bench_inproc"
BENCH_PUB="$BUILD_DIR/bench_pub"
BENCH_SUB="$BUILD_DIR/bench_sub"
BENCH_SVC="$BUILD_DIR/bench_service"

# ============ 默认参数 ============
MODE="multiproc"
COUNT_SMALL=5000
COUNT_LARGE=200
COUNT_SVC=1000
INTERVAL_SMALL=1000    # us
INTERVAL_LARGE=50000   # us
IMAGE_PATH=""
BASE_PORT=16000

# ============ 子进程跟踪 ============
CHILD_PIDS=()

cleanup() {
    echo ""
    echo "[cleanup] 正在终止子进程..."
    for pid in "${CHILD_PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null
    echo "[cleanup] 完成"
    exit 1
}
trap cleanup SIGINT SIGTERM

# ============ 参数解析 ============
while [[ $# -gt 0 ]]; do
    case "$1" in
        multiproc|inproc|all)
            MODE="$1"; shift ;;
        --count)
            COUNT_SMALL="$2"; COUNT_LARGE=$(( $2 / 25 )); COUNT_SVC="$2"; shift 2 ;;
        --count-small)
            COUNT_SMALL="$2"; shift 2 ;;
        --count-large)
            COUNT_LARGE="$2"; shift 2 ;;
        --count-svc)
            COUNT_SVC="$2"; shift 2 ;;
        --image)
            IMAGE_PATH="$2"; shift 2 ;;
        --interval-small)
            INTERVAL_SMALL="$2"; shift 2 ;;
        --interval-large)
            INTERVAL_LARGE="$2"; shift 2 ;;
        --base-port)
            BASE_PORT="$2"; shift 2 ;;
        --help|-h)
            echo "用法: $0 [multiproc|inproc|all] [--count N] [--image PATH]"
            echo "  multiproc  多进程测试 (默认)"
            echo "  inproc     单进程冒烟测试"
            echo "  all        全部测试"
            echo "  --count N          小数据消息数 (默认 $COUNT_SMALL)"
            echo "  --count-large N    大数据消息数 (默认 $COUNT_LARGE)"
            echo "  --count-svc N      Service 调用次数 (默认 $COUNT_SVC)"
            echo "  --image PATH       大数据测试图片路径 (默认: 生成 640x480 数据)"
            echo "  --interval-small N 小数据发送间隔(us, 默认 $INTERVAL_SMALL)"
            echo "  --interval-large N 大数据发送间隔(us, 默认 $INTERVAL_LARGE)"
            echo "  --base-port PORT   ZMQ 基准端口 (默认 $BASE_PORT)"
            exit 0 ;;
        *)
            echo "未知参数: $1"; exit 1 ;;
    esac
done

# ============ 输出目录 ============
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_DIR="$SCRIPT_DIR/bench_results/${TIMESTAMP}"
mkdir -p "$OUTPUT_DIR/logs"
echo "=============================================="
echo "  ROS2 vs ZeroMQ 通信效率基准测试"
echo "=============================================="
echo "  模式:     $MODE"
echo "  输出目录: $OUTPUT_DIR"
echo "  小数据:   $COUNT_SMALL 条, 间隔 ${INTERVAL_SMALL}us"
echo "  大数据:   $COUNT_LARGE 条, 间隔 ${INTERVAL_LARGE}us"
echo "  Service:  $COUNT_SVC 次"
[ -n "$IMAGE_PATH" ] && echo "  图片:     $IMAGE_PATH"
echo "=============================================="

# ============ 二进制校验 ============
check_binary() {
    if [ ! -x "$1" ]; then
        echo "ERROR: 找不到可执行文件: $1"
        echo "请先构建: cd $PROJECT_DIR/build && cmake .. && make"
        exit 1
    fi
}

# ============ ROS2 环境检查 ============
check_ros2() {
    if [ -f /opt/ros/humble/setup.bash ]; then
        (source /opt/ros/humble/setup.bash >/dev/null 2>&1) || true
    fi
    # 加载项目本地环境 (vision_interfaces 类型支持库)
    if [ -f "$PROJECT_DIR/build/local_setup.bash" ]; then
        (source "$PROJECT_DIR/build/local_setup.bash" >/dev/null 2>&1) || true
    fi
    # 确保 vision_interfaces 的 fastrtps 类型支持库可被找到
    if [ -d "$PROJECT_DIR/build/vision_interfaces" ]; then
        export LD_LIBRARY_PATH="$PROJECT_DIR/build/vision_interfaces:${LD_LIBRARY_PATH:-}"
    fi
    if ! command -v ros2 &>/dev/null && [ ! -f /opt/ros/*/setup.bash ]; then
        echo "WARNING: ROS2 环境不可用, 将跳过 ROS2 测试"
        return 1
    fi
    return 0
}

HAS_ROS2=true
check_ros2 || HAS_ROS2=false

# ============ 多进程 Pub-Sub 测试函数 ============
run_multiproc_pubsub() {
    local transport="$1"
    local mode="$2"    # small | large
    local count="$3"
    local interval="$4"
    local image_arg=""

    echo ""
    echo "--- [$transport] Pub-Sub $mode ($count 条) ---"

    if [ "$mode" = "large" ] && [ -n "$IMAGE_PATH" ]; then
        image_arg="--image $IMAGE_PATH"
    fi

    check_binary "$BENCH_SUB"
    check_binary "$BENCH_PUB"

    # 启动 subscriber (后台)
    # 动态计算 timeout：发送时间 + 额外缓冲时间
    # timeout_ms = count * interval_us / 1000 + 30000ms 缓冲
    local send_time_ms=$(( count * interval / 1000 ))
    local timeout_ms=$(( send_time_ms + 30000 ))
    echo "  预计发送时间: ${send_time_ms}ms, timeout: ${timeout_ms}ms"

    "$BENCH_SUB" "--$transport" --mode "$mode" \
        --expected "$count" \
        --output-dir "$OUTPUT_DIR" \
        --base-port "$BASE_PORT" \
        --timeout-ms "$timeout_ms" \
        > "$OUTPUT_DIR/logs/${transport}_sub_${mode}.log" 2>&1 &
    local sub_pid=$!
    CHILD_PIDS+=("$sub_pid")

    # 等待 subscriber 就绪
    echo "  等待 subscriber 启动..."
    sleep 3

    # 启动 publisher (前台)
    "$BENCH_PUB" "--$transport" --mode "$mode" \
        --total "$count" \
        --interval-us "$interval" \
        --base-port "$BASE_PORT" \
        $image_arg \
        > "$OUTPUT_DIR/logs/${transport}_pub_${mode}.log" 2>&1

    echo "  Publisher 完成, 等待 subscriber 收齐..."

    # 等待 subscriber 完成 (最多再等 30s)
    local wait_count=0
    while kill -0 "$sub_pid" 2>/dev/null && [ $wait_count -lt 60 ]; do
        sleep 0.5
        ((wait_count++)) || true
    done

    # 如果 subscriber 还在运行，终止它
    if kill -0 "$sub_pid" 2>/dev/null; then
        echo "  subscriber 超时, 正在终止..."
        kill "$sub_pid" 2>/dev/null || true
    fi
    wait "$sub_pid" 2>/dev/null || true

    # 从 CHILD_PIDS 中移除
    CHILD_PIDS=("${CHILD_PIDS[@]/$sub_pid}")

    echo "  [$transport] Pub-Sub $mode 完成"
    echo "  日志: $OUTPUT_DIR/logs/${transport}_{pub,sub}_${mode}.log"

    # 提取 JSON 摘要
    grep "__RESULT_JSON__" "$OUTPUT_DIR/logs/${transport}_sub_${mode}.log" 2>/dev/null || true
}

# ============ 多进程 Service 测试函数 ============
run_multiproc_service() {
    local transport="$1"
    local count="$2"

    echo ""
    echo "--- [$transport] Service 调用 ($count 次) ---"

    check_binary "$BENCH_SVC"

    # 启动 server (后台)
    "$BENCH_SVC" "--$transport" --role server \
        --base-port "$BASE_PORT" \
        > "$OUTPUT_DIR/logs/${transport}_svc_server.log" 2>&1 &
    local srv_pid=$!
    CHILD_PIDS+=("$srv_pid")

    echo "  等待 server 启动..."
    sleep 3

    # 启动 client (前台)
    "$BENCH_SVC" "--$transport" --role client \
        --count "$count" \
        --output-dir "$OUTPUT_DIR" \
        --base-port "$BASE_PORT" \
        > "$OUTPUT_DIR/logs/${transport}_svc_client.log" 2>&1

    echo "  Client 完成, 正在关闭 server..."

    kill "$srv_pid" 2>/dev/null || true
    wait "$srv_pid" 2>/dev/null || true
    CHILD_PIDS=("${CHILD_PIDS[@]/$srv_pid}")

    echo "  [$transport] Service 完成"

    grep "__RESULT_JSON__" "$OUTPUT_DIR/logs/${transport}_svc_client.log" 2>/dev/null || true
}

# ============ 单进程冒烟测试 ============
run_inproc() {
    check_binary "$BENCH_INPROC"

    echo ""
    echo "=== 单进程冒烟测试 ==="

    local transport_arg="--all"
    if [ "$HAS_ROS2" = "false" ]; then
        transport_arg="--zmq"
        echo "  (ROS2 不可用, 仅测试 ZMQ)"
    fi

    local image_arg=""
    [ -n "$IMAGE_PATH" ] && image_arg="--image $IMAGE_PATH"

    "$BENCH_INPROC" $transport_arg \
        --count "$COUNT_SMALL" \
        --output-dir "$OUTPUT_DIR" \
        --base-port "$BASE_PORT" \
        $image_arg \
        2>&1 | tee "$OUTPUT_DIR/logs/inproc.log"
}

# ============ 汇总打印 ============
print_summary() {
    echo ""
    echo "=============================================="
    echo "  测试结果汇总"
    echo "=============================================="

    if ls "$OUTPUT_DIR"/*summary*.csv 1>/dev/null 2>&1; then
        echo ""
        echo "Summary CSV 文件:"
        for f in "$OUTPUT_DIR"/*summary*.csv; do
            echo "  - $(basename "$f")"
            # 打印内容（跳过表头）
            tail -n +2 "$f" | while IFS=, read -r name transport expected received lost loss_rate \
                    avg min max p50 p95 p99 msg_s mbps elapsed payload; do
                printf "    %-20s %-8s recv=%-5s lost=%-4s loss=%-6s avg=%-10s p99=%-10s msg/s=%-8s\n" \
                    "$name" "$transport" "$received" "$lost" "${loss_rate}%" "${avg}us" "${p99}us" "$msg_s"
            done
        done
    fi

    echo ""
    echo "Detail CSV 文件:"
    if ls "$OUTPUT_DIR"/*detail*.csv 1>/dev/null 2>&1; then
        for f in "$OUTPUT_DIR"/*detail*.csv; do
            local lines=$(wc -l < "$f")
            echo "  - $(basename "$f") ($(( lines - 1 )) 条记录)"
        done
    fi

    echo ""
    echo "日志目录: $OUTPUT_DIR/logs/"
    echo "=============================================="
}

# ============ 主流程 ============
case "$MODE" in
    multiproc)
        # Pub-Sub 小数据
        run_multiproc_pubsub zmq  small "$COUNT_SMALL" "$INTERVAL_SMALL"
        [ "$HAS_ROS2" = "true" ] && run_multiproc_pubsub ros2 small "$COUNT_SMALL" "$INTERVAL_SMALL"

        # Pub-Sub 大数据
        run_multiproc_pubsub zmq  large "$COUNT_LARGE" "$INTERVAL_LARGE"
        [ "$HAS_ROS2" = "true" ] && run_multiproc_pubsub ros2 large "$COUNT_LARGE" "$INTERVAL_LARGE"

        # Service
        run_multiproc_service zmq  "$COUNT_SVC"
        [ "$HAS_ROS2" = "true" ] && run_multiproc_service ros2 "$COUNT_SVC"
        ;;

    inproc)
        run_inproc
        ;;

    all)
        run_inproc
        echo ""
        echo "=============================================="
        echo "  切换到多进程测试..."
        echo "=============================================="
        # Pub-Sub 小数据
        run_multiproc_pubsub zmq  small "$COUNT_SMALL" "$INTERVAL_SMALL"
        [ "$HAS_ROS2" = "true" ] && run_multiproc_pubsub ros2 small "$COUNT_SMALL" "$INTERVAL_SMALL"
        # Pub-Sub 大数据
        run_multiproc_pubsub zmq  large "$COUNT_LARGE" "$INTERVAL_LARGE"
        [ "$HAS_ROS2" = "true" ] && run_multiproc_pubsub ros2 large "$COUNT_LARGE" "$INTERVAL_LARGE"
        # Service
        run_multiproc_service zmq  "$COUNT_SVC"
        [ "$HAS_ROS2" = "true" ] && run_multiproc_service ros2 "$COUNT_SVC"
        ;;
esac

print_summary

echo ""
echo "全部测试完成！"
echo "输出目录: $OUTPUT_DIR"
