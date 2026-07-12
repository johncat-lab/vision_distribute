# Vision Messages Registry

> 最后更新: 2026-06-23

## Core Messages

| 消息类型 | 文件 | 维护者 | 版本 | 依赖方 |
|---------|------|-------|------|--------|
| FrameMsg | core/frame_msg.proto | 相机模块 | 1.0 | detector_node, manager |
| ServiceRequest | core/service_msg.proto | 框架团队 | 1.0 | 所有节点 |
| ServiceResponse | core/service_msg.proto | 框架团队 | 1.0 | 所有节点 |

## Detection Messages

| 消息类型 | 文件 | 维护者 | 版本 | 依赖方 |
|---------|------|-------|------|--------|
| DetectionMsg | detection/detection_msg.proto | 检测模块 | 1.0 | comm_node, manager |
| AnnotationMsg | detection/annotation_msg.proto | 检测模块 | 1.0 | manager |
| ObjectAnnotation | detection/annotation_msg.proto | 检测模块 | 1.0 | (嵌套消息) |

## Camera Messages (预留)

| 消息类型 | 文件 | 维护者 | 版本 | 依赖方 |
|---------|------|-------|------|--------|
| (待添加) | camera/*.proto | 相机模块 | - | - |

## Communication Messages (预留)

| 消息类型 | 文件 | 维护者 | 版本 | 依赖方 |
|---------|------|-------|------|--------|
| (待添加) | communication/*.proto | 通信模块 | - | - |

---

## 修改流程

1. 评估影响范围（查看依赖方）
2. 向后兼容性检查
3. 编写单元测试
4. 集成测试
5. 更新本文档
6. Code Review
7. 合并到主分支

---

## 版本历史

### v1.0 (2026-06-23)
- 初始版本
- 添加 FrameMsg、ServiceRequest/Response
- 添加 DetectionMsg、AnnotationMsg
- 建立消息注册表机制
