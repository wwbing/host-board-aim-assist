#include "app.hpp"
#include "logging/logger.hpp"
#include "profiling/clock_offset_client.hpp"
#include "sender/ffmpeg_sender_service.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace
{

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr char kWindowClassName[] = "RemoteBoxReceiverWindowClass";
constexpr char kWindowTitle[] = "remote_box_receiver";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerIntervalMs = 33;
constexpr int kClientWidth = 980;
constexpr int kClientHeight = 760;
constexpr int kMargin = 20;
constexpr int kCanvasSize = 640;
constexpr int kInfoPanelWidth = 260;
constexpr auto kStaleTimeout = std::chrono::milliseconds(1500);
constexpr int kToggleAutoMoveHotkeyId = 1;
volatile LONG g_stop_requested = 0;

const char* TrackerStateToString(const TrackerState state)
{
    switch (state)
    {
    case TrackerState::kIdle:
        return "空闲";
    case TrackerState::kCandidate:
        return "候选";
    case TrackerState::kTracking:
        return "跟踪";
    case TrackerState::kLost:
        return "丢失";
    default:
        return "未知";
    }
}

double Clamp01(const double value)
{
    return std::clamp(value, 0.0, 1.0);
}

BOOL WINAPI ConsoleCtrlHandler(const DWORD control_type)
{
    switch (control_type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        InterlockedExchange(&g_stop_requested, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

bool IsStopRequested()
{
    return InterlockedCompareExchange(&g_stop_requested, 0, 0) != 0;
}

std::wstring NarrowToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int required_size = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (required_size <= 1)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required_size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, result.data(), required_size);
    result.pop_back();
    return result;
}

void PrintConsoleLineAtomic(const std::string& line)
{
    spdlog::info("{}", line);
}

std::int64_t CurrentWallTimeNs()
{
    const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now());
    return now.time_since_epoch().count();
}

std::int64_t CurrentWallTimeMs()
{
    return CurrentWallTimeNs() / 1000000;
}

FfmpegSenderOptions BuildSenderOptionsFromConfig(const Config& config)
{
    FfmpegSenderOptions options;
    options.ffmpeg_path = NarrowToWide(config.ffmpeg_path);
    options.output_ip = NarrowToWide(config.sender_output_ip);
    options.output_port = static_cast<std::uint16_t>(config.sender_output_port);
    options.output_idx = config.sender_output_idx;
    options.framerate = config.sender_framerate;
    options.crop_width = config.sender_crop_width;
    options.crop_height = config.sender_crop_height;
    options.offset_x = config.sender_offset_x;
    options.offset_y = config.sender_offset_y;
    options.bitrate = NarrowToWide(config.sender_bitrate);
    options.maxrate = NarrowToWide(config.sender_maxrate);
    options.bufsize = NarrowToWide(config.sender_bufsize);
    options.gop = config.sender_gop;
    options.pkt_size = config.sender_pkt_size;
    options.udp_buffer_size = config.sender_udp_buffer_size;
    options.pts_per_second = config.sender_pts_per_second;
    return options;
}

bool ReadRequiredInt(const json& object, const char* key, int& value, std::string& error_message)
{
    const auto it = object.find(key);
    if (it == object.end())
    {
        error_message = std::string("缺少整数字段: ") + key;
        return false;
    }

    if (!it->is_number_integer())
    {
        error_message = std::string("字段不是整数: ") + key;
        return false;
    }

    value = it->get<int>();
    return true;
}

bool ReadRequiredUint64(const json& object, const char* key, std::uint64_t& value, std::string& error_message)
{
    const auto it = object.find(key);
    if (it == object.end())
    {
        error_message = std::string("缺少整数字段: ") + key;
        return false;
    }

    if (!it->is_number_unsigned() && !it->is_number_integer())
    {
        error_message = std::string("字段不是整数: ") + key;
        return false;
    }

    value = it->get<std::uint64_t>();
    return true;
}

bool ReadNumber(const json& object, const char* key, double& value)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number())
    {
        return false;
    }

    value = it->get<double>();
    return true;
}

bool ReadOptionalInt64(const json& object, const char* key, std::optional<std::int64_t>& value)
{
    const auto it = object.find(key);
    if (it == object.end() || (!it->is_number_integer() && !it->is_number_unsigned()))
    {
        return false;
    }

    value = it->get<std::int64_t>();
    return true;
}

bool ReadOptionalDouble(const json& object, const char* key, std::optional<double>& value)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number())
    {
        return false;
    }

    value = it->get<double>();
    return true;
}

bool ReadOptionalInt(const json& object, const char* key, int& value)
{
    const auto it = object.find(key);
    if (it == object.end())
    {
        return false;
    }
    if (!it->is_number_integer())
    {
        return false;
    }

    value = it->get<int>();
    return true;
}

COLORREF Rgb(const int r, const int g, const int b)
{
    return RGB(r, g, b);
}

void DrawCross(HDC hdc, const int x, const int y, const int half_span)
{
    MoveToEx(hdc, x - half_span, y, nullptr);
    LineTo(hdc, x + half_span + 1, y);
    MoveToEx(hdc, x, y - half_span, nullptr);
    LineTo(hdc, x, y + half_span + 1);
}

std::string FormatDouble(const double value, const int precision = 2)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

std::string FormatOptionalMillis(const std::optional<double>& value, const int precision = 2)
{
    return value.has_value() ? FormatDouble(*value, precision) + "ms" : "无";
}

std::string FormatOptionalInteger(const std::optional<std::int64_t>& value)
{
    return value.has_value() ? std::to_string(*value) : "无";
}

std::string FormatOptionalWallMs(const std::optional<double>& value)
{
    return value.has_value() ? std::to_string(static_cast<std::int64_t>(std::llround(*value))) : "无";
}

std::string FormatOptionalTimeBase(
    const std::optional<std::int64_t>& num,
    const std::optional<std::int64_t>& den)
{
    if (!num.has_value() || !den.has_value())
    {
        return "无";
    }
    return std::to_string(*num) + "/" + std::to_string(*den);
}

const char* ClockOffsetStatusToString(const ClockOffsetStatus status)
{
    switch (status)
    {
    case ClockOffsetStatus::kDisabled:
        return "已禁用";
    case ClockOffsetStatus::kIdle:
        return "空闲";
    case ClockOffsetStatus::kRequestSent:
        return "已发请求";
    case ClockOffsetStatus::kResponseReceived:
        return "已收响应";
    case ClockOffsetStatus::kValid:
        return "有效";
    case ClockOffsetStatus::kTimeout:
        return "超时";
    case ClockOffsetStatus::kInvalidResponse:
        return "响应无效";
    case ClockOffsetStatus::kSocketError:
        return "套接字错误";
    case ClockOffsetStatus::kInitFailed:
        return "初始化失败";
    default:
        return "未知";
    }
}

std::string FormatClockOffsetRespState(const ClockOffsetStats& stats)
{
    if (stats.status == ClockOffsetStatus::kValid)
    {
        return "正常";
    }
    if (stats.last_raw_response_received && stats.last_response_parsed)
    {
        return "已解析";
    }
    if (stats.last_raw_response_received)
    {
        return "仅收到原始包";
    }
    return "无";
}

std::string FormatClockOffsetStatus(const ClockOffsetStats* stats)
{
    if (stats == nullptr)
    {
        return "已禁用";
    }
    return ClockOffsetStatusToString(stats->status);
}

} // namespace

App::App(Config config)
    : config_(std::move(config))
{
}

App::~App()
{
    ShutdownSenderService();
    UnregisterToggleHotkey();
}

std::optional<TargetPoint> App::GetLatestTargetPoint() const
{
    if (tracker_state_ != TrackerState::kTracking || !tracked_target_.has_value())
    {
        return std::nullopt;
    }

    return TargetPoint{ tracked_target_->selection.screen_x, tracked_target_->selection.screen_y };
}

std::optional<TargetOffset> App::GetLatestTargetOffset() const
{
    if (tracker_state_ != TrackerState::kTracking || !tracked_target_.has_value())
    {
        return std::nullopt;
    }

    return TargetOffset{ tracked_target_->selection.dx, tracked_target_->selection.dy };
}

int App::Run()
{
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    if (!InitializeNetworking())
    {
        return 1;
    }
    if (!InitializeMouseController())
    {
        return 1;
    }

    InitializeJsonSaver();
    InitializeSenderService();
    InitializeProfiling();
    spdlog::info(
        "[信息] 目标选择模式={}，移动滤波={}",
        config_.enable_tracker_state_machine ? "状态机" : "最近中心",
        config_.enable_aim_motion_filters ? "开启" : "关闭");

    spdlog::info(
        "[信息] 当前自瞄控制参数：gain={}，单步上限={}px，死区={}px，更新间隔={}ms",
        config_.aim_gain,
        config_.aim_max_step_px,
        config_.aim_deadzone_px,
        config_.aim_update_interval_ms);

    if (!config_.enable_display)
    {
        RegisterToggleHotkey(nullptr);
        return RunConsoleLoop();
    }

    if (!CreateMainWindow())
    {
        return 1;
    }

    RegisterToggleHotkey(hwnd_);

    MSG message = {};
    while (GetMessageA(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    ShutdownSenderService();
    return static_cast<int>(message.wParam);
}

bool App::InitializeNetworking()
{
    std::string error_message;
    if (!receiver_.Open(config_.listen_ip, config_.listen_port, error_message))
    {
        spdlog::error("[错误] 初始化 UDP 接收器失败：{}", error_message);
        status_line_ = error_message;
        return false;
    }

    return true;
}

bool App::InitializeMouseController()
{
    if (!mouse_controller_.Initialize())
    {
        status_line_ = "鼠标 DLL 初始化失败";
        mouse_status_ = "DLL 不可用";
        return false;
    }

    if (!mouse_controller_.SupportsRelativeMove())
    {
        status_line_ = "鼠标 DLL 缺少 MoveR";
        mouse_status_ = "不支持相对移动";
        spdlog::error("[错误] 鼠标 DLL 不支持相对移动 MoveR");
        return false;
    }

    mouse_status_ = "DLL 相对移动";
    return true;
}

void App::InitializeJsonSaver()
{
    if (!config_.save_json)
    {
        save_json_status_ = "关闭";
        return;
    }

    save_json_stream_.open(config_.save_json_path, std::ios::out | std::ios::trunc);
    if (!save_json_stream_.is_open())
    {
        save_json_status_ = "错误";
        spdlog::error("[错误] 打开 JSON 保存文件失败：{}", config_.save_json_path);
        return;
    }

    save_json_status_ = "开启";
}

void App::InitializeSenderService()
{
    if (!config_.enable_sender)
    {
        ffmpeg_sender_status_ = "已禁用";
        return;
    }

    sender_ = std::make_unique<FfmpegSenderService>(BuildSenderOptionsFromConfig(config_));
    if (!sender_->Start())
    {
        ffmpeg_sender_status_ = "启动失败";
        sender_exit_reported_ = true;
        spdlog::error("[错误] 启动 sender 失败：{}", sender_->LastError());
        return;
    }

    ffmpeg_sender_status_ = "运行中";
    sender_exit_reported_ = false;
    spdlog::info(
        "[信息] 当前 sender 推流配置：目标={}：{}，实际生效帧率={}Hz",
        config_.sender_output_ip,
        config_.sender_output_port,
        config_.sender_framerate);
    spdlog::info("[信息] Sender 已启动，目标={}：{}", config_.sender_output_ip, config_.sender_output_port);
}

void App::ShutdownSenderService()
{
    if (!sender_)
    {
        return;
    }

    sender_->Stop();
    ffmpeg_sender_status_ = "已停止";
}

void App::InitializeProfiling()
{
    if (sender_)
    {
        timing_profiler_.SetSenderTimingContext(sender_->TimingContext());
        timing_profiler_.SetSenderRunning(ffmpeg_sender_status_ == "运行中");
    }

    if (!config_.enable_clock_offset)
    {
        return;
    }

    clock_offset_client_ = std::make_unique<ClockOffsetClient>();
    ClockOffsetClientConfig clock_config;
    clock_config.remote_ip = config_.clock_offset_ip;
    clock_config.remote_port = config_.clock_offset_port;
    clock_config.timeout_ms = config_.clock_offset_timeout_ms;

    std::string error_message;
    if (!clock_offset_client_->Initialize(clock_config, error_message))
    {
        spdlog::warn("[警告] 时钟校准初始化失败：{}", error_message);
        last_clock_offset_request_at_ = Clock::now();
        return;
    }

    last_clock_offset_request_at_ = Clock::now() - std::chrono::milliseconds(config_.clock_offset_interval_ms);
}

void App::PollSenderService()
{
    if (!config_.enable_sender || !sender_)
    {
        return;
    }

    if (sender_->IsRunning())
    {
        ffmpeg_sender_status_ = "运行中";
        return;
    }

    ffmpeg_sender_status_ = "已退出(" + std::to_string(sender_->ExitCode()) + ")";
    if (!sender_exit_reported_)
    {
        spdlog::error("[错误] Sender 已退出：{}", sender_->LastError());
        sender_exit_reported_ = true;
    }
}

void App::PollProfiling()
{
    if (sender_)
    {
        timing_profiler_.SetSenderTimingContext(sender_->TimingContext());
        timing_profiler_.SetSenderRunning(ffmpeg_sender_status_ == "运行中");
    }
    else
    {
        timing_profiler_.SetSenderRunning(false);
    }

    if (clock_offset_client_)
    {
        const auto now = Clock::now();
        if ((now - last_clock_offset_request_at_) >= std::chrono::milliseconds(config_.clock_offset_interval_ms))
        {
            clock_offset_client_->StartMeasurement();
            last_clock_offset_request_at_ = now;
        }
        clock_offset_client_->Poll();
    }

    MaybeLogProfilingSummary();
}

void App::MaybeLogProfilingSummary()
{
    const auto now = Clock::now();
    if ((now - last_profiling_log_at_) < std::chrono::milliseconds(config_.profiling_log_interval_ms))
    {
        return;
    }

    last_profiling_log_at_ = now;

    const ClockOffsetStats* clock_stats = nullptr;
    if (clock_offset_client_)
    {
        clock_stats = &clock_offset_client_->LastStats();
    }

    const auto& estimate = timing_profiler_.LatestEstimate();
    std::ostringstream oss;
    oss << "[时序] 发送端=" << (timing_profiler_.SenderRunning() ? "运行中" : "已停止");
    if (clock_stats != nullptr)
    {
        oss << " 偏移状态=" << FormatClockOffsetStatus(clock_stats)
            << " 最近有效偏移=" << FormatOptionalMillis(clock_stats->last_valid_offset_ms)
            << " 往返延迟=" << FormatOptionalMillis(clock_stats->last_valid_delay_ms)
            << " 偏移可用=" << (clock_stats->last_valid_offset_ms.has_value() ? "是" : "否")
            << " 远端=" << clock_stats->remote_ip << ":" << clock_stats->remote_port
            << " 本地端口=" << clock_stats->local_port
            << " 请求状态=" << (clock_stats->measurement_in_flight ? "等待响应" : "空闲")
            << " 响应状态=" << FormatClockOffsetRespState(*clock_stats)
            << " 已解析=" << (clock_stats->last_response_parsed ? "是" : "否")
            << " 响应来源=" << clock_stats->last_response_ip << ":" << clock_stats->last_response_port
            << " 响应字节数=" << clock_stats->last_response_bytes;
    }
    else
    {
        oss << " 偏移状态=已禁用"
            << " 最近有效偏移=无"
            << " 往返延迟=无"
            << " 偏移可用=否";
    }

    if (timing_profiler_.LatestSeq().has_value())
    {
        oss << " 帧序号=" << *timing_profiler_.LatestSeq();
    }
    else
    {
        oss << " 帧序号=无";
    }

    oss << " 帧PTS=" << FormatOptionalInteger(timing_profiler_.LatestFramePts())
        << " 锚点=" << (estimate.anchor_ready ? "已就绪" : "未建立")
        << " 锚点PTS=" << FormatOptionalInteger(estimate.anchor_frame_pts)
        << " 锚点时基=" <<
            FormatOptionalTimeBase(estimate.anchor_time_base_num, estimate.anchor_time_base_den)
        << " 锚点板端推理完成时间=" << FormatOptionalWallMs(estimate.anchor_board_ms)
        << " 锚点板端流水线耗时=" << FormatOptionalMillis(estimate.anchor_pipeline_delay_ms)
        << " 锚点年龄=" << FormatOptionalInteger(estimate.anchor_age_ms)
        << " 锚点重置次数=" << estimate.anchor_resets
        << " 估算发送时刻=" << FormatOptionalInteger(estimate.nominal_send_ms)
        << " 板端推理完成时间=" << FormatOptionalInteger(estimate.board_wall_infer_done_ms)
        << " 板端预处理耗时=" << FormatOptionalMillis(estimate.board_pre_ms)
        << " 板端推理耗时=" << FormatOptionalMillis(estimate.board_infer_ms)
        << " 板端后处理耗时=" << FormatOptionalMillis(estimate.board_post_ms)
        << " 板端结果发送耗时=" << FormatOptionalMillis(estimate.board_result_send_ms)
        << " 估算链路到推理完成耗时=" <<
            FormatOptionalMillis(estimate.estimated_pipeline_to_infer_done_ms)
        << " 结果回传耗时=" << FormatOptionalMillis(estimate.result_return_ms);
    PrintConsoleLineAtomic(oss.str());
}

bool App::RegisterToggleHotkey(HWND target_window)
{
    UnregisterToggleHotkey();

    if (!RegisterHotKey(target_window, kToggleAutoMoveHotkeyId, MOD_NOREPEAT, 'Q'))
    {
        hotkey_status_ = "Q 不可用";
        spdlog::error("[错误] 注册热键 Q 失败，错误码={}", GetLastError());
        return false;
    }

    hotkey_registered_ = true;
    hotkey_target_ = target_window;
    hotkey_status_ = "Q 切换";
    return true;
}

void App::UnregisterToggleHotkey()
{
    if (!hotkey_registered_)
    {
        return;
    }

    UnregisterHotKey(hotkey_target_, kToggleAutoMoveHotkeyId);
    hotkey_registered_ = false;
    hotkey_target_ = nullptr;
}

int App::RunConsoleLoop()
{
    while (!IsStopRequested())
    {
        ProcessPendingThreadMessages();
        ProcessIncomingPackets();
        PollSenderService();
        PollProfiling();
        ApplyAutoMove();
        Sleep(10);
    }

    ShutdownSenderService();
    return 0;
}

void App::ProcessPendingThreadMessages()
{
    MSG message = {};
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_HOTKEY && message.wParam == kToggleAutoMoveHotkeyId)
        {
            ToggleAutoMove();
        }
    }
}

bool App::ProcessIncomingPackets(bool* state_changed)
{
    const bool had_recent_packet_before = HasRecentPacket();
    bool changed = false;

    UdpPacket packet;
    if (receiver_.ReceiveLatest(packet))
    {
        const std::int64_t host_result_recv_wall_ms = CurrentWallTimeMs();
        has_received_packet_ = true;
        last_packet_at_ = Clock::now();
        latest_frame_received_at_ = last_packet_at_;
        latest_sender_ = packet.sender_ip + ":" + std::to_string(packet.sender_port);

        FrameData frame;
        std::string error_message;
        if (ParseFrameJson(packet.payload, frame, error_message))
        {
            latest_frame_ = std::move(frame);
            SaveJsonPacket(packet, host_result_recv_wall_ms);
            const std::optional<double> clock_offset_ms =
                (clock_offset_client_ != nullptr && clock_offset_client_->HasValidOffset())
                ? clock_offset_client_->LatestOffsetMs()
                : std::optional<double>{};
            const FrameTimingEstimate timing_estimate =
                timing_profiler_.UpdateFrame(*latest_frame_, host_result_recv_wall_ms, clock_offset_ms);
            if (timing_estimate.anchor_reset)
            {
                spdlog::warn(
                    "[警告] [时序] 锚点已重置，原因={}，累计重置次数={}",
                    timing_estimate.anchor_reset_reason,
                    timing_estimate.anchor_resets);
            }
            const TrackerDecision decision = UpdateTracker(*latest_frame_);
            latest_selection_ = decision.selection;
            status_line_ = "正常";
        }
        else
        {
            status_line_ = error_message;
            spdlog::warn("[警告] 解析结果 JSON 失败：{}", error_message);
        }

        changed = true;
    }

    if (had_recent_packet_before != HasRecentPacket())
    {
        changed = true;
    }

    if (state_changed != nullptr)
    {
        *state_changed = changed;
    }

    return changed;
}

void App::SaveJsonPacket(const UdpPacket& packet, const std::int64_t host_result_recv_wall_ms)
{
    if (!config_.save_json || !save_json_stream_.is_open())
    {
        return;
    }

    try
    {
        const json payload = json::parse(packet.payload);
        const json record = {
            { "host_recv_wall_ms", host_result_recv_wall_ms },
            { "sender_ip", packet.sender_ip },
            { "sender_port", packet.sender_port },
            { "payload", payload }
        };
        save_json_stream_ << record.dump() << '\n';
        save_json_stream_.flush();
    }
    catch (const std::exception&)
    {
        // The packet was already parsed successfully for control flow.
        // Save failure is debug-only and must not affect the main pipeline.
    }
}

bool App::CreateMainWindow()
{
    WNDCLASSEXA window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &App::WindowProc;
    window_class.hInstance = GetModuleHandleA(nullptr);
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClassName;

    if (RegisterClassExA(&window_class) == 0)
    {
        spdlog::error("[错误] 注册窗口类失败，错误码={}", GetLastError());
        return false;
    }

    RECT window_rect = { 0, 0, kClientWidth, kClientHeight };
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = CreateWindowExA(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr,
        nullptr,
        window_class.hInstance,
        this);

    if (hwnd_ == nullptr)
    {
        spdlog::error("[错误] 创建主窗口失败，错误码={}", GetLastError());
        return false;
    }

    ui_font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, kTimerId, kTimerIntervalMs, nullptr);
    return true;
}

void App::ToggleAutoMove()
{
    auto_move_enabled_ = !auto_move_enabled_;

    if (auto_move_enabled_)
    {
        last_auto_move_seq_.reset();
        last_auto_move_at_ = {};
        auto_move_enabled_at_ = Clock::now();
        wait_fresh_frame_after_enable_ = true;
    }
    else
    {
        wait_fresh_frame_after_enable_ = false;
    }

    if (hwnd_ != nullptr)
    {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void App::ApplyAutoMove()
{
    if (!auto_move_enabled_ || !HasRecentPacket() || tracker_state_ != TrackerState::kTracking ||
        !tracked_target_.has_value() || !latest_frame_.has_value() ||
        !mouse_controller_.SupportsRelativeMove())
    {
        return;
    }

    if (wait_fresh_frame_after_enable_)
    {
        if (latest_frame_received_at_.time_since_epoch().count() == 0 ||
            latest_frame_received_at_ <= auto_move_enabled_at_)
        {
            return;
        }

        wait_fresh_frame_after_enable_ = false;
    }

    const std::uint64_t move_seq = tracked_target_->last_seen_seq;
    if (last_auto_move_seq_.has_value() && *last_auto_move_seq_ == move_seq)
    {
        return;
    }

    const int aim_update_interval_ms = std::max(0, config_.aim_update_interval_ms);
    if (aim_update_interval_ms > 0 &&
        last_auto_move_at_.time_since_epoch().count() != 0)
    {
        const auto now = Clock::now();
        if ((now - last_auto_move_at_) < std::chrono::milliseconds(aim_update_interval_ms))
        {
            return;
        }
    }

    const double error_dx = static_cast<double>(tracked_target_->selection.dx);
    const double error_dy = static_cast<double>(tracked_target_->selection.dy);
    const double active_radius_px = std::max(0.0, config_.aim_active_radius_px);
    const double error_dist_sq = error_dx * error_dx + error_dy * error_dy;
    if (error_dist_sq > active_radius_px * active_radius_px)
    {
        last_auto_move_seq_ = move_seq;
        return;
    }

    if (!config_.enable_aim_motion_filters)
    {
        const double deadzone_px = std::max(0.0, config_.aim_deadzone_px);
        if (error_dist_sq <= deadzone_px * deadzone_px)
        {
            last_auto_move_seq_ = move_seq;
            return;
        }

        double move_dx = error_dx * std::max(0.0, config_.aim_gain);
        double move_dy = error_dy * std::max(0.0, config_.aim_gain);
        const double max_step_px = std::max(1.0, config_.aim_max_step_px);
        const double move_len = std::sqrt(move_dx * move_dx + move_dy * move_dy);
        if (move_len > max_step_px)
        {
            const double scale = max_step_px / std::max(move_len, 1e-6);
            move_dx *= scale;
            move_dy *= scale;
        }

        const int cmd_dx = static_cast<int>(std::lround(move_dx));
        const int cmd_dy = static_cast<int>(std::lround(move_dy));
        if (cmd_dx == 0 && cmd_dy == 0)
        {
            last_auto_move_seq_ = move_seq;
            return;
        }

        mouse_controller_.MoveRelative(cmd_dx, cmd_dy);
        last_auto_move_at_ = Clock::now();
        last_auto_move_seq_ = move_seq;
        return;
    }

    const double deadzone_px = std::max(0.0, config_.aim_deadzone_px);
    if (error_dist_sq <= deadzone_px * deadzone_px)
    {
        last_auto_move_seq_ = move_seq;
        return;
    }

    double move_dx = error_dx * config_.aim_smooth_factor;
    double move_dy = error_dy * config_.aim_smooth_factor;
    double move_len = std::sqrt(move_dx * move_dx + move_dy * move_dy);
    const double max_step_px = std::max(1.0, config_.aim_max_step_px);
    if (move_len > max_step_px)
    {
        const double scale = max_step_px / std::max(move_len, 1e-6);
        move_dx *= scale;
        move_dy *= scale;
        move_len = max_step_px;
    }

    int cmd_dx = static_cast<int>(std::lround(move_dx));
    int cmd_dy = static_cast<int>(std::lround(move_dy));
    if (cmd_dx == 0 && std::fabs(error_dx) >= 1.0)
    {
        cmd_dx = (error_dx > 0.0) ? 1 : -1;
    }
    if (cmd_dy == 0 && std::fabs(error_dy) >= 1.0)
    {
        cmd_dy = (error_dy > 0.0) ? 1 : -1;
    }

    if (cmd_dx == 0 && cmd_dy == 0)
    {
        last_auto_move_seq_ = move_seq;
        return;
    }

    mouse_controller_.MoveRelative(cmd_dx, cmd_dy);
    last_auto_move_at_ = Clock::now();
    last_auto_move_seq_ = move_seq;
}

void App::OnTimer()
{
    bool needs_repaint = false;
    ProcessIncomingPackets(&needs_repaint);
    PollSenderService();
    PollProfiling();
    ApplyAutoMove();

    if (needs_repaint && hwnd_ != nullptr)
    {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void App::OnPaint()
{
    PAINTSTRUCT paint_struct = {};
    HDC hdc = BeginPaint(hwnd_, &paint_struct);

    RECT client_rect = {};
    GetClientRect(hwnd_, &client_rect);

    HBRUSH background_brush = CreateSolidBrush(Rgb(18, 18, 22));
    FillRect(hdc, &client_rect, background_brush);
    DeleteObject(background_brush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, Rgb(232, 236, 240));
    if (ui_font_ != nullptr)
    {
        SelectObject(hdc, ui_font_);
    }

    if (!HasRecentPacket() || !latest_frame_.has_value())
    {
        DrawWaitingState(hdc, client_rect);
    }
    else
    {
        DrawFrame(hdc, client_rect);
    }

    EndPaint(hwnd_, &paint_struct);
}

void App::DrawWaitingState(HDC hdc, const RECT& client_rect) const
{
    RECT canvas_rect = GetCanvasRect(client_rect);
    HBRUSH canvas_brush = CreateSolidBrush(Rgb(26, 30, 36));
    FillRect(hdc, &canvas_rect, canvas_brush);
    DeleteObject(canvas_brush);

    HPEN border_pen = CreatePen(PS_SOLID, 1, Rgb(80, 86, 98));
    HGDIOBJ old_pen = SelectObject(hdc, border_pen);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, canvas_rect.left, canvas_rect.top, canvas_rect.right, canvas_rect.bottom);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(border_pen);

    const std::string waiting_text = has_received_packet_
        ? "最近没有新的 UDP 数据包"
        : "等待 UDP 数据包";

    RECT text_rect = canvas_rect;
    DrawTextA(hdc, waiting_text.c_str(), -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT info_rect = {
        canvas_rect.right + kMargin,
        canvas_rect.top,
        client_rect.right - kMargin,
        client_rect.bottom - kMargin
    };
    DrawInfoPanel(hdc, info_rect);
}

void App::DrawFrame(HDC hdc, const RECT& client_rect) const
{
    const FrameData& frame = *latest_frame_;
    RECT canvas_rect = GetCanvasRect(client_rect);

    HBRUSH canvas_brush = CreateSolidBrush(Rgb(24, 28, 34));
    FillRect(hdc, &canvas_rect, canvas_brush);
    DeleteObject(canvas_brush);

    HPEN border_pen = CreatePen(PS_SOLID, 1, Rgb(88, 96, 110));
    HGDIOBJ old_pen = SelectObject(hdc, border_pen);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, canvas_rect.left, canvas_rect.top, canvas_rect.right, canvas_rect.bottom);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(border_pen);

    const double scale_x = static_cast<double>(canvas_rect.right - canvas_rect.left) / static_cast<double>(frame.frame_width);
    const double scale_y = static_cast<double>(canvas_rect.bottom - canvas_rect.top) / static_cast<double>(frame.frame_height);
    const int roi_center_x = canvas_rect.left + static_cast<int>(std::lround(frame.frame_width * 0.5 * scale_x));
    const int roi_center_y = canvas_rect.top + static_cast<int>(std::lround(frame.frame_height * 0.5 * scale_y));

    HPEN center_pen = CreatePen(PS_SOLID, 1, Rgb(120, 160, 255));
    old_pen = SelectObject(hdc, center_pen);
    DrawCross(hdc, roi_center_x, roi_center_y, 8);
    SelectObject(hdc, old_pen);
    DeleteObject(center_pen);

    for (std::size_t i = 0; i < frame.boxes.size(); ++i)
    {
        const DetectionBox& box = frame.boxes[i];
        const bool is_selected = latest_selection_.has_value() && latest_selection_->box_index == i;

        const int left = canvas_rect.left + static_cast<int>(std::lround(box.x1 * scale_x));
        const int top = canvas_rect.top + static_cast<int>(std::lround(box.y1 * scale_y));
        const int right = canvas_rect.left + static_cast<int>(std::lround(box.x2 * scale_x));
        const int bottom = canvas_rect.top + static_cast<int>(std::lround(box.y2 * scale_y));
        const int center_x = canvas_rect.left + static_cast<int>(std::lround(box.cx * scale_x));
        const int center_y = canvas_rect.top + static_cast<int>(std::lround(box.cy * scale_y));

        HPEN pen = CreatePen(PS_SOLID, is_selected ? 3 : 1, is_selected ? Rgb(255, 208, 74) : Rgb(160, 210, 170));
        old_pen = SelectObject(hdc, pen);
        old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, left, top, right, bottom);
        DrawCross(hdc, center_x, center_y, is_selected ? 7 : 4);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        std::ostringstream label;
        label << "#" << i << " s=" << std::fixed << std::setprecision(2) << box.score;
        const std::string label_text = label.str();
        SetTextColor(hdc, is_selected ? Rgb(255, 220, 130) : Rgb(210, 222, 214));
        TextOutA(hdc, left, (top > canvas_rect.top + 16) ? top - 16 : top + 4, label_text.c_str(), static_cast<int>(label_text.size()));
    }

    SetTextColor(hdc, Rgb(232, 236, 240));
    RECT info_rect = {
        canvas_rect.right + kMargin,
        canvas_rect.top,
        client_rect.right - kMargin,
        client_rect.bottom - kMargin
    };
    DrawInfoPanel(hdc, info_rect);
}

void App::DrawInfoPanel(HDC hdc, const RECT& panel_rect) const
{
    HBRUSH panel_brush = CreateSolidBrush(Rgb(28, 33, 40));
    FillRect(hdc, &panel_rect, panel_brush);
    DeleteObject(panel_brush);

    HPEN panel_pen = CreatePen(PS_SOLID, 1, Rgb(76, 86, 98));
    HGDIOBJ old_pen = SelectObject(hdc, panel_pen);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, panel_rect.left, panel_rect.top, panel_rect.right, panel_rect.bottom);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(panel_pen);

    int y = panel_rect.top + 14;
    const int line_height = 22;
    const int x = panel_rect.left + 12;

    const auto write_line = [&](const std::string& line, const COLORREF color)
    {
        SetTextColor(hdc, color);
        TextOutA(hdc, x, y, line.c_str(), static_cast<int>(line.size()));
        y += line_height;
    };
    const ClockOffsetStats* clock_stats =
        (clock_offset_client_ != nullptr) ? &clock_offset_client_->LastStats() : nullptr;
    const FrameTimingEstimate& timing_estimate = timing_profiler_.LatestEstimate();
    const auto write_profiling_lines = [&]()
    {
        write_line("", Rgb(205, 214, 224));
        write_line("profiling", Rgb(236, 241, 246));
        write_line("offset_state: " + FormatClockOffsetStatus(clock_stats),
            (clock_stats != nullptr && clock_stats->status == ClockOffsetStatus::kValid)
                ? Rgb(162, 227, 184)
                : Rgb(205, 214, 224));
        if (clock_stats != nullptr)
        {
            write_line("offset_ep: " + clock_stats->remote_ip + ":" + std::to_string(clock_stats->remote_port), Rgb(205, 214, 224));
            write_line("offset_local_port: " + std::to_string(clock_stats->local_port), Rgb(205, 214, 224));
            write_line("offset_last_valid_ms: " + FormatOptionalMillis(clock_stats->last_valid_offset_ms), Rgb(205, 214, 224));
            write_line("offset_resp: " + FormatClockOffsetRespState(*clock_stats), Rgb(205, 214, 224));
            write_line("offset_resp_ep: " + clock_stats->last_response_ip + ":" + std::to_string(clock_stats->last_response_port), Rgb(205, 214, 224));
            write_line("offset_rtt_ms: " + FormatOptionalMillis(clock_stats->last_valid_delay_ms), Rgb(205, 214, 224));
        }
        write_line("latest_frame_pts: " + FormatOptionalInteger(timing_profiler_.LatestFramePts()), Rgb(205, 214, 224));
        write_line("anchor: " + std::string(timing_estimate.anchor_ready ? "ready" : "no"), Rgb(205, 214, 224));
        write_line("anchor_pts: " + FormatOptionalInteger(timing_estimate.anchor_frame_pts), Rgb(205, 214, 224));
        write_line(
            "anchor_tb: " +
                FormatOptionalTimeBase(timing_estimate.anchor_time_base_num, timing_estimate.anchor_time_base_den),
            Rgb(205, 214, 224));
        write_line("anchor_board_ms: " + FormatOptionalWallMs(timing_estimate.anchor_board_ms), Rgb(205, 214, 224));
        write_line(
            "anchor_pipe_ms: " + FormatOptionalMillis(timing_estimate.anchor_pipeline_delay_ms),
            Rgb(205, 214, 224));
        write_line("anchor_age_ms: " + FormatOptionalInteger(timing_estimate.anchor_age_ms), Rgb(205, 214, 224));
        write_line("anchor_resets: " + std::to_string(timing_estimate.anchor_resets), Rgb(205, 214, 224));
        write_line("nominal_send_ms: " + FormatOptionalInteger(timing_estimate.nominal_send_ms), Rgb(205, 214, 224));
        write_line("board_wall_ms: " + FormatOptionalInteger(timing_estimate.board_wall_infer_done_ms), Rgb(205, 214, 224));
        write_line("board_pre_ms: " + FormatOptionalMillis(timing_estimate.board_pre_ms), Rgb(205, 214, 224));
        write_line("board_infer_ms: " + FormatOptionalMillis(timing_estimate.board_infer_ms), Rgb(205, 214, 224));
        write_line("board_post_ms: " + FormatOptionalMillis(timing_estimate.board_post_ms), Rgb(205, 214, 224));
        write_line("board_send_ms: " + FormatOptionalMillis(timing_estimate.board_result_send_ms), Rgb(205, 214, 224));
        write_line(
            "est_pipe_infer_ms: " +
                FormatOptionalMillis(timing_estimate.estimated_pipeline_to_infer_done_ms),
            Rgb(205, 214, 224));
        write_line("return_ms: " + FormatOptionalMillis(timing_estimate.result_return_ms), Rgb(205, 214, 224));
    };

    write_line("remote_box_receiver", Rgb(236, 241, 246));
    write_line("listen: " + config_.listen_ip + ":" + std::to_string(config_.listen_port), Rgb(205, 214, 224));
    write_line("udp_sender: " + latest_sender_, Rgb(205, 214, 224));
    write_line("tracker_mode: " + std::string(config_.enable_tracker_state_machine ? "状态机" : "最近中心"), Rgb(205, 214, 224));
    write_line("aim_filter: " + std::string(config_.enable_aim_motion_filters ? "开" : "关"), Rgb(205, 214, 224));
    write_line("ffmpeg_sender: " + ffmpeg_sender_status_, ffmpeg_sender_status_ == "运行中" ? Rgb(162, 227, 184) : Rgb(205, 214, 224));
    write_line("status: " + status_line_, status_line_ == "正常" ? Rgb(162, 227, 184) : Rgb(255, 189, 120));
    write_line("tracker: " + std::string(TrackerStateToString(tracker_state_)),
        tracker_state_ == TrackerState::kTracking ? Rgb(162, 227, 184) :
        tracker_state_ == TrackerState::kLost ? Rgb(255, 214, 102) : Rgb(205, 214, 224));
    write_line("mouse: " + mouse_status_, Rgb(205, 214, 224));
    write_line("auto_move: " + std::string(auto_move_enabled_ ? "开" : "关"), auto_move_enabled_ ? Rgb(255, 214, 102) : Rgb(205, 214, 224));
    write_line("hotkey: " + hotkey_status_, Rgb(205, 214, 224));
    write_line("", Rgb(205, 214, 224));

    if (!latest_frame_.has_value())
    {
        write_line("seq: 无", Rgb(205, 214, 224));
        write_line("box_count: 无", Rgb(205, 214, 224));
        write_line("screen_xy: 无", Rgb(205, 214, 224));
        write_profiling_lines();
        return;
    }

    const FrameData& frame = *latest_frame_;
    write_line("seq: " + std::to_string(frame.seq), Rgb(205, 214, 224));
    write_line("frame: " + std::to_string(frame.frame_width) + "x" + std::to_string(frame.frame_height), Rgb(205, 214, 224));
    write_line("box_count: " + std::to_string(frame.box_count), Rgb(205, 214, 224));
    write_line("parsed_boxes: " + std::to_string(frame.boxes.size()), Rgb(205, 214, 224));
    write_line("roi_offset: (" + std::to_string(config_.roi_offset_x) + ", " + std::to_string(config_.roi_offset_y) + ")", Rgb(205, 214, 224));
    write_line("screen: " + std::to_string(config_.screen_width) + "x" + std::to_string(config_.screen_height), Rgb(205, 214, 224));
    write_line("screen_center: (" + std::to_string(config_.screen_width / 2) + ", " + std::to_string(config_.screen_height / 2) + ")", Rgb(205, 214, 224));
    write_line("tracking: " + std::string(tracker_state_ == TrackerState::kTracking ? "是" : "否"), Rgb(205, 214, 224));
    write_line("lost: " + std::string(tracker_state_ == TrackerState::kLost ? "是" : "否"), Rgb(205, 214, 224));
    write_line("", Rgb(205, 214, 224));

    const SelectionResult* display_target = nullptr;
    if (tracker_state_ == TrackerState::kTracking && tracked_target_.has_value())
    {
        display_target = &tracked_target_->selection;
    }
    else if (tracker_state_ == TrackerState::kCandidate && candidate_target_.has_value())
    {
        display_target = &candidate_target_->selection;
    }
    else if (tracker_state_ == TrackerState::kLost && tracked_target_.has_value())
    {
        display_target = &tracked_target_->selection;
    }

    if (display_target == nullptr)
    {
        write_line("locked target: 无", Rgb(255, 214, 102));
        write_profiling_lines();
        return;
    }

    const SelectionResult& selection = *display_target;
    write_line("locked target: #" + std::to_string(selection.box_index), Rgb(255, 214, 102));
    write_line("class_id: " + std::to_string(selection.box.class_id), Rgb(205, 214, 224));
    write_line("score: " + FormatDouble(selection.box.score), Rgb(205, 214, 224));
    write_line("total_score: " + FormatDouble(selection.total_score), Rgb(205, 214, 224));
    write_line("roi_xy: (" + std::to_string(static_cast<int>(std::lround(selection.box.cx))) + ", "
        + std::to_string(static_cast<int>(std::lround(selection.box.cy))) + ")", Rgb(205, 214, 224));
    write_line("screen_xy: (" + std::to_string(selection.screen_x) + ", " + std::to_string(selection.screen_y) + ")", Rgb(205, 214, 224));
    write_line("delta_xy: (" + std::to_string(selection.dx) + ", " + std::to_string(selection.dy) + ")", Rgb(205, 214, 224));
    write_line("center_dist_sq: " + FormatDouble(selection.distance_sq, 1), Rgb(205, 214, 224));
    write_profiling_lines();
}

void App::LogCurrentSelection() const
{
    // Runtime console output is limited to profiling summaries.
}

void App::LogTrackerStateTransition(const TrackerState previous_state, const TrackerState next_state) const
{
    (void)previous_state;
    (void)next_state;
}

bool App::ParseFrameJson(const std::string& json_text, FrameData& frame, std::string& error_message) const
{
    json root;
    try
    {
        root = json::parse(json_text);
    }
    catch (const std::exception& ex)
    {
        error_message = std::string("JSON 非法: ") + ex.what();
        return false;
    }

    if (!root.is_object())
    {
        error_message = "JSON 根节点必须是对象";
        return false;
    }

    if (!ReadRequiredUint64(root, "seq", frame.seq, error_message) ||
        !ReadRequiredInt(root, "frame_width", frame.frame_width, error_message) ||
        !ReadRequiredInt(root, "frame_height", frame.frame_height, error_message) ||
        !ReadRequiredInt(root, "box_count", frame.box_count, error_message))
    {
        return false;
    }

    ReadOptionalInt64(root, "frame_pts", frame.frame_pts);
    ReadOptionalInt64(root, "frame_best_effort_ts", frame.frame_best_effort_ts);
    ReadOptionalInt64(root, "frame_time_base_num", frame.frame_time_base_num);
    ReadOptionalInt64(root, "frame_time_base_den", frame.frame_time_base_den);

    const auto timing_it = root.find("timing");
    if (timing_it != root.end() && timing_it->is_object())
    {
        ReadOptionalInt64(*timing_it, "board_wall_infer_done_ms", frame.board_wall_infer_done_ms);
        ReadOptionalDouble(*timing_it, "board_preprocess_ms", frame.board_preprocess_ms);
        ReadOptionalDouble(*timing_it, "board_inference_ms", frame.board_inference_ms);
        ReadOptionalDouble(*timing_it, "board_postprocess_ms", frame.board_postprocess_ms);
        ReadOptionalInt64(*timing_it, "board_result_send_start_ms", frame.board_result_send_start_ms);
        ReadOptionalInt64(*timing_it, "board_result_send_end_ms", frame.board_result_send_end_ms);
        if (!frame.board_result_send_start_ms.has_value())
        {
            ReadOptionalInt64(*timing_it, "result_send_start_ms", frame.board_result_send_start_ms);
        }
        if (!frame.board_result_send_end_ms.has_value())
        {
            ReadOptionalInt64(*timing_it, "result_send_end_ms", frame.board_result_send_end_ms);
        }

        // Backward-compatible fallback for older board timing payloads.
        if (!frame.board_inference_ms.has_value())
        {
            std::optional<std::int64_t> board_infer_us;
            if (ReadOptionalInt64(*timing_it, "board_infer_us", board_infer_us))
            {
                frame.board_inference_ms = static_cast<double>(*board_infer_us) / 1000.0;
            }
        }
        if (!frame.board_postprocess_ms.has_value())
        {
            std::optional<std::int64_t> board_post_us;
            if (ReadOptionalInt64(*timing_it, "board_post_us", board_post_us))
            {
                frame.board_postprocess_ms = static_cast<double>(*board_post_us) / 1000.0;
            }
        }
    }

    const auto boxes_it = root.find("boxes");
    if (boxes_it == root.end())
    {
        error_message = "缺少数组字段: boxes";
        return false;
    }
    if (!boxes_it->is_array())
    {
        error_message = "字段不是数组: boxes";
        return false;
    }

    if (frame.frame_width <= 0 || frame.frame_height <= 0)
    {
        error_message = "frame_width 和 frame_height 必须为正数";
        return false;
    }

    frame.boxes.clear();
    frame.boxes.reserve(boxes_it->size());

    for (std::size_t i = 0; i < boxes_it->size(); ++i)
    {
        const json& box_json = (*boxes_it)[i];
        if (!box_json.is_object())
        {
            continue;
        }

        DetectionBox box;
        if (!ReadOptionalInt(box_json, "class_id", box.class_id) && box_json.contains("class_id"))
        {
        }
        if (!ReadNumber(box_json, "score", box.score) && box_json.contains("score"))
        {
        }

        if (!ReadNumber(box_json, "x1", box.x1) ||
            !ReadNumber(box_json, "y1", box.y1) ||
            !ReadNumber(box_json, "x2", box.x2) ||
            !ReadNumber(box_json, "y2", box.y2))
        {
            continue;
        }

        const bool has_cx = ReadNumber(box_json, "cx", box.cx);
        const bool has_cy = ReadNumber(box_json, "cy", box.cy);
        if (!has_cx)
        {
            box.cx = (box.x1 + box.x2) * 0.5;
        }
        if (!has_cy)
        {
            box.cy = (box.y1 + box.y2) * 0.5;
        }

        frame.boxes.push_back(box);
    }

    if (frame.box_count < 0)
    {
        error_message = "box_count 必须大于等于 0";
        return false;
    }

    if (frame.box_count != static_cast<int>(boxes_it->size()))
    {
    }

    return true;
}

TrackerDecision App::UpdateTracker(const FrameData& frame)
{
    const TrackerState previous_state = tracker_state_;
    TrackerDecision decision = {};
    decision.state = tracker_state_;

    if (!config_.enable_tracker_state_machine)
    {
        candidate_target_.reset();
        switch_candidate_.reset();
        lost_since_valid_ = false;

        const auto best = SelectNearestToCenterTarget(frame);
        if (best.has_value())
        {
            tracked_target_ = TrackingTarget{ *best, frame.seq };
            tracker_state_ = TrackerState::kTracking;
            decision.selection = tracked_target_->selection;
            decision.should_output = true;
        }
        else
        {
            tracked_target_.reset();
            latest_selection_.reset();
            tracker_state_ = TrackerState::kIdle;
        }

        decision.state = tracker_state_;
        decision.state_changed = previous_state != tracker_state_;
        if (decision.state_changed)
        {
            LogTrackerStateTransition(previous_state, tracker_state_);
        }
        return decision;
    }

    switch (tracker_state_)
    {
    case TrackerState::kIdle:
    {
        tracked_target_.reset();
        switch_candidate_.reset();
        latest_selection_.reset();

        const auto best = SelectBestTarget(frame, std::nullopt, false);
        if (best.has_value())
        {
            candidate_target_ = TargetCandidate{ *best, 1 };
            tracker_state_ = TrackerState::kCandidate;
            decision.selection = best;
        }
        else
        {
            candidate_target_.reset();
        }
        break;
    }
    case TrackerState::kCandidate:
    {
        if (!candidate_target_.has_value())
        {
            tracker_state_ = TrackerState::kIdle;
            break;
        }

        const auto continued = SelectBestTarget(frame, candidate_target_->selection, true);
        if (continued.has_value())
        {
            candidate_target_->selection = *continued;
            ++candidate_target_->confirm_frames;
            decision.selection = continued;

            if (candidate_target_->confirm_frames >= config_.acquire_confirm_frames)
            {
                tracked_target_ = TrackingTarget{ candidate_target_->selection, frame.seq };
                tracker_state_ = TrackerState::kTracking;
                candidate_target_.reset();
                decision.selection = tracked_target_->selection;
                decision.should_output = true;
            }
            break;
        }

        const auto replacement = SelectBestTarget(frame, std::nullopt, false);
        if (replacement.has_value())
        {
            candidate_target_ = TargetCandidate{ *replacement, 1 };
            decision.selection = replacement;
        }
        else
        {
            candidate_target_.reset();
            tracker_state_ = TrackerState::kIdle;
        }
        break;
    }
    case TrackerState::kTracking:
    {
        if (!tracked_target_.has_value())
        {
            tracker_state_ = TrackerState::kIdle;
            break;
        }

        const auto continued = SelectBestTarget(frame, tracked_target_->selection, true);
        const auto best_overall = SelectBestTarget(frame, std::nullopt, false);

        if (!continued.has_value())
        {
            tracker_state_ = TrackerState::kLost;
            lost_since_ = Clock::now();
            lost_since_valid_ = true;
            latest_selection_.reset();
            switch_candidate_.reset();
            break;
        }

        tracked_target_->selection = *continued;
        tracked_target_->last_seen_seq = frame.seq;
        decision.selection = tracked_target_->selection;
        decision.should_output = true;

        if (best_overall.has_value() &&
            !IsSameTarget(*best_overall, tracked_target_->selection) &&
            best_overall->total_score > tracked_target_->selection.total_score + config_.switch_margin)
        {
            if (switch_candidate_.has_value() &&
                IsSameTarget(switch_candidate_->selection, *best_overall))
            {
                switch_candidate_->selection = *best_overall;
                ++switch_candidate_->confirm_frames;
            }
            else
            {
                switch_candidate_ = TargetCandidate{ *best_overall, 1 };
            }

            if (switch_candidate_->confirm_frames >= config_.switch_confirm_frames)
            {
                tracked_target_->selection = switch_candidate_->selection;
                tracked_target_->last_seen_seq = frame.seq;
                decision.selection = tracked_target_->selection;
                decision.target_switched = true;
                switch_candidate_.reset();
            }
        }
        else
        {
            switch_candidate_.reset();
        }
        break;
    }
    case TrackerState::kLost:
    {
        if (!tracked_target_.has_value())
        {
            tracker_state_ = TrackerState::kIdle;
            lost_since_valid_ = false;
            break;
        }

        const auto reacquired = SelectBestTarget(frame, tracked_target_->selection, true);
        if (reacquired.has_value())
        {
            tracked_target_->selection = *reacquired;
            tracked_target_->last_seen_seq = frame.seq;
            tracker_state_ = TrackerState::kTracking;
            lost_since_valid_ = false;
            switch_candidate_.reset();
            decision.selection = tracked_target_->selection;
            decision.should_output = true;
            break;
        }

        if (lost_since_valid_ &&
            (Clock::now() - lost_since_) > std::chrono::milliseconds(config_.lost_timeout_ms))
        {
            tracker_state_ = TrackerState::kIdle;
            tracked_target_.reset();
            candidate_target_.reset();
            switch_candidate_.reset();
            lost_since_valid_ = false;
        }
        break;
    }
    default:
        tracker_state_ = TrackerState::kIdle;
        break;
    }

    decision.state = tracker_state_;
    decision.state_changed = previous_state != tracker_state_;

    if (decision.state_changed)
    {
        LogTrackerStateTransition(previous_state, tracker_state_);
    }
    return decision;
}

std::optional<SelectionResult> App::SelectBestTarget(
    const FrameData& frame,
    const std::optional<SelectionResult>& continuity_reference,
    const bool require_within_gate) const
{
    std::optional<SelectionResult> best;

    for (std::size_t i = 0; i < frame.boxes.size(); ++i)
    {
        const DetectionBox& box = frame.boxes[i];
        if (box.score < config_.min_score)
        {
            continue;
        }
        if (require_within_gate && continuity_reference.has_value() &&
            !IsWithinContinueGate(box, *continuity_reference))
        {
            continue;
        }

        SelectionResult candidate = BuildSelectionResult(frame, i, box, continuity_reference);
        if (!best.has_value() || candidate.total_score > best->total_score)
        {
            best = std::move(candidate);
        }
    }

    return best;
}

std::optional<SelectionResult> App::SelectNearestToCenterTarget(const FrameData& frame) const
{
    std::optional<SelectionResult> best;

    for (std::size_t i = 0; i < frame.boxes.size(); ++i)
    {
        const DetectionBox& box = frame.boxes[i];
        if (box.score < config_.min_score)
        {
            continue;
        }

        SelectionResult candidate = BuildSelectionResult(frame, i, box, std::nullopt);
        if (!best.has_value() ||
            candidate.distance_sq < best->distance_sq ||
            (candidate.distance_sq == best->distance_sq && candidate.box.score > best->box.score))
        {
            best = std::move(candidate);
        }
    }

    return best;
}

SelectionResult App::BuildSelectionResult(
    const FrameData& frame,
    const std::size_t box_index,
    const DetectionBox& box,
    const std::optional<SelectionResult>& continuity_reference) const
{
    const double center_x = static_cast<double>(frame.frame_width) * 0.5;
    const double center_y = static_cast<double>(frame.frame_height) * 0.5;
    const double dx_center = box.cx - center_x;
    const double dy_center = box.cy - center_y;
    const double center_distance = std::sqrt(dx_center * dx_center + dy_center * dy_center);
    const double max_center_distance = std::sqrt(center_x * center_x + center_y * center_y);

    double continuity_score = 0.0;
    if (continuity_reference.has_value())
    {
        const double dx_prev = box.cx - continuity_reference->box.cx;
        const double dy_prev = box.cy - continuity_reference->box.cy;
        const double distance_prev = std::sqrt(dx_prev * dx_prev + dy_prev * dy_prev);
        continuity_score = 1.0 - Clamp01(distance_prev / std::max(config_.continue_gate_radius, 1.0));
    }

    SelectionResult selection = {};
    selection.box_index = box_index;
    selection.box = box;
    selection.distance_sq = dx_center * dx_center + dy_center * dy_center;
    selection.center_score = 1.0 - Clamp01(center_distance / std::max(max_center_distance, 1.0));
    selection.confidence_score = Clamp01(box.score);
    selection.continuity_score = continuity_score;
    selection.total_score =
        config_.weight_center * selection.center_score +
        config_.weight_confidence * selection.confidence_score +
        config_.weight_continuity * selection.continuity_score;

    // Board result coordinates are in frame space. Convert them back into the configured ROI space
    // before generating the relative MoveR command, otherwise frame/ROI size mismatch skews movement.
    const double roi_scale_x =
        static_cast<double>(config_.roi_width) / std::max(frame.frame_width, 1);
    const double roi_scale_y =
        static_cast<double>(config_.roi_height) / std::max(frame.frame_height, 1);
    const int roi_x = static_cast<int>(std::lround(box.cx * roi_scale_x));
    const int roi_y = static_cast<int>(std::lround(box.cy * roi_scale_y));

    selection.screen_x = config_.roi_offset_x + roi_x;
    selection.screen_y = config_.roi_offset_y + roi_y;
    selection.dx = selection.screen_x - config_.screen_width / 2;
    selection.dy = selection.screen_y - config_.screen_height / 2;
    return selection;
}

bool App::IsWithinContinueGate(const DetectionBox& box, const SelectionResult& reference) const
{
    const double dx = box.cx - reference.box.cx;
    const double dy = box.cy - reference.box.cy;
    const double distance = std::sqrt(dx * dx + dy * dy);
    return distance <= config_.continue_gate_radius;
}

bool App::IsSameTarget(const SelectionResult& lhs, const SelectionResult& rhs) const
{
    const double dx = lhs.box.cx - rhs.box.cx;
    const double dy = lhs.box.cy - rhs.box.cy;
    const double distance = std::sqrt(dx * dx + dy * dy);
    return distance <= config_.continue_gate_radius;
}

bool App::HasRecentPacket() const
{
    if (!has_received_packet_)
    {
        return false;
    }

    return (Clock::now() - last_packet_at_) <= kStaleTimeout;
}

RECT App::GetCanvasRect(const RECT& client_rect) const
{
    RECT canvas_rect = {
        kMargin,
        kMargin,
        kMargin + kCanvasSize,
        kMargin + kCanvasSize
    };

    const int max_right = client_rect.right - kMargin - kInfoPanelWidth - kMargin;
    if (canvas_rect.right > max_right)
    {
        canvas_rect.right = max_right;
    }

    const int max_bottom = client_rect.bottom - kMargin;
    if (canvas_rect.bottom > max_bottom)
    {
        canvas_rect.bottom = max_bottom;
    }

    if (canvas_rect.right <= canvas_rect.left)
    {
        canvas_rect.right = canvas_rect.left + 1;
    }
    if (canvas_rect.bottom <= canvas_rect.top)
    {
        canvas_rect.bottom = canvas_rect.top + 1;
    }

    return canvas_rect;
}

LRESULT CALLBACK App::WindowProc(HWND hwnd, const UINT message, const WPARAM w_param, const LPARAM l_param)
{
    App* app = nullptr;

    if (message == WM_NCCREATE)
    {
        const auto* create_struct = reinterpret_cast<const CREATESTRUCTA*>(l_param);
        app = static_cast<App*>(create_struct->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<App*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }

    if (app != nullptr)
    {
        return app->HandleMessage(hwnd, message, w_param, l_param);
    }

    return DefWindowProcA(hwnd, message, w_param, l_param);
}

LRESULT App::HandleMessage(HWND hwnd, const UINT message, const WPARAM w_param, const LPARAM l_param)
{
    switch (message)
    {
    case WM_HOTKEY:
        if (w_param == kToggleAutoMoveHotkeyId)
        {
            ToggleAutoMove();
            return 0;
        }
        break;
    case WM_TIMER:
        if (w_param == kTimerId)
        {
            OnTimer();
            return 0;
        }
        break;
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        UnregisterToggleHotkey();
        KillTimer(hwnd, kTimerId);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcA(hwnd, message, w_param, l_param);
}
