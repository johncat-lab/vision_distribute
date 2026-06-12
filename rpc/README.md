# rpc — 传输抽象层

## 功能

提供统一的 Pub/Sub + RPC 通信接口，屏蔽底层传输差异。所有节点通过 `NodeFactory` 创建接口实例，运行时根据 `NodeConfig.transport` 选择 ZeroMQ / ROS2 / Zenoh 后端。

## 核心接口

### IService\<Request, Response\>

RPC 服务接口，同时承担**服务端**和**客户端**双重角色：

```cpp
template<typename Request, typename Response>
class IService {
    virtual bool serve(const std::string& endpoint, Handler handler) = 0;   // 注册 handler (服务端)
    virtual Response call(const std::string& endpoint, const Request& req) = 0;  // 调用 (客户端)
    virtual void preconnect() = 0;  // 预连接，避免首次调用延迟
};
```

### IPublisher\<T\>

```cpp
template<typename T>
class IPublisher {
    virtual bool publish(const T& msg) = 0;
    virtual std::string getTopic() const = 0;
};
```

### ISubscriber\<T\>

```cpp
template<typename T>
class ISubscriber {
    using Callback = std::function<void(const T&)>;
    virtual bool subscribe(Callback cb) = 0;
    virtual std::string getTopic() const = 0;
};
```

## NodeFactory

工厂类，根据 `NodeConfig` 创建对应后端的 Publisher / Subscriber / Service 实例：

```cpp
NodeFactory factory(config);

auto pub = factory.createPublisher<FrameMsg>("vision/frame");
auto sub = factory.createSubscriber<DetectionMsg>("vision/detection");
auto svc = factory.createService<ServiceRequest, ServiceResponse>("camera");
```

### 创建逻辑

| 方法 | ZeroMQ | ROS2 | Zenoh |
|------|--------|------|-------|
| `createPublisher` | `ZmqPublisher` (PUB socket) | `Ros2Publisher` (DDS topic) | `ZenohPublisher` |
| `createSubscriber` | `ZmqSubscriber` (SUB socket) | `Ros2Subscriber` (DDS topic) | `ZenohSubscriber` |
| `createService` | `ZmqService` (REQ/REP) | `Ros2Service` (native service) | `ZenohService` |

## NodeConfig — 节点配置

```cpp
struct NodeConfig {
    TransportType transport = TransportType::ZEROMQ;
    std::string node_name;
    std::string domain_id;       // ROS2: domain id, Zenoh: router addr
    uint16_t base_port = 15550;  // ZMQ 基础端口
    std::map<std::string, TopicConfig> topics;

    // ROS2 专用
    int ros2_executor_threads = 4;
    int service_timeout_ms = 5000;
    int service_wait_ms = 3000;
    int service_max_retries = 3;
};
```

通过 `ConfigLoader::loadSystemConfig(xml_path)` 从 `system_config_*.xml` 加载。

## 消息类型

所有消息类型定义在 `rpc/include/rpc/message_types.h`，均实现 `serialize()` / `deserialize()` 二进制序列化。

### FrameMsg — 图像帧

| 字段 | 类型 | 大小 | 说明 |
|------|------|------|------|
| camera_id | int32 | 4B | 相机编号 |
| timestamp | int64 | 8B | 采集时间戳 |
| width | uint16 | 2B | 图像宽度 |
| height | uint16 | 2B | 图像高度 |
| pixel_type | uint32 | 4B | 像素类型 (1=RGB, 其他=Gray) |
| frame_num | uint32 | 4B | 帧序号 |
| exposure_time | float | 4B | 曝光时间 |
| gain | float | 4B | 增益 |
| data_size | uint64 | 8B | 像素数据大小 |
| data | bytes | data_size | 像素数据 |

头部固定 48 字节 + 像素数据。

### DetectionMsg — 检测结果

| 字段 | 类型 | 大小 | 说明 |
|------|------|------|------|
| frame_num | uint32 | 4B | 对应帧序号 |
| timestamp | int64 | 8B | 检测时间戳 |
| object_count | int32 | 4B | 检测物体数 |
| string_length | uint32 | 4B | 协议字符串长度 |
| protocol_string | string | variable | 协议串 (`TA,x,y,a,t,...;` 或 `NG`) |

头部固定 20 字节 + 协议字符串。

### AnnotationMsg — 标注消息

| 字段 | 类型 | 大小 | 说明 |
|------|------|------|------|
| frame_num | uint32 | 4B | 对应帧序号 |
| timestamp | int64 | 8B | 时间戳 |
| template_width | uint32 | 4B | 模板宽度 |
| template_height | uint32 | 4B | 模板高度 |
| object_count | uint32 | 4B | 物体数量 |
| objects[] | — | 40B/个 | 每个物体: x(8B) + y(8B) + angle(8B) + score(8B) + type(4B) + id(4B) |

头部固定 28 字节 + N×40 字节。

### ServiceRequest / ServiceResponse

| 类型 | 字段 | 说明 |
|------|------|------|
| ServiceRequest | `endpoint` + `payload` | 通用请求 (endpoint 路由 + 自定义参数) |
| ServiceResponse | `success` (1B) + `data` (string) | 通用响应 |

## ROS2 后端 — 原生 Service 类型映射

ROS2 后端通过 `registerNativeEndpoint<SrvType>()` 注册 4 个类型转换 lambda，实现通用 `ServiceRequest/Response` 与 ROS2 原生 `.srv` 类型的双向转换：

```
客户端调用:  ServiceRequest → toNativeReq → ROS2 native request → 网络 → 服务端
服务端响应:  服务端 handler → ServiceResponse → fromSrvResp → ROS2 native response → 网络 → 客户端
```

4 个 lambda 参数：

| Lambda | 方向 | 说明 |
|--------|------|------|
| `toSrvReq` | 服务端入 | ROS2 native request → ServiceRequest |
| `fromSrvResp` | 服务端出 | ServiceResponse → ROS2 native response |
| `toNativeReq` | 客户端出 | ServiceRequest → ROS2 native request |
| `fromNativeResp` | 客户端入 | ROS2 native response → ServiceResponse |

### ROS2 后端配置

| 参数 | 默认 | 说明 |
|------|------|------|
| `service_timeout_ms` | 5000 | 调用超时（含自动重试） |
| `service_max_retries` | 3 | 失败重试次数 |
| `ros2_executor_threads` | 4 | MultiThreadedExecutor 线程数 |

### 关键设计

- **不使用 `wait_for_service()`**: 避免内部临时 executor 与已运行的 MultiThreadedExecutor 冲突
- **`async_send_request` 无回调版本**: 直接返回 `SharedFuture`，通过 `future.wait_for(timeout)` 超时
- **`services_holder_`**: 保存 `create_service()` 返回的 shared_ptr，防止 service 对象提前销毁
- **`preconnect()`**: 提前创建所有已注册 endpoint 的原生 client，避免首次调用延迟

## ConfigLoader

从 XML 文件加载 `NodeConfig`：

```cpp
NodeConfig config = ConfigLoader::loadSystemConfig("system_config_ros2.xml");
```

支持的 XML 配置文件：

| 文件 | 传输后端 |
|------|---------|
| `system_config_zeromq.xml` | ZeroMQ |
| `system_config_ros2.xml` | ROS2 |
| `system_config_zenoh.xml` | Zenoh |

## 目录结构

```
rpc/
├── include/rpc/
│   ├── types.h            # TransportType, NodeConfig 定义
│   ├── publisher.h         # IPublisher<T> 接口
│   ├── subscriber.h        # ISubscriber<T> 接口
│   ├── service.h           # IService<Req, Resp> 接口
│   ├── message_types.h     # FrameMsg, DetectionMsg, AnnotationMsg, ServiceRequest/Response
│   ├── config_loader.h     # ConfigLoader 接口
│   └── node_factory.h      # NodeFactory + 模板实现
└── src/
    ├── node_factory.cpp    # NodeFactory 构造 (ZMQ context 初始化)
    ├── config_loader.cpp   # XML 配置解析
    ├── zeromq_backend.h/.cpp   # ZeroMQ 后端实现
    ├── ros2_backend.h/.cpp     # ROS2 后端实现
    └── zenoh_backend.h/.cpp    # Zenoh 后端实现
```
