# Vision Distribute DAG 架构设计规范

> 版本: v1.0  
> 日期: 2026-06-15  
> 状态: 已评审

---

## 1. 目标

将 vision_distribute 系统从隐式 pub/sub 拓扑升级为**显式 DAG（有向无环图）架构**，实现：

1. **拓扑声明化** — 连接关系集中在 `pipeline.xml`，不散落在各节点代码中
2. **节点端口化** — 节点只声明逻辑端口（in/out），topic 名称由外部配置决定
3. **多实例支持** — 同一 binary 可运行多个实例（如双相机），各自拥有独立配置和 topic
4. **可视化编辑** — Qt DAG 编辑器支持拖拽、连线、校验、一键导出 pipeline.xml
5. **自动调度** — DAG 调度器按拓扑序启动节点，取代固定 sleep

**不修改现有传输层**（NodeFactory / ZMQ / ROS2 / Zenoh 保持不变），DAG 机制在上层管理。

---

## 2. 架构分层

```
┌───────────────────────────────────────────────┐
│  Layer 3: 可视化编辑器 (pipeline_editor)       │
│  - 扫描节点 manifest                           │
│  - 拖拽连线                                    │
│  - 校验 + 导出 pipeline.xml                    │
├───────────────────────────────────────────────┤
│  Layer 2: DAG 调度器 (dag_scheduler)           │
│  - 解析 pipeline.xml                           │
│  - 拓扑排序 + 环检测                            │
│  - 按依赖序启动节点                            │
├───────────────────────────────────────────────┤
│  Layer 1: 节点接口层 (edge_manager)            │
│  - NodeManifest (--describe)                   │
│  - NodeEdgeManager (port → topic 映射)         │
│  - ServiceRegistry (运行时服务发现，传输无关)  │
├───────────────────────────────────────────────┤
│  Layer 0: 传输层 (现有，不改动)                 │
│  - NodeFactory / Publisher / Subscriber        │
│  - ZMQ / ROS2 / Zenoh backends                │
└───────────────────────────────────────────────┘
```

---

## 3. 节点自描述：--describe 协议

每个节点 binary 支持 `--describe` 参数，输出 JSON 格式的 manifest：

```bash
$ ./camera_node --describe
```

```json
{
  "name": "camera_node",
  "binary": "camera_node",
  "version": "1.0",
  "config_file": "camera_config.xml",
  "data_ports": {
    "outputs": [
      { "port": "frame_output", "type": "FrameMsg", "desc": "相机采集的图像帧" }
    ],
    "inputs": []
  },
  "provides_services": [
    {
      "role": "camera",
      "endpoints": ["set_exposure", "set_gain", "set_trigger_mode", "soft_trigger", "get_config"]
    }
  ],
  "requires_services": []
}
```

### 3.1 NodeManifest 结构定义

```cpp
struct NodeManifest {
    std::string name;        // 节点类型名（如 "camera_node"）
    std::string binary;      // 二进制文件名
    std::string version;     // 版本
    std::string config_file; // 默认配置文件名

    struct PortInfo {
        std::string port;    // 端口名（如 "frame_output"）
        std::string type;    // 消息类型（如 "FrameMsg"）
        std::string desc;    // 描述
        // input 端口始终单输入；output 端口天然多输出（pub/sub 特性）
    };

    struct ServiceInfo {
        std::string role;                     // 服务角色（如 "camera"）
        std::vector<std::string> endpoints;   // 可用端点
    };

    std::vector<PortInfo> inputs;
    std::vector<PortInfo> outputs;
    std::vector<ServiceInfo> provides_services;
    std::vector<std::string> requires_services;  // 需要的 service role 列表

    std::string toJson() const;
    static NodeManifest fromJson(const std::string& json);
};
```

---

## 4. 端口模型

### 4.1 端口类型

编辑器中只有**数据端口**参与连线。Service 不是端口，而是 `--describe` 中的元数据（见第 7 节）。

| 端口类型 | 方向 | 符号 | 特性 |
|---|---|---|---|
| Input | 订阅 topic | ● 圆形，左侧 | **单输入**：每个 input 端口恰好接受 1 条连线 |
| Output | 发布 topic | ● 圆形，右侧 | **多输出**：每个 output 端口可被多个 input 订阅（pub/sub 天然特性） |

> **为什么没有 Service 端口？** Service 是"算出来的"——由运行时 ServiceRegistry 按 role 自动发现，不在编辑器中画线。`--describe` 中的 `provides_services` / `requires_services` 只是供调度器和编辑器展示用的元数据。

### 4.2 端口连线规则

| 规则 | 说明 |
|---|---|
| 方向 | 只能 output → input |
| 类型匹配 | 两端 PortInfo.type 必须相同 |
| 自连接禁止 | 不能连同一节点 |
| 单输入 | 每个 input 端口恰好接受 1 条连线 |
| 多输出 | output 端口天然支持多路订阅（pub/sub 模型） |
| 多输入 | 需要多路输入时，声明多个命名 input 端口 |
| 无环约束 | 连线不能形成环 |

### 4.3 多输入设计：命名端口

需要接收多个源的数据时，为每个源声明独立的命名端口：

```json
{
  "inputs": [
    { "port": "detection_left",  "type": "DetectionMsg" },
    { "port": "detection_right", "type": "DetectionMsg" }
  ]
}
```

每个 input 端口永远是单输入，多路输入就多开端口。

### 4.4 节点代码中的 API

```cpp
// 每个 input 端口独立订阅
auto sub_left  = edges.subscribe<DetectionMsg>("detection_left");
auto sub_right = edges.subscribe<DetectionMsg>("detection_right");

// output 端口发布（多个 subscriber 可监听同一 topic）
auto pub = edges.publish<FrameMsg>("frame_output");
```

---

## 5. pipeline.xml 格式

### 5.1 完整结构

```xml
<?xml version="1.0"?>
<pipeline>

<!-- 节点模板（由 --describe 扫描生成，编辑器维护） -->
<templates>
  <template name="camera_node">
    <binary>camera_node</binary>
    <out port="frame_output" type="FrameMsg"/>
    <service role="camera"
             endpoints="set_exposure,set_gain,set_trigger_mode,soft_trigger,get_config"/>
  </template>

  <template name="detector_node">
    <binary>detector_node</binary>
    <in  port="frame_input"       type="FrameMsg"/>
    <out port="detection_output"  type="DetectionMsg"/>
    <out port="annotation_output" type="AnnotationMsg"/>
    <service role="detector"
             endpoints="get_result,get_config,onoff,set_threshold,reload_template"/>
  </template>

  <template name="comm_node">
    <binary>comm_node</binary>
    <in  port="detection_input" type="DetectionMsg"/>
    <service role="comm" endpoints="set_config,get_config,get_status"/>
  </template>
</templates>

<!-- 节点实例 -->
<instances>
  <instance name="cam_left"  template="camera_node">
    <config>instances/cam_left/camera_config.xml</config>
  </instance>
  <instance name="cam_right" template="camera_node">
    <config>instances/cam_right/camera_config.xml</config>
  </instance>
  <instance name="det_left"  template="detector_node">
    <config>instances/det_left/detector.xml</config>
  </instance>
  <instance name="det_right" template="detector_node">
    <config>instances/det_right/detector.xml</config>
  </instance>
  <instance name="comm" template="comm_node">
    <config>communication.xml</config>
  </instance>
  <instance name="manager" template="manager"/>
</instances>

<!-- 数据流连线 -->
<wiring>
  <wire from="cam_left"  from_port="frame_output"
        to="det_left"    to_port="frame_input"
        topic="cam_left/frame_output"/>

  <wire from="cam_right" from_port="frame_output"
        to="det_right"   to_port="frame_input"
        topic="cam_right/frame_output"/>

  <wire from="det_left"  from_port="detection_output"
        to="comm"        to_port="detection_left"
        topic="det_left/detection_output"/>

  <wire from="det_right" from_port="detection_output"
        to="comm"        to_port="detection_right"
        topic="det_right/detection_output"/>
</wiring>

<!-- Profile 机制暂不实现，后续按需添加 -->
</pipeline>
```

### 5.2 关键约束

- `<templates>` 中的信息来自 `--describe`，编辑器自动维护
- `<instances>` 中每个实例引用一个 template
- `<wiring>` 只描述数据流（pub/sub），不包含 service 连线
- topic 名称可自动生成（`<instance>/<port>`），也可手动指定
- Service 关系由运行时 `ServiceRegistry` 自动发现，不在 XML 中画线

---

## 6. 单 Binary 多实例

### 6.1 Template vs Instance

| 概念 | 含义 | 示例 |
|---|---|---|
| Template | binary 的端口定义 | `camera_node`（有一个 frame_output） |
| Instance | 运行中的进程 | `cam_left`、`cam_right`（两个独立进程） |

### 6.2 实例配置隔离

```
install/bins/
├── camera_node/            # binary + 默认配置
│   ├── camera_node
│   └── camera_config.xml
├── detector_node/
│   ├── detector_node
│   └── detector.xml
├── instances/              # 实例配置（由编辑器或用户创建）
│   ├── cam_left/
│   │   └── camera_config.xml   # camera_index=0
│   ├── cam_right/
│   │   └── camera_config.xml   # camera_index=1
│   ├── det_left/
│   │   └── detector.xml
│   └── det_right/
│       └── detector.xml
└── pipeline.xml
```

### 6.3 Topic 映射传递

节点启动时通过 `--topic-map` 接收端口到 topic 的映射：

```bash
./camera_node \
    --config instances/cam_left/camera_config.xml \
    --instance cam_left \
    --topic-map frame_output=cam_left/frame_output
```

节点内部通过 `NodeEdgeManager` 解析映射，未映射时使用默认 topic（向后兼容）。

---

## 7. Service 发现机制

### 7.1 设计原则

**数据流是"画出来的"，Service 是"算出来的"。**

- Service 不在 pipeline.xml 中画线
- 节点的 `--describe` 声明 `provides_services`（角色 + 端点）和 `requires_services`（需要的角色）
- DAG 调度器从 requires 推导隐含的启动依赖
- 运行时通过 `ServiceRegistry` 自动发现

### 7.2 ServiceRegistry（传输无关）

ServiceRegistry 必须兼容所有传输层（ZMQ / ROS2 / Zenoh），不能只依赖某一种广播机制。

**方案：复用现有 IService 基础设施**

每个节点启动时，通过已有的 `NodeFactory::createService()` 在对应传输层上注册 service。ServiceRegistry 只是在现有 service 之上封装了一层**角色发现**逻辑：

```cpp
struct ServiceEndpoint {
    std::string instance_name;
    std::string service_name;
    std::string role;
};

class ServiceRegistry {
public:
    // 构建时传入 NodeFactory（自动适配 ZMQ/ROS2/Zenoh）
    explicit ServiceRegistry(NodeFactory& factory);

    // 节点启动时注册自己的 service role
    void registerRole(const std::string& role,
                      const std::string& instance_name,
                      const std::string& service_name);

    // 按角色查找服务提供者
    std::vector<ServiceEndpoint> discover(const std::string& role) const;

    // 等待某个角色的服务可用（启动同步）
    bool waitForRole(const std::string& role, int timeout_ms) const;

    // 创建指向某 role 的 service client（传输无关）
    std::shared_ptr<IService<ServiceRequest, ServiceResponse>>
    createClient(const std::string& role, int index = 0);

private:
    NodeFactory& factory_;
    std::map<std::string, std::vector<ServiceEndpoint>> registry_;
};
```

**各传输层的发现机制：**

| 传输层 | 注册方式 | 发现方式 |
|---|---|---|
| ZMQ | 节点在约定端口发布注册信息 | 订阅注册 topic + 本地缓存 |
| ROS2 | `rclcpp::create_service()` 注册到 ROS2 graph | `list_services()` + `wait_for_service()` |
| Zenoh | `zenoh::declare_queryable()` | `zenoh::get()` 查询 |

> 核心思想：ServiceRegistry 不引入新的通信通道，而是复用已有的传输层 service 机制。切换传输方式时，service 发现自动适配。

### 7.3 启动依赖推导

```
数据流依赖:  cam_left → det_left → comm (从 wiring 推导)
Service 依赖: manager requires [camera, detector, comm]
             → manager 在所有 service provider 之后启动

最终启动顺序: cam_left, cam_right, det_left, det_right, comm, manager
```

---

## 8. DAG 调度器 (dag_scheduler)

### 8.1 核心功能

```cpp
class DagScheduler {
public:
    bool loadFromXml(const std::string& pipeline_path);

    // 校验：无环、无缺失依赖、端口类型匹配
    bool validate(std::string& error_msg) const;

    // 拓扑排序（数据流 + service 依赖）
    std::vector<std::string> computeStartupOrder() const;
};
```

### 8.2 启动流程

```
for instance in startup_order:
    1. 等待上游 service 可用 (waitForProvider)
    2. 构建命令行 (binary + config + --topic-map)
    3. fork/exec 启动进程
    4. 等待进程就绪 (health check)
```

---

## 9. 可视化编辑器

> 已剥离为独立 spec: **`docs/pipeline_editor_spec.md`**
>
> 编辑器在核心功能（Phase 1/2）验证完成后启动开发。

---

## 10. 节点代码改造

### 10.1 改造前后对比

```cpp
// ===== 改造前 (detector_node/main.cpp) =====
auto frame_sub = factory.createSubscriber<FrameMsg>("vision/frame");
g_detection_pub = factory.createPublisher<DetectionMsg>("vision/detection");
g_annotation_pub = factory.createPublisher<AnnotationMsg>("vision/annotation");

// ===== 改造后 =====
NodeEdgeManager edges(factory, "detector_node");
edges.loadFromXml(pipeline_xml_path);

auto frame_sub = edges.subscribe<FrameMsg>("frame_input");
g_detection_pub = edges.publish<DetectionMsg>("detection_output");
g_annotation_pub = edges.publish<AnnotationMsg>("annotation_output");
```

### 10.2 向后兼容

不传 `--topic-map` 时，`NodeEdgeManager` 使用硬编码的默认 topic，现有代码无需改动即可运行。

---

## 11. 依赖说明

| 组件 | 用途 | 引入方式 |
|---|---|---|
| tinyxml2 | 解析/生成 pipeline.xml（标准 XML） | deps/ 子模块或系统包 |
| Qt (QGraphicsView) | DAG 可视化编辑器画布 | 已有（manager 已使用） |
| nlohmann/json 或手写 | --describe JSON 解析 | 头文件库，放入 deps/ |

> 节点各自的配置文件（camera_config.xml 等）仍使用 OpenCV FileStorage，不变。
> pipeline.xml 使用标准 XML，由 tinyxml2 解析，支持 XPath 和 schema 验证。

---

## 12. 落地路线

### Phase 1: 基础设施（核心）

```
  ├─ NodeManifest 结构 + JSON 序列化/反序列化
  ├─ NodeEdgeManager (port → topic 映射, --topic-map 解析)
  ├─ 每个节点 main() 添加 --describe 支持
  ├─ 每个节点 main() 添加 --topic-map 解析
  └─ 单元测试: manifest JSON roundtrip, edge_manager topic 映射
```

### Phase 2: DAG 调度

```
  ├─ DagScheduler (pipeline.xml 解析 + 拓扑排序 + 环检测)
  ├─ ServiceRegistry (传输无关, 复用 IService)
  ├─ dag_launcher (按拓扑序启动节点, 替代 launch_nogui.sh 的固定 sleep)
  └─ 集成测试: 双相机 pipeline 启动 + service 发现
```

### Phase 3: 可视化编辑器 (独立 spec)

```
  → 见 docs/pipeline_editor_spec.md
```

### Phase 4: 集成

```
  ├─ 集成到 manager 的 DAG tab
  ├─ build.sh 新增 pipeline-editor 目标
  └─ 节点健康状态在 DAG 图上显示
```

---

## 13. 已决策项

| # | 决策项 | 结论 |
|---|---|---|
| 1 | pipeline.xml 存储格式 | **标准 XML (tinyxml2/pugixml)** — 更灵活，支持 XPath 查询和 schema 验证 |
| 2 | ServiceRegistry 实现方式 | **传输无关** — 复用现有 IService 基础设施，ZMQ/ROS2/Zenoh 各自用原生 service 机制注册和发现 |
| 3 | DAG 编辑器形态 | **先独立工具，后集成** — 独立 spec (`pipeline_editor_spec.md`)，成熟后集成到 manager |
| 4 | 多实例配置管理 | **全自动生成** — 编辑器/调度器自动创建 instances/ 目录和配置 |
| 5 | Profile 机制 | **暂不实现** — 先做好基础 DAG 功能，Profile 后续按需添加 |
| 6 | --describe 输出格式 | **JSON** — 更通用，编辑器和外部工具解析方便 |
| 7 | topic 名称自动生成 | **`<instance>/<port>`** — 多实例场景下自然隔离，单实例时也清晰 |

---

## 附录 A: 当前系统数据流

```
camera_node ──vision/frame──→ detector_node ──vision/detection──→ comm_node
                                    │
                                    └──vision/annotation──→ manager
camera_node ──vision/frame──→ manager
detector_node ──vision/detection──→ manager
```

## 附录 B: 节点 Manifest 清单

| 节点 | data inputs | data outputs | provides_services | requires_services |
|---|---|---|---|---|
| camera_node | (无) | frame_output (FrameMsg) | camera | (无) |
| detector_node | frame_input (FrameMsg) | detection_output (DetectionMsg), annotation_output (AnnotationMsg) | detector | (无) |
| comm_node | detection_input (DetectionMsg) | (无) | comm | (无) |
| image_publisher_node | (无) | frame_output (FrameMsg) | (无) | (无) |
| manager | frame_input, detection_input, annotation_input | (无) | (无) | camera, detector, comm |
