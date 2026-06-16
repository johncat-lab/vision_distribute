# Pipeline Editor 可视化编辑器规范

> 版本: v1.0
> 日期: 2026-06-15
> 依赖: dag_architecture_spec.md (核心 DAG 架构)
> 状态: 待实现（在核心功能 Phase 1/2 完成后启动）

---

## 1. 定位

Pipeline Editor 是一个**独立 Qt 工具**，用于图形化管理 vision_distribute 的 DAG 拓扑。
核心功能验证（NodeManifest、NodeEdgeManager、DagScheduler、ServiceRegistry）在主 spec Phase 1/2 中完成后，再启动本编辑器开发。

**先独立工具 (`pipeline_editor`)，成熟后集成到 manager。**

---

## 2. 功能列表

| 功能 | 说明 |
|---|---|
| Scan Nodes | 扫描 install/bins/ 下所有 binary，执行 `--describe` 获取 JSON manifest |
| 拖拽节点 | 从模板面板（Palette）拖入画布，创建实例 |
| 连线 | 从 output 端口拖到 input 端口，自动校验类型和规则 |
| 属性编辑 | 点击节点/连线编辑实例名、配置路径、topic |
| Validate | 环检测 + 类型匹配 + 服务依赖检查 + 所有 input 已连接 |
| Export | 生成 pipeline.xml + 自动创建 instances/ 目录和配置 |
| Load | 从已有 pipeline.xml 恢复到画布 |

---

## 3. 技术栈

| 组件 | 用途 |
|---|---|
| Qt QGraphicsScene / QGraphicsView | 画布渲染、节点拖拽、连线交互 |
| tinyxml2 | 解析/生成 pipeline.xml |
| NodeManifest (共享) | 复用核心层的 --describe 解析 |
| DagScheduler (共享) | 复用核心层的拓扑排序和环检测 |

---

## 4. 项目结构

```
pipeline_editor/
├── main.cpp                     # 入口
├── include/
│   ├── DagEditorWindow.h        # 主窗口
│   ├── NodeGraphicsItem.h       # 节点图形项（可拖拽方块）
│   ├── EdgeGraphicsItem.h       # 连线图形项（贝塞尔曲线）
│   ├── PortGraphicsItem.h       # 端口图形项（节点上的小圆点）
│   ├── NodePalette.h            # 左侧节点模板面板
│   ├── PropertyPanel.h          # 右侧属性编辑面板
│   ├── ServiceTable.h           # 底部服务信息面板（只读）
│   ├── PipelineExporter.h       # pipeline.xml 生成器
│   └── PipelineImporter.h       # pipeline.xml 加载器
├── src/
│   ├── DagEditorWindow.cpp
│   ├── NodeGraphicsItem.cpp
│   ├── EdgeGraphicsItem.cpp
│   ├── PortGraphicsItem.cpp
│   ├── NodePalette.cpp
│   ├── PropertyPanel.cpp
│   ├── ServiceTable.cpp
│   ├── PipelineExporter.cpp
│   └── PipelineImporter.cpp
└── CMakeLists.txt
```

---

## 5. 视觉规范

### 5.1 节点外观

```
┌──────────────────────────────────┐
│         cam_left                  │  ← 标题栏（深蓝底白字）
│         (camera_node)             │  ← 模板名（灰色小字）
│──────────────────────────────────│
│ ● frame_output    [FrameMsg]     │  ← 端口（圆形 + 名称 + 类型标签）
└──────────────────────────────────┘
```

- 节点宽度：160px（最小），自适应内容
- 端口间距：24px
- 标题栏高度：30px
- 圆角：8px
- 选中时高亮边框

### 5.2 端口外观

- **形状**: ● 圆形，半径 6px
- **位置**: input 在节点左侧边缘，output 在节点右侧边缘
- **颜色**: 按消息类型着色

| 消息类型 | 颜色 |
|---|---|
| FrameMsg | 绿色 (0, 180, 0) |
| DetectionMsg | 橙色 (255, 165, 0) |
| AnnotationMsg | 蓝色 (100, 100, 255) |
| 其他 | 灰色 (200, 200, 200) |

- 端口名标签：input 在圆点右侧，output 在圆点左侧
- hover 时放大 + 高亮

### 5.3 连线外观

- **样式**: 实线，2.5px 宽，圆头端点
- **形状**: 三次贝塞尔曲线
- **颜色**: 跟随源端口的消息类型颜色
- **箭头**: 目标端绘制三角箭头
- **选中**: 加粗到 4px + 虚线边框

### 5.4 布局

```
┌───────────────────────────────────────────────────────────┐
│  Vision DAG Editor          [Scan] [Validate] [Export]    │  ← 工具栏
├────────────┬──────────────────────────────────┬────────────┤
│  Palette   │         Canvas                   │ Properties │
│            │                                  │            │
│ ▼ camera_  │    [node] ──→ [node] ──→ [node] │ 实例名:    │
│   node     │                                  │ 模板:      │
│            │                                  │ 配置:      │
│ ▼ detector │    ● ──→ ●                      │ topic:     │
│   _node    │                                  │            │
│            │                                  │            │
│ ▼ comm_    │                                  │            │
│   node     │                                  │            │
├────────────┴──────────────────────────────────┴────────────┤
│  Services (只读)                                            │
│  cam_left    provides: camera  [set_exposure, get_config]   │
│  det_left    provides: detector [onoff, get_result]         │
│  manager     requires: camera, detector, comm               │
│                                                             │
│  Startup Order: cam_left → det_left → comm → manager        │
└─────────────────────────────────────────────────────────────┘
```

- 左侧 Palette: QDockWidget，列表显示所有可用模板
- 中间 Canvas: QGraphicsView，主编辑区域
- 右侧 Properties: QDockWidget，选中项的属性编辑
- 底部 Services: QDockWidget，服务信息只读面板

---

## 6. 核心组件设计

### 6.1 NodeGraphicsItem

```cpp
class NodeGraphicsItem : public QGraphicsItem {
public:
    NodeGraphicsItem(const NodeManifest& manifest,
                     const std::string& instance_name,
                     QGraphicsItem* parent = nullptr);

    const NodeManifest& manifest() const;
    const std::string& instanceName() const;
    void setInstanceName(const std::string& name);

    QPointF inputPortPos(const std::string& port_name) const;
    QPointF outputPortPos(const std::string& port_name) const;
    PortGraphicsItem* findPort(const std::string& port_name, bool is_input) const;

    QRectF boundingRect() const override;
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override;
};
```

### 6.2 PortGraphicsItem

```cpp
class PortGraphicsItem : public QGraphicsItem {
public:
    PortGraphicsItem(const NodeManifest::PortInfo& info,
                     QGraphicsItem* parent);

    const NodeManifest::PortInfo& portInfo() const;
    NodeGraphicsItem* ownerNode() const;
    QPointF sceneCenter() const;

    // 兼容性检查（用于连线时判断）
    bool isCompatible(const PortGraphicsItem* other) const;

    void setHighlighted(bool on);
    QRectF boundingRect() const override;
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override;
};
```

### 6.3 EdgeGraphicsItem

```cpp
class EdgeGraphicsItem : public QGraphicsPathItem {
public:
    EdgeGraphicsItem(PortGraphicsItem* source, PortGraphicsItem* target);

    PortGraphicsItem* sourcePort() const;
    PortGraphicsItem* targetPort() const;
    NodeGraphicsItem* sourceNode() const;
    NodeGraphicsItem* targetNode() const;

    std::string topic() const;
    void setTopic(const std::string& topic);

    void updatePath();  // 重新计算贝塞尔曲线
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override;
};
```

### 6.4 DagEditorWindow

```cpp
class DagEditorWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit DagEditorWindow(const std::string& bins_dir, QWidget* parent = nullptr);

private slots:
    void onScanNodes();      // 扫描 install/bins/
    void onValidate();       // 校验 DAG
    void onExport();         // 导出 pipeline.xml
    void onLoad();           // 加载 pipeline.xml

private:
    // 扫描
    void scanNodes();
    std::vector<NodeManifest> manifests_;

    // 画布
    QGraphicsScene* scene_;
    QGraphicsView* view_;
    std::vector<NodeGraphicsItem*> canvas_nodes_;
    std::vector<EdgeGraphicsItem*> canvas_edges_;

    // 拖拽连线状态
    PortGraphicsItem* drag_source_ = nullptr;
    QGraphicsPathItem* temp_edge_ = nullptr;

    // 子组件
    NodePalette* palette_;
    PropertyPanel* properties_;
    ServiceTable* service_table_;
};
```

---

## 7. 交互流程

### 7.1 扫描节点

```
用户点击 [Scan]
  → 遍历 install/bins/ 下所有子目录
  → 对每个 binary 执行 ./<binary> --describe
  → 解析 JSON 输出为 NodeManifest
  → 填充左侧 Palette 面板
  → 底部 ServiceTable 更新
```

### 7.2 创建实例

```
用户从 Palette 拖拽模板到 Canvas
  → 弹出命名对话框（默认: <template_name>_<N>）
  → 创建 NodeGraphicsItem 实例
  → 添加到 canvas_nodes_
  → 底部 ServiceTable 更新
```

### 7.3 连线

```
用户在 output 端口按下鼠标
  → drag_source_ = 该端口
  → 创建临时虚线 EdgeGraphicsItem

鼠标移动
  → 更新临时连线的终点到鼠标位置
  → 兼容的 input 端口高亮（类型匹配 + 未满）
  → 不兼容的端口灰显

鼠标释放
  → 如果释放在兼容的 input 端口上:
      → 弹出类型检查（通过 isCompatible）
      → 创建正式 EdgeGraphicsItem
      → 添加到 canvas_edges_
  → 否则取消连线
  → 清理临时连线
```

### 7.4 连线校验规则

```cpp
bool canConnect(PortGraphicsItem* src, PortGraphicsItem* dst) {
    // 1. 方向: output → input
    if (!src->portInfo().is_output || dst->portInfo().is_output) return false;
    // 2. 类型匹配
    if (src->portInfo().type != dst->portInfo().type) return false;
    // 3. 不能自连接
    if (src->ownerNode() == dst->ownerNode()) return false;
    // 4. input 端口单输入: 检查是否已有连线
    for (auto* e : canvas_edges_) {
        if (e->targetPort() == dst) return false;
    }
    // 5. 无环检测
    if (wouldCreateCycle(src->ownerNode(), dst->ownerNode())) return false;
    return true;
}
```

### 7.5 校验 DAG

```
用户点击 [Validate]
  → 检查所有 input 端口是否已连接
  → 检查是否有孤立节点
  → 运行 DagScheduler::validate() (环检测 + 类型匹配)
  → 检查 service 依赖是否满足 (requires_services 都有 provider)
  → 显示启动顺序预览
  → 结果弹窗: ✓ 通过 / ✗ N 个问题
```

### 7.6 导出 pipeline.xml

```
用户点击 [Export]
  → PipelineExporter 生成:
      1. <templates> 从 manifests_ 生成
      2. <instances> 从 canvas_nodes_ 生成
      3. <wiring> 从 canvas_edges_ 生成
      4. topic 自动生成: <instance>/<port>（可在属性面板覆盖）
  → 自动创建 instances/ 子目录
  → 自动复制/生成每个实例的配置文件
  → 写入 pipeline.xml
```

### 7.7 加载 pipeline.xml

```
用户点击 [Load]
  → PipelineImporter 解析 XML
  → 清空画布
  → 恢复 <instances> 为 NodeGraphicsItem
  → 恢复 <wiring> 为 EdgeGraphicsItem
  → 恢复节点位置（如果 XML 中有保存）
```

---

## 8. PipelineExporter

```cpp
class PipelineExporter {
public:
    static bool exportToXml(
        const std::string& output_path,
        const std::string& instances_dir,
        const std::vector<NodeGraphicsItem*>& nodes,
        const std::vector<EdgeGraphicsItem*>& edges);
};
```

生成流程:
1. 收集所有 unique templates → 写入 `<templates>`
2. 遍历 canvas_nodes_ → 写入 `<instances>`
3. 遍历 canvas_edges_ → 写入 `<wiring>`
4. topic 规则: 如果用户未手动设置，自动生成 `<source_instance>/<source_port>`
5. 为每个实例创建 `instances/<name>/` 目录并复制默认配置

---

## 9. ServiceTable（只读面板）

扫描完成后自动填充，显示:

| Instance | Role | Type | Endpoints |
|---|---|---|---|
| cam_left | camera | provides | set_exposure, get_config, ... |
| cam_right | camera | provides | set_exposure, get_config, ... |
| det_left | detector | provides | onoff, get_result, ... |
| manager | camera, detector, comm | requires | — |

底部显示自动推导的启动顺序:
```
Startup Order: cam_left → cam_right → det_left → det_right → comm → manager
```

---

## 10. 落地路线

```
Phase E1: 骨架
  ├─ CMakeLists.txt + 项目结构
  ├─ DagEditorWindow 主窗口布局（三栏 + 底部面板）
  └─ 基本 QGraphicsScene 画布

Phase E2: 节点与端口
  ├─ NodeGraphicsItem（渲染 + 拖拽移动）
  ├─ PortGraphicsItem（渲染 + hover 高亮）
  └─ 从 Palette 拖入画布创建实例

Phase E3: 连线
  ├─ EdgeGraphicsItem（贝塞尔曲线）
  ├─ 鼠标拖拽连线交互
  ├─ canConnect() 校验（类型 + 方向 + 单输入 + 无环）
  └─ 删除连线

Phase E4: 扫描与导出
  ├─ scanNodes()（执行 --describe）
  ├─ PipelineExporter（生成 pipeline.xml + instances/）
  ├─ PipelineImporter（从 XML 恢复画布）
  └─ Validate DAG

Phase E5: 集成
  ├─ PropertyPanel（属性编辑）
  ├─ ServiceTable（服务信息 + 启动顺序）
  ├─ build.sh 新增 pipeline-editor 目标
  └─ 后续: 集成到 manager DAG tab
```

---

## 附录: 与主 spec 的关系

| 主 spec (dag_architecture_spec.md) | 本 spec (pipeline_editor_spec.md) |
|---|---|
| Phase 1: NodeManifest + --describe | 本 spec 消费 --describe 输出 |
| Phase 2: DagScheduler + 调度 | 本 spec 复用 DagScheduler 的校验逻辑 |
| 端口模型（第 4 节） | 本 spec 的 PortGraphicsItem 实现端口渲染 |
| pipeline.xml 格式（第 5 节） | 本 spec 的 Exporter/Importer 读写此格式 |
| Service 发现（第 7 节） | 本 spec 的 ServiceTable 展示服务信息 |
