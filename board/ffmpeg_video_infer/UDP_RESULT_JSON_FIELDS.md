# ffmpeg_video_infer UDP JSON 协议与程序用法说明

这份文档专门说明两件事：

1. `ffmpeg_video_infer` 当前通过 UDP 发出去的 JSON 协议有哪些
2. 当前程序怎么用，参数是什么意思，默认值是什么

当前项目里，和网络 JSON 协议直接相关的有两类 UDP 数据：

- 结果包：板子端把“当前帧全部检测框 + timing 信息”发回主机
- clock offset 包：板子端作为一个极小的 UDP 时间回显服务，给主机做时钟偏移测量

## 1. 先说明：当前不是“写 JSON 文件到磁盘”

当前 `ffmpeg_video_infer` 的主协议仍然是：

- 每处理完一帧，构造一个 JSON 字符串
- 通过 `DetectionUdpSender::SendFrameDetections()` 用 UDP 发给主机

另外，如果开启：

- `--savejson`

则程序会把“实际发出去的那份 JSON 字符串”额外追加保存到本地：

- `output_dir/result_udp_packets.jsonl`

所以这里说的 JSON，主要指的是：

- 每帧一个 UDP JSON 包

不是默认会落磁盘的 JSON 文件。

## 2. 当前有哪两种 UDP JSON 协议

### 2.1 结果 JSON 包

用途：

- 板子端把每一帧的检测结果发给主机

发送模块：

- `include/detection_udp_sender.hpp`
- `src/detection_udp_sender.cpp`

发送方向：

- 板子 -> 主机

默认目标：

- `192.168.7.1:6000`

### 2.2 clock offset JSON 包

用途：

- 主机发一个时间探测请求到板子
- 板子立即回显 `request_id / t1_ns / t2_ns / t3_ns`
- 主机用它估计板子与主机之间的时钟偏移

服务模块：

- `include/clock_offset_server.hpp`
- `src/clock_offset_server.cpp`

传输方向：

- 主机 -> 板子 -> 主机

默认监听端口：

- `45678`

## 3. 当前结果 JSON 包结构

当前发送的结果 JSON，逻辑上等价于：

```json
{
  "seq": 123,
  "frame_pts": 456000,
  "frame_best_effort_ts": 456000,
  "frame_time_base_num": 1,
  "frame_time_base_den": 90000,
  "frame_width": 640,
  "frame_height": 640,
  "box_count": 2,
  "boxes": [
    {
      "class_id": 0,
      "score": 0.91,
      "x1": 100.0,
      "y1": 120.0,
      "x2": 180.0,
      "y2": 220.0,
      "cx": 140.0,
      "cy": 170.0
    },
    {
      "class_id": 0,
      "score": 0.88,
      "x1": 240.0,
      "y1": 130.0,
      "x2": 300.0,
      "y2": 210.0,
      "cx": 270.0,
      "cy": 170.0
    }
  ],
  "timing": {
    "board_wall_infer_done_ms": 1711111111111,
    "board_preprocess_ms": 0.82,
    "board_inference_ms": 12.4,
    "board_postprocess_ms": 0.7,
    "board_result_send_start_ms": 1711111111112,
    "board_result_send_end_ms": null
  }
}
```

说明：

- 顶层是一帧的数据
- `boxes` 里放当前帧的全部检测框
- `timing` 里放当前帧 profiling 信息
- 当前仍然严格保持“每帧一个 UDP 包，不做一个框一个包”

## 4. 结果 JSON 是在哪里拼出来的

当前结果 JSON 的序列化代码在：

- `src/detection_udp_sender.cpp`
- 函数：`BuildPayload(...)`

当前实现不再是手工字符串拼接，而是：

- `nlohmann::ordered_json`

也就是说，协议字段仍然是这些字段，但 JSON 组装方式已经改成了 `nlohmann_json`。

## 5. 结果 JSON 顶层字段说明

| 字段 | 类型 | 当前来源 | 说明 |
| --- | --- | --- | --- |
| `seq` | `uint64` | `DetectionUdpSender::next_seq_` | 当前结果 UDP 发送序号 |
| `frame_pts` | `int64` | `FrameData.pts` | 当前 latest frame 的 FFmpeg `pts` |
| `frame_best_effort_ts` | `int64` | `FrameData.best_effort_timestamp` | 当前 latest frame 的 FFmpeg `best_effort_timestamp` |
| `frame_time_base_num` | `int` | `FrameData.time_base_num` | 当前视频流 time base 分子 |
| `frame_time_base_den` | `int` | `FrameData.time_base_den` | 当前视频流 time base 分母 |
| `frame_width` | `int` | `FrameData.width` | 当前发送坐标系图像宽度 |
| `frame_height` | `int` | `FrameData.height` | 当前发送坐标系图像高度 |
| `box_count` | `size_t` | `detections.size()` | 当前帧检测框数量 |
| `boxes` | `array` | `std::vector<infer_v6_cpp::Detection>` | 当前帧全部检测框 |
| `timing` | `object` | `DetectionFrameTimingInfo` | 当前帧 timing/profiling 信息 |

## 6. `boxes` 数组里的字段说明

每个检测框对象当前字段如下：

| 字段 | 类型 | 当前来源 | 说明 |
| --- | --- | --- | --- |
| `class_id` | `int` | `infer_v6_cpp::Detection.class_id` | 类别 ID |
| `score` | `float` | `infer_v6_cpp::Detection.score` | 置信度 |
| `x1` | `float` | `infer_v6_cpp::Detection.x1` | 左上角 x |
| `y1` | `float` | `infer_v6_cpp::Detection.y1` | 左上角 y |
| `x2` | `float` | `infer_v6_cpp::Detection.x2` | 右下角 x |
| `y2` | `float` | `infer_v6_cpp::Detection.y2` | 右下角 y |
| `cx` | `float` | `(x1 + x2) * 0.5f` | 框中心点 x |
| `cy` | `float` | `(y1 + y2) * 0.5f` | 框中心点 y |

### 6.1 `cx / cy` 是哪里算出来的

`cx` 和 `cy` 不是来自 `infer_v6_cpp` 的原始结构体字段，而是在：

- `src/detection_udp_sender.cpp`
- `BuildPayload(...)`

里现场计算：

```cpp
const float center_x = (detection.x1 + detection.x2) * 0.5f;
const float center_y = (detection.y1 + detection.y2) * 0.5f;
```

## 7. `timing` 对象里的字段说明

| 字段 | 类型 | 当前来源 | 说明 |
| --- | --- | --- | --- |
| `board_wall_infer_done_ms` | `int64` | `NowWallClockMs()` | `Infer()` 成功返回后的板子 wall clock 毫秒时间 |
| `board_preprocess_ms` | `double` | `InferenceStats.preprocess_ms` | 预处理耗时 |
| `board_inference_ms` | `double` | `InferenceStats.inference_ms` | 推理耗时 |
| `board_postprocess_ms` | `double` | `InferenceStats.postprocess_ms` | 后处理耗时 |
| `board_result_send_start_ms` | `int64` | `NowWallClockMs()` | 调 `SendFrameDetections()` 之前采集的板子 wall clock 毫秒时间 |
| `board_result_send_end_ms` | `int64/null` | 当前 outgoing JSON 中为 `null` | 因为它只有在 `sendto()` 返回后才知道，当前单包协议下先按 optional 设计 |

## 8. 每个结果字段在代码里是怎么装进去的

### 8.1 `FrameData` 相关字段

这些字段是在：

- `src/video_infer_app.cpp`
- `VideoInferApp::Impl::ProcessFrame(const FrameData& frame)`

里从 `frame` 直接取出来，填进 `DetectionFramePayload.metadata`：

```cpp
result_payload.metadata.frame_pts = frame.pts;
result_payload.metadata.frame_best_effort_ts = frame.best_effort_timestamp;
result_payload.metadata.frame_time_base_num = frame.time_base_num;
result_payload.metadata.frame_time_base_den = frame.time_base_den;
result_payload.metadata.frame_width = frame.width;
result_payload.metadata.frame_height = frame.height;
```

所以：

- `frame_pts`
- `frame_best_effort_ts`
- `frame_time_base_num`
- `frame_time_base_den`
- `frame_width`
- `frame_height`

都来自当前 latest frame 的 `FrameData`。

### 8.2 `InferenceStats` 相关字段

这些字段同样是在：

- `src/video_infer_app.cpp`
- `VideoInferApp::Impl::ProcessFrame(...)`

里从 `stats` 直接取出来，填进 `DetectionFramePayload.timing`：

```cpp
result_payload.timing.board_preprocess_ms = stats.preprocess_ms;
result_payload.timing.board_inference_ms = stats.inference_ms;
result_payload.timing.board_postprocess_ms = stats.postprocess_ms;
```

所以：

- `board_preprocess_ms`
- `board_inference_ms`
- `board_postprocess_ms`

都直接来自 `infer_v6_cpp` 已有的 `InferenceStats`。

### 8.3 wall clock 字段

当前板子端 wall clock 时间获取函数在：

- `src/video_infer_app.cpp`
- `NowWallClockMs()`

实现方式是：

```cpp
std::chrono::system_clock::now()
```

再转成毫秒 epoch。

原因：

- 这是 wall clock
- 适合后续与主机端做 clock offset 修正
- 不是 purely monotonic 时间

#### `board_wall_infer_done_ms`

采集时机：

- `infer.Infer(...)` 成功返回之后
- 当前帧推理和后处理都已经完成
- 还没开始发送结果

#### `board_result_send_start_ms`

采集时机：

- 调 `result_udp_sender.SendFrameDetections(...)` 之前

#### `board_result_send_end_ms`

当前真实采集时机：

- `SendFrameDetections(...)` 返回之后

但要特别注意：

- 当前 outgoing JSON 是在 `sendto()` 之前构造好的
- 因此 `board_result_send_end_ms` 在当前 outgoing JSON 里还不能写成真实的 send 返回时间
- 目前先按 optional 设计，在 JSON 中输出 `null`
- 真实的 send end 时间目前会进入应用层低频日志

## 9. `seq` 是从哪里来的

`seq` 不是 `FrameData` 或 `InferenceStats` 里带出来的，它来自发送器自己的内部计数器：

- 成员：`DetectionUdpSender::next_seq_`

发送器序列化 JSON 时会把当前 `next_seq_` 写入 `seq`，然后自增。

另外应用层为了日志对齐，会先调用：

```cpp
result_seq = result_udp_sender.NextSequence();
```

这样日志中的 `result_seq` 和当前包里的 `seq` 是一致的。

## 10. 如果没有检测框，会发什么

如果 `detections.size() == 0`：

- `box_count = 0`
- `boxes = []`
- `timing` 仍然会附带
- `frame_*` 字段仍然会附带

也就是说：

- 空框包照常发
- 不会因为没有检测框就不发这帧

## 11. 如果 `pts` 或 `best_effort_timestamp` 无效怎么办

当前处理方式是：

- 直接把 `FrameData` 里的原值写出去
- 不额外改写成别的 magic number

因此如果底层给的是 `AV_NOPTS_VALUE`，JSON 中就会出现对应的原始值。

## 12. 当前数值输出格式说明

当前 JSON 由：

- `nlohmann::ordered_json`

生成并 `dump()` 输出。

这意味着：

- 不再是手工 `std::fixed << std::setprecision(3)` 格式化
- 浮点数会按 `nlohmann_json` 的默认数字序列化规则输出
- 所以你在抓包或 `--savejson` 文件里看到的浮点数位数，可能与示例里的位数不同

但字段含义不变。

## 13. 结果 JSON 字段来源总表

| JSON 字段 | 直接来源 |
| --- | --- |
| `seq` | `DetectionUdpSender::next_seq_` |
| `frame_pts` | `FrameData.pts` |
| `frame_best_effort_ts` | `FrameData.best_effort_timestamp` |
| `frame_time_base_num` | `FrameData.time_base_num` |
| `frame_time_base_den` | `FrameData.time_base_den` |
| `frame_width` | `FrameData.width` |
| `frame_height` | `FrameData.height` |
| `box_count` | `detections.size()` |
| `boxes[].class_id` | `Detection.class_id` |
| `boxes[].score` | `Detection.score` |
| `boxes[].x1` | `Detection.x1` |
| `boxes[].y1` | `Detection.y1` |
| `boxes[].x2` | `Detection.x2` |
| `boxes[].y2` | `Detection.y2` |
| `boxes[].cx` | `(x1 + x2) / 2` |
| `boxes[].cy` | `(y1 + y2) / 2` |
| `timing.board_wall_infer_done_ms` | `NowWallClockMs()` after `Infer()` |
| `timing.board_preprocess_ms` | `InferenceStats.preprocess_ms` |
| `timing.board_inference_ms` | `InferenceStats.inference_ms` |
| `timing.board_postprocess_ms` | `InferenceStats.postprocess_ms` |
| `timing.board_result_send_start_ms` | `NowWallClockMs()` before `SendFrameDetections()` |
| `timing.board_result_send_end_ms` | 当前 outgoing JSON 中为 `null`，真实值在 send 返回后才知道 |

## 14. clock offset server 协议

## 14.1 作用

clock offset server 用来做：

- 主机和板子之间的时钟偏移估计

主机发请求，板子回响应，主机得到：

- `t1_ns`：主机发送时刻
- `t2_ns`：板子收到请求时刻
- `t3_ns`：板子发出响应时刻

这样主机端可以继续自己记录：

- `t4`：主机收到响应时刻

从而估计：

- RTT
- offset

## 14.2 默认监听端口

当前默认监听：

- `UDP 45678`

该默认值来自：

- `VideoInferOptions.clock_offset_server_port = 45678`

## 14.3 生命周期

当前实现里，clock offset server 由 `VideoInferApp` 管理生命周期：

- `Initialize()` 时尝试启动
- `Run()` 退出时停止

如果启动失败：

- 只打印 `WARN`
- 不影响当前视频接收、硬解、推理、结果发送主流程

回包目标地址也有一个关键约束：

- 响应不是写死发往某个固定端口
- 当前实现是把响应发回 `recvfrom()` 收到的源 IP + 源端口
- 这样才能兼容主机端用临时源端口发请求的 clock offset client

## 14.4 请求 JSON 格式

主机发送到板子的请求，建议格式如下：

```json
{
  "type": "clock_offset_request",
  "request_id": 1,
  "t1_ns": 1711111111111111111
}
```

字段说明：

| 字段 | 说明 |
| --- | --- |
| `type` | 当前应为 `"clock_offset_request"` |
| `request_id` | 请求序号，整数 |
| `t1_ns` | 主机发送请求时刻，整数，单位 ns，原样回传 |

当前板子端对请求的要求很少：

- 只要是一个合法 JSON object
- `type` 为 `clock_offset_request`
- 有 `request_id`
- 有 `t1_ns`

就可以工作。

## 14.5 响应 JSON 格式

板子回给主机的响应，当前格式是：

```json
{
  "request_id": 1,
  "t1_ns": 1711111111111111111,
  "t2_ns": 1711111112111111111,
  "t3_ns": 1711111112111112222
}
```

字段说明：

| 字段 | 说明 |
| --- | --- |
| `request_id` | 原请求中的 `request_id`，原样回传 |
| `t1_ns` | 原请求中的 `t1_ns`，原样回传 |
| `t2_ns` | 板子收到请求时刻，整数，单位 ns |
| `t3_ns` | 板子发出响应时刻，整数，单位 ns |

## 14.6 `t2_ns / t3_ns` 的时间单位

当前实现里：

- `t2_ns`
- `t3_ns`

都是：

- 板子端 `system_clock` 的 epoch nanoseconds

即：

- wall clock
- 单位纳秒

这和结果 JSON 里的 `*_ms` 不同，clock offset server 用的是纳秒。

## 14.7 clock offset server 的代码位置

实现位置：

- `src/clock_offset_server.cpp`

主要逻辑：

1. `recvfrom()` 收到 JSON 请求
2. 记录 `t2_ns = NowWallClockNs()`
3. 解析 `type / request_id / t1_ns`
4. 再记录 `t3_ns = NowWallClockNs()`
5. 用 `sendto(..., &from_addr, from_len)` 回到请求源地址
6. 回发 `{"request_id","t1_ns","t2_ns","t3_ns"}`

## 15. 当前程序怎么用

## 15.1 最常用启动方式

默认运行：

```bash
cd /home/orangepi/code/ffmpeg_video_infer
./build/ffmpeg_video_infer
```

默认行为：

- 不显示窗口
- 不保存结果图片
- 不保存 JSON 到文件
- 默认启用结果 UDP 回传
- 默认启用 clock offset server

## 15.2 当前命令行总览

当前 `main.cpp` 实际支持的命令行形式是：

```bash
./build/ffmpeg_video_infer \
  [--display] \
  [--save] \
  [--savejson] \
  [--disable_result_udp] \
  [--result_udp_ip IP] \
  [--result_udp_port PORT] \
  [--model MODEL] \
  [--output_dir DIR] \
  [udp_url]
```

说明：

- `udp_url` 是最后一个可选位置参数
- 如果不传 `udp_url`，就使用程序内置默认输入地址
- `--output_dir` 会同时影响结果图片保存目录和 `--savejson` 的默认输出文件路径
- 当前还没有命令行参数控制 clock offset server 的开关和端口，它们走代码默认值

## 15.3 常见运行示例

只做后台接流 + 推理 + 结果回传：

```bash
./build/ffmpeg_video_infer
```

打开显示：

```bash
./build/ffmpeg_video_infer --display
```

保存可视化结果图：

```bash
./build/ffmpeg_video_infer --save
```

把发送出去的结果 JSON 额外保存到本地文件：

```bash
./build/ffmpeg_video_infer --savejson
```

改结果回传目标主机：

```bash
./build/ffmpeg_video_infer \
  --result_udp_ip 192.168.7.1 \
  --result_udp_port 6000
```

禁用结果 UDP 回传：

```bash
./build/ffmpeg_video_infer --disable_result_udp
```

指定模型和输出目录：

```bash
./build/ffmpeg_video_infer \
  --model /home/orangepi/code/rknn_infer/model/v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn \
  --output_dir /home/orangepi/code/ffmpeg_video_infer/result
```

指定输入流地址：

```bash
./build/ffmpeg_video_infer \
  "udp://0.0.0.0:5000?pkt_size=188&fifo_size=32768&buffer_size=8388608&overrun_nonfatal=1"
```

## 15.4 当前参数说明

| 参数 | 作用 |
| --- | --- |
| `-h`, `--help` | 打印帮助 |
| `--display` | 打开 OpenCV 显示窗口 |
| `--no-display` | 关闭显示窗口 |
| `--save` | 保存带检测框的结果图片 |
| `--savejson` | 把实际发送出去的结果 JSON 追加保存到本地 `jsonl` 文件 |
| `--disable_result_udp` | 禁用结果 UDP 回传 |
| `--result_udp_ip IP` | 设置结果 UDP 目标 IP |
| `--result_udp_port PORT` | 设置结果 UDP 目标端口 |
| `--model MODEL` | 指定 RKNN 模型路径 |
| `--output_dir DIR` | 指定结果图片和 JSON 文件输出目录 |
| `udp_url` | 覆盖默认输入流 URL |

## 15.5 当前默认值

下面这些默认值，以当前程序实际运行时构造出来的值为准：

| 项目 | 默认值 |
| --- | --- |
| `model_path` | `/home/orangepi/code/rknn_infer/model/v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn` |
| `output_dir` | `/home/orangepi/code/ffmpeg_video_infer/result` |
| `result_json_path` | `/home/orangepi/code/ffmpeg_video_infer/result/result_udp_packets.jsonl` |
| `enable_display` | `false` |
| `save_result` | `false` |
| `save_result_json` | `false` |
| `enable_result_udp` | `true` |
| `result_udp_ip` | `192.168.7.1` |
| `result_udp_port` | `6000` |
| `enable_clock_offset_server` | `true` |
| `clock_offset_server_port` | `45678` |
| `input_url` | `udp://0.0.0.0:5000?pkt_size=188&fifo_size=32768&buffer_size=8388608&overrun_nonfatal=1` |
| `idle_sleep_ms` | `5` |
| `duplicate_sleep_ms` | `1` |
| `display_width` | `960` |
| `display_height` | `960` |

补充说明：

- 如果传了 `--output_dir /some/dir`，则 `result_json_path` 也会自动变成 `/some/dir/result_udp_packets.jsonl`
- `--result_udp_ip` 或 `--result_udp_port` 会同时把 `enable_result_udp` 设为 `true`
- 当前程序没有 `--disable_clock_offset_server` 或 `--clock_offset_port` 这类参数

## 15.6 当前没有命令行参数控制的项

虽然当前默认启用了 clock offset server，但目前还没有单独的命令行参数去：

- 关闭 clock offset server
- 修改 clock offset server 端口

现在它们是通过代码默认值控制的：

- `enable_clock_offset_server = true`
- `clock_offset_server_port = 45678`

如果后续需要，我可以再给你补两个很小的 CLI 参数，例如：

- `--disable_clock_offset_server`
- `--clock_offset_port`

## 16. 总结

当前 `ffmpeg_video_infer` 对外相关的协议和行为可以概括为：

1. 结果 UDP JSON：
   - 每帧一个包
   - 包含当前帧全部检测框
   - 包含 `FrameData` 时间戳信息
   - 包含 profiling/timing 字段
2. clock offset server：
   - 默认监听 `45678/udp`
   - 回显 `request_id / t1_ns / t2_ns / t3_ns`
   - 失败不影响主流程
3. 程序默认行为：
   - 后台接流
   - latest-frame
   - RKNN 推理
   - 结果回传
   - clock offset server 开启

如果后续你希望，我可以继续再补两份文档中的任意一项：

- “主机端如何根据 `t1/t2/t3/t4` 计算 offset 和 RTT”
- “结果 JSON 与 clock offset JSON 的主机端接收示例”
