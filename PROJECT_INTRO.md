# 项目介绍

## 1. 项目概述

这个项目目前由三部分组成：

- `udp_client_recv`：负责从主机端接收实时推流画面，在 RK3588 开发板上完成 UDP 接收、MPEG-TS 解封装、H.264 硬件解码，并始终保留“最新一帧”。
- `rknn_infer`：负责加载已经训练并量化好的 `.rknn` 模型，对输入图像进行推理，并输出检测结果与性能分析信息。
- `ffmpeg_video_infer`：负责把 `udp_client_recv` 的最新帧接入 `infer_v6_cpp`，组成一个完整的“实时接收 + 板端推理”C++ demo。

两个项目配合后的整体目标是：

1. 在主机端采集桌面或指定区域画面。
2. 通过 UDP 将 H.264 码流推送到 RK3588 开发板。
3. 开发板端实时接收并解码视频流，拿到最新图像帧。
4. 使用 RKNN 模型在板端完成目标检测推理。

当前项目中，检测类别为单类：

- `Head`

---

## 2. 整体架构

当前完整链路如下：

```text
Windows 主机桌面采集
    -> FFmpeg + NVENC 编码
    -> UDP / MPEG-TS / H.264 推流
    -> RK3588 开发板接收
    -> FFmpeg + h264_rkmpp 硬件解码
    -> LatestFrameBuffer 保留最新帧
    -> infer_v6_cpp / ffmpeg_video_infer
    -> RKNN 模型推理
    -> 输出检测框与性能信息
```

这个架构的重点有两个：

- 视频链路侧重低延迟，采用 UDP + MPEG-TS + H.264，并尽量避免缓存堆积。
- 推理链路侧重板端部署，使用 RKNN 模型在 RK3588 NPU 上执行推理。

---

## 3. `udp_client_recv` 项目说明

路径：

- `../udp_client_recv`

### 3.1 项目作用

`udp_client_recv` 是板端视频接收模块，用于接收主机端推流来的画面。它的核心目标不是保存所有帧，而是始终只保留一张“最新帧”，方便后续视觉算法或推理模块直接读取最新图像，避免因为排队导致延迟不断累积。

### 3.2 主要组成

主要文件如下：

- `../udp_client_recv/src/main.cpp`
- `../udp_client_recv/src/receiver.cpp`
- `../udp_client_recv/src/latest_frame_buffer.cpp`
- `../udp_client_recv/include/receiver.h`
- `../udp_client_recv/include/latest_frame_buffer.h`
- `../udp_client_recv/host_server/main.cpp`

### 3.3 板端接收程序

板端主程序是 `udp_latest_frame_receiver`，通过 CMake 构建，依赖：

- OpenCV
- FFmpeg

构建配置见：

- `../udp_client_recv/CMakeLists.txt`

当前这个目录除了可执行程序，也导出了一个可复用的接收核心库：

- `udp_receiver_core`

这里要注意，`udp_receiver_core` 不是一个新的源码目录，也不是新的独立项目，而是 `udp_client_recv/CMakeLists.txt` 里定义的一个 CMake 库 target。它实际编译的仍然是下面这两份原有源码：

- `../udp_client_recv/src/receiver.cpp`
- `../udp_client_recv/src/latest_frame_buffer.cpp`

这样后续其他项目可以直接复用 `UdpTsH264Receiver` 和 `FrameData`，而不用把接收逻辑复制一份，也不会出现“维护两份接收器实现”的问题。

该程序默认监听的输入地址为：

```text
udp://0.0.0.0:5000?pkt_size=188&fifo_size=32768&buffer_size=8388608&overrun_nonfatal=1
```

可选功能：

- `--display`：打开 OpenCV 预览窗口
- `--no-display`：仅接收不显示

### 3.4 接收与解码流程

`UdpTsH264Receiver` 是核心类，负责：

1. 打开 UDP MPEG-TS 输入流。
2. 从流中自动选择 H.264 视频流。
3. 等待带有 SPS/PPS 的关键帧后再启动解码器。
4. 使用 `h264_rkmpp` 进行硬件解码。
5. 将解码得到的图像转换成 `BGR24`。
6. 把结果写入 `LatestFrameBuffer`。
7. 在 EOF、I/O 超时或读流失败时自动重连。

其中几个关键实现点：

- 只支持 H.264 视频流。
- 优先使用 RK3588 上的 `h264_rkmpp` 硬件解码器。
- 对于刚加入的接收端，会等待能正确初始化解码器的 SPS/PPS + IDR 包，避免直接解码失败。
- 解码后统一转成 OpenCV 更方便处理的 `BGR24` 格式。

### 3.5 Latest Frame 模式

`LatestFrameBuffer` 的设计非常直接：

- 新帧来了就覆盖旧帧。
- 外部读取时总是拿到当前最新一帧。
- 不维护长队列，不做历史帧缓存。

这样做的优点是：

- 可以有效降低端到端延迟。
- 避免下游处理速度不稳定时导致帧堆积。
- 更适合实时视觉系统。

### 3.6 主机端推流程序

`../udp_client_recv/host_server/main.cpp` 是一个 Windows 侧辅助程序，用来启动 FFmpeg 推流。

它的工作方式是：

- 通过 `ddagrab` 抓取桌面画面。
- 裁剪一个固定区域，当前配置是 `640x640`。
- 使用 `h264_nvenc` 编码。
- 通过 `mpegts` 封装后经 UDP 推送到开发板。

默认目标地址配置为：

```text
192.168.7.2:5000
```

这个程序的设计重点是低延迟，因此使用了：

- `-forced-idr 1`
- `-bf 0`
- `-zerolatency 1`
- `-flush_packets 1`
- `-muxdelay 0`
- `-muxpreload 0`

这些参数的目的是尽量让开发板端能够更快收到可解码的关键帧和最新画面。

---

## 4. `rknn_infer` 项目说明

路径：

- `.`

### 4.1 项目作用

`rknn_infer` 用于加载训练好、转换好、量化好的 RKNN 模型，并在 RK3588 板端执行推理。

项目中包含：

- RKNN Toolkit 相关文件
- RKNN Model Zoo 参考代码
- 自己编写的 Python / C++ 推理实现
- 模型文件、测试图片和推理结果

当前实际使用的目录结构如下：

- `model`：存放 `.rknn` 模型
- `images`：存放测试图片
- `infer_v6_python`：Python 版推理实现
- `infer_v6_cpp`：C++ 版推理实现
- `rknn-toolkit2-2.3.2`：RKNN Toolkit2
- `rknn_model_zoo-2.3.2`：官方示例与参考实现

### 4.2 目录说明

重要目录如下：

- `./model/v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn`：当前使用的 RKNN 模型
- `./images/*.jpg`：当前测试图片
- `./infer_v6_python/yolov6_rknn_infer.py`：Python 推理脚本
- `./infer_v6_cpp/include/yolov6_rknn.hpp`：C++ 推理模块头文件
- `./infer_v6_cpp/src/yolov6_rknn.cpp`：C++ 推理模块实现
- `./infer_v6_cpp/src/main.cpp`：C++ 批量图片推理入口
- `./infer_v6_cpp/result`：C++ 推理结果图
- `./rknn-toolkit2-2.3.2`：RKNN Toolkit2
- `./rknn_model_zoo-2.3.2`：官方示例与参考实现

### 4.3 推理脚本职责

当前 Python 推理脚本为：

- `infer_v6_python/yolov6_rknn_infer.py`

这个脚本负责完成：

1. 加载 `.rknn` 模型。
2. 初始化 RK3588 runtime。
3. 对输入图片做预处理。
4. 调用 `rknn.inference()` 执行推理。
5. 对输出做后处理，得到检测框。
6. 将检测结果画到原图上并保存。
7. 调用 `eval_perf` 输出每层的性能统计。

当前 C++ 推理模块为：

- `infer_v6_cpp`

这个模块的设计目标是：

1. 提供一个可复用的 `YoloV6RknnInfer` 类。
2. 输入直接使用 `cv::Mat`，方便与 OpenCV / FFmpeg 接收链路对接。
3. 独立完成 `letterbox`、RKNN 推理、后处理、NMS 和画框。
4. 支持单张图和目录批量推理。
5. 输出每张图的预处理、推理、后处理耗时。

### 4.4 当前推理特性

Python 脚本中已经包含以下能力：

- 目标平台指定为 `rk3588`
- 开启 `perf_debug=True`
- 调用 `rknn.eval_perf(is_print=True)` 输出性能表
- 对输入图片进行 `letterbox` 预处理
- 支持单类检测，类别名为 `Head`
- 将检测结果保存到 `infer_v6_python/result`

`eval_perf` 的输出里可以看到每一层运行在哪个目标上，常见情况是：

- `InputOperator` / `OutputOperator` 在 `CPU`
- 大部分卷积、激活、拼接等算子在 `NPU`

这部分信息非常适合用来分析模型是否充分利用了 RK3588 的 NPU。

C++ 推理模块当前具备以下能力：

- 默认从 `./model` 读取模型
- 默认从 `./images` 批量读取测试图片
- 默认把结果写到 `./infer_v6_cpp/result`
- 支持单类 `Head` 检测
- 输出每张图的 `preprocess_ms`、`inference_ms`、`postprocess_ms`
- 导出 `infer_v6_cpp_lib` 供其他 C++ 项目直接链接复用

从当前模型的输出属性可以看到：

- 输入为 `NHWC int8`
- 输出为 9 个张量
- 三个尺度分别对应 `80x80`、`40x40`、`20x20`
- 输出为量化 `INT8 + AFFINE` 格式

这也是 C++ 后处理实现按多输出 YOLOv6 分支来解析的依据。

### 4.5 典型使用方式

在开发板上进入项目目录后，可以直接运行：

```bash
python3 infer_v6_python/yolov6_rknn_infer.py
```

如果只想看推理，不想执行 `eval_perf`，可以使用：

```bash
python3 infer_v6_python/yolov6_rknn_infer.py --skip_eval_perf
```

运行后会：

- 读取 `images` 下的测试图片
- 调用 RKNN 模型完成推理
- 将结果保存到 `infer_v6_python/result`
- 在终端打印原始输出统计、检测结果和性能信息

当前 C++ 版本的典型使用方式为：

```bash
cd rknn_infer/infer_v6_cpp/build
./infer_v6_cpp_demo
```

如果只想跑一张图，可以使用：

```bash
./infer_v6_cpp_demo --image /home/orangepi/code/rknn_infer/images/1.jpg
```

运行后会：

- 从 `../model` 读取 RKNN 模型
- 从 `../images` 读取测试图片
- 将带框结果保存到 `../result`
- 打印每张图的检测结果和耗时统计

### 4.6 当前验证结论

目前 `infer_v6_cpp` 已经在板端完成了独立验证：

- 可以成功加载 RKNN 模型
- 可以正确解析 9 个 INT8 输出张量
- 可以完成批量图片推理
- 可以保存每张图的检测结果
- 可以输出单张图和平均推理耗时

因此后续与视频接收链路的拼接，可以直接复用 `infer_v6_cpp_lib`，不需要再单独重写 RKNN 推理部分。

---

## 5. 两个项目如何配合

从系统视角看，`udp_client_recv` 和 `rknn_infer` 分别负责“图像到达板子”和“板子上做智能分析”这两个阶段。

分工可以概括为：

- `udp_client_recv`：负责实时接收和解码视频
- `rknn_infer`：负责模型推理和目标检测

如果后续要做成完整实时系统，一般会按下面方式组合：

1. 主机端持续推送视频流。
2. RK3588 上的 `udp_client_recv` 持续接收并维护最新帧。
3. `ffmpeg_video_infer` 从 `udp_receiver_core` 读取最新帧。
4. `ffmpeg_video_infer` 调用 `infer_v6_cpp_lib` 对该帧执行 RKNN 推理。
5. 输出检测框、类别、置信度和性能信息。

更准确地说，`ffmpeg_video_infer` 不是“再去启动一个外部的 udp_client_recv 进程”，而是直接在同一个进程里链接并调用 `udp_receiver_core`。因此板子端运行时只需要启动：

```bash
./build/ffmpeg_video_infer
```

不需要先单独启动 `udp_latest_frame_receiver`。如果两者同时运行，它们会竞争同一个 UDP 监听端口。

这样拆分后有两个明显好处：

- 接收模块和推理模块相互独立，后续替换任何一端都不会影响另一端接口。
- 实时 demo 只做“编排”和“显示”，不承担底层接收或模型后处理细节。

这是一种典型的“实时视频接收 + 板端 NPU 推理”方案。

---

## 6. 项目特点总结

这个项目的几个核心特点是：

- 面向 RK3588 平台，强调板端部署
- 视频接收链路强调低延迟而不是完整存档
- 接收端采用最新帧模式，避免帧堆积
- 推理端使用 RKNN 模型，适配 NPU 部署
- Python 与 C++ 两套推理入口并存，便于调试与最终部署
- 实时 demo 独立成单独项目，便于后续扩展和维护
- 支持通过 `eval_perf` 查看每层是在 CPU 还是 NPU 上执行
- 当前检测任务为单类 `Head` 检测

---

## 7. 适合对外介绍时的简短版本

如果需要更简短地向别人介绍，可以直接说：

> 这个系统分成三层：`udp_client_recv` 负责把主机端推送过来的 H.264 画面实时接收到 RK3588 板子上并解码成最新帧；`rknn_infer` 负责加载训练和量化好的 RKNN 模型做目标检测；`ffmpeg_video_infer` 负责把接收和推理两层拼起来，组成一套完整的“主机推流 + 板端接收 + NPU 实时推理”的 C++ 实时系统。
