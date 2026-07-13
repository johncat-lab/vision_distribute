#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
#  Vision Distribute - DAG 流水线统一启动脚本
#
#  用法:
#    ./launch.sh <pipeline-name> [options]
#    ./launch.sh --help          查看所有可用的流水线配置
#    ./launch.sh --list          同上
#
#  示例:
#    ./launch.sh camera-detector-commu           # 相机+检测+通信 完整流水线
#    ./launch.sh camera-detector                  # 相机+检测（无通信）
#    ./launch.sh image-detector                   # 图像发布+检测+通信
#    ./launch.sh camera-detector-commu --dry-run  # 仅打印启动命令，不实际执行
#    ./launch.sh camera-detector-commu --parallel # 按层并行启动
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
INSTALL_DIR="${PROJECT_DIR}/build/install"
BINS_DIR="${INSTALL_DIR}/bins"
DAG_LAUNCHER="${BINS_DIR}/dag_launcher/dag_launcher"
CONFIG_SRC_DIR="${PROJECT_DIR}/config"

# 额外传递给 dag_launcher 的参数
EXTRA_ARGS=()

# =============================================================================
#  流水线注册表
#  格式: "短名称|配置文件名|描述"
#  新增流水线只需在此处追加一行
# =============================================================================
PIPELINES=(
    "camera-detector-commu|dag_camera_detector_comm.xml|相机采集 → 目标检测 → TCP通信（完整流水线）"
    "camera-detector|dag_camera_detector.xml|相机采集 → 目标检测（无通信节点）"
    "image-detector|dag_image_detector.xml|图像发布 → 目标检测 → TCP通信（离线图片测试）"
    "pipeline-detect|pipeline_detect.xml|图像发布 → 目标检测 → 通信（精简格式）"
    "pipeline-monitor|pipeline_monitor.xml|图像发布 → Manager GUI 监控"
    "example|dag_example.xml|完整示例：camera_left → detector_left → comm"
    "test|test_pipeline.xml|测试流水线：cam_left → det_left → comm"
)

# =============================================================================
#  帮助信息
# =============================================================================
show_help() {
    echo ""
    echo "Vision Distribute - DAG 流水线统一启动脚本"
    echo ""
    echo "用法:"
    echo "  ./launch.sh <pipeline-name> [dag_launcher options]"
    echo ""
    echo "可用的流水线配置:"
    echo "  ──────────────────────────────────────────────────────────────────────────"
    printf "  %-30s %s\n" "名称" "说明"
    echo "  ──────────────────────────────────────────────────────────────────────────"
    for entry in "${PIPELINES[@]}"; do
        IFS='|' read -r name config desc <<< "$entry"
        printf "  %-30s %s\n" "$name" "$desc"
    done
    echo "  ──────────────────────────────────────────────────────────────────────────"
    echo ""
    echo "传输后端选项:"
    echo "  --ros2              使用 ROS2 传输后端（自动 source ROS2 环境）"
    echo "  --transport <type>  指定传输后端: zeromq | ros2 | zenoh"
    echo ""
    echo "选项 (透传给 dag_launcher):"
    echo "  --dry-run           仅打印启动命令，不实际启动"
    echo "  --parallel          按层并行启动（同层节点无依赖，同时启动）"
    echo "  --startup-delay <ms>  节点间/层间启动间隔（毫秒，默认 200）"
    echo "  --max-retries <n>   启动失败重试次数（默认 3）"
    echo "  --no-auto-restart   关闭崩溃自动重启功能"
    echo "  --status-report     启动过程中定期打印状态报告"
    echo ""
    echo "其他选项:"
    echo "  --help, -h          显示此帮助"
    echo "  --list              列出所有可用的流水线配置（同 --help）"
    echo ""
    echo "示例:"
    echo "  ./launch.sh camera-detector-commu"
    echo "  ./launch.sh camera-detector-commu --dry-run"
    echo "  ./launch.sh camera-detector-commu --ros2     # 使用 ROS2 传输后端"
    echo "  ./launch.sh camera-detector-commu --parallel --status-report"
    echo "  ./launch.sh image-detector --parallel"
    echo ""
}

# =============================================================================
#  根据短名称查找流水线配置
# =============================================================================
find_pipeline() {
    local target="$1"
    for entry in "${PIPELINES[@]}"; do
        IFS='|' read -r name config desc <<< "$entry"
        if [[ "$name" == "$target" ]]; then
            echo "$config"
            return 0
        fi
    done
    return 1
}

# =============================================================================
#  解析配置文件中的节点信息（用于显示摘要）
# =============================================================================
print_pipeline_summary() {
    local config_file="$1"
    echo ""
    echo "========================================"
    echo "  DAG Pipeline: $(basename "$config_file" .xml)"
    echo "========================================"
    echo "  配置文件: ${config_file}"
    echo "  Launcher:  ${DAG_LAUNCHER}"
    echo "  Bins目录:  ${BINS_DIR}"
    echo ""
}

# =============================================================================
#  主逻辑
# =============================================================================

# 无参数时显示帮助
if [[ $# -eq 0 ]]; then
    show_help
    exit 0
fi

PIPELINE_NAME=""

# 解析第一个参数（流水线名称 或 --help/--list）
case "$1" in
    --help|-h)
        show_help
        exit 0
        ;;
    --list)
        show_help
        exit 0
        ;;
    -*)
        echo "错误: 未知选项 '$1'"
        echo "运行 './launch.sh --help' 查看用法"
        exit 1
        ;;
    *)
        PIPELINE_NAME="$1"
        shift
        ;;
esac

# 收集剩余参数透传给 dag_launcher
USE_ROS2=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ros2)
            USE_ROS2=true
            EXTRA_ARGS+=("--transport" "ros2")
            ;;
        *)
            EXTRA_ARGS+=("$1")
            ;;
    esac
    shift
done

# 查找对应的配置文件
CONFIG_FILE=$(find_pipeline "$PIPELINE_NAME") || {
    echo "错误: 未找到流水线 '$PIPELINE_NAME'"
    echo ""
    echo "可用的流水线:"
    for entry in "${PIPELINES[@]}"; do
        IFS='|' read -r name config desc <<< "$entry"
        printf "  %-30s %s\n" "$name" "$desc"
    done
    echo ""
    echo "运行 './launch.sh --help' 查看详细用法"
    exit 1
}

# 确定配置文件的实际路径
# 优先使用 install 目录下的副本，否则使用 config 源目录
CONFIG_PATH_IN_INSTALL="${BINS_DIR}/dag_launcher/${CONFIG_FILE}"
CONFIG_PATH_IN_SRC="${CONFIG_SRC_DIR}/${CONFIG_FILE}"

if [[ -f "$CONFIG_PATH_IN_INSTALL" ]]; then
    PIPELINE_XML="$CONFIG_PATH_IN_INSTALL"
elif [[ -f "$CONFIG_PATH_IN_SRC" ]]; then
    PIPELINE_XML="$CONFIG_PATH_IN_SRC"
else
    echo "错误: 配置文件不存在"
    echo "  尝试路径1: ${CONFIG_PATH_IN_INSTALL}"
    echo "  尝试路径2: ${CONFIG_PATH_IN_SRC}"
    exit 1
fi

# 检查 dag_launcher 是否存在
if [[ ! -x "$DAG_LAUNCHER" ]]; then
    echo "错误: dag_launcher 不存在或不可执行"
    echo "  路径: ${DAG_LAUNCHER}"
    echo "  请先执行 build.sh 编译项目"
    exit 1
fi

# 打印摘要
print_pipeline_summary "$PIPELINE_XML"

# 设置动态库路径（macOS 使用 DYLD_LIBRARY_PATH，Linux 使用 LD_LIBRARY_PATH）
if [[ "$(uname)" == "Darwin" ]]; then
    export DYLD_LIBRARY_PATH="${INSTALL_DIR}/lib:${BINS_DIR}/lib:${DYLD_LIBRARY_PATH:-}"
else
    export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${BINS_DIR}/lib:${LD_LIBRARY_PATH:-}"
fi

# 如果使用 ROS2 传输后端，自动 source ROS2 环境并配置 Fast DDS
if [[ "$USE_ROS2" == "true" ]]; then
    if [[ -f "/opt/ros/humble/setup.bash" ]]; then
        set +u  # ROS2 setup.bash 引用未定义变量，临时关闭 nounset
        source /opt/ros/humble/setup.bash
        set -u
        echo "[launch.sh] 已 source ROS2 Humble 环境"
    elif [[ -f "/opt/ros/jazzy/setup.bash" ]]; then
        set +u
        source /opt/ros/jazzy/setup.bash
        set -u
        echo "[launch.sh] 已 source ROS2 Jazzy 环境"
    else
        echo "警告: 未找到 ROS2 setup.bash，请手动 source ROS2 环境"
    fi

    # Fast DDS: 增大 UDP buffer 以支持大帧图像 (~12MB)
    FASTDDS_PROFILES="${CONFIG_SRC_DIR}/fastdds_large_buffer.xml"
    if [[ -f "$FASTDDS_PROFILES" ]]; then
        export FASTRTPS_DEFAULT_PROFILES_FILE="$FASTDDS_PROFILES"
        echo "[launch.sh] Fast DDS 配置已加载: $FASTDDS_PROFILES"
    fi
fi

# 构建命令
CMD=("${DAG_LAUNCHER}" "--pipeline" "${PIPELINE_XML}" "--bins-dir" "${BINS_DIR}")

# 追加额外参数
if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
    CMD+=("${EXTRA_ARGS[@]}")
fi

echo "执行命令:"
echo "  ${CMD[*]}"
echo ""

# 执行 dag_launcher
exec "${CMD[@]}"
