# ffmpeg_video_infer

`ffmpeg_video_infer` 是当前板子端的实时视频推理主项目。  
它不重复实现 UDP 视频接收，不重复实现 RKNN 推理，而是把已经跑通的两个模块在应用层串起来：

- 视频接收/FFmpeg 解封装/`h264_rkmpp` 硬解/`latest-frame`：复用 `../udp_client_recv` 导出的 `udp_receiver_core`
- RKNN 推理/后处理/检测框绘制：复用 `../rknn_infer/infer_v6_cpp` 导出的 `infer_v6_cpp_lib`
- 板子端检测结果回传：在本项目内新增 `DetectionUdpSender`，每帧通过一个 UDP 包发送所有检测框

当前项目的默认运行形态是：

1. 后台持续接收 Windows 主机推过来的 UDP MPEG-TS H.264 视频流
2. 使用 FFmpeg + `h264_rkmpp` 在 RK3588 上做硬解
3. 只保留最新一帧 BGR 图像
4. 把最新帧送入 RKNN 模型推理
5. 把当前帧的全部检测框通过一个 UDP JSON 包发回主机

显示窗口和结果图片保存仍然保留为兼容功能，但不是默认主路径。

## 1. 项目定位

从职责上看，三个项目的边界如下：

- `udp_client_recv`
  - 负责网络接收、MPEG-TS 解封装、H.264 硬解、BGR 转换、latest-frame 缓冲
- `infer_v6_cpp`
  - 负责对单张 `cv::Mat` 做 YOLOv6 RKNN 推理、后处理和可视化
- `ffmpeg_video_infer`
  - 负责把“最新帧输入”和“单帧推理输出”连接起来
  - 负责应用级循环、日志、参数、可选显示/保存
  - 负责把每帧检测结果通过 UDP 发送给主机

所以，`ffmpeg_video_infer` 本身不是一个“重写版接收器”，也不是一个“重写版 RKNN 推理库”，而是当前系统的整合入口。

## 2. 总体数据流

```text
Windows 主机推流
    |
    v
UDP MPEG-TS H.264
    |
    v
ffmpeg_video_infer
    |
    +--> UdpTsH264Receiver (来自 udp_client_recv / udp_receiver_core)
    |       |
    |       +--> avformat_open_input(mpegts + udp://...)
    |       +--> av_read_frame()
    |       +--> avcodec_find_decoder_by_name("h264_rkmpp")
    |       +--> avcodec_send_packet() / avcodec_receive_frame()
    |       +--> av_hwframe_transfer_data() [硬件帧转软件帧]
    |       +--> sws_scale() -> BGR24
    |       +--> LatestFrameBuffer::update()
    |
    +--> VideoInferApp::Loop()
            |
            +--> getLatestFrame()
            +--> FrameData -> cv::Mat
            +--> YoloV6RknnInfer::Infer() (来自 infer_v6_cpp_lib)
            +--> 得到 detections + InferenceStats
            +--> DetectionUdpSender::SendFrameDetections()
            +--> 可选 DrawDetections / Overlay / imshow / imwrite
```

## 3. 当前目录结构

### 3.1 `ffmpeg_video_infer` 自身文件

```text
ffmpeg_video_infer
├── CMakeLists.txt
├── README.md
├── include
│   ├── detection_udp_sender.hpp
│   ├── video_infer_app.hpp
│   └── video_infer_options.hpp
└── src
    ├── detection_udp_sender.cpp
    ├── main.cpp
    └── video_infer_app.cpp
```

### 3.2 运行时依赖的兄弟项目

```text
../udp_client_recv
├── include/receiver.h
└── src
    ├── latest_frame_buffer.cpp
    └── receiver.cpp

../rknn_infer/infer_v6_cpp
├── include/yolov6_rknn.hpp
└── src/yolov6_rknn.cpp
```

## 4. 每个文件的职责

### 4.1 本项目文件

| 文件 | 作用 |
| --- | --- |
| `CMakeLists.txt` | 定义 `ffmpeg_video_infer_lib` 和 `ffmpeg_video_infer`，并把 `udp_client_recv` 与 `infer_v6_cpp` 当作子目录引入 |
| `include/video_infer_options.hpp` | 定义应用层运行参数，包括输入流地址、模型路径、显示/保存开关、结果 UDP 发送配置 |
| `include/video_infer_app.hpp` | 定义 `VideoInferApp` 对外接口，隐藏内部实现细节 |
| `include/detection_udp_sender.hpp` | 定义轻量级 UDP 结果发送类 |
| `src/main.cpp` | 解析命令行参数，设置默认路径，注册信号，启动 `VideoInferApp` |
| `src/video_infer_app.cpp` | 主业务逻辑：初始化接收器和推理器、拉取 latest frame、做推理、发送结果、可选显示/保存 |
| `src/detection_udp_sender.cpp` | 使用 POSIX UDP socket 把当前帧全部检测框序列化为一个 JSON 包并发送 |

### 4.2 被复用的外部模块

| 模块 | 文件 | 作用 |
| --- | --- | --- |
| `udp_receiver_core` | `../udp_client_recv/src/receiver.cpp` | FFmpeg 输入、MPEG-TS 解封装、`h264_rkmpp` 硬解、像素格式转换、最新帧更新 |
| `udp_receiver_core` | `../udp_client_recv/src/latest_frame_buffer.cpp` | latest-frame only 缓冲，只保留最后一帧 |
| `infer_v6_cpp_lib` | `../rknn_infer/infer_v6_cpp/src/yolov6_rknn.cpp` | RKNN 模型加载、预处理、NPU 推理、后处理、NMS、可视化 |

## 5. CMake 与项目依赖关系

`ffmpeg_video_infer/CMakeLists.txt` 当前做了这些事情：

1. 关闭两个兄弟项目的独立 demo 程序构建
   - `UDP_CLIENT_RECV_BUILD_APP OFF`
   - `INFER_V6_CPP_BUILD_APP OFF`
2. 通过 `add_subdirectory()` 直接把两个兄弟项目编译进来
3. 构建本项目库 `ffmpeg_video_infer_lib`
4. 构建入口可执行程序 `ffmpeg_video_infer`

### 5.1 target 依赖关系

```text
ffmpeg_video_infer
    |
    v
ffmpeg_video_infer_lib
    |
    +--> udp_receiver_core
    |       +--> FFmpeg: libavformat / libavcodec / libavutil / libswscale
    |       +--> OpenCV: core / imgproc / highgui
    |
    +--> infer_v6_cpp_lib
    |       +--> OpenCV: core / imgproc / highgui / imgcodecs
    |       +--> RKNN runtime: librknnrt.so
    |       +--> dl
    |
    +--> OpenCV: core / imgproc / highgui / imgcodecs
```

### 5.2 重要说明

- `ffmpeg_video_infer` 不是通过 shell 启动 `udp_client_recv/build/udp_latest_frame_receiver`
- 它是在同一个进程里直接 new 一个 `UdpTsH264Receiver`
- 同样，它也不是通过子进程调用 `infer_v6_cpp_demo`
- 它是直接链接 `infer_v6_cpp_lib`，然后在进程内调用 `YoloV6RknnInfer`

因此，运行 `ffmpeg_video_infer` 时，不要再单独启动 `udp_client_recv` 的 demo 程序，否则会竞争同一个 UDP 监听端口。

## 6. 运行前提

当前工程默认假设：

- 板子为 RK3588
- FFmpeg 已支持 `h264_rkmpp`
- RKNN runtime 库路径有效
- 主机把视频推到板子 `udp://BOARD_IP:5000`
- 当前默认模型为：
  - `../rknn_infer/model/v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn`

## 7. 构建方法

```bash
cd /home/orangepi/code/host-board-aim-assist/board/ffmpeg_video_infer
cmake -S . -B build
cmake --build build -j4
```

构建输出：

- 可执行文件：`build/ffmpeg_video_infer`
- 静态库：`build/libffmpeg_video_infer_lib.a`

## 8. 运行方法

### 8.1 默认运行

```bash
cd /home/orangepi/code/host-board-aim-assist/board/ffmpeg_video_infer/build
./ffmpeg_video_infer
```

默认行为：

- 不显示窗口
- 不保存结果图片
- 默认启用检测结果 UDP 回传
- 默认目标主机：
  - `192.168.7.1:6000`

### 8.2 常见运行方式

打开显示：

```bash
./ffmpeg_video_infer --display
```

打开显示并保存结果图：

```bash
./ffmpeg_video_infer --display --save
```

自定义结果 UDP 目标：

```bash
./ffmpeg_video_infer \
  --result_udp_ip 192.168.7.1 \
  --result_udp_port 6000
```

指定 120Hz 来流：

```bash
./ffmpeg_video_infer --videofrequency=120
```

禁用结果 UDP 回传：

```bash
./ffmpeg_video_infer --disable_result_udp
```

指定模型和输出目录：

```bash
./ffmpeg_video_infer \
  --model /home/orangepi/code/host-board-aim-assist/board/rknn_infer/model/v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn \
  --output_dir /home/orangepi/code/host-board-aim-assist/board/ffmpeg_video_infer/result
```

指定输入流 URL：

```bash
./ffmpeg_video_infer \
  "udp://0.0.0.0:5000?pkt_size=188&fifo_size=32768&buffer_size=8388608&overrun_nonfatal=1"
```

### 8.3 参数说明

| 参数 | 作用 |
| --- | --- |
| `--display` | 打开 OpenCV 显示窗口 |
| `--no-display` | 关闭显示窗口 |
| `--save` | 将推理可视化结果保存到输出目录 |
| `--disable_result_udp` | 禁用检测结果 UDP 回传 |
| `--result_udp_ip IP` | 设置结果 UDP 目标 IP |
| `--result_udp_port PORT` | 设置结果 UDP 目标端口 |
| `--videofrequency HZ` | 设置目标视频频率，默认 `60`，例如 `--videofrequency=120` |
| `--model MODEL` | 指定 RKNN 模型路径 |
| `--output_dir DIR` | 指定结果图片保存目录 |
| `udp_url` | 覆盖默认输入流地址 |

## 9. 接收与 FFmpeg 解码链路

这一部分不是 `ffmpeg_video_infer` 自己重写的，而是由 `udp_receiver_core` 提供。  
`VideoInferApp` 只是创建 `UdpTsH264Receiver receiver;` 并调用：

- `receiver.start()`
- `receiver.getLatestFrame(frame)`
- `receiver.currentDecoderName()`

### 9.1 `ffmpeg_video_infer` 实际复用了什么

当前复用的是 `udp_client_recv` 里的这条已验证链路：

1. `avformat_open_input()` 打开 `mpegts` 输入
2. `av_read_frame()` 逐包读取 UDP MPEG-TS
3. 检测视频流，只接受 `AV_CODEC_ID_H264`
4. 等到包含 SPS/PPS 的关键帧后，再打开硬件解码器
5. `avcodec_find_decoder_by_name("h264_rkmpp")`
6. `avcodec_send_packet()` / `avcodec_receive_frame()` 做硬解
7. 如果得到的是硬件像素格式帧，调用 `av_hwframe_transfer_data()` 转成软件帧
8. 使用 `sws_getCachedContext()` + `sws_scale()` 转成 `BGR24`
9. 把转换好的 `FrameData` 写入 `LatestFrameBuffer`

### 9.2 `ffmpeg_video_infer` 没有做什么

- 没有自己实现 UDP socket 收视频
- 没有自己实现 TS 解封装
- 没有自己实现 H.264 解码
- 没有自己实现 `latest-frame` 缓冲
- 没有自己改 `h264_rkmpp` 的打开逻辑

### 9.3 `ReceiverOptions` 默认值

`ffmpeg_video_infer` 目前只覆盖了 `input_url`，其余接收配置保持 `udp_client_recv` 默认值：

| 字段 | 默认值 |
| --- | --- |
| `input_format` | `mpegts` |
| `probesize` | `2000000` |
| `analyzeduration` | `2000000` |
| `max_delay` | `0` |
| `reconnect_delay_ms` | `1000` |
| `io_timeout_ms` | `3000` |
| `prefer_hardware_decoder` | `true` |

这意味着当前工程默认就是走硬件解码路径，不会在应用层切到软件解码。

### 9.4 latest-frame 机制是什么

`LatestFrameBuffer` 的行为非常简单：

- `update(FrameData frame)`：写入最新帧，覆盖旧帧
- `getLatestFrame(FrameData& out)`：返回当前最新帧的拷贝
- 不做队列，不积压旧帧

因此整套链路天然是“只处理最新帧”，而不是“逐帧排队处理”。

### 9.5 `FrameData` 里有什么

从接收器取出来的核心数据结构是 `FrameData`：

| 字段 | 含义 |
| --- | --- |
| `width` | 图像宽度 |
| `height` | 图像高度 |
| `stride` | 每行字节跨度 |
| `pixel_format` | 当前固定为 `bgr24` |
| `pts` | FFmpeg 帧时间戳 |
| `best_effort_timestamp` | FFmpeg 最佳努力时间戳 |
| `time_base_num` | 视频流 time base 分子 |
| `time_base_den` | 视频流 time base 分母 |
| `data` | 实际 BGR 像素数据 |

应用层把它包装成：

```cpp
cv::Mat bgr(frame.height, frame.width, CV_8UC3, frame.data.data(), frame.stride);
```

然后再 clone 一份送给推理模块。

## 10. 应用层主循环是怎么串起来的

主循环位于 `VideoInferApp::Impl::Loop()`，逻辑是：

1. 从 `receiver.getLatestFrame(frame)` 取最新帧
2. 如果还没拿到帧：
   - 可选显示等待画面
   - `sleep(idle_sleep_ms)`
3. 如果拿到的是重复帧：
   - 只轮询显示退出键
   - `sleep(duplicate_sleep_ms)`
4. 如果是新帧：
   - 调 `ProcessFrame(frame)`
   - 处理完成后更新最近一次 `pts / best_effort_timestamp`

### 10.1 如何判断“重复帧”

当前实现不是按图像内容比对，而是按时间戳：

- 若 `pts` 和 `best_effort_timestamp` 都是 `AV_NOPTS_VALUE`
  - 认为无法判断重复，默认每次都当新帧
- 否则只要当前帧的 `pts` 或 `best_effort_timestamp` 变化
  - 就认为是新帧

这能保证在 latest-frame 模式下，不会对同一帧反复推理。

## 11. 推理链路说明

推理由 `infer_v6_cpp::YoloV6RknnInfer` 完成，`ffmpeg_video_infer` 只在应用层做这些事：

1. 把 `FrameData` 包装成 `cv::Mat`
2. clone 成独立 `frame_copy`
3. 调 `infer.Infer(frame_copy, detections, &stats)`
4. 取到 `detections` 和 `stats`
5. 立即发送当前帧检测结果
6. 再做可选可视化、显示、保存

### 11.1 `Infer()` 之前输入的是什么

当前输入给 RKNN 的就是接收器产出的 BGR 图像副本：

```cpp
cv::Mat bgr(...);
cv::Mat frame_copy = bgr.clone();
infer.Infer(frame_copy, detections, &stats);
```

也就是说：

- `ffmpeg_video_infer` 不自己做 ROI 决策
- 不自己做目标筛选
- 不自己改模型输入格式
- 输入坐标系保持为当前送入 `Infer()` 的图像坐标系

### 11.2 `Detection` 结构

`infer_v6_cpp` 返回的检测结构是：

| 字段 | 含义 |
| --- | --- |
| `x1` | 左上角 x |
| `y1` | 左上角 y |
| `x2` | 右下角 x |
| `y2` | 右下角 y |
| `score` | 置信度 |
| `class_id` | 类别 ID |

### 11.3 推理延迟是怎么计算的

`InferenceStats` 来自 `infer_v6_cpp`，当前字段如下：

| 字段 | 含义 |
| --- | --- |
| `preprocess_ms` | 预处理耗时 |
| `inference_ms` | 推理耗时 |
| `postprocess_ms` | 后处理耗时 |
| `output_count` | 模型输出 tensor 数量 |
| `raw_score_max` | 后处理阶段观察到的最大原始分数 |

### 11.4 预处理时间 `preprocess_ms`

当前计时范围是：

1. `LetterBox(bgr_image, input_size, &letter_box)`
2. `cv::cvtColor(letterboxed, input_rgb, cv::COLOR_BGR2RGB)`
3. 如果 `input_rgb` 不是连续内存，则 clone 成连续内存

也就是：

- resize / padding
- BGR -> RGB
- 输入内存整理

都算在 `preprocess_ms` 里。

### 11.5 推理时间 `inference_ms`

基础 wall-clock 计时范围是：

1. `rknn_run(ctx_, nullptr)`
2. `rknn_outputs_get(...)`

随后如果 `rknn_query(ctx_, RKNN_QUERY_PERF_RUN, ...)` 成功，当前实现会用 `perf_run.run_duration / 1000.0` 覆盖前面的 wall-clock 推理时间。

因此当前日志里的 `inference_ms` 优先代表：

- RKNN runtime/NPU 上报的推理运行时长

如果查询失败，则退回：

- `rknn_run + rknn_outputs_get` 的实测 wall-clock 时长

### 11.6 后处理时间 `postprocess_ms`

当前计时范围是 `PostProcess(...)` 整段，包括：

- 解码输出 tensor
- 阈值过滤
- NMS
- 把模型空间坐标映射回输入图像坐标

### 11.7 日志里会打印什么

应用层当前约每秒打印一次：

- 当前帧 `pts`
- 当前帧尺寸
- `detections.size()`
- `preprocess_ms`
- `inference_ms`
- `postprocess_ms`

可视化叠加图上还会显示：

- `raw_score_max`
- 当前 decoder 名称
- 检测框数量

## 12. 结果 UDP 回传

### 12.1 模块位置

结果 UDP 回传完全在本项目内实现：

- 头文件：`include/detection_udp_sender.hpp`
- 实现：`src/detection_udp_sender.cpp`

它只负责：

1. 初始化 UDP socket
2. 保存目标 IP / 端口
3. 序列化一帧检测结果
4. 发送一个 UDP 数据包

它不参与：

- 视频接收
- FFmpeg 解码
- RKNN 推理
- 业务决策
- 目标筛选
- 鼠标控制

### 12.2 发送时机

发送发生在：

1. `Infer()` 成功返回
2. `detections` 已经拿到
3. `InferenceStats` 已经拿到
4. 还没进入可视化显示/保存逻辑之前

也就是说，发送位置在“后处理完成之后，立即发送当前帧结果”。

### 12.3 一个帧一个 UDP 包

当前实现严格按下面的原则：

- 每处理完一帧，只发一个 UDP 包
- 一个 UDP 包里包含该帧全部检测框
- 不做“一个框一个包”

### 12.4 JSON 数据格式

发送内容是紧凑的单行 JSON 文本。逻辑上等价于：

```json
{
  "seq": 123,
  "frame_width": 640,
  "frame_height": 640,
  "box_count": 2,
  "boxes": [
    {
      "class_id": 0,
      "score": 0.910,
      "x1": 100.000,
      "y1": 120.000,
      "x2": 180.000,
      "y2": 220.000,
      "cx": 140.000,
      "cy": 170.000
    },
    {
      "class_id": 0,
      "score": 0.880,
      "x1": 240.000,
      "y1": 130.000,
      "x2": 300.000,
      "y2": 210.000,
      "cx": 270.000,
      "cy": 170.000
    }
  ]
}
```

### 12.5 字段说明

| 字段 | 说明 |
| --- | --- |
| `seq` | 发送序号 |
| `frame_width` | 当前发送坐标系的图像宽度 |
| `frame_height` | 当前发送坐标系的图像高度 |
| `box_count` | 当前帧检测框数量 |
| `boxes` | 当前帧全部检测框数组 |
| `class_id` | 检测类别 ID |
| `score` | 置信度 |
| `x1/y1/x2/y2` | 检测框坐标 |
| `cx/cy` | 检测框中心点 |

### 12.6 坐标系说明

当前实现直接发送 `Infer()` 返回的检测框坐标，并使用：

- `frame_copy.cols`
- `frame_copy.rows`

作为 `frame_width` / `frame_height`。

因此：

- 发送坐标系就是当前推理输入图像坐标系
- `ffmpeg_video_infer` 不对检测框再做额外重映射
- 如果当前输入图像本身就是 640x640，那么发送坐标系就是 640x640

### 12.7 无检测框时如何发送

若当前帧没有检测框：

- `box_count = 0`
- `boxes = []`

仍然会发送这个帧包。

### 12.8 `seq` 的行为

`DetectionUdpSender` 内部维护 `next_seq_`，从初始化后开始递增。

当前实现细节是：

- 每次调用 `SendFrameDetections()` 都会构造一个 `seq`
- 构造完 payload 后就自增
- 即使当前 `sendto()` 失败，这个序号也会被消费

所以主机端若遇到发送失败，可能看到序号跳跃，这是当前实现的正常现象。

### 12.9 当前发送方式

当前第一版实现很直接：

- 不开异步线程
- 不做发送队列
- 在当前推理线程里直接 `sendto()`

这样做的好处是结构简单，问题定位容易。后续如果要改异步发送，只需要替换 `DetectionUdpSender` 内部实现或在应用层加队列即可。

### 12.10 错误处理策略

发送模块遵循“失败不影响主流程”的原则：

- socket 初始化失败
  - 记录 `WARN`
  - 禁用结果 UDP 功能
  - 推理继续
- `sendto()` 失败
  - 记录 `WARN`
  - 当前主循环继续
- 不会因为发送失败退出接收或推理

## 13. 显示与保存功能的地位

当前项目虽然保留了：

- `--display`
- `--save`

但它们都不是主流程依赖项。

默认情况下：

- `enable_display = false`
- `save_result = false`
- `enable_result_udp = true`

也就是默认只做：

- 接收
- latest-frame
- 推理
- 结果回传

### 13.1 显示功能做了什么

只有在 `--display` 打开时才会：

- 创建 OpenCV 窗口
- 显示等待画面
- 显示检测框叠加结果
- 处理 `q / ESC` 退出

### 13.2 保存功能做了什么

只有在 `--save` 打开时才会：

- 创建输出目录
- 把可视化结果按 `frame_000000.jpg` 这类名字保存到 `output_dir`

这个保存功能保存的是“带框的结果图”，不是编码视频，也不是 latest-frame 原图。

## 14. 关键实现细节备忘

### 14.1 为什么只处理 latest frame

因为接收层用的是 `LatestFrameBuffer`：

- 新帧会覆盖旧帧
- 应用层不会积压处理落后的历史帧

所以这个工程更关注“尽量处理当前画面”，而不是“保证每一帧都被处理”。

### 14.2 为什么不会重复推理同一帧

应用层记录了上一帧的：

- `last_pts`
- `last_best_effort_timestamp`

若时间戳未变化，就视为重复 latest frame 并跳过推理。

### 14.3 为什么收流和推理是同一个工程

因为现在板子端实际可运行主入口已经是 `ffmpeg_video_infer`：

- `udp_client_recv` 的稳定接收能力复用进来
- `infer_v6_cpp` 的稳定推理能力复用进来
- 结果回传也在这里统一完成

从工程维护角度看，后续新增业务逻辑，优先应放在 `ffmpeg_video_infer` 这一层，而不是再去改动底层接收器或推理库。

## 15. 常见问题

### 15.1 为什么运行 `ffmpeg_video_infer` 时不要再单独运行 `udp_latest_frame_receiver`

因为它们都要绑定同一个 UDP 输入端口，通常是 `5000`。

### 15.2 如果主机端暂时没开接收，板子端会不会退出

不会。

- UDP 结果发送失败只会打 `WARN`
- 推理主循环会继续

### 15.3 如果没有开启显示，程序是不是就不工作了

不是。

当前默认就是不显示，但仍然会：

- 接流
- 硬解
- 推理
- 发送检测结果

### 15.4 如果想把结果发送到别的主机怎么办

使用：

```bash
./ffmpeg_video_infer --result_udp_ip <IP> --result_udp_port <PORT>
```

### 15.5 如果只想验证本地推理，不想发 UDP

使用：

```bash
./ffmpeg_video_infer --disable_result_udp
```

## 16. 结论

当前 `ffmpeg_video_infer` 已经是板子端的统一实时推理入口，负责把三件事接在一起：

1. 复用 `udp_client_recv` 的 FFmpeg + `h264_rkmpp` latest-frame 接收链
2. 复用 `infer_v6_cpp` 的 YOLOv6 RKNN 推理链
3. 在应用层把每帧检测结果通过一个 UDP JSON 包发回主机

如果后续要继续扩展，推荐仍然遵循当前边界：

- 接收底层问题，优先在 `udp_client_recv` 处理
- 模型和后处理问题，优先在 `infer_v6_cpp` 处理
- 业务逻辑、网络协议、结果转发、调度控制，优先在 `ffmpeg_video_infer` 处理
