#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
#  System Inspector GUI - 启动脚本
#
#  用法:
#    ./inspector_gui.sh <pipeline-name> [base_port]
#    ./inspector_gui.sh --pipeline <pipeline.xml> [base_port]
#    ./inspector_gui.sh <path/to/pipeline.xml> [base_port]
#    ./inspector_gui.sh --help
#
#  示例:
#    ./inspector_gui.sh camera-detector-commu
#    ./inspector_gui.sh image-detector 15550
#    ./inspector_gui.sh --pipeline config/dag_image_detector.xml
#    ./inspector_gui.sh config/dag_camera_detector_comm.xml
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
INSTALL_DIR="${PROJECT_DIR}/build/install"
BINS_DIR="${INSTALL_DIR}/bins"
INSPECTOR_BIN="${BINS_DIR}/inspector/system_inspector_gui"
CONFIG_SRC_DIR="${PROJECT_DIR}/config"

PIPELINES=(
    "camera-detector-commu|dag_camera_detector_comm.xml"
    "camera-detector|dag_camera_detector.xml"
    "image-detector|dag_image_detector.xml"
    "pipeline-detect|pipeline_detect.xml"
    "pipeline-monitor|pipeline_monitor.xml"
    "example|dag_example.xml"
    "test|test_pipeline.xml"
)

show_help() {
    echo ""
    echo "System Inspector GUI - 可视化调试工具"
    echo ""
    echo "用法:"
    echo "  ./inspector_gui.sh <pipeline-name> [base_port]"
    echo "  ./inspector_gui.sh --pipeline <pipeline.xml> [base_port]"
    echo "  ./inspector_gui.sh <path/to/pipeline.xml> [base_port]"
    echo ""
    echo "可用的流水线短名称:"
    for entry in "${PIPELINES[@]}"; do
        IFS='|' read -r name config <<< "$entry"
        printf "  %-30s %s\n" "$name" "$config"
    done
    echo ""
    echo "示例:"
    echo "  ./inspector_gui.sh camera-detector-commu"
    echo "  ./inspector_gui.sh image-detector 15550"
    echo "  ./inspector_gui.sh --pipeline config/dag_image_detector.xml"
    echo ""
}

if [[ $# -eq 0 ]]; then
    show_help
    exit 0
fi

PIPELINE_XML=""
BASE_PORT="15550"

# ========== 参数解析 ==========
if [[ "$1" == "--help" || "$1" == "-h" ]]; then
    show_help
    exit 0
elif [[ "$1" == "--pipeline" ]]; then
    # --pipeline <file> [base_port]
    if [[ $# -lt 2 ]]; then
        echo "错误: --pipeline 需要指定 XML 文件路径"
        exit 1
    fi
    PIPELINE_XML="$2"
    BASE_PORT="${3:-15550}"
elif [[ "$1" == *.xml ]]; then
    # 直接传入 XML 文件路径
    PIPELINE_XML="$1"
    BASE_PORT="${2:-15550}"
else
    # 按短名称查找
    PIPELINE_NAME="$1"
    BASE_PORT="${2:-15550}"

    CONFIG_FILE=""
    for entry in "${PIPELINES[@]}"; do
        IFS='|' read -r name config <<< "$entry"
        if [[ "$name" == "$PIPELINE_NAME" ]]; then
            CONFIG_FILE="$config"
            break
        fi
    done

    if [[ -z "$CONFIG_FILE" ]]; then
        echo "错误: 未找到流水线 '$PIPELINE_NAME'"
        show_help
        exit 1
    fi

    # 查找配置文件路径
    CONFIG_PATH_INSTALL="${BINS_DIR}/inspector/${CONFIG_FILE}"
    CONFIG_PATH_SRC="${CONFIG_SRC_DIR}/${CONFIG_FILE}"

    if [[ -f "$CONFIG_PATH_INSTALL" ]]; then
        PIPELINE_XML="$CONFIG_PATH_INSTALL"
    elif [[ -f "$CONFIG_PATH_SRC" ]]; then
        PIPELINE_XML="$CONFIG_PATH_SRC"
    else
        echo "错误: 配置文件不存在"
        echo "  尝试路径1: ${CONFIG_PATH_INSTALL}"
        echo "  尝试路径2: ${CONFIG_PATH_SRC}"
        exit 1
    fi
fi

# 验证 XML 文件存在
if [[ ! -f "$PIPELINE_XML" ]]; then
    echo "错误: pipeline XML 文件不存在: ${PIPELINE_XML}"
    exit 1
fi

if [[ ! -x "$INSPECTOR_BIN" ]]; then
    echo "错误: system_inspector_gui 不存在或不可执行"
    echo "  路径: ${INSPECTOR_BIN}"
    echo "  请先执行 build.sh 编译项目 (需要 -DBUILD_GUI=ON)"
    exit 1
fi

# 设置动态库路径
if [[ "$(uname)" == "Darwin" ]]; then
    export DYLD_LIBRARY_PATH="${INSTALL_DIR}/lib:${BINS_DIR}/lib:${DYLD_LIBRARY_PATH:-}"
else
    export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${BINS_DIR}/lib:${LD_LIBRARY_PATH:-}"
fi

echo "启动 System Inspector GUI..."
echo "  Pipeline: ${PIPELINE_XML}"
echo "  Base Port: ${BASE_PORT}"
echo ""

exec "${INSPECTOR_BIN}" "${PIPELINE_XML}" "${BASE_PORT}"
