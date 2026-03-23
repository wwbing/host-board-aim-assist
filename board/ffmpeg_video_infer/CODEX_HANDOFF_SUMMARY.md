# ffmpeg_video_infer 可接力开发总结文档

说明：

- 本文档以当前代码真实实现为准，不以 README 口径为准。
- 已交叉阅读 `ffmpeg_video_infer`、`udp_client_recv`、`infer_v6_cpp` 的头文件、源文件、CMake、配置项和日志输出逻辑。
- 如果 README 与代码不一致，本文档会明确指出。

# 1. 项目概述

当前开发板端的统一实时推理入口是 `ffmpeg_video_infer`。它当前负责的职责是：

- 接收主机端推送过来的 `UDP MPEG-TS H.264` 视频流
- 通过 FFmpeg 走 `h264_rkmpp` 硬解
- 只保留 latest frame，不排队处理旧帧
- 将 latest-frame 的 BGR 图像送入 RKNN 推理
- 完成推理后处理，得到检测框
- 通过一个 UDP JSON 包把当前帧的全部检测框发回主机
- 在结果 JSON 里附带当前已接入的 timing / profiling 字段
- 额外启动一个 UDP clock offset server，供主机端做时间校准

三层职责边界当前是清晰的：

- `udp_client_recv`
  负责收流、MPEG-TS 解封装、H.264 硬解、BGR 转换、latest-frame
- `infer_v6_cpp`
  负责单张 `cv::Mat` 的 RKNN 推理、后处理、检测框绘制
- `ffmpeg_video_infer`
  负责把最新帧接到推理，再把结果和 timing 通过 UDP 发出去

# 2. 当前主流程

当前真实主流程如下：

```text
主机推流
    -> 板子 udp://0.0.0.0:5000 上收 UDP MPEG-TS H.264
    -> ffmpeg + mpegts demux
    -> h264_rkmpp 硬解
    -> 硬件帧转软件帧
    -> sws_scale 转 BGR24
    -> LatestFrameBuffer 只保留最新一帧
    -> VideoInferApp 轮询 getLatestFrame()
    -> 如果不是新帧则跳过
    -> 新帧转 cv::Mat
    -> YoloV6RknnInfer::Infer()
    -> 得到 detections + InferenceStats
    -> 采集 frame metadata + timing
    -> DetectionUdpSender 组装 JSON
    -> 一个 UDP 包回传当前帧全部框
    -> 可选显示 / 可选保存图片
```

程序启动后，`main.cpp` 解析参数，构造 `VideoInferOptions`，然后创建 `VideoInferApp`。`VideoInferApp::Initialize()` 当前会做：

- 检查模型路径
- 必要时创建输出目录
- `infer.Load(model)`
- 启动 `clock_offset_server`
- 配置 `result_json_path` 的 JSONL 落盘
- 初始化 `DetectionUdpSender`

`VideoInferApp::Run()` 当前会做：

- 如启用显示，则建 OpenCV 窗口
- 调 `receiver.start()`
- 进入 `Loop()`
- 退出时 `receiver.stop()`，再 `clock_offset_server.Stop()`

`Loop()` 当前行为：

- 不断调用 `receiver.getLatestFrame(frame)`
- 没有帧时显示等待画面或 sleep
- 有帧但不是“新帧”时跳过，避免重复推理同一 latest frame
- 有新帧时执行 `ProcessFrame(frame)`

`clock offset server` 的启动时机在 `VideoInferApp::Initialize()` 阶段，早于 `receiver.start()`。

# 3. 目录结构与模块职责

按当前代码实际结构，核心模块如下。

`ffmpeg_video_infer`

- `CMakeLists.txt`
  组装主项目，`add_subdirectory()` 引入 `udp_client_recv` 和 `infer_v6_cpp`
- `include/video_infer_options.hpp`
  应用层配置项，包含显示、保存、结果 UDP、clock offset server 开关和端口
- `include/video_infer_app.hpp`
  应用入口类接口
- `src/main.cpp`
  参数解析、默认路径、信号处理、启动 app
- `src/video_infer_app.cpp`
  实际主循环，串接接收、推理、发送、显示、保存、低频日志
- `include/detection_udp_sender.hpp`
  定义 `DetectionFrameMetadata`、`DetectionFrameTimingInfo`、`DetectionFramePayload`
- `src/detection_udp_sender.cpp`
  组装结果 JSON、`sendto()` 回传、可选 JSONL 落盘
- `include/clock_offset_server.hpp`
  clock offset server 接口
- `src/clock_offset_server.cpp`
  UDP 时间回显服务实现

`udp_client_recv`

- `include/receiver.h`
  定义 `FrameData`、`ReceiverOptions`、`UdpTsH264Receiver`
- `src/receiver.cpp`
  FFmpeg 打开输入、选流、等待 SPS/PPS、打开 `h264_rkmpp`、解码、转 BGR、更新时间戳
- `include/latest_frame_buffer.h`
  latest-frame 缓冲接口
- `src/latest_frame_buffer.cpp`
  latest-frame 覆盖写入和读取
- `src/main.cpp`
  这个项目的 standalone demo，只做接收和可选显示

`infer_v6_cpp`

- `include/yolov6_rknn.hpp`
  定义 `Detection`、`InferenceStats`、`YoloV6RknnInfer`
- `src/yolov6_rknn.cpp`
  模型加载、tensor 查询、预处理、`rknn_run`、后处理、NMS、绘制
- `src/main.cpp`
  这个项目的 standalone demo，只做图片推理

# 4. 接收与解码链路总结

当前接收链完全来自 `udp_client_recv`，`ffmpeg_video_infer` 只在 `BuildReceiverOptions()` 里覆盖了 `input_url`。

当前默认输入 URL：

- `udp://0.0.0.0:5000?pkt_size=188&fifo_size=32768&buffer_size=8388608&overrun_nonfatal=1`

当前 `udp_client_recv` 的实现要点：

- 输入格式固定是 `mpegts`
- 只接受 `AV_CODEC_ID_H264`
- 会先 `avformat_open_input()` 打开输入
- 然后 `av_read_frame()` 逐包读取
- 只在收到带 SPS/PPS 的关键包后才打开 `h264_rkmpp`
- 解码完成后，如果是硬件像素格式帧，会先 `av_hwframe_transfer_data()`
- 最后统一用 `sws_scale()` 转成 `BGR24`

latest-frame 逻辑：

- `LatestFrameBuffer::update(FrameData frame)` 直接覆盖旧帧
- 没有队列
- 没有 backlog
- `getLatestFrame()` 返回当前最新帧的拷贝

所以当前避免排队处理旧帧的核心不是“应用层清理队列”，而是底层从设计上就只保留最后一帧。

`FrameData` 当前关键字段：

- `width`
- `height`
- `stride`
- `pixel_format`
- `pts`
- `best_effort_timestamp`
- `time_base_num`
- `time_base_den`
- `data`

其中 `pts / best_effort_timestamp / time_base_num / time_base_den` 当前是在 `convertFrameToBgr()` 里从 FFmpeg 帧和 `video_stream_->time_base` 填进 `FrameData`，然后一路传到 `VideoInferApp`，再进入结果 JSON。

# 5. RKNN 推理链总结

`infer_v6_cpp` 当前负责：

- 加载 RKNN 模型
- 查询输入输出 tensor 属性
- 对单张 BGR `cv::Mat` 做 letterbox + BGR->RGB
- 调 `rknn_inputs_set()` / `rknn_run()` / `rknn_outputs_get()`
- 对输出做 decode + NMS + 坐标缩放
- 返回 `std::vector<Detection>`
- 可选画框

当前输入：

- 一张 3 通道 BGR `cv::Mat`

当前输出：

- `std::vector<Detection>`
- 可选 `InferenceStats`

`Detection` 当前字段：

- `x1`
- `y1`
- `x2`
- `y2`
- `score`
- `class_id`

`InferenceStats` 当前字段：

- `preprocess_ms`
- `inference_ms`
- `postprocess_ms`
- `output_count`
- `raw_score_max`

当前后处理是在 `infer_v6_cpp` 内部完成，不是在 `ffmpeg_video_infer` 里做。`ffmpeg_video_infer` 拿到的已经是后处理后的检测框。

# 6. 结果 UDP JSON 协议总结

当前结果回传协议是：

- 每帧一个 UDP 包
- 一个包里包含该帧全部检测框
- 无框也照样发包

当前 JSON 顶层关键字段：

- `seq`
- `frame_pts`
- `frame_best_effort_ts`
- `frame_time_base_num`
- `frame_time_base_den`
- `frame_width`
- `frame_height`
- `box_count`
- `boxes`
- `timing`

`boxes` 中每个框当前字段：

- `class_id`
- `score`
- `x1`
- `y1`
- `x2`
- `y2`
- `cx`
- `cy`

`timing` 中当前字段：

- `board_wall_infer_done_ms`
- `board_preprocess_ms`
- `board_inference_ms`
- `board_postprocess_ms`
- `board_result_send_start_ms`
- `board_result_send_end_ms`

当前实际发送行为：

- `seq` 来自 `DetectionUdpSender::next_seq_`
- `boxes=[]` 时也会发
- `box_count=0` 时也会发
- `board_result_send_end_ms` 当前大多数情况下不是“缺字段”，而是明确发成 `null`

当前协议里“必有但可能为空”的典型字段：

- `timing.board_result_send_end_ms`

当前协议里“总会有值但可能是无效时间”的字段：

- `frame_pts`
- `frame_best_effort_ts`

因为如果 FFmpeg 给的是 `AV_NOPTS_VALUE`，当前代码是原值直传。

# 7. timing / profiling 当前真实状态

## 7.1 FrameData 时间信息

当前 `FrameData` 时间字段来自 `udp_client_recv`：

- `pts` 来自 `source_frame->pts`
- `best_effort_timestamp` 来自 `source_frame->best_effort_timestamp`
- `time_base_num/den` 来自 `video_stream_->time_base`

传递路径是：

- FFmpeg 解码帧
- `convertFrameToBgr()`
- `FrameData`
- `LatestFrameBuffer`
- `VideoInferApp::ProcessFrame()`
- `DetectionFramePayload.metadata`
- 结果 JSON

当前用途：

- 主机端可以结合 `frame_pts` 和 `frame_time_base_*` 还原流内时间尺度
- 也可以用来判断 latest-frame 当前拿到的是哪一个时间戳的解码帧

## 7.2 InferenceStats

当前 `InferenceStats` 由 `infer_v6_cpp` 在 `Infer()` 内部采集：

- `preprocess_ms`
  用 `steady_clock` 统计 letterbox + BGR->RGB + contiguous 处理
- `inference_ms`
  先用 `steady_clock` 统计 `rknn_run + outputs_get`
  然后如果 `RKNN_QUERY_PERF_RUN` 成功，会被 `perf_run.run_duration / 1000.0` 覆盖
- `postprocess_ms`
  用 `steady_clock` 统计 `PostProcess()`

可信度判断：

- `preprocess_ms` 和 `postprocess_ms` 是当前进程内直接计时，可信
- `inference_ms` 当前优先采用 RKNN perf query 返回值，通常比外层 wall time 更贴近 NPU 执行时间
- 但它表示的是 RKNN 运行耗时，不等于整帧从进入 `Infer()` 到返回的总耗时

## 7.3 board_wall_infer_done_ms

当前采集时机：

- `infer.Infer(frame_copy, detections, &stats)` 成功返回之后立刻采集

当前语义：

- 当前帧“推理 + 后处理已经完成”的板子 wall clock 毫秒时间
- 还没开始发结果 UDP 包

为什么主机端要用 offset 修正：

- 这是板子本地 `system_clock` 的毫秒时间
- 它和主机 `system_clock` 不天然对齐
- 要想和主机侧采样、显示、控制链路对齐，必须用 clock offset 估计值修正

## 7.4 result_send_start / result_send_end

当前状态：

- `board_result_send_start_ms`
  已经打点
- `board_result_send_end_ms`
  已经在应用层打点，但当前 outgoing JSON 里仍然是 `null`

当前精度：

- 毫秒精度
- 使用 `std::chrono::system_clock` 转毫秒

为什么大多数情况下会出现 start == end：

- `sendto()` 对本机 UDP 发送通常很快
- 采样粒度只有毫秒
- 所以开始和结束大概率落在同一毫秒

这并不表示发送逻辑一定有问题，更像是时间粒度太粗。这是基于代码行为的推断，不是抓包结论。

补充一点：

- 当前 `board_result_send_end_ms` 的真实值只进入低频日志
- 没有被回填到当前这一帧已经发出去的 JSON 里

## 7.5 当前板子端 timing 哪些是可信的

可以放心看的：

- `preprocess_ms`
- `postprocess_ms`
- `inference_ms` 的量级
- `frame_pts / best_effort_timestamp / time_base`
- `board_wall_infer_done_ms` 的时间顺序语义

只能粗粒度看的：

- `board_result_send_start_ms`
- `board_result_send_end_ms`
- 它们的差值

原因不是逻辑没写，而是当前只有毫秒精度。

# 8. clock offset server 总结

当前板子端 offset server 已实现为独立模块。

默认监听端口：

- `45678/udp`

协议格式：

请求：

```json
{
  "type": "clock_offset_request",
  "request_id": 1,
  "t1_ns": 1711111111111111111
}
```

响应：

```json
{
  "request_id": 1,
  "t1_ns": 1711111111111111111,
  "t2_ns": 1711111111112222222,
  "t3_ns": 1711111111113333333
}
```

当前 `t2/t3` 的采集方式：

- `t2_ns`
  `recvfrom()` 成功后立刻用 `system_clock` 取 epoch ns
- `t3_ns`
  组织响应前立刻再取一次 `system_clock` epoch ns

为什么必须回到请求源地址和源端口：

- 主机端 offset client 很可能使用临时源端口
- 如果板子回包不是发回 `recvfrom()` 拿到的源 IP + 源端口，主机侧会 timeout

当前是否已经正常可用：

- 从代码实现上看，协议字段、单位、回包地址都已具备
- 当前也保留了对旧 `seq/t1` 请求的兼容入口
- 但“主机端整链已实测打通到什么程度”，这里仍建议结合你实际 Windows client 再复测一次，不只依赖代码判断

# 9. 当前已验证正常的内容

基于当前代码和已有运行产物，可以认为以下内容已经跑通或具备直接运行状态：

- `udp_client_recv` 的收流、MPEG-TS 解封装、`h264_rkmpp` 硬解链存在且已作为依赖集成
- latest-frame only 机制已实际接入主项目
- `ffmpeg_video_infer` 默认后台运行链路完整
- RKNN 推理链已接入并能返回 `detections + InferenceStats`
- 结果 UDP JSON 回传已接入
- `--savejson` 已能把实际发出的 JSON 追加落盘
- timing 字段已进入结果 JSON
- clock offset server 已接入主项目生命周期

从现有 `result/result_udp_packets.jsonl` 样本还能看到：

- `frame_pts / best_effort_ts / time_base` 已经在结果包里出现
- `preprocess_ms / inference_ms / postprocess_ms` 已经在结果包里出现
- `board_result_send_end_ms` 当前确实是 `null`

当前比较可信的 timing 指标：

- RKNN 内部三段耗时量级
- 帧级 FFmpeg 时间戳
- 板子侧推理完成 wall clock 时间点

# 10. 当前已知问题 / 不足 / 注意事项

当前明确存在的不足有这些：

- `README.md` 与代码不完全一致
  例如当前代码已有 `--savejson` 和 clock offset server，但 README 口径有滞后，因此后续开发应以代码为准
- `board_result_send_end_ms` 没有进入当前 outgoing JSON
  只是本地打了点并写进日志
- send timing 只有毫秒粒度
  很难区分几十微秒到几百微秒级的发送差异
- `DetectionUdpSender` 现在是同步 `sendto()`
  没有异步队列，也没有单独发送线程
- `VideoInferApp` 当前 still 带一点 demo 装配痕迹
  显示、保存、发送、低频日志都在同一个 `ProcessFrame()` 里
- latest-frame 的“新帧判断”依赖 `pts/best_effort_timestamp`
  当二者都无效时，当前代码会把每次拿到的 latest frame 都视为新帧
- `DetectionUdpSender` 当前没有显式 `bind()` 结果发送 socket
  本地发送源端口由系统分配，不是固定端口
- 主项目当前没有 CLI 去控制 clock offset server 的启停和端口

# 11. 后续开发建议

按优先级建议如下。

必修 bug / 小修复：

- 把 `README` 更新到和代码一致
- 给 clock offset server 增加 CLI 开关和端口参数
- 明确记录 `board_result_send_end_ms` 当前为什么是 `null`

建议增强项：

- 把发送 timing 从毫秒升级到微秒或纳秒
- 如果主机端确实要看 send end，把 JSON 组装和发送结构调整成能回填 end 时间的形式
- 给结果 sender 增加更明确的错误统计和低频日志
- 给主项目加一份“端到端 profiling 调试方法”文档

可选优化项：

- 把显示 / 保存 / UDP 发送继续拆分成更清晰的应用层子模块
- 把 `ProcessFrame()` 中的 overlay/save/display 路径再解耦一点
- 给结果协议加版本号字段，方便未来扩展

暂时不要动的大项：

- 不要动 `udp_client_recv` 已验证的硬解主链
- 不要轻易重构 latest-frame 机制
- 不要先把 `ffmpeg_video_infer` 改成复杂多线程架构
- 不要在没有明确收益前重写 `infer_v6_cpp` 的核心推理逻辑

# 12. 重新接手开发时建议先看哪些文件

推荐阅读顺序：

1. `ffmpeg_video_infer/src/main.cpp`
   先看程序入口、参数和默认值
2. `ffmpeg_video_infer/include/video_infer_options.hpp`
   看应用层配置项
3. `ffmpeg_video_infer/src/video_infer_app.cpp`
   这是当前真实主流程
4. `ffmpeg_video_infer/include/detection_udp_sender.hpp`
   先看结果 payload 的结构定义
5. `ffmpeg_video_infer/src/detection_udp_sender.cpp`
   看结果 JSON 真实协议
6. `ffmpeg_video_infer/src/clock_offset_server.cpp`
   看时间校准协议和回包逻辑
7. `udp_client_recv/include/receiver.h`
   看 `FrameData` 和 `ReceiverOptions`
8. `udp_client_recv/src/receiver.cpp`
   看接收、解码、BGR 转换和时间戳来源
9. `udp_client_recv/src/latest_frame_buffer.cpp`
   看 latest-frame 覆盖逻辑
10. `infer_v6_cpp/include/yolov6_rknn.hpp`
    看 `Detection` 和 `InferenceStats`
11. `infer_v6_cpp/src/yolov6_rknn.cpp`
    看预处理、推理、后处理和统计采集
12. 再回来看 `ffmpeg_video_infer/CMakeLists.txt`
    确认三个模块当前是怎么组装在一起的

# 13. 当前系统一句话总结

当前开发板端项目已经完成了“UDP MPEG-TS H.264 接收 + `h264_rkmpp` 硬解 + latest-frame + RKNN 推理 + 每帧一个 UDP JSON 回传 + 基础 timing + clock offset server”这条主链，核心功能已经具备持续运行能力。

目前最好用、最可信的是：

- latest-frame 主链
- RKNN 三段耗时
- FFmpeg 帧级时间戳
- 推理完成时刻的板子 wall clock

目前仍然只是粗粒度的主要是：

- result send start/end
- 板子侧发送阶段的精细耗时

现在最适合继续做的方向是：

- 提升 timing 精度
- 把 clock offset 和结果 timing 的主机侧链路完全闭环
- 顺手把 README 和 CLI 小缺口补齐
