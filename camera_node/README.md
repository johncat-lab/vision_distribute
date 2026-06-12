# camera_node — 相机采集节点

## 功能

通过海康 MVS SDK 驱动工业相机，采集图像帧并通过 Pub/Sub 发布到 `vision/frame` topic，同时提供 RPC service 供 manager 远程控制相机参数。

## 启动命令

```bash
camera_node --config <system_config.xml> --camera-config <camera_config.xml>
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `--config` | 是 | 系统传输配置 (`system_config_zeromq.xml` 等) |
| `--camera-config` | 否 | 相机参数配置，缺失时使用默认值 |

## 数据输出

### Pub/Sub Topic

| Topic | 消息类型 | 说明 |
|-------|---------|------|
| `vision/frame` | `FrameMsg` | 图像帧（含像素数据 + 元信息） |

`FrameMsg` 二进制格式: 48 字节头部 + 像素数据

```
[4B camera_id][8B timestamp][2B width][2B height][4B pixel_type]
[4B frame_num][4B exposure_time][4B gain][8B data_size][data...]
```

### RPC Service Endpoints

所有 endpoint 注册在 service name `camera` 下（ROS2 模式映射为 `/camera/<endpoint>`）：

| Endpoint | ROS2 Service Type | 请求 | 响应 |
|----------|-------------------|------|------|
| `set_exposure` | `CameraSetExposure` | `float32 exposure_time` | `bool success, string message` |
| `set_gain` | `CameraSetGain` | `float32 gain` | `bool success, string message` |
| `set_trigger_mode` | `CameraSetTriggerMode` | `string mode` | `bool success, string message` |
| `soft_trigger` | `CameraSoftTrigger` | (空) | `bool success, string message` |
| `get_config` | `CameraGetConfig` | (空) | `bool success, string config_data` |

#### ROS2 CLI 调用示例

```bash
ros2 service call /camera/set_exposure vision_interfaces/srv/CameraSetExposure "{exposure_time: 8000.0}"
ros2 service call /camera/get_config vision_interfaces/srv/CameraGetConfig "{}"
ros2 service call /camera/set_trigger_mode vision_interfaces/srv/CameraSetTriggerMode "{mode: 'software'}"
ros2 service call /camera/soft_trigger vision_interfaces/srv/CameraSoftTrigger "{}"
```

## 配置文件

### system_config_*.xml — 系统传输配置

```xml
<transport>zeromq</transport>           <!-- zeromq | ros2 | zenoh -->
<base_port>15550</base_port>            <!-- ZMQ 基础端口 -->
<topic_frame>vision/frame</topic_frame> <!-- 帧 topic 名称 -->
```

ROS2 模式额外配置：

```xml
<domain_id>0</domain_id>                <!-- ROS2 domain ID -->
<ros2_executor_threads>4</ros2_executor_threads>
<service_timeout_ms>5000</service_timeout_ms>
<service_max_retries>3</service_max_retries>
```

### camera_config.xml — 相机参数配置

```xml
<camera_index>0</camera_index>
<trigger_mode>continuous</trigger_mode>  <!-- continuous | off | line0 | line1 | line2 | software -->
<pixel_format>Mono8</pixel_format>       <!-- Mono8 | RGB8 | BayerRG8 -->
<exposure_auto>continuous</exposure_auto><!-- off | once | continuous -->
<exposure_time>10000</exposure_time>     <!-- 曝光时间 (μs)，仅 exposure_auto=off 时生效 -->
<gain_auto>continuous</gain_auto>
<gain>0</gain>                           <!-- 增益 (dB)，仅 gain_auto=off 时生效 -->
<frame_rate>30</frame_rate>
```

## 实现说明

- 相机未检测到时节点仍以"未就绪"状态运行，service 可调用但返回错误
- 触发模式为 `continuous`/`off` 时自动采集，`software` 模式需通过 `soft_trigger` endpoint 触发
- 图像回调中将像素数据序列化为 `FrameMsg` 后发布，使用 BestEffort QoS (ROS2) 避免拥塞
