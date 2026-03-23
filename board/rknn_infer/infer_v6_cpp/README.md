# infer_v6_cpp

`infer_v6_cpp` 是当前 `Head` 单类检测模型的 C++ 推理实现。

它的目标不是做一个一次性的 demo，而是提供一个可以被其他 C++ 项目直接复用的 RKNN 推理模块。当前 `ffmpeg_video_infer` 会直接链接这里导出的 `infer_v6_cpp_lib`。

## 目录结构

```text
infer_v6_cpp
├── CMakeLists.txt
├── include
│   └── yolov6_rknn.hpp
├── src
│   ├── main.cpp
│   └── yolov6_rknn.cpp
└── result
```

相关资源位于 `rknn_infer` 根目录：

- `../model/v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn`
- `../images/*.jpg`

## 模块说明

### `infer_v6_cpp_lib`

核心类是 `YoloV6RknnInfer`，定义在：

- `include/yolov6_rknn.hpp`

这个类负责：

1. 加载 RKNN 模型
2. 查询输入输出张量属性
3. 对 `cv::Mat` 做 `letterbox`
4. 调用 RKNN C API 执行推理
5. 解析 YOLOv6 多分支输出
6. 做 NMS
7. 返回检测框和耗时统计

它的输入接口是 `cv::Mat`，因此非常适合和 OpenCV、FFmpeg 解码后的图像直接对接。

### `infer_v6_cpp_demo`

命令行入口位于：

- `src/main.cpp`

它负责：

- 读取单张图片或整个目录
- 逐张调用 `YoloV6RknnInfer`
- 打印检测结果和耗时
- 保存带框结果图到 `result`

## 默认路径

不传参数时，默认路径如下：

- 模型：`../model/v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn`
- 图片目录：`../images`
- 结果目录：`./result`

## 构建方式

```bash
cd /home/orangepi/code/rknn_infer/infer_v6_cpp
cmake -S . -B build
cmake --build build -j4
```

生成的可执行文件为：

- `build/infer_v6_cpp_demo`

## 使用方式

批量跑 `rknn_infer/images`：

```bash
cd /home/orangepi/code/rknn_infer/infer_v6_cpp/build
./infer_v6_cpp_demo
```

只跑单张图片：

```bash
./infer_v6_cpp_demo --image /home/orangepi/code/rknn_infer/images/1.jpg
```

指定模型、图片目录和输出目录：

```bash
./infer_v6_cpp_demo \
  --model /home/orangepi/code/rknn_infer/model/v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn \
  --image_dir /home/orangepi/code/rknn_infer/images \
  --output_dir /home/orangepi/code/rknn_infer/infer_v6_cpp/result
```

## 运行输出

程序会对每张图打印：

- 检测数量
- `preprocess_ms`
- `inference_ms`
- `postprocess_ms`
- `raw_score_max`
- 结果图保存路径

最后还会打印：

- `avg_preprocess_ms`
- `avg_inference_ms`
- `avg_postprocess_ms`

## 当前实现特点

- 当前类别固定为单类 `Head`
- 默认阈值：
  - `obj_threshold = 0.25`
  - `nms_threshold = 0.45`
- 输入直接使用 `cv::Mat`
- 输出结果使用 `Detection` 结构体返回
- 结果绘制通过 `DrawDetections()` 完成

## 已验证情况

当前版本已经在 RK3588 板端完成独立验证：

- 能正常加载模型
- 能解析 9 个输出张量
- 能对 `images` 目录做批量推理
- 能输出每张图的耗时统计
- 能把带框结果图保存到 `result`

## 后续对接方式

如果后续要接视频流，不建议在视频工程里重新实现 RKNN 推理。更合适的方式是：

1. 视频接收模块拿到一张 `cv::Mat`
2. 直接调用 `YoloV6RknnInfer::Infer()`
3. 需要可视化时调用 `DrawDetections()`

这样可以保证视频工程和模型后处理逻辑完全解耦。
