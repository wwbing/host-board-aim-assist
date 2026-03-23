# remote_box_receiver

`Net_Aim_Control` 当前是一个统一主控的 Windows C++ 项目，负责同时完成两条链路：

- 发送链路：启动 `ffmpeg.exe`，抓取屏幕中心 ROI，通过 `UDP + MPEG-TS` 推流到开发板
- 控制链路：接收开发板回传的 UDP 检测结果，做状态机/评分目标选择，并调用 `ddll64.dll` 的 `MoveR(dx, dy)` 做相对移动

当前主流程：

```text
ffmpeg sender service -> 开发板视频输入
开发板检测结果 -> UDP JSON -> 状态机/评分选目标 -> ROI 映射 -> delta_xy -> MoveR(dx, dy)
```

## 当前已实现功能

- UDP 接收开发板回传的每帧检测结果
- JSON 解析与字段校验
- `IDLE / CANDIDATE / TRACKING / LOST` 目标状态机
- `center_score + confidence_score + continuity_score` 综合评分
- 将 ROI 内目标点映射到真实屏幕坐标
- 输出 `screen_x / screen_y`
- 输出相对屏幕中心偏移 `dx / dy`
- 使用 `ddll64.dll` 的 `MoveR(dx, dy)` 做相对移动
- `Q` 热键切换自动移动开关
- 可选 `--display` Win32/GDI 调试显示
- 内置 ffmpeg sender service，由主程序统一启动/停止和监控

## 项目结构

```text
Net_Aim_Control/
  CMakeLists.txt
  README.md
  include/
    app.hpp
    mouse_driver.hpp
    types.hpp
    udp_receiver.hpp
    sender/
      ffmpeg_sender_options.hpp
      ffmpeg_command_builder.hpp
      ffmpeg_process.hpp
      ffmpeg_sender_service.hpp
  runtime/
    ddll64.dll
  src/
    app.cpp
    main.cpp
    udp_receiver.cpp
    sender/
      ffmpeg_command_builder.cpp
      ffmpeg_process.cpp
      ffmpeg_sender_service.cpp
  third_party/
    include/
      nlohmann/
    share/
      nlohmann_json/
```

## sender 模块说明

`UDP_SendFrame` 没有被粗暴并进主程序，而是拆成了 4 个 sender 模块：

- `ffmpeg_sender_options`
  保存 ffmpeg 推流配置
- `ffmpeg_command_builder`
  负责构建 `ddagrab` filter、UDP URL 和最终命令行
- `ffmpeg_process`
  负责 `CreateProcessW`、stdout/stderr 管道、日志线程、优雅停止
- `ffmpeg_sender_service`
  作为主程序调用的后台服务接口，提供 `Start / Stop / IsRunning / LastError`

`App` 只做最小接入：

- 初始化 UDP 接收
- 初始化鼠标 DLL
- 按配置启动 sender service
- 运行期检查 sender 是否还在运行
- 退出时停止 sender service

第一版不做 sender 自动重启，也不让 sender 直接参与目标选择或鼠标控制。

## 目标状态机说明

- `IDLE`
  当前没有可信目标，不输出有效移动
- `CANDIDATE`
  发现候选目标后，连续观察若干帧确认，不因单帧目标立刻锁定
- `TRACKING`
  已锁定主目标，优先保持目标延续，只在新目标明显更优且连续确认后才切换
- `LOST`
  锁定目标短时丢失，先等待恢复，超时后回到 `IDLE`

只有状态机进入 `TRACKING` 后，才会真正输出有效 `delta_xy` 并调用 `MoveR(dx, dy)`。

## 评分机制说明

每个候选框的总分：

```text
total_score =
    weight_center * center_score
  + weight_confidence * confidence_score
  + weight_continuity * continuity_score
```

- `center_score`
  越靠近 ROI 中心越高
- `confidence_score`
  直接使用检测框 `score`
- `continuity_score`
  越接近上一帧锁定目标中心越高，只在延续/恢复阶段起主要作用

## 默认配置

### 接收与控制

- `listen_ip = 0.0.0.0`
- `listen_port = 6000`
- `enable_display = false`
- `screen_width / screen_height = 启动时自动检测`
- `roi_width = 640`
- `roi_height = 640`
- `roi_offset_x / roi_offset_y = 未显式指定时自动居中`

### tracker

- `min_score = 0.45`
- `acquire_confirm_frames = 2`
- `lost_timeout_ms = 80`
- `continue_gate_radius = 120`
- `switch_margin = 0.15`
- `switch_confirm_frames = 2`
- `weight_center = 0.45`
- `weight_confidence = 0.20`
- `weight_continuity = 0.35`

### sender

- `enable_sender = true`
- `ffmpeg_path = C:\Users\jiahao\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1-full_build\bin\ffmpeg.exe`
- `sender_output_ip = 192.168.7.2`
- `sender_output_port = 5000`
- `sender_output_idx = 0`
- `sender_framerate = 60`
- `sender_crop_width = roi_width`
- `sender_crop_height = roi_height`
- `sender_offset_x = roi_offset_x`
- `sender_offset_y = roi_offset_y`
- `sender_bitrate = 4M`
- `sender_maxrate = 4M`
- `sender_bufsize = 150k`
- `sender_gop = 10`
- `sender_pkt_size = 188`
- `sender_udp_buffer_size = 1048576`

默认情况下 sender 抓取区域和当前控制链路使用的 ROI 一致。

## ffmpeg 参数保持策略

sender 仍然沿用原 `UDP_SendFrame` 已验证可用的方案：

- `CreateProcessW` 启动 `ffmpeg.exe`
- `ddagrab`
- `h264_nvenc`
- `UDP + MPEG-TS`
- stdout/stderr 管道实时日志
- 优雅停止时先发 `CTRL_BREAK_EVENT`，超时再强制结束

不会改成：

- TCP
- 自写抓屏线程
- bat 脚本外壳启动
- 新的发送协议

## 命令行参数

### 基础参数

- `--listen_ip <ip>`
- `--listen_port <port>`
- `--display`
- `--savejson`
- `--screen_width <value>`
- `--screen_height <value>`
- `--roi_width <value>`
- `--roi_height <value>`
- `--roi_offset_x <value>`
- `--roi_offset_y <value>`

### sender 参数

- `--disable_sender`
- `--ffmpeg_path <path>`
- `--sender_output_ip <ip>`
- `--sender_output_port <port>`
- `--sender_output_idx <index>`
- `--sender_framerate <value>`
- `--sender_crop_width <value>`
- `--sender_crop_height <value>`
- `--sender_offset_x <value>`
- `--sender_offset_y <value>`
- `--sender_bitrate <value>`
- `--sender_maxrate <value>`
- `--sender_bufsize <value>`
- `--sender_gop <value>`
- `--sender_pkt_size <value>`
- `--sender_udp_buffer_size <value>`
- `--help`

## 运行示例

纯控制台模式：

```powershell
.\build-vs\remote_box_receiver.exe
```

打开调试显示：

```powershell
.\build-vs\remote_box_receiver.exe --display
```

指定 sender 输出地址：

```powershell
.\build-vs\remote_box_receiver.exe --display --sender_output_ip 192.168.7.2 --sender_output_port 5000
```

禁用 sender，仅保留接收与控制：

```powershell
.\build-vs\remote_box_receiver.exe --disable_sender
```

开启 UDP JSON 保存调试：

```powershell
.\build-vs\remote_box_receiver.exe --savejson
```

`--savejson` 开启后，程序会把每个“成功解析的 UDP JSON 包”按一行一条记录保存到当前工作目录下的 `saved_udp_packets.jsonl`。

## 构建

运行时依赖：

- `runtime/ddll64.dll`
- 项目内 vendored `nlohmann_json` package

当前 CMake 会自动：

- 使用 `third_party/share/nlohmann_json` 和 `third_party/include/nlohmann`
- 编译 sender 模块和现有主控模块
- 在 Windows 下把 `ddll64.dll` 复制到可执行文件目录

构建命令：

```powershell
cmake -S . -B build-vs
cmake --build build-vs
```

如果当前 shell 没有编译器环境，请先进入 Visual Studio Developer Command Prompt。

## 调试显示

传 `--display` 后会打开一个 Win32/GDI 调试窗口，当前会显示：

- 检测框
- 当前选中目标
- `screen_xy`
- `delta_xy`
- tracker 状态
- 自动移动开关
- ffmpeg sender 运行状态

## 输入 JSON 格式

每个 UDP 包包含一帧 JSON 文本，例如：

```json
{
  "seq": 123,
  "frame_pts": 456000,
  "frame_best_effort_ts": 456000,
  "frame_width": 640,
  "frame_height": 640,
  "box_count": 2,
  "timing": {
    "board_wall_infer_done_ms": 1711111111111,
    "board_infer_us": 12400,
    "board_post_us": 700
  },
  "boxes": [
    {
      "class_id": 0,
      "score": 0.91,
      "x1": 100,
      "y1": 120,
      "x2": 180,
      "y2": 220,
      "cx": 140,
      "cy": 170
    }
  ]
}
```

## Host Profiling / Timing

当前主机端已经补上 profiling 基础设施，但不会改变现有目标选择和 `MoveR(dx, dy)` 控制逻辑。

已加入的 profiling 能力：

- sender 成功启动时记录：
  - `sender_start_wall_ms`
  - `sender_start_wall_ns`
  - `sender_framerate`
- 结果包解析层前向兼容支持：
  - `frame_pts`
  - `frame_best_effort_ts`
  - `timing.board_wall_infer_done_ms`
  - `timing.board_infer_us`
  - `timing.board_post_us`
- 根据：
  - `sender_start_wall_ms`
  - `frame_pts`
  - `first_frame_pts`
  - `pts_per_second`
  估算 `nominal_send_ms`
- 支持 NTP 风格四时间戳的 clock offset client：
  - `t1 / t2 / t3 / t4`
  - `offset = ((t2 - t1) + (t3 - t4)) / 2`
  - `delay = (t4 - t1) - (t3 - t2)`
- 低频输出 profiling 日志，并在 `--display` 面板中显示 timing 指标

当前设计是前向兼容的：

- 板子端还没发 timing 字段时，接收、状态机、显示和控制链照常运行
- 板子端后续加上 timing 字段后，主机端可以直接解析并参与 profiling 统计
