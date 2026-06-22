# DAG 步骤控制功能实现方案

## 背景

当前DAG调度器已实现基于数据流和Service依赖的拓扑排序,但缺少显式的步骤控制。本方案添加`<steps>`段支持,允许用户在满足数据流依赖的前提下,按步骤顺序启动节点。

## 需求总结

1. **步骤定义**: pipeline.xml中添加`<steps>`段,将节点分组到不同步骤
2. **并行执行**: 同一步骤内节点并行启动,步骤间顺序执行
3. **依赖优先**: 数据流依赖 > Service依赖 > 步骤顺序
4. **向后兼容**: 无`<steps>`段时保持现有行为

---

## 实现方案

### 1. 数据结构设计

#### 1.1 新增步骤定义结构

在 `dag_scheduler.h` 中添加:

```cpp
/// @brief pipeline.xml 中的步骤定义
struct StepDef {
    int order;                  // 步骤顺序 (1, 2, 3...)
    std::string name;           // 步骤名称 (可选,用于日志)
    std::vector<std::string> instances;  // 该步骤包含的实例名
};
```

#### 1.2 修改启动顺序返回值

```cpp
/// @brief 计算启动顺序(支持步骤分组)
/// @return 按启动顺序排列的实例名列表(步骤内按字母序)
std::vector<std::string> computeStartupOrder() const;

/// @brief 计算带步骤分组的启动顺序
/// @return 步骤列表,每个步骤包含实例名列表
std::vector<std::vector<std::string>> computeStartupOrderWithSteps() const;
```

#### 1.3 添加成员变量

```cpp
std::vector<StepDef> steps_;  // 按 order 排序的步骤列表
bool hasSteps_;               // 是否定义了步骤
```

### 2. XML 解析逻辑

#### 2.1 在 `loadFromXml()` 中添加步骤解析

```cpp
// 在 wiring 解析之后添加
bool has_steps = false;
XMLElement* steps_elem = root->FirstChildElement("steps");
if (steps_elem) {
    has_steps = true;
    for (XMLElement* step = steps_elem->FirstChildElement("step"); 
         step; step = step->NextSiblingElement("step")) {
        StepDef sd;
        
        // 解析 order 属性
        const char* order_attr = step->Attribute("order");
        if (order_attr) {
            sd.order = std::atoi(order_attr);
        } else {
            sd.order = 999;  // 默认值
        }
        
        // 解析 name 属性
        sd.name = step->Attribute("name") ? step->Attribute("name") : "";
        
        // 解析包含的实例
        for (XMLElement* inst = step->FirstChildElement("instance"); 
             inst; inst = inst->NextSiblingElement("instance")) {
            const char* inst_name = inst->Attribute("name");
            if (inst_name) {
                sd.instances.push_back(inst_name);
            }
        }
        
        steps_.push_back(sd);
    }
    
    // 按 order 排序
    std::sort(steps_.begin(), steps_.end(), 
              [](const StepDef& a, const StepDef& b) {
                  return a.order < b.order;
              });
}
hasSteps_ = has_steps;
```

#### 2.2 示例 pipeline.xml

```xml
<?xml version="1.0"?>
<pipeline>
  <templates>
    <!-- 模板定义(不变) -->
  </templates>

  <instances>
    <!-- 实例定义(不变) -->
  </instances>

  <wiring>
    <!-- 连线定义(不变) -->
  </wiring>

  <!-- 新增步骤定义 -->
  <steps>
    <step order="1" name="init_cameras">
      <instance name="cam_left"/>
      <instance name="cam_right"/>
    </step>
    <step order="2" name="init_detectors">
      <instance name="det_left"/>
      <instance name="det_right"/>
    </step>
    <step order="3" name="start_comm">
      <instance name="comm"/>
    </step>
  </steps>
</pipeline>
```

### 3. 核心算法: 步骤感知的拓扑排序

#### 3.1 算法设计

```
算法: computeStartupOrderWithSteps()

输入:
  - 数据流依赖图(从 wires_ 构建的邻接表)
  - Service依赖(从 templates 的 requires_services 推导)
  - 步骤定义(steps_)

输出:
  - 步骤列表: [[step1_instances], [step2_instances], ...]

步骤:

1. 构建完整的依赖图(数据流 + Service)
   adj: instance → set<instance>  (instance 依赖 adj[instance])

2. 如果未定义步骤,使用现有拓扑排序逻辑

3. 如果定义了步骤:
   a. 校验步骤定义不违反依赖关系
      对每条依赖边 A → B (A依赖B):
        - 找到A所在的步骤 step_A
        - 找到B所在的步骤 step_B
        - 必须满足: step_B.order < step_A.order
        - 否则报错: "步骤冲突: A(step_X) 依赖 B(step_Y),但 step_Y >= step_X"

   b. 按步骤顺序处理
      result_steps = []
      for step in steps_(按order升序):
        step_instances = step.instances
        
        // 检查步骤内实例是否都已满足依赖
        for inst in step_instances:
          检查 inst 的所有依赖是否都在之前的步骤中
          如果有依赖不在之前步骤,报错
        
        // 步骤内按字母序排序(确定性)
        sort(step_instances)
        
        result_steps.push(step_instances)
      
      // 检查是否有实例未被步骤覆盖
      uncovered = all_instances - 所有步骤中的实例
      if uncovered not empty:
        // 选项1: 报错
        // 选项2: 自动追加到最后(推荐)
        sort(uncovered)
        result_steps.push(uncovered)
      
      return result_steps

4. 兼容性: computeStartupOrder() 调用 computeStartupOrderWithSteps() 
   并flatten结果
```

#### 3.2 C++ 实现要点

```cpp
std::vector<std::vector<std::string>> 
DagScheduler::computeStartupOrderWithSteps() const {
    // 1. 构建依赖图
    std::map<std::string, std::set<std::string>> dependencies;
    // ... 从 wires_ 和 service 依赖构建
    
    // 2. 无步骤定义,回退到拓扑排序
    if (!hasSteps_) {
        auto flat_order = topoSort();
        return {flat_order};  // 单步骤
    }
    
    // 3. 校验步骤不违反依赖
    std::map<std::string, int> instance_to_step;
    for (size_t i = 0; i < steps_.size(); i++) {
        for (const auto& inst : steps_[i].instances) {
            instance_to_step[inst] = steps_[i].order;
        }
    }
    
    // 检查依赖冲突
    for (const auto& [inst, deps] : dependencies) {
        if (instance_to_step.count(inst) == 0) continue;
        int inst_step = instance_to_step.at(inst);
        
        for (const auto& dep : deps) {
            if (instance_to_step.count(dep) == 0) continue;
            int dep_step = instance_to_step.at(dep);
            
            if (dep_step >= inst_step) {
                // 依赖的步骤序号 >= 当前实例的步骤序号,冲突
                throw std::runtime_error(
                    "步骤冲突: " + inst + "(step " + std::to_string(inst_step) + 
                    ") 依赖 " + dep + "(step " + std::to_string(dep_step) + ")");
            }
        }
    }
    
    // 4. 按步骤顺序输出
    std::vector<std::vector<std::string>> result;
    std::set<std::string> covered_instances;
    
    for (const auto& step : steps_) {
        std::vector<std::string> step_insts = step.instances;
        std::sort(step_insts.begin(), step_insts.end());
        
        result.push_back(step_insts);
        covered_instances.insert(step_insts.begin(), step_insts.end());
    }
    
    // 5. 处理未覆盖的实例
    std::vector<std::string> uncovered;
    for (const auto& [name, _] : instances_) {
        if (covered_instances.find(name) == covered_instances.end()) {
            uncovered.push_back(name);
        }
    }
    if (!uncovered.empty()) {
        std::sort(uncovered.begin(), uncovered.end());
        result.push_back(uncovered);
    }
    
    return result;
}
```

### 4. 启动逻辑改造 (dag/main.cpp)

#### 4.1 按步骤并行启动

```cpp
// 替换现有的线性启动逻辑
auto step_groups = scheduler.computeStartupOrderWithSteps();

int step_num = 0;
for (const auto& step_instances : step_groups) {
    step_num++;
    LOG_INFO("[DagLauncher] === 步骤 %d (%zu 个节点) ===", 
             step_num, step_instances.size());
    
    // 并行启动本步骤的所有节点
    for (const auto& inst_name : step_instances) {
        // ... 构建命令行
        // ... fork/exec
        
        // 注意: 不在这里 sleep,而是等所有节点启动后再sleep
    }
    
    // 步骤间等待(确保上一步节点就绪)
    if (startup_delay_ms > 0 && step_num < step_groups.size()) {
        LOG_INFO("[DagLauncher] 等待 %d ms 后执行下一步...", startup_delay_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(startup_delay_ms));
    }
}
```

#### 4.2 关键改动点

1. 使用 `computeStartupOrderWithSteps()` 替代 `computeStartupOrder()`
2. 外层循环遍历步骤,内层循环遍历步骤内实例
3. 步骤内节点连续启动(无延迟),步骤间加延迟
4. 日志输出增强,显示步骤信息

### 5. 校验增强

#### 5.1 在 `validate()` 中新增检查

```cpp
bool DagScheduler::validate(std::string& error_msg) const {
    std::ostringstream errors;
    
    // ... 现有校验逻辑 ...
    
    // 新增: 步骤相关校验
    if (hasSteps_) {
        // 5.1.1 检查步骤引用的实例是否存在
        std::set<std::string> all_step_instances;
        for (const auto& step : steps_) {
            for (const auto& inst : step.instances) {
                if (instances_.find(inst) == instances_.end()) {
                    errors << "步骤 '" << step.name 
                           << "' 引用了不存在的实例 '" << inst << "'\n";
                }
                all_step_instances.insert(inst);
            }
        }
        
        // 5.1.2 检查实例是否在多个步骤中重复出现
        std::map<std::string, int> instance_step_count;
        for (const auto& step : steps_) {
            for (const auto& inst : step.instances) {
                instance_step_count[inst]++;
            }
        }
        for (const auto& [inst, count] : instance_step_count) {
            if (count > 1) {
                errors << "实例 '" << inst << "' 在多个步骤中重复出现(" 
                       << count << "次)\n";
            }
        }
        
        // 5.1.3 检查步骤顺序号是否重复
        std::set<int> step_orders;
        for (const auto& step : steps_) {
            if (!step_orders.insert(step.order).second) {
                errors << "步骤顺序号 " << step.order << " 重复\n";
            }
        }
        
        // 5.1.4 检查步骤是否违反依赖关系(已在排序算法中检查,这里可提前)
        // 可选: 提前检查并报告详细错误
    }
    
    // ... 后续逻辑 ...
}
```

### 6. API 设计总结

#### 6.1 新增/修改的公共方法

```cpp
class DagScheduler {
public:
    // 现有方法(不变)
    bool loadFromXml(const std::string& pipeline_path);
    bool validate(std::string& error_msg) const;
    std::vector<std::string> computeStartupOrder() const;  // 保持兼容
    
    // 新增方法
    std::vector<std::vector<std::string>> computeStartupOrderWithSteps() const;
    
    bool hasSteps() const { return hasSteps_; }
    const std::vector<StepDef>& steps() const { return steps_; }
    
    // 现有 getters(不变)
    const std::map<std::string, TemplateDef>& templates() const { return templates_; }
    const std::map<std::string, InstanceDef>& instances() const { return instances_; }
    const std::vector<WireDef>& wires() const { return wires_; }
};
```

#### 6.2 computeStartupOrder() 的兼容实现

```cpp
std::vector<std::string> DagScheduler::computeStartupOrder() const {
    if (!hasSteps_) {
        return topoSort();  // 无步骤,使用原逻辑
    }
    
    auto step_groups = computeStartupOrderWithSteps();
    std::vector<std::string> flat_result;
    for (const auto& group : step_groups) {
        flat_result.insert(flat_result.end(), group.begin(), group.end());
    }
    return flat_result;
}
```

---

## 改造文件清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `dag/include/dag/dag_scheduler.h` | 修改 | 添加StepDef结构体,新增方法声明 |
| `dag/src/dag_scheduler.cpp` | 修改 | 添加步骤解析,实现新排序算法,增强校验 |
| `dag/main.cpp` | 修改 | 改造启动逻辑支持步骤并行 |
| `docs/dag_architecture_spec.md` | 更新 | 补充步骤控制的架构文档 |
| `config/pipeline_detect.xml` | 示例 | 添加步骤定义示例(可选) |

---

## 测试方案

### 1. 单元测试

- 解析带`<steps>`的pipeline.xml
- 解析不带`<steps>`的pipeline.xml(向后兼容)
- 步骤校验:实例不存在、重复出现、顺序号重复
- 步骤冲突检测:步骤定义违反数据流依赖
- 排序算法:有步骤 vs 无步骤的输出对比

### 2. 集成测试

- 使用带步骤的pipeline启动节点
- 验证步骤内节点并行启动
- 验证步骤间顺序执行
- 验证无步骤时行为不变

### 3. 测试用例示例

```xml
<!-- test_steps_conflict.xml: 步骤违反依赖 -->
<wiring>
  <wire from="A" ... to="B" .../>  <!-- A→B, A必须在B之后 -->
</wiring>
<steps>
  <step order="1"><instance name="A"/></step>  <!-- A在步骤1 -->
  <step order="2"><instance name="B"/></step>  <!-- B在步骤2,但依赖A,冲突! -->
</steps>
```

---

## 关键设计决策

| 决策项 | 选择 | 理由 |
|--------|------|------|
| 步骤定义位置 | `<steps>`独立段 | 清晰,不影响现有结构 |
| 未覆盖实例处理 | 自动追加到最后 | 向后兼容,不强制所有实例都要在步骤中 |
| 步骤内排序 | 字母序 | 确定性,易于调试 |
| 步骤冲突处理 | 抛出异常 | 明确错误,不允许隐式覆盖 |
| 向后兼容 | 无`<steps>`时使用原逻辑 | 不破坏现有pipeline |

---

## 实施步骤

### Phase 1: 数据结构与解析 (1-2小时)
1. 在 `dag_scheduler.h` 添加 `StepDef` 结构体
2. 在 `loadFromXml()` 实现步骤解析
3. 单元测试: 解析正确性

### Phase 2: 排序算法 (2-3小时)
1. 实现 `computeStartupOrderWithSteps()`
2. 实现步骤冲突检测
3. 修改 `computeStartupOrder()` 保持兼容
4. 单元测试: 排序正确性、冲突检测

### Phase 3: 校验增强 (1小时)
1. 在 `validate()` 添加步骤校验
2. 单元测试: 各种校验场景

### Phase 4: 启动逻辑改造 (1-2小时)
1. 修改 `dag/main.cpp` 支持步骤并行
2. 增强日志输出
3. 集成测试: 端到端验证

### Phase 5: 文档与示例 (1小时)
1. 更新架构文档
2. 添加示例pipeline.xml
3. 编写使用说明

---

## 预期效果

### 改造前
```
启动顺序: cam_left, cam_right, det_left, det_right, comm
(完全由数据流推导,用户无法控制)
```

### 改造后
```
步骤 1: cam_left, cam_right (并行启动)
[等待 500ms]
步骤 2: det_left, det_right (并行启动)
[等待 500ms]
步骤 3: comm (启动)

用户完全控制步骤顺序,同时保证数据流依赖不被违反
```

---

## 风险与注意事项

1. **步骤定义错误**: 用户可能定义违反依赖的步骤,需清晰报错
2. **性能影响**: 步骤内并行启动可能瞬间消耗较多资源,需控制并发数(可选)
3. **调试难度**: 并行启动可能导致日志交错,建议步骤日志加前缀
4. **Service依赖**: 确保Service依赖也在步骤排序中考虑(已在依赖图中包含)
