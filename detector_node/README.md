# detector_node — 视觉检测节点

## 功能

订阅 `vision/frame` topic 接收图像帧，执行模板匹配/目标检测，将检测结果发布到 `vision/detection` topic，同时提供 RPC service 供 manager 控制检测参数。

## 启动命令

```bash
detector_node --config <system_config.xml> --detector-config <detector.xml>
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `--config` | 是 | 系统传输配置 |
| `--detector-config` | 否 | 检测器配置，缺失时使用默认值 |

## 数据流

### Pub/Sub

| 方向 | Topic | 消息类型 | 说明 |
|------|-------|---------|------|
| 订阅 | `vision/frame` | `FrameMsg` | 从 camera_node 接收图像帧 |
| 发布 | `vision/detection` | `DetectionMsg` | 检测结果协议字符串 |
| 发布 | `vision/annotation` | `AnnotationMsg` | 标注数据（坐标+角度+分数） |

`DetectionMsg` 格式：20 字节头部 + 协议字符串

```
[4B frame_num][8B timestamp][4B object_count][4B str_len][protocol_string...]
```

协议字符串示例：`TA,123.4,567.8,45.2,0.95,1;` 或 `NG`

### RPC Service Endpoints

service name `detector`（ROS2 模式映射为 `/detector/<endpoint>`）：

| Endpoint | ROS2 Service Type | 请求 | 响应 |
|----------|-------------------|------|------|
| `get_result` | `DetectorGetResult` | (空) | `bool success, uint8[] detection_data` |
| `get_config` | `DetectorGetConfig` | (空) | `bool ready, string config_data` |
| `onoff` | `DetectorOnOff` | `string command` ("on"/"off") | `bool success, string message` |
| `set_threshold` | `DetectorSetThreshold` | `float32 threshold` | `bool success, string message` |
| `set_v_threshold` | `DetectorSetVThreshold` | `int32 threshold` | `bool success, string message` |
| `set_grad_threshold` | `DetectorSetGradThreshold` | `int32 threshold` | `bool success, string message` |
| `set_segment_mode` | `DetectorSetSegmentMode` | `string mode` ("value"/"gradient") | `bool success, string message` |
| `reload_template` | `DetectorReloadTemplate` | (空) | `bool success, string message` |

#### ROS2 CLI 调用示例

```bash
ros2 service call /detector/onoff vision_interfaces/srv/DetectorOnOff "{command: 'on'}"
ros2 service call /detector/set_threshold vision_interfaces/srv/DetectorSetThreshold "{threshold: 0.65}"
ros2 service call /detector/set_segment_mode vision_interfaces/srv/DetectorSetSegmentMode "{mode: 'gradient'}"
ros2 service call /detector/reload_template vision_interfaces/srv/DetectorReloadTemplate "{}"
ros2 service call /detector/get_config vision_interfaces/srv/DetectorGetConfig "{}"
```

## 配置文件

### detector.xml — 检测器配置

```xml
<detector>opencv</detector>                    <!-- opencv | yolo | ncnn | edge | conveyor -->
<template_dir>./template</template_dir>        <!-- 模板图像目录 -->
<match_threshold>0.70</match_threshold>        <!-- 模板匹配阈值 (0-1) -->
<segment_mode>value</segment_mode>             <!-- value | gradient -->
<v_threshold>50</v_threshold>                  <!-- V 通道阈值 (HSV 空间) -->
<grad_threshold>30</grad_threshold>            <!-- 梯度阈值 -->
<roi_y_center>1500</roi_y_center>             <!-- ROI 区域 Y 中心 -->
<roi_y_margin>600</roi_y_margin>              <!-- ROI 区域 Y 范围 -->
<model_path></model_path>                      <!-- YOLO ONNX 模型路径 -->
<verify_with_template>1</verify_with_template> <!-- 是否用模板验证 -->
<enabled>0</enabled>                           <!-- 启动时是否启用检测 -->
```

### 日志配置（嵌入 detector.xml）

```xml
<log_level>debug</log_level>    <!-- off | debug | info | warn | error -->
<log_output>both</log_output>   <!-- console | file | both -->
<log_dir>./logs</log_dir>
```

## 检测器类型

| 类型 | 说明 | 依赖 |
|------|------|------|
| `opencv` | OpenCV 模板匹配 + HSV 分割 | OpenCV |
| `yolo` | YOLOv11 OBB 目标检测 | ONNX Runtime |
| `ncnn` | NCNN 轻量推理 | NCNN SDK |
| `edge` | 边缘梯度检测 | OpenCV |
| `conveyor` | 传送带专用检测器 | OpenCV |

## 实现说明

- 默认启动时检测器处于关闭状态 (`enabled=0`)，需通过 `onoff` endpoint 开启
- `reload_template` 无需重启节点，运行时重新加载模板图像
- 帧处理使用互斥锁保护，确保同一时刻仅一帧在处理中
- `segment_mode` 切换后阈值需重新设置才生效
