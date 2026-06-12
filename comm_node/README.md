# comm_node — 通信输出节点

## 功能

接收 `detector_node` 发布的检测结果，通过 TCP Server/Client 转发给外部设备（PLC、上位机等）。同时提供 RPC service 供 manager 动态配置通信参数。

## 启动命令

```bash
comm_node --config <system_config.xml> --comm-config <communication.xml>
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `--config` | 是 | 系统传输配置 (`system_config_zeromq.xml` 等) |
| `--comm-config` | 否 | 通信参数配置，缺失时使用默认值 (server, 0.0.0.0:7930) |

## 数据流

### Pub/Sub 订阅

| Topic | 消息类型 | 说明 |
|-------|---------|------|
| `vision/detection` | `DetectionMsg` | 检测结果协议字符串 |

订阅后自动将 `DetectionMsg.protocol_string` 通过 TCP 发送给已连接的客户端。

### RPC Service Endpoints

所有 endpoint 注册在 service name `comm` 下（ROS2 模式映射为 `/comm/<endpoint>`）：

| Endpoint | ROS2 Service Type | 请求 | 响应 |
|----------|-------------------|------|------|
| `set_config` | `CommSetConfig` | `string config_data` | `bool success, string message` |
| `get_config` | `CommGetConfig` | (空) | `bool success, string config_data` |
| `get_status` | `CommGetStatus` | (空) | `bool success, string status_data` |

#### ROS2 CLI 调用示例

```bash
# 获取当前配置
ros2 service call /comm/get_config vision_interfaces/srv/CommGetConfig "{}"

# 修改端口
ros2 service call /comm/set_config vision_interfaces/srv/CommSetConfig "{config_data: 'port=8000'}"

# 切换为 client 模式
ros2 service call /comm/set_config vision_interfaces/srv/CommSetConfig "{config_data: 'mode=client,host=192.168.1.100'}"

# 查看连接状态
ros2 service call /comm/get_status vision_interfaces/srv/CommGetStatus "{}"
```

#### set_config payload 格式

`key=value` 逗号分隔，支持的字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `host` | string | 监听/连接地址 |
| `port` | int | TCP 端口 |
| `mode` | string | `server` 或 `client` |
| `server_mode` | int | 1=连接即发, 2=请求即发, 3=周期发送 |
| `interval_ms` | int | 周期发送间隔 (仅 server_mode=3) |

修改生效后节点会自动重启 TCP 连接。

## 配置文件

### communication.xml — 通信参数

```xml
<?xml version="1.0"?>
<opencv_storage>
<mode>"server"</mode>           <!-- server | client -->
<host>"0.0.0.0"</host>          <!-- 监听地址(server) 或 连接地址(client) -->
<port>7930</port>               <!-- TCP 端口 -->
<server_mode>2</server_mode>    <!-- 1=SEND_ON_CONNECT 2=SEND_ON_REQUEST 3=SEND_PERIODIC -->
<interval_ms>100</interval_ms>  <!-- 周期发送间隔 (ms)，仅 server_mode=3 生效 -->
</opencv_storage>
```

### TCP Server 工作模式

| server_mode | 名称 | 行为 |
|-------------|------|------|
| 1 | SEND_ON_CONNECT | 客户端连接后立即发送最新检测结果 |
| 2 | SEND_ON_REQUEST | 等待客户端请求后发送（推荐工业场景） |
| 3 | SEND_PERIODIC | 按 `interval_ms` 间隔持续发送最新结果 |

### Client 模式

- 支持自动重连（3 秒间隔）
- 连接状态变化时输出日志

## 实现说明

- Server 模式支持多客户端同时连接，结果广播给所有活跃客户端
- `set_config` 会同时将配置保存到 `communication.xml` 文件
- 检测结果使用 `DetectionMsg.protocol_string` 原样转发（格式: `TA,x,y,a,t,...;` 多目标以 `;` 分隔）
