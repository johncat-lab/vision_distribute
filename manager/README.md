# manager — GUI 控制端

## 功能

基于 Qt6 的图形化控制中心，实时显示相机画面与检测结果叠加，通过 RPC 远程管理 camera / detector / comm 三个节点的参数配置。

## 启动命令

```bash
manager --config <system_config.xml>
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `--config` | 是 | 系统传输配置 (`system_config_zeromq.xml` 等) |

需要 `--gui` 标志编译（`bash build.sh --gui`）。

## 界面布局

```
┌─────────────────────────────────────┐
│        图像显示区 (640x480+)         │  ← 实时帧 + 检测结果叠加
├─────────────────────────────────────┤
│ Camera: OK │ Detector: OK │ Comm: OK│  ← 状态栏 (2s 刷新)
├─────────────────────────────────────┤
│ [Camera] [Detector] [Communication] │  ← 选项卡
└─────────────────────────────────────┘
```

### Camera 选项卡

| 控件 | 功能 | 调用 Endpoint |
|------|------|---------------|
| Exposure (SpinBox) | 设置曝光时间 (μs) | `camera/set_exposure` |
| Gain (SpinBox) | 设置增益 (dB) | `camera/set_gain` |
| Trigger Mode (ComboBox) | 触发模式 | `camera/set_trigger_mode` |
| Soft Trigger (按钮) | 软触发（仅 software 模式） | `camera/soft_trigger` |
| Camera Info (只读) | 当前相机配置 | `camera/get_config` |

### Detector 选项卡

| 控件 | 功能 | 调用 Endpoint |
|------|------|---------------|
| Match Threshold | 匹配阈值 (0~1) | `detector/set_threshold` |
| Segment Mode | 分割模式 (value/gradient/hsv) | `detector/set_segment_mode` |
| V Threshold | V 通道阈值 (0~255) | `detector/set_v_threshold` |
| Grad Threshold | 梯度阈值 (0~255) | `detector/set_grad_threshold` |
| Reload Template | 重新加载模板 | `detector/reload_template` |
| Enable/Disable | 检测器开关 | `detector/onoff` |
| Detector Info (只读) | 当前检测器配置 | `detector/get_config` |

### Communication 选项卡

| 控件 | 功能 | 调用 Endpoint |
|------|------|---------------|
| Host (LineEdit) | 设置监听/连接地址 | `comm/set_config` |
| Port (SpinBox) | 设置端口 | `comm/set_config` |
| Mode (ComboBox) | server/client 切换 | `comm/set_config` |
| Check Status | 查看连接状态 | `comm/get_status` |
| Comm Info (只读) | 当前通信配置 | `comm/get_config` |

## 数据订阅

| Topic | 消息类型 | 用途 |
|-------|---------|------|
| `vision/frame` | `FrameMsg` | 实时图像帧显示 |
| `vision/detection` | `DetectionMsg` | 检测结果叠加（回退模式） |
| `vision/annotation` | `AnnotationMsg` | 完整标注叠加（优先使用） |

### 图像叠加

- **AnnotationMsg**（优先）: 绘制旋转矩形框 + 中心十字 + 编号标签 + 坐标信息 + 角度指示线
- **DetectionMsg**（回退）: 绘制十字准心 + 圆圈 + 角度指示线

## RPC 调用机制

- 所有 service 调用**异步执行**，不阻塞 UI 线程
- 后台线程发起 RPC 调用，完成后通过 `QMetaObject::invokeMethod` 回到主线程执行回调
- 每个 service 有 `atomic<bool>` 防抖标志，防止并发调用堆积
- 启动时自动调用 `preconnect()` 预创建所有原生 client

### 定时任务

| 定时器 | 间隔 | 功能 |
|--------|------|------|
| display_timer | 33ms (~30fps) | 刷新图像显示 + 叠加检测结果 |
| status_timer | 2s | 轮询各节点状态（get_config/get_status） |
| initUI (单次) | 启动后 500ms | 从各节点拉取配置填充 UI 初始值 |

## 配置文件

manager 自身无专用配置文件，仅通过 `system_config_*.xml` 确定传输后端，其余参数通过 RPC 从各节点动态拉取。

## 实现说明

- 基于 Qt6 Widgets，使用 `QMainWindow` + `QTabWidget` 布局
- OpenCV `cv::Mat` → `QImage` 转换支持 Mono8 / RGB8 像素格式
- 图像按 `QLabel` 尺寸等比缩放显示（`Qt::KeepAspectRatio`）
- 窗口关闭时自动停止所有定时器和 RPC 连接
