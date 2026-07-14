# Debug Session: ros2-frame-segmentation-fault

## Status: [OPEN]

### Problem Summary
ROS2 收到 `vision/frame` 消息（4029543 bytes）后发生段错误，核心已转储。

### Environment
- OS: Linux (Ubuntu)
- ROS2: Humble
- Framework: C++
- Message Size: ~4MB

### Reproduction Steps
1. Run `inspector_gui.sh` with ROS2 transport
2. Send image frame to `vision/frame` topic
3. Segmentation fault occurs when receiving message

### Hypotheses
1. **内存分配不足**: 消息数据量较大（约4MB），可能存在栈溢出或堆分配失败
2. **缓冲区越界**: 在解析消息数据时，访问了超出分配缓冲区的内存
3. **生命周期问题**: 消息数据的引用在回调完成后失效（悬空指针）
4. **并发竞争**: 多线程环境下对共享数据的非线程安全访问
5. **序列化/反序列化错误**: 在消息序列化或反序列化过程中出现问题

### Evidence Collection Plan
- 在消息接收回调中添加详细日志，记录数据大小、缓冲区地址、处理步骤
- 在内存分配/复制操作前后添加日志
- 使用 valgrind 或 gdb 获取堆栈信息

### Progress
- [x] Step 1: Create hypotheses
- [x] Step 2: Add instrumentation
- [x] Step 3: Reproduce and collect logs
- [x] Step 4: Analyze evidence
- [x] Step 5: Implement fix
- [x] Step 6: Verify fix
- [x] Step 7: Cleanup

### Root Cause Analysis
**问题根因**：`image_publisher_node` 将图像数据压缩为 PNG 后发送（`pixel_type=0`），但 `InspectorWindow::displayImage` 没有正确处理 PNG 压缩数据，直接将压缩后的 PNG 数据当作原始像素数据处理。

**日志证据**：
```
[DEBUG] [Inspector] pixel_type=0 (灰度), 期望大小=12288000, 实际大小=4029523 
[ERROR] [Inspector] 数据不足: 需要 12288000, 实际 4029523
```

图像参数是 width=4096, height=3000, pixel_type=0（灰度），期望数据大小应该是 `4096 * 3000 = 12,288,000` 字节，但实际只有 `4,029,523` 字节（PNG 压缩后）。

**修复方案**：按照用户要求，移除 PNG 压缩，直接发送原始未压缩的像素数据。

### Fix Applied
修改 `image_publisher_node/main.cpp`：
- 移除 `cv::imencode(".png", gray, compressed)` PNG 压缩逻辑
- 直接发送原始像素数据
- 保持 `pixel_type=0x01080001`（Mono8 格式）

### Instrumentation Added
1. **ros2_backend.h (Ros2Subscriber)**: 在消息接收回调中添加详细日志，追踪：
   - 反序列化前数据大小
   - std::string 创建状态
   - 反序列化完成状态
   - 回调调用完成状态

2. **inspector_window.cpp**: 在 `handleFrameMsg` 和 `displayImage` 方法中添加详细日志：
   - handleFrameMsg: 帧参数、std::vector 创建、invokeMethod 前后
   - displayImage: 各 pixel_type 分支的数据大小检查、cv::Mat 创建、cvtColor 调用、QImage/ QPixmap 创建

**注意**: 之前错误地在 `main_window.cpp` 中添加了日志，但 `system_inspector_gui` 使用的是 `InspectorWindow`。

### Expected Log Output
当收到帧消息时，应该看到类似这样的日志序列：
```
[DEBUG] [ROS2] 收到 topic 'vision/frame' 消息, 4029543 bytes
[DEBUG] [ROS2] 开始反序列化, 数据大小=4029543
[DEBUG] [ROS2] std::string 创建完成, size=4029543, capacity=xxx
[DEBUG] [ROS2] 反序列化完成, 准备调用回调
[DEBUG] [Manager] 帧回调开始, msg.data() size=xxx
[DEBUG] [Manager] 帧参数: height=xxx, width=xxx, pixel_type=xxx, cv_type=xxx
[DEBUG] [Manager] 创建 cv::Mat, size=xxx
[DEBUG] [Manager] cv::Mat 创建完成, data=0x..., size=xxx
[DEBUG] [Manager] 数据大小检查: 期望=xxx, 实际=xxx
[DEBUG] [Manager] 开始 memcpy, data.data()=0x..., current_frame_.data=0x..., size=xxx
[DEBUG] [Manager] memcpy 完成
[DEBUG] [Manager] 帧#xxx 处理完成
[DEBUG] [ROS2] 回调调用完成
```

如果段错误发生在某个步骤之间，就能定位具体问题。
