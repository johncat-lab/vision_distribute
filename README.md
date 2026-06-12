# Vision Distribute

分布式工业视觉检测系统，采用多节点架构，通过可插拔的传输后端（ZeroMQ / ROS2 / Zenoh）实现相机采集、图像检测、通信输出的解耦部署。

## 系统架构

```
┌──────────────┐    Topic     ┌───────────────┐    Topic     ┌──────────────┐
│  camera_node │──────────────▶│ detector_node │──────────────▶│  comm_node   │
│  (图像采集)   │ vision/frame │  (视觉检测)    │vision/detection│  (通信输出)   │
└──────┬───────┘              └───────┬───────┘              └──────┬───────┘
       │ RPC                          │ RPC                         │ RPC
       │ 5 endpoints                  │ 8 endpoints                 │ 3 endpoints
       └──────────────────────────────┼─────────────────────────────┘
                                      │
                              ┌───────▼───────┐
                              │   manager     │
                              │  (GUI 控制)    │
                              └───────────────┘
```

### 数据流

| 流向 | 传输方式 | Topic / Service | 说明 |
|------|---------|-----------------|------|
| camera → detector | Pub/Sub | `vision/frame` | 图像帧数据（二进制序列化） |
| detector → comm | Pub/Sub | `vision/detection` | 检测结果协议字符串 |
| manager ↔ camera | RPC | `/camera/*` (5个endpoint) | 相机参数控制与配置查询 |
| manager ↔ detector | RPC | `/detector/*` (8个endpoint) | 检测器开关、阈值、模板管理 |
| manager ↔ comm | RPC | `/comm/*` (3个endpoint) | 通信模式、主机地址配置 |

### 传输后端

系统通过 `NodeConfig.transport` 在启动时选择传输后端，所有节点共享同一套 `IService` / `IPublisher` / `ISubscriber` 接口：

| 后端 | 配置文件 | 特点 |
|------|---------|------|
| **ZeroMQ** | `system_config_zeromq.xml` | 轻量级、低延迟、无需中间件 |
| **ROS2** | `system_config_ros2.xml` | DDS 发现机制、原生 `ros2 service call` 兼容 |
| **Zenoh** | `system_config_zenoh.xml` | 分布式 KV + Pub/Sub |

## 目录结构

```
vision_distribute/
├── camera_node/          # 相机采集节点 (海康 MVS SDK)
├── detector_node/        # 视觉检测节点 (OpenCV / YOLO / NCNN)
├── comm_node/            # 通信输出节点 (TCP Server/Client)
├── manager/              # Qt6 GUI 控制端
├── rpc/                  # 传输抽象层 (IPublisher/ISubscriber/IService)
│   ├── include/rpc/      #   接口定义 + NodeFactory
│   └── src/              #   ZeroMQ / ROS2 / Zenoh 后端实现
├── vision_interfaces/    # ROS2 .srv 定义 (16 个 service 类型)
├── logger/               # 日志库 (控制台+文件, 多级别)
├── config/               # XML 配置文件
├── template/             # 检测模板图像
├── scripts/              # 启动/测试脚本
├── build.sh              # 一键构建脚本
└── tests/                # 通信验证测试
```

## 构建

```bash
# 完整构建 (含 GUI)
bash build.sh --clean --gui

# 仅构建指定节点
bash build.sh camera          # 仅 camera_node
bash build.sh detector        # 仅 detector_node
bash build.sh comm            # 仅 comm_node
bash build.sh --gui manager   # 仅 manager

# Debug 构建
bash build.sh --debug --gui
```

### 构建依赖

| 依赖 | 必需 | 说明 |
|------|------|------|
| OpenCV | 是 | 模板检测、图像处理 |
| ZeroMQ (cppzmq) | 是 | ZMQ 传输后端 |
| 海康 MVS SDK | 否 | camera_node 相机驱动 |
| ROS2 Humble | 否 | ROS2 传输后端 + vision_interfaces |
| Qt6 | 否 | manager GUI |
| ONNX Runtime | 否 | YOLO 检测器 |
| NCNN | 否 | 轻量推理后端 |
| Zenoh (zenohc) | 否 | Zenoh 传输后端 |

## 启动

```bash
# 无 GUI 模式 (ZeroMQ)
./scripts/launch_nogui.sh

# 无 GUI 模式 (ROS2)
./scripts/launch_nogui.sh --transport ros2

# GUI 模式
./scripts/launch_gui.sh
```

启动后二进制文件位于 `build/install/bin/`，配置位于 `build/install/config/`。

## 配置文件

| 文件 | 用途 |
|------|------|
| `system_config_zeromq.xml` | ZeroMQ 传输配置 (端口、topic) |
| `system_config_ros2.xml` | ROS2 传输配置 (domain_id、executor线程、超时) |
| `system_config_zenoh.xml` | Zenoh 传输配置 |
| `camera_config.xml` | 相机参数 (曝光、增益、触发模式) |
| `detector.xml` | 检测器参数 (阈值、模板、分割模式) |
| `communication.xml` | 通信参数 (TCP 模式、端口) |
| `log_config.xml` | 日志配置 (级别、输出方式) |

## 各节点文档

- [camera_node/README.md](camera_node/README.md) — 相机采集节点
- [detector_node/README.md](detector_node/README.md) — 视觉检测节点
- [comm_node/README.md](comm_node/README.md) — 通信输出节点
- [manager/README.md](manager/README.md) — GUI 控制端
- [rpc/README.md](rpc/README.md) — 传输抽象层
