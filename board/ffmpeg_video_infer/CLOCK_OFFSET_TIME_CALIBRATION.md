# ffmpeg_video_infer 时间校准与 Clock Offset 协议说明

这份文档专门说明 `ffmpeg_video_infer` 当前板子端的时间校准链路，包括：

1. 相关 UDP 端口分别做什么
2. clock offset server 的请求/响应协议
3. 板子端在什么时机记录哪些时间
4. 主机端如何用 `t1/t2/t3/t4` 计算 RTT 和 offset
5. 结果 UDP JSON 里的 timing 字段如何与 clock offset 配合使用

## 1. 总体目的

当前 `ffmpeg_video_infer` 里有两类和时间相关的 UDP 通道：

1. 视频输入通道
   板子接收主机推送的 `UDP MPEG-TS H.264` 视频流。
2. 结果回传通道
   板子把每帧检测结果和 profiling 字段发回主机。
3. clock offset 校准通道
   主机向板子发一个极小的时间探测请求，板子立即回包，主机据此估计板子和主机之间的时钟偏移。

这里单独讲的是第 3 条，也就是“板子和主机怎么做时间校准”。

## 2. 当前用到的 UDP 端口

| 端口 | 方向 | 用途 | 默认值来源 |
| --- | --- | --- | --- |
| `5000` | 主机 -> 板子 | 视频输入，接收 UDP MPEG-TS H.264 | `VideoInferOptions.input_url` |
| `6000` | 板子 -> 主机 | 检测结果 JSON 回传 | `VideoInferOptions.result_udp_port` |
| `45678` | 主机 -> 板子 -> 主机 | clock offset 时间校准 | `VideoInferOptions.clock_offset_server_port` |

补充说明：

- clock offset server 当前默认监听 `0.0.0.0:45678`
- 结果回传 UDP 当前目标默认是 `192.168.7.1:6000`
- clock offset 响应不是发回 `6000`
- clock offset 响应必须发回请求的源 IP + 源端口

## 3. 板子端相关模块

当前时间校准只涉及下面这个模块：

- 头文件：`include/clock_offset_server.hpp`
- 实现：`src/clock_offset_server.cpp`

这个模块的职责只有一件事：

- 监听一个 UDP 端口
- 收到主机的时间探测 JSON
- 立即记录板子本地 wall clock 时间
- 立即通过 UDP 回一个 JSON

它不参与视频接收、硬解、latest-frame、RKNN 推理，也不参与目标筛选逻辑。

## 4. 启动与生命周期

clock offset server 的默认配置在：

- `include/video_infer_options.hpp`

当前默认值是：

- `enable_clock_offset_server = true`
- `clock_offset_server_port = 45678`

生命周期由 `VideoInferApp` 管理：

1. `Initialize()` 时尝试启动
2. `Run()` 退出时停止

如果启动失败：

- 只打印 `WARN`
- 不会阻断视频接收、硬解、推理、结果 UDP 回传主流程

## 5. 监听方式

当前实现的监听方式如下：

- 协议：`UDP`
- 地址：`0.0.0.0`
- 端口：`45678`
- socket 类型：`AF_INET + SOCK_DGRAM`

当前代码是：

- `socket(AF_INET, SOCK_DGRAM, 0)`
- `bind(0.0.0.0:45678)`

也就是说：

- 板子会监听所有本机 IPv4 网卡地址
- 主机只要能访问板子的对应 IP，就可以向这个端口发请求

## 6. 时间来源与单位

clock offset server 当前用的时间来源是：

```cpp
std::chrono::system_clock::now()
```

再转换成：

- `epoch nanoseconds`

也就是：

- wall clock
- 单位是 `ns`
- 不是 monotonic 时间

这点很关键，因为主机端后续要把板子时间和主机时间放到同一套绝对时间参考下做 offset 估计。

## 7. 主机发给板子的请求格式

当前标准请求 JSON 是：

```json
{
  "type": "clock_offset_request",
  "request_id": 1,
  "t1_ns": 1711111111111111111
}
```

字段含义如下：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `type` | `string` | 固定为 `"clock_offset_request"` |
| `request_id` | `integer` | 请求序号，主机自增或自行管理 |
| `t1_ns` | `integer` | 主机发送该请求时刻，单位 `ns` |

要求：

- `type` 必须是 `"clock_offset_request"`
- `request_id` 必须是整数，且不能为负数
- `t1_ns` 必须是整数
- `t1_ns` 单位必须是 `ns`
- 不要发字符串时间，不要发浮点时间

## 8. 板子收到请求后做什么

板子端处理顺序如下：

1. `recvfrom()` 收到一个 UDP 包，同时拿到请求源 IP 和源端口
2. 在收包后立即记录 `t2_ns`
3. 解析请求 JSON，取出 `request_id` 和 `t1_ns`
4. 在准备发响应前立即记录 `t3_ns`
5. 组织响应 JSON
6. 用 `sendto()` 把响应发回 `recvfrom()` 收到的源 IP + 源端口

这几个时刻的含义如下：

| 字段 | 谁记录 | 含义 | 单位 |
| --- | --- | --- | --- |
| `t1_ns` | 主机 | 主机发请求时刻 | `ns` |
| `t2_ns` | 板子 | 板子收到请求时刻 | `ns` |
| `t3_ns` | 板子 | 板子发响应时刻 | `ns` |
| `t4_ns` | 主机 | 主机收到响应时刻 | `ns` |

注意：

- `t4_ns` 不在板子响应里
- `t4_ns` 是主机端自己记录的

## 9. 板子返回给主机的响应格式

当前板子端返回的 JSON 是：

```json
{
  "request_id": 1,
  "t1_ns": 1711111111111111111,
  "t2_ns": 1711111111112222222,
  "t3_ns": 1711111111113333333
}
```

字段含义如下：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `request_id` | `integer` | 原样回传主机请求里的 `request_id` |
| `t1_ns` | `integer` | 原样回传主机请求里的 `t1_ns` |
| `t2_ns` | `integer` | 板子收到请求时刻 |
| `t3_ns` | `integer` | 板子发出响应时刻 |

要求：

- 这 4 个字段都必须是整数
- 不要发字符串
- 不要发浮点
- 字段名里的 `_ns` 就表示纳秒，不要塞毫秒

## 10. 响应必须发回哪里

这是 clock offset 能否打通的关键点。

板子端的响应必须发回：

- `recvfrom()` 收到的源 IP
- `recvfrom()` 收到的源端口

不能做的事：

- 不能写死发回主机 `6000`
- 不能写死发回某个固定端口
- 不能把回包地址改成结果回传通道的目标地址

原因：

- 主机端 clock offset client 往往使用临时源端口发请求
- 如果板子不回到这个临时源端口，主机端就会表现为 timeout

当前代码正是这样实现的：

```cpp
recvfrom(sock, ..., &peer_address, &peer_address_size);
sendto(sock, response, ..., &peer_address, peer_address_size);
```

## 11. 当前板子端实现位置

clock offset 的主要实现逻辑在：

- `src/clock_offset_server.cpp`

关键点如下：

1. `NowWallClockNs()`
   使用 `std::chrono::system_clock::now()` 生成板子 wall clock 的 `ns`
2. `TryParseClockOffsetRequest(...)`
   解析请求里的 `type / request_id / t1_ns`
3. `WorkerLoop()`
   完成 `recvfrom -> 记录 t2_ns -> 记录 t3_ns -> sendto`

## 12. 当前还保留的兼容行为

虽然当前标准协议已经是：

- `type`
- `request_id`
- `t1_ns`

但为了兼容之前的测试版本，当前板子端还接受旧格式请求：

```json
{
  "seq": 1,
  "t1": 1711111111111111111
}
```

不过要注意：

- 这只是兼容入口
- 当前响应仍然统一按新格式返回：
  `request_id / t1_ns / t2_ns / t3_ns`

如果主机端已经切到新版本，建议只使用新格式。

## 13. 主机端如何计算 RTT 和 Offset

主机端一轮探测会有四个时间：

| 记号 | 记录位置 | 含义 |
| --- | --- | --- |
| `t1_ns` | 主机 | 发请求时刻 |
| `t2_ns` | 板子 | 收请求时刻 |
| `t3_ns` | 板子 | 发响应时刻 |
| `t4_ns` | 主机 | 收响应时刻 |

在“网络双向时延大致对称”的常见近似下，可用下面公式：

### 13.1 RTT

```text
delay_ns = (t4_ns - t1_ns) - (t3_ns - t2_ns)
```

它表示：

- 主机看到的往返时间
- 扣掉板子端从收包到发包之间的停留时间

### 13.2 Offset

```text
offset_ns = ((t2_ns - t1_ns) + (t3_ns - t4_ns)) / 2
```

这里的 `offset_ns` 可以理解为：

- `board_clock - host_clock`

也就是：

- 如果 `offset_ns > 0`，说明板子时钟比主机快
- 如果 `offset_ns < 0`，说明板子时钟比主机慢

### 13.3 把板子时间换算到主机时间域

如果主机已经估计出：

```text
offset_ns = board_clock - host_clock
```

那么一个板子时间戳 `board_ts_ns` 换算到主机时间域时，可以近似写成：

```text
host_domain_ts_ns = board_ts_ns - offset_ns
```

如果板子时间是毫秒字段，例如结果 JSON 里的：

- `board_wall_infer_done_ms`
- `board_result_send_start_ms`

则先转成纳秒或把 offset 转成毫秒后再做同样的换算。

## 14. 时间校准和结果 JSON 的关系

clock offset server 本身只负责返回：

- `request_id`
- `t1_ns`
- `t2_ns`
- `t3_ns`

真正和业务推理过程相关的时间字段，在结果 UDP JSON 的 `timing` 对象里：

```json
"timing": {
  "board_wall_infer_done_ms": 1711111111111,
  "board_preprocess_ms": 0.82,
  "board_inference_ms": 12.4,
  "board_postprocess_ms": 0.7,
  "board_result_send_start_ms": 1711111111112,
  "board_result_send_end_ms": null
}
```

这些字段的作用不同：

| 字段 | 含义 | 单位 |
| --- | --- | --- |
| `board_wall_infer_done_ms` | 当前帧推理和后处理完成时刻 | `ms` |
| `board_preprocess_ms` | 当前帧预处理耗时 | `ms` |
| `board_inference_ms` | 当前帧推理耗时 | `ms` |
| `board_postprocess_ms` | 当前帧后处理耗时 | `ms` |
| `board_result_send_start_ms` | 当前帧结果包发送前时刻 | `ms` |
| `board_result_send_end_ms` | 当前 outgoing JSON 中暂为 `null` | `ms` |

其中：

- `board_wall_infer_done_ms` 和 `board_result_send_start_ms` 是板子 wall clock 绝对时间
- 这些字段可以借助 clock offset 估计值，转换到主机时间域
- `board_preprocess_ms / board_inference_ms / board_postprocess_ms` 是耗时，不需要做 offset 修正

## 15. 推荐的主机侧使用流程

一套比较直接的使用方式如下：

1. 主机周期性向板子 `45678/udp` 发 `clock_offset_request`
2. 主机收到响应后记录 `t4_ns`
3. 主机根据 `t1_ns / t2_ns / t3_ns / t4_ns` 计算 `delay_ns` 和 `offset_ns`
4. 主机保留一份最近的、质量较好的 offset 估计
5. 主机收到结果 UDP JSON 后，把其中的板子 wall clock 字段映射到主机时间域
6. 主机再和本机采集、显示、控制链路的时间做统一分析

## 16. 最小联调样例

一轮最小联调只要满足下面几点就够了：

1. 板子端启动 `ffmpeg_video_infer`
2. 确认板子端 `45678/udp` 可达
3. 主机发：

```json
{
  "type": "clock_offset_request",
  "request_id": 1,
  "t1_ns": 1711111111111111111
}
```

4. 板子回：

```json
{
  "request_id": 1,
  "t1_ns": 1711111111111111111,
  "t2_ns": 1711111111112222222,
  "t3_ns": 1711111111113333333
}
```

5. 主机自己记录 `t4_ns`
6. 主机计算 `delay_ns` 和 `offset_ns`

如果这条链通了，主机端 profiling 一般就能进一步把结果包里的板子时间字段接起来。

## 17. 常见失败表现与排查方向

### 17.1 timeout

常见原因：

- 板子没启动 `ffmpeg_video_infer`
- 板子 `45678/udp` 不通
- 板子收到了请求，但没有回到请求源端口
- 主机发错 IP 或端口

### 17.2 invalid_response

常见原因：

- 板子回包字段名不对
- 把 `request_id` 写成了 `seq`
- 把 `t1_ns` 写成了 `t1`
- 把 `t2_ns / t3_ns` 写成了毫秒字段
- 某些字段被写成字符串或浮点

### 17.3 时间对不上

常见原因：

- 板子用的不是 wall clock，而是 monotonic 时间
- 主机侧把 `ns` 当成 `ms`
- offset 的正负方向用反了

## 18. 当前实现结论

当前 `ffmpeg_video_infer` 的时间校准链路可以概括为：

1. 板子端默认启动一个 `45678/udp` 的 clock offset server
2. 主机发送 `clock_offset_request`
3. 板子记录 `t2_ns`
4. 板子记录 `t3_ns`
5. 板子把 `request_id / t1_ns / t2_ns / t3_ns` 回到请求源地址
6. 主机结合本地 `t4_ns` 算 offset
7. 主机再用这个 offset 去解释结果 JSON 里的板子 wall clock 时间字段

这就是当前板子端时间校准的完整链路。
