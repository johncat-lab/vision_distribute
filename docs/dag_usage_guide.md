# DAG 调度实例使用指南

## 📋 概述

本文档介绍如何使用 DAG Launcher 启动和管理视觉检测流水线。

## 🏗️ 流水线架构

```
┌─────────────┐   FrameMsg   ┌──────────────┐  DetectionMsg  ┌─────────────┐
│ camera_node │─────────────>│ detector_node│───────────────>│  comm_node  │
│  (图像采集)  │              │  (目标检测)   │                │  (通信上传)  │
└─────────────┘              └──────────────┘                └─────────────┘
                                  │
                                  │ AnnotationMsg
                                  ↓
                          (可选：可视化/存储)
```

## 🚀 快速开始

### 1. 编译项目

```bash
cd build
cmake --build . --config Release -j$(sysctl -n hw.ncpu)
cmake --install .
```

### 2. 准备配置文件

将配置文件复制到安装目录：

```bash
cd build/install/bins

# 复制示例配置
cp ../../../config/dag_example.xml .
cp ../../../config/camera_config.xml camera_node/
cp ../../../config/detector.xml detector_node/
cp ../../../config/communication.xml comm_node/
cp ../../../config/system_config_zeromq.xml */
```

### 3. 启动 DAG 流水线

```bash
# 方式 1：使用相对路径（在安装目录内）
./dag_launcher/dag_launcher dag_example.xml

# 方式 2：使用绝对路径
./dag_launcher/dag_launcher /path/to/dag_example.xml

# 方式 3：指定传输后端
# 修改 system_config.xml 中的 transport_backend 配置
# - zeromq: ZeroMQ 传输
# - ros2: ROS2 传输
# - zenoh: Zenoh 传输
```

## 📝 配置文件详解

### 完整配置示例

参见 [`config/dag_example.xml`](../config/dag_example.xml)

### 配置结构

```xml
<pipeline>
  <!-- 1. 节点模板定义 -->
  <templates>
    <template name="模板名">
      <binary>可执行文件名</binary>
      <in port="输入端口名" type="消息类型"/>
      <out port="输出端口名" type="消息类型"/>
      <service role="角色名" endpoints="端点列表"/>
    </template>
  </templates>

  <!-- 2. 节点实例化 -->
  <instances>
    <instance name="实例名" template="模板名">
      <config>配置文件路径</config>
      <param name="参数名" value="参数值"/>
    </instance>
  </instances>

  <!-- 3. 数据流连线 -->
  <wiring>
    <wire from="源实例" from_port="源端口"
          to="目标实例" to_port="目标端口"
          topic="ZeroMQ/ROS2 Topic名"/>
  </wiring>

  <!-- 4. 启动顺序（可选） -->
  <startup>
    <order>
      <step>实例名1</step>
      <step>实例名2</step>
    </order>
    <delay_ms>启动间隔(毫秒)</delay_ms>
  </startup>

  <!-- 5. 服务路由（可选） -->
  <service_routing>
    <route path="/路由路径" 
           target="目标实例" 
           endpoint="服务端点"/>
  </service_routing>
</pipeline>
```

## 🔧 常用场景

### 场景 1：单相机单检测器

最简单的流水线配置：

```xml
<instances>
  <instance name="cam" template="camera_node">
    <config>camera_config.xml</config>
  </instance>
  <instance name="det" template="detector_node">
    <config>detector.xml</config>
  </instance>
</instances>

<wiring>
  <wire from="cam" from_port="frame_output"
        to="det" to_port="frame_input"
        topic="camera/frame"/>
</wiring>
```

### 场景 2：多相机并行检测

```xml
<instances>
  <instance name="cam1" template="camera_node"/>
  <instance name="cam2" template="camera_node"/>
  <instance name="det1" template="detector_node"/>
  <instance name="det2" template="detector_node"/>
  <instance name="comm" template="comm_node"/>
</instances>

<wiring>
  <!-- 相机1流水线 -->
  <wire from="cam1" from_port="frame_output"
        to="det1" to_port="frame_input"
        topic="cam1/frame"/>
  <wire from="det1" from_port="detection_output"
        to="comm" to_port="detection_input"
        topic="det1/result"/>
  
  <!-- 相机2流水线 -->
  <wire from="cam2" from_port="frame_output"
        to="det2" to_port="frame_input"
        topic="cam2/frame"/>
  <wire from="det2" from_port="detection_output"
        to="comm" to_port="detection_input"
        topic="det2/result"/>
</wiring>
```

### 场景 3：动态参数调整

通过服务路由实现运行时参数调整：

```bash
# 调整相机曝光
ros2 service call /camera/set_exposure \
  vision_interfaces/srv/CameraSetExposure "{exposure_time: 5000.0}"

# 获取检测结果
ros2 service call /detector/get_result \
  vision_interfaces/srv/DetectorGetResult "{}"

# 查看通信状态
ros2 service call /comm/get_status \
  vision_interfaces/srv/CommGetStatus "{}"
```

## 🎯 节点端口说明

### camera_node

| 端口 | 方向 | 消息类型 | 说明 |
|------|------|---------|------|
| frame_output | OUT | FrameMsg | 图像帧输出 |

**服务端点**：
- `set_exposure`: 设置曝光时间
- `set_gain`: 设置增益
- `set_trigger_mode`: 设置触发模式
- `soft_trigger`: 软触发
- `get_config`: 获取配置

### detector_node

| 端口 | 方向 | 消息类型 | 说明 |
|------|------|---------|------|
| frame_input | IN | FrameMsg | 图像帧输入 |
| detection_output | OUT | DetectionMsg | 检测结果输出 |
| annotation_output | OUT | AnnotationMsg | 标注信息输出 |

**服务端点**：
- `get_result`: 获取检测结果
- `get_config`: 获取配置
- `onoff`: 开关检测器
- `set_threshold`: 设置阈值
- `reload_template`: 重载模板

### comm_node

| 端口 | 方向 | 消息类型 | 说明 |
|------|------|---------|------|
| detection_input | IN | DetectionMsg | 检测结果输入 |

**服务端点**：
- `set_config`: 设置通信配置
- `get_config`: 获取配置
- `get_status`: 获取状态

## 🐛 故障排查

### 问题 1：节点启动失败

**症状**：某个节点无法启动

**排查步骤**：
```bash
# 1. 检查可执行文件是否存在
ls -lh build/install/bins/<node_name>/<node_name>

# 2. 检查配置文件路径
ls -lh build/install/bins/<node_name>/*.xml

# 3. 手动启动节点测试
cd build/install/bins/<node_name>
./<node_name>
```

### 问题 2：节点间无法通信

**症状**：节点启动成功但数据无法传输

**排查步骤**：
```bash
# 1. 检查 system_config.xml 中的传输后端配置
cat build/install/bins/<node_name>/system_config.xml | grep transport_backend

# 2. 检查 Topic 名称是否匹配
# 在 dag_example.xml 中确认 wire 的 topic 配置

# 3. 使用 ZeroMQ 工具监控消息
# 安装：pip install pyzmq
python3 -c "
import zmq
ctx = zmq.Context()
sock = ctx.socket(zmq.SUB)
sock.connect('tcp://localhost:5555')
sock.setsockopt(zmq.SUBSCRIBE, b'')
while True:
    msg = sock.recv()
    print(f'Received: {msg}')
"
```

### 问题 3：服务调用失败

**症状**：ros2 service call 超时

**排查步骤**：
```bash
# 1. 查看服务列表
ros2 service list

# 2. 检查服务路由配置
# 在 dag_example.xml 的 <service_routing> 中确认路由映射

# 3. 检查节点是否注册了服务
# 查看节点日志输出
```

## 📊 性能优化建议

1. **启动顺序**：使用 `<startup>` 控制启动顺序，避免数据丢失
2. **消息队列**：ZeroMQ 默认有队列，但建议控制生产消费速率
3. **传输后端选择**：
   - 单机部署：ZeroMQ（最低延迟）
   - ROS2 生态：ROS2（最佳兼容）
   - 分布式部署：Zenoh（最佳扩展性）

## 📚 参考资源

- [DAG 架构设计文档](../docs/dag_architecture_spec.md)
- [Pipeline 编辑器规范](../docs/pipeline_editor_spec.md)
- [Protobuf 迁移指南](../docs/protobuf_migration_guide.md)
