#!/usr/bin/env bash
# =============================================================================
# vision_distribute 环境安装与检测脚本
#
# 支持平台: Ubuntu / Debian x86_64 (22.04 LTS 推荐)
#
# 用法:
#   ./setup_env.sh check              仅检查环境是否就绪
#   ./setup_env.sh install            检查 + 自动安装缺失依赖
#   ./setup_env.sh build [args]       检查 + 安装 + 构建 (透传参数给 build.sh)
#   ./setup_env.sh clean              清理构建目录
#   ./setup_env.sh run [name] [args]  运行指定可执行文件 (默认 camera_node)
#   ./setup_env.sh env                输出可 source 的环境变量
#   ./setup_env.sh help               显示帮助
#
# 可被外部覆盖的变量:
#   HIK_SDK_PATH    海康 MVS SDK 安装路径 (默认 /opt/MVS)
#   ORT_VERSION     ONNX Runtime 版本     (默认 1.17.3)
#   ORT_SDK_PATH    ONNX Runtime 目标路径 (默认 ./deps/onnxruntime-linux-x64-<ver>)
#   NCNN_SDK_PATH   NCNN SDK 目标路径     (默认 ./deps/ncnn-linux-x64)
#   NCNN_VERSION    NCNN submodule 版本    (默认 20240102，对应 git tag)
#   ROS2_SETUP      ROS2 setup.bash 路径  (默认 /opt/ros/humble/setup.bash)
# =============================================================================

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

OS_NAME="$(uname -s)"
ARCH_NAME="$(uname -m)"

ORT_VERSION="${ORT_VERSION:-1.17.3}"
NCNN_VERSION="${NCNN_VERSION:-20240102}"
HIK_SDK_PATH="${HIK_SDK_PATH:-/opt/MVS}"
ROS2_SETUP="${ROS2_SETUP:-/opt/ros/humble/setup.bash}"

ORT_PKG="onnxruntime-linux-x64-${ORT_VERSION}"
ORT_TGZ="${ORT_PKG}.tgz"
ORT_URL="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_TGZ}"

DEPS_DIR="${SCRIPT_DIR}/deps"
ORT_SDK_PATH="${ORT_SDK_PATH:-${DEPS_DIR}/${ORT_PKG}}"
NCNN_SDK_PATH="${NCNN_SDK_PATH:-${DEPS_DIR}/ncnn-linux-x64}"

BUILD_DIR="${SCRIPT_DIR}/build"
INSTALL_DIR="${BUILD_DIR}/install"

NCPU="$(nproc 2>/dev/null || echo 4)"

# ========== 日志工具 ==========
if [[ -t 1 ]]; then
    C_RESET="\033[0m"; C_INFO="\033[36m"; C_OK="\033[32m"
    C_WARN="\033[33m"; C_ERR="\033[31m"; C_BOLD="\033[1m"
else
    C_RESET=""; C_INFO=""; C_OK=""; C_WARN=""; C_ERR=""; C_BOLD=""
fi
log()  { printf "${C_INFO}[INFO]${C_RESET}  %s\n" "$*"; }
ok()   { printf "${C_OK}[ OK ]${C_RESET}  %s\n" "$*"; }
warn() { printf "${C_WARN}[WARN]${C_RESET}  %s\n" "$*"; }
err()  { printf "${C_ERR}[FAIL]${C_RESET}  %s\n" "$*" >&2; }
hdr()  { printf "\n${C_BOLD}==== %s ====${C_RESET}\n" "$*"; }

has_cmd() { command -v "$1" >/dev/null 2>&1; }

# ========== 环境变量导出 ==========
export_env_vars() {
    export HIK_SDK_PATH
    export ORT_SDK_PATH
    export NCNN_SDK_PATH

    local ld_paths="${HIK_SDK_PATH}/lib/64:${ORT_SDK_PATH}/lib:${NCNN_SDK_PATH}/lib:${LD_LIBRARY_PATH:-}"
    export LD_LIBRARY_PATH="$ld_paths"

    if [[ -f "$ROS2_SETUP" ]]; then
        source "$ROS2_SETUP" 2>/dev/null || true
    fi
}

print_env_script() {
    cat <<EOF
export HIK_SDK_PATH="${HIK_SDK_PATH}"
export ORT_SDK_PATH="${ORT_SDK_PATH}"
export NCNN_SDK_PATH="${NCNN_SDK_PATH}"
export LD_LIBRARY_PATH="${HIK_SDK_PATH}/lib/64:${ORT_SDK_PATH}/lib:${NCNN_SDK_PATH}/lib:\${LD_LIBRARY_PATH:-}"
EOF
    if [[ -f "$ROS2_SETUP" ]]; then
        echo "source ${ROS2_SETUP}"
    fi
}

# ========== 平台检查 ==========
check_platform() {
    hdr "平台检查"
    log "操作系统: $OS_NAME"
    log "CPU 架构: $ARCH_NAME"
    if [[ "$OS_NAME" != "Linux" ]]; then
        err "仅支持 Linux (Ubuntu 22.04+)"
        return 1
    fi
    if [[ "$ARCH_NAME" != "x86_64" ]]; then
        err "仅支持 x86_64 架构，当前为 $ARCH_NAME"
        return 1
    fi
    ok "平台受支持 (Linux x86_64)"

    # 检测发行版
    if [[ -f /etc/os-release ]]; then
        local distro
        distro=$(. /etc/os-release && echo "$PRETTY_NAME")
        log "发行版: $distro"
    fi
    return 0
}

# ========== 编译器 ==========
check_compiler() {
    hdr "编译器检查"
    if has_cmd g++; then
        local ver
        ver=$(g++ --version | head -n1)
        ok "g++ : $ver"
        # 检查版本 >= 11 (C++17 模板内 lambda 支持)
        local major
        major=$(g++ -dumpversion | cut -d. -f1)
        if [[ "$major" -lt 11 ]]; then
            warn "GCC 版本 $major < 11，部分模板特性可能编译失败 (建议 GCC 11+)"
        fi
        return 0
    fi
    err "未检测到 g++ 编译器"
    return 1
}

install_compiler() {
    if has_cmd g++; then return 0; fi
    log "安装 build-essential (GCC/G++/Make)..."
    if has_cmd apt-get; then
        sudo apt-get update -qq && sudo apt-get install -y build-essential
    elif has_cmd yum; then
        sudo yum groupinstall -y "Development Tools"
    else
        err "无法自动安装编译器，请手动安装 GCC 11+"
        return 1
    fi
}

# ========== CMake ==========
check_cmake() {
    hdr "CMake 检查"
    if has_cmd cmake; then
        local ver
        ver=$(cmake --version | head -n1)
        ok "cmake: $ver"
        # 检查版本 >= 3.16
        local major minor
        major=$(cmake --version | head -n1 | grep -oP '\d+\.\d+' | cut -d. -f1)
        minor=$(cmake --version | head -n1 | grep -oP '\d+\.\d+' | cut -d. -f2)
        if [[ "$major" -lt 3 ]] || { [[ "$major" -eq 3 ]] && [[ "$minor" -lt 16 ]]; }; then
            warn "CMake 版本 ${major}.${minor} < 3.16，可能构建失败"
        fi
        return 0
    fi
    err "未安装 cmake"
    return 1
}

install_cmake() {
    if has_cmd cmake; then return 0; fi
    log "安装 cmake..."
    if has_cmd apt-get; then
        sudo apt-get install -y cmake
    elif has_cmd yum; then
        sudo yum install -y cmake
    fi
}

# ========== pkg-config ==========
check_pkg_config() {
    if has_cmd pkg-config; then
        return 0
    fi
    warn "未安装 pkg-config (部分依赖检测需要)"
    return 1
}

install_pkg_config() {
    if has_cmd pkg-config; then return 0; fi
    if has_cmd apt-get; then
        sudo apt-get install -y pkg-config
    fi
}

# ========== OpenCV (必需) ==========
check_opencv() {
    hdr "OpenCV 检查"

    if has_cmd pkg-config && pkg-config --modversion opencv4 >/dev/null 2>&1; then
        local ver
        ver=$(pkg-config --modversion opencv4 2>/dev/null)
        ok "OpenCV ${ver} (pkg-config)"
        return 0
    elif has_cmd dpkg && dpkg -l libopencv-dev >/dev/null 2>&1; then
        ok "OpenCV (dpkg: libopencv-dev)"
        return 0
    elif [[ -f /usr/include/opencv4/opencv2/opencv.hpp ]]; then
        ok "OpenCV (头文件已安装)"
        return 0
    fi
    err "未找到 OpenCV (需要 core, imgproc, imgcodecs, highgui, calib3d)"
    return 1
}

install_opencv() {
    if check_opencv >/dev/null 2>&1; then return 0; fi
    if ! has_cmd apt-get; then
        err "仅支持 apt-get 包管理器（Ubuntu/Debian）"
        return 1
    fi
    log "安装 OpenCV (apt-get)..."
    sudo apt-get update -qq && sudo apt-get install -y libopencv-dev \
        || { err "OpenCV 安装失败"; return 1; }
    ok "OpenCV 安装完成"
    return 0
}

# ========== ZeroMQ (必需) ==========
check_zmq() {
    hdr "ZeroMQ 检查"

    # 检查 cppzmq 头文件
    local zmq_hpp=""
    for path in /usr/include/zmq.hpp /usr/local/include/zmq.hpp; do
        if [[ -f "$path" ]]; then
            zmq_hpp="$path"
            break
        fi
    done

    # 检查 libzmq
    local libzmq=""
    for path in /usr/lib/x86_64-linux-gnu/libzmq.so /usr/lib/libzmq.so /usr/local/lib/libzmq.so; do
        if [[ -f "$path" ]]; then
            libzmq="$path"
            break
        fi
    done

    if [[ -n "$zmq_hpp" ]]; then
        ok "zmq.hpp: $zmq_hpp"
    else
        err "未找到 zmq.hpp 头文件 (需要 cppzmq-dev)"
    fi

    if [[ -n "$libzmq" ]]; then
        ok "libzmq: $libzmq"
    else
        err "未找到 libzmq.so (需要 libzmq3-dev)"
    fi

    if [[ -n "$zmq_hpp" ]] && [[ -n "$libzmq" ]]; then
        return 0
    fi
    return 1
}

install_zmq() {
    if check_zmq >/dev/null 2>&1; then return 0; fi
    if ! has_cmd apt-get; then
        err "仅支持 apt-get 包管理器"
        return 1
    fi
    log "安装 ZeroMQ (libzmq3-dev + cppzmq-dev)..."
    sudo apt-get update -qq && sudo apt-get install -y libzmq3-dev cppzmq-dev \
        || { err "ZeroMQ 安装失败"; return 1; }
    ok "ZeroMQ 安装完成"
    return 0
}

# ========== 海康 MVS SDK (可选 - camera_node) ==========
check_hik_sdk() {
    hdr "海康 MVS SDK 检查 (camera_node)"
    log "查找路径: $HIK_SDK_PATH"
    if [[ ! -d "$HIK_SDK_PATH" ]]; then
        err "未找到海康 SDK 目录: $HIK_SDK_PATH"
        return 1
    fi
    local inc="$HIK_SDK_PATH/include/MvCameraControl.h"
    local lib="$HIK_SDK_PATH/lib/64/libMvCameraControl.so"
    if [[ ! -f "$inc" ]]; then
        err "缺少头文件: $inc"
        return 1
    fi
    if [[ ! -f "$lib" ]]; then
        err "缺少动态库: $lib"
        return 1
    fi
    ok "头文件: $inc"
    ok "动态库: $lib"
    return 0
}

install_hik_sdk() {
    if check_hik_sdk >/dev/null 2>&1; then return 0; fi
    warn "海康 MVS SDK 需要手动安装（官方 deb/rpm），本脚本不能自动下载。"
    cat <<EOF

  安装步骤:
    1) 访问 https://www.hikrobotics.com/cn/machinevision/service/download
       下载 Linux x86_64 版 "MVS 客户端" 安装包
    2) 使用 deb/rpm 安装，默认路径为 /opt/MVS
    3) 重新运行: ./setup_env.sh check

EOF
    return 1
}

# ========== ONNX Runtime (可选 - YOLO 检测器) ==========
check_ort() {
    hdr "ONNX Runtime 检查 (YOLO 检测器)"
    log "查找路径: $ORT_SDK_PATH"
    if [[ ! -d "$ORT_SDK_PATH" ]]; then
        err "未找到 ONNX Runtime"
        return 1
    fi
    local inc="$ORT_SDK_PATH/include/onnxruntime_cxx_api.h"
    local lib=""
    for cand in \
        "$ORT_SDK_PATH/lib/libonnxruntime.so" \
        "$ORT_SDK_PATH/lib/libonnxruntime.so.${ORT_VERSION}"; do
        if [[ -f "$cand" ]]; then lib="$cand"; break; fi
    done
    if [[ ! -f "$inc" ]]; then
        err "缺少头文件: $inc"
        return 1
    fi
    if [[ -z "$lib" ]]; then
        err "缺少动态库: libonnxruntime.so"
        return 1
    fi
    ok "头文件: $inc"
    ok "动态库: $lib"
    return 0
}

install_ort() {
    if check_ort >/dev/null 2>&1; then return 0; fi
    log "下载 ONNX Runtime ${ORT_VERSION} ..."
    mkdir -p "$DEPS_DIR"
    local tgz_path="${DEPS_DIR}/${ORT_TGZ}"
    if [[ ! -f "$tgz_path" ]]; then
        if has_cmd curl; then
            curl -L --fail -o "$tgz_path" "$ORT_URL" || { err "下载失败: $ORT_URL"; return 1; }
        elif has_cmd wget; then
            wget -O "$tgz_path" "$ORT_URL" || { err "下载失败: $ORT_URL"; return 1; }
        else
            err "缺少 curl / wget，无法下载"
            return 1
        fi
    else
        log "已有本地包: $tgz_path"
    fi
    log "解压到: $DEPS_DIR"
    tar -xzf "$tgz_path" -C "$DEPS_DIR" || { err "解压失败"; return 1; }
    check_ort
}

# ========== NCNN (可选 - 轻量推理后端) ==========
check_ncnn() {
    hdr "NCNN 检查 (轻量推理后端)"
    log "查找路径: $NCNN_SDK_PATH"
    if [[ ! -d "$NCNN_SDK_PATH" ]]; then
        err "未找到 NCNN SDK 目录: $NCNN_SDK_PATH"
        return 1
    fi
    local inc="$NCNN_SDK_PATH/include/ncnn/net.h"
    if [[ ! -f "$inc" ]]; then
        err "缺少头文件: $inc"
        return 1
    fi
    local lib=""
    if [[ -f "$NCNN_SDK_PATH/lib/libncnn.so" ]]; then
        lib="$NCNN_SDK_PATH/lib/libncnn.so"
    elif [[ -f "$NCNN_SDK_PATH/lib/libncnn.a" ]]; then
        lib="$NCNN_SDK_PATH/lib/libncnn.a"
    fi
    if [[ -z "$lib" ]]; then
        err "缺少 NCNN 库文件 (libncnn.so / libncnn.a)"
        return 1
    fi
    ok "头文件: $inc"
    ok "库文件: $lib"
    return 0
}

install_ncnn() {
    if check_ncnn >/dev/null 2>&1; then return 0; fi

    hdr "从 submodule 源码编译安装 NCNN"

    if ! has_cmd cmake; then
        err "需要 cmake 来编译 NCNN"; return 1
    fi
    
    # NCNN 需要 Protobuf
    if ! has_cmd protoc; then
        log "安装 Protobuf (NCNN 需要)..."
        if has_cmd apt-get; then
            sudo apt-get install -y protobuf-compiler libprotobuf-dev \
                || { err "Protobuf 安装失败"; return 1; }
        else
            err "仅支持 apt-get 包管理器安装 Protobuf"
            return 1
        fi
    fi

    local ncnn_src="${DEPS_DIR}/ncnn-src"
    local ncnn_build="${DEPS_DIR}/ncnn-build"

    if [[ ! -d "$ncnn_src/.git" ]]; then
        log "初始化 NCNN submodule..."
        git submodule update --init --recursive "$ncnn_src" \
            || { err "初始化 NCNN submodule 失败"; return 1; }
    fi

    if [[ -n "${NCNN_VERSION:-}" ]]; then
        (cd "$ncnn_src" && git checkout "$NCNN_VERSION") \
            || { err "切换 NCNN 版本 $NCNN_VERSION 失败"; return 1; }
    fi
    ok "源码就绪: $ncnn_src"

    mkdir -p "$ncnn_build"
    local cmake_args=(
        -DCMAKE_INSTALL_PREFIX="$NCNN_SDK_PATH"
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DNCNN_SHARED_LIB=ON
        -DNCNN_BUILD_EXAMPLES=OFF
        -DNCNN_BUILD_TESTS=OFF
        -DNCNN_BUILD_TOOLS=OFF
        -DNCNN_PYTHON=OFF
        -DNCNN_ENABLE_LTO=ON
        -DNCNN_RUNTIME_CPU=ON
        -DNCNN_BUILD_BENCHMARK=OFF
        -DNCNN_VULKAN=OFF
    )

    log "CMake 配置..."
    (cd "$ncnn_build" && cmake "$ncnn_src" "${cmake_args[@]}") \
        || { err "NCNN CMake 配置失败"; return 1; }

    log "编译 NCNN (${NCPU} 线程并行)..."
    (cd "$ncnn_build" && cmake --build . -j"$NCPU") \
        || { err "NCNN 编译失败"; return 1; }

    log "安装到: $NCNN_SDK_PATH"
    (cd "$ncnn_build" && cmake --install .) \
        || { err "NCNN 安装失败"; return 1; }

    rm -rf "$ncnn_build"
    check_ncnn
}

# ========== Qt6 (可选 - manager GUI) ==========
check_qt6() {
    hdr "Qt6 检查 (manager GUI)"

    if has_cmd qmake6; then
        ok "qmake6: $(qmake6 --version 2>&1 | tail -n1)"
        return 0
    fi

    # 检查 cmake 配置文件（Core, Gui, Widgets）
    local qt6_ok=1
    for comp in Qt6Core Qt6Gui Qt6Widgets; do
        if [[ -f "/usr/lib/x86_64-linux-gnu/cmake/${comp}/${comp}Config.cmake" ]] || \
           [[ -f "/usr/lib/cmake/${comp}/${comp}Config.cmake" ]] || \
           [[ -f "/usr/lib/${comp}/${comp}Config.cmake" ]]; then
            continue
        else
            qt6_ok=0
            break
        fi
    done
    if [[ $qt6_ok -eq 1 ]]; then
        ok "Qt6 (cmake config: Core Gui Widgets)"
        return 0
    fi

    if [[ -f /usr/include/x86_64-linux-gnu/qt6/QtCore/qglobal.h ]] || \
       [[ -f /usr/include/qt6/QtCore/qglobal.h ]]; then
        ok "Qt6 (头文件已安装，cmake 配置文件可能不完整)"
        return 0
    fi

    err "未找到 Qt6（GUI 构建需要）"
    return 1
}

install_qt6() {
    if check_qt6 >/dev/null 2>&1; then return 0; fi
    if ! has_cmd apt-get; then
        err "仅支持 apt-get 包管理器安装 Qt6"
        return 1
    fi
    log "通过 apt-get 安装 Qt6..."
    sudo apt-get update -qq && \
    sudo apt-get install -y qt6-base-dev libgl1-mesa-dev \
        || { err "Qt6 安装失败"; return 1; }
    ok "Qt6 安装完成"
    return 0
}

# ========== ROS2 Humble (可选 - ROS2 传输后端) ==========
check_ros2() {
    hdr "ROS2 检查 (ROS2 传输后端)"

    if [[ -f "$ROS2_SETUP" ]]; then
        ok "ROS2 setup.bash: $ROS2_SETUP"
    else
        err "未找到 ROS2 setup.bash: $ROS2_SETUP"
    fi

    # 检查 rclcpp
    if [[ -d /opt/ros/humble/include/rclcpp ]]; then
        ok "rclcpp 头文件: /opt/ros/humble/include/rclcpp"
    elif has_cmd dpkg && dpkg -l ros-humble-rclcpp >/dev/null 2>&1; then
        ok "rclcpp (dpkg: ros-humble-rclcpp)"
    else
        err "未找到 rclcpp"
    fi

    # 检查 vision_interfaces
    if [[ -d /opt/ros/humble/share/vision_interfaces ]] || \
       [[ -f /opt/ros/humble/include/vision_interfaces/vision_interfaces/srv/camera_get_config.hpp ]]; then
        ok "vision_interfaces 已安装"
    else
        warn "vision_interfaces 未找到 (ROS2 .srv 类型)，需在 build.sh 中自动编译"
    fi

    if [[ -f "$ROS2_SETUP" ]]; then
        return 0
    fi
    return 1
}

install_ros2() {
    # 检查基础 ROS2 是否安装
    if [[ ! -f "$ROS2_SETUP" ]]; then
        warn "ROS2 Humble 需要手动安装，请参考官方文档:"
        cat <<EOF

  安装步骤:
    1) 参考 https://docs.ros.org/en/humble/Installation/Ubuntu-Install-Debs.html
    2) sudo apt install ros-humble-desktop
    3) 安装后 setup.bash 路径: /opt/ros/humble/setup.bash
    4) 重新运行: ./setup_env.sh install

EOF
        return 1
    fi
    
    # 尝试编译 vision_interfaces
    build_vision_interfaces
    return 0
}

# ========== 编译 ROS2 vision_interfaces ==========
build_vision_interfaces() {
    if [[ -d /opt/ros/humble/share/vision_interfaces ]]; then
        ok "vision_interfaces 已安装"
        return 0
    fi
    
    # 需要 colcon 和 ament_cmake
    if ! has_cmd colcon; then
        warn "未找到 colcon，跳过 vision_interfaces 编译"
        warn "建议安装: sudo apt install python3-colcon-common-extensions"
        return 1
    fi
    
    log "编译 vision_interfaces..."
    local ws_dir="${DEPS_DIR}/ros2_ws"
    mkdir -p "${ws_dir}/src"
    
    # 复制 vision_interfaces 到工作空间
    if [[ ! -d "${ws_dir}/src/vision_interfaces" ]]; then
        cp -r "${SCRIPT_DIR}/vision_interfaces" "${ws_dir}/src/" \
            || { err "复制 vision_interfaces 失败"; return 1; }
    fi
    
    # 编译并安装到 ROS2 安装目录
    source "$ROS2_SETUP" 2>/dev/null || true
    (cd "$ws_dir" && colcon build --packages-select vision_interfaces --install-base /opt/ros/humble) \
        || { err "vision_interfaces 编译失败"; return 1; }
    
    ok "vision_interfaces 编译完成"
    return 0
}

# ========== Zenoh (可选 - Zenoh 传输后端) ==========
check_zenoh() {
    hdr "Zenoh 检查 (Zenoh 传输后端)"

    # 检查 zenohc cmake 配置
    local found=0
    for path in /usr/local/lib/cmake/zenohc /usr/lib/cmake/zenohc \
                /usr/local/lib/x86_64-linux-gnu/cmake/zenohc; do
        if [[ -f "$path/zenohcConfig.cmake" ]] || [[ -f "$path/zenohc-config.cmake" ]]; then
            ok "zenohc cmake: $path"
            found=1
            break
        fi
    done

    # 检查 libzenohc
    if [[ $found -eq 0 ]]; then
        for path in /usr/local/lib/libzenohc.so /usr/lib/libzenohc.so \
                    /usr/local/lib/x86_64-linux-gnu/libzenohc.so; do
            if [[ -f "$path" ]]; then
                ok "libzenohc: $path"
                found=1
                break
            fi
        done
    fi

    if [[ $found -eq 0 ]]; then
        warn "Zenoh 未找到 (可选，不影响 ZeroMQ/ROS2 传输)"
        return 1
    fi
    return 0
}

install_zenoh() {
    if check_zenoh >/dev/null 2>&1; then return 0; fi
    warn "Zenoh 为可选传输后端，暂不支持自动安装。"
    log "如需使用 Zenoh，请从 https://github.com/eclipse-zenoh/zenoh-c 手动编译安装"
    return 1
}

# ========== 模型文件 (可选) ==========
check_model() {
    hdr "模型文件检查"
    local found=0

    # ONNX 模型
    local onnx_model=""
    for f in "${SCRIPT_DIR}/models/yolov11_obb.onnx" \
             "${SCRIPT_DIR}/template/model.onnx"; do
        if [[ -f "$f" ]]; then
            ok "ONNX 模型: $f"
            onnx_model="$f"
            found=1
            break
        fi
    done
    if [[ -z "$onnx_model" ]]; then
        warn "未找到 ONNX 模型 (YOLO 检测器需要)"
    fi

    # NCNN 模型
    local ncnn_dir="${SCRIPT_DIR}/../vision/model"
    if [[ -f "${ncnn_dir}/model.ncnn.param" ]] && [[ -f "${ncnn_dir}/model.ncnn.bin" ]]; then
        ok "NCNN 模型: ${ncnn_dir}/model.ncnn.{param,bin}"
        found=1
    else
        warn "未找到 NCNN 模型 (可选)"
    fi

    if [[ $found -eq 0 ]]; then
        return 1
    fi
    return 0
}

# ========== 模板文件 (detector_node) ==========
check_template() {
    hdr "模板文件检查"
    local tpl_dir="${SCRIPT_DIR}/template"
    if [[ -d "$tpl_dir" ]]; then
        local count
        count=$(find "$tpl_dir" -type f 2>/dev/null | wc -l)
        ok "template 目录: $tpl_dir (${count} 个文件)"
        return 0
    fi
    
    # 也检查安装后的路径
    tpl_dir="${INSTALL_DIR}/config/template"
    if [[ -d "$tpl_dir" ]]; then
        local count
        count=$(find "$tpl_dir" -type f 2>/dev/null | wc -l)
        ok "template 目录 (已安装): $tpl_dir (${count} 个文件)"
        return 0
    fi
    
    warn "template 目录不存在: $tpl_dir (detector_node 需要模板图像)"
    return 1
}

# ========== 清理 ==========
do_clean() {
    hdr "清理构建目录"
    if [[ -d "$BUILD_DIR" ]]; then
        log "删除: $BUILD_DIR"
        rm -rf "$BUILD_DIR"
    fi
    ok "清理完成"
}

# ========== 构建 ==========
do_build() {
    hdr "构建项目"
    export_env_vars
    log "调用 build.sh $*"
    bash "${SCRIPT_DIR}/build.sh" "$@"
}

# ========== 运行 ==========
do_run() {
    local exe_name="${1:-camera_node}"
    shift 2>/dev/null || true

    local bin="${INSTALL_DIR}/bin/${exe_name}"
    if [[ ! -x "$bin" ]]; then
        # 尝试在 build 子目录中查找
        for d in camera_node detector_node comm_node manager; do
            if [[ -x "${BUILD_DIR}/${d}/${exe_name}" ]]; then
                bin="${BUILD_DIR}/${d}/${exe_name}"
                break
            fi
        done
    fi

    if [[ ! -x "$bin" ]]; then
        err "未找到可执行文件: $exe_name"
        log "可选: camera_node, detector_node, comm_node, manager"
        log "请先执行 build"
        return 1
    fi

    export_env_vars
    
    # 添加 ROS2 环境
    if [[ -f "$ROS2_SETUP" ]]; then
        source "$ROS2_SETUP" 2>/dev/null || true
    fi
    
    hdr "启动 $exe_name"

    # 自动查找配置文件
    local config=""
    for f in "${INSTALL_DIR}/config/system_config_ros2.xml" \
             "${INSTALL_DIR}/config/system_config_zeromq.xml"; do
        if [[ -f "$f" ]]; then
            config="$f"
            break
        fi
    done

    local extra_args=""
    if [[ -n "$config" ]]; then
        extra_args="--config $config"
    fi

    # 节点特定的额外配置
    case "$exe_name" in
        camera_node)
            local cam_cfg="${INSTALL_DIR}/config/camera_config.xml"
            [[ -f "$cam_cfg" ]] && extra_args="$extra_args --camera-config $cam_cfg"
            ;;
        detector_node)
            local det_cfg="${INSTALL_DIR}/config/detector.xml"
            [[ -f "$det_cfg" ]] && extra_args="$extra_args --detector-config $det_cfg"
            ;;
        comm_node)
            local comm_cfg="${INSTALL_DIR}/config/communication.xml"
            [[ -f "$comm_cfg" ]] && extra_args="$extra_args --comm-config $comm_cfg"
            ;;
    esac

    log "命令: $bin $extra_args $*"
    "$bin" $extra_args "$@"
}

# ========== 汇总检查 ==========
run_checks() {
    local rc=0
    local required_fail=0

    check_platform || rc=1

    # 必需依赖
    check_compiler || { rc=1; required_fail=1; }
    check_pkg_config || true
    check_cmake    || { rc=1; required_fail=1; }
    check_opencv   || { rc=1; required_fail=1; }
    check_zmq      || { rc=1; required_fail=1; }

    # 可选依赖
    check_hik_sdk  || true   # camera_node
    check_ort      || true   # YOLO detector
    check_ncnn     || true   # NCNN inference
    check_qt6      || true   # manager GUI
    check_ros2     || true   # ROS2 transport
    check_zenoh    || true   # Zenoh transport
    check_model    || true
    check_template || true

    hdr "检查结果"
    if [[ $required_fail -eq 0 ]]; then
        ok "全部必需依赖均已就绪，可以构建"
    else
        err "存在必需依赖缺失，运行 './setup_env.sh install' 自动修复"
    fi
    return $rc
}

# ========== 汇总安装 ==========
run_install() {
    check_platform || return 1

    # 必需依赖
    install_compiler
    check_compiler || { err "编译器安装失败"; return 1; }
    install_pkg_config
    install_cmake
    check_cmake || { err "cmake 安装失败"; return 1; }
    install_opencv || { err "OpenCV 安装失败"; return 1; }
    install_zmq    || { err "ZeroMQ 安装失败"; return 1; }

    # 可选依赖 (尽力安装，不阻断流程)
    install_ort    || warn "ONNX Runtime 安装跳过"
    install_ncnn   || warn "NCNN 安装跳过"
    install_qt6    || warn "Qt6 安装跳过"
    install_ros2   || warn "ROS2 安装跳过 (需手动安装)"
    install_zenoh  || warn "Zenoh 安装跳过"
    install_hik_sdk || warn "海康 SDK 安装跳过 (需手动安装)"

    check_model    || true
    check_template || true

    ok "依赖安装流程完成"
}

# ========== 帮助 ==========
show_help() {
    sed -n '2,18p' "$0" | sed 's/^# //;s/^#//'
}

# ========== 主入口 ==========
CMD="${1:-check}"
shift 2>/dev/null || true

case "$CMD" in
    check)
        run_checks
        ;;
    install)
        run_install && run_checks
        ;;
    build)
        run_install && run_checks && do_build "$@"
        ;;
    clean)
        do_clean
        ;;
    run)
        do_run "$@"
        ;;
    env)
        print_env_script
        ;;
    help|-h|--help)
        show_help
        ;;
    *)
        err "未知命令: $CMD"
        show_help
        exit 1
        ;;
esac

