#pragma once

#include "mouse_driver.hpp"
#include "profiling/timing_profiler.hpp"
#include "types.hpp"
#include "udp_receiver.hpp"

#include <Windows.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

class FfmpegSenderService;
class ClockOffsetClient;

class App
{
public:
    explicit App(Config config);
    ~App();
    int Run();
    std::optional<TargetPoint> GetLatestTargetPoint() const;
    std::optional<TargetOffset> GetLatestTargetOffset() const;

private:
    bool InitializeNetworking();
    bool InitializeMouseController();
    void InitializeJsonSaver();
    void InitializeSenderService();
    void ShutdownSenderService();
    void PollSenderService();
    void InitializeProfiling();
    void PollProfiling();
    void MaybeLogProfilingSummary();
    bool RegisterToggleHotkey(HWND target_window);
    void UnregisterToggleHotkey();
    bool CreateMainWindow();
    int RunConsoleLoop();
    void ProcessPendingThreadMessages();
    bool ProcessIncomingPackets(bool* state_changed = nullptr);
    void SaveJsonPacket(const UdpPacket& packet, std::int64_t host_result_recv_wall_ms);
    void ToggleAutoMove();
    void ApplyAutoMove();
    void OnTimer();
    void OnPaint();
    void DrawWaitingState(HDC hdc, const RECT& client_rect) const;
    void DrawFrame(HDC hdc, const RECT& client_rect) const;
    void DrawInfoPanel(HDC hdc, const RECT& panel_rect) const;
    void LogCurrentSelection() const;
    void LogTrackerStateTransition(TrackerState previous_state, TrackerState next_state) const;
    bool ParseFrameJson(const std::string& json_text, FrameData& frame, std::string& error_message) const;
    TrackerDecision UpdateTracker(const FrameData& frame);
    std::optional<SelectionResult> SelectBestTarget(
        const FrameData& frame,
        const std::optional<SelectionResult>& continuity_reference,
        bool require_within_gate) const;
    std::optional<SelectionResult> SelectNearestToCenterTarget(const FrameData& frame) const;
    SelectionResult BuildSelectionResult(
        const FrameData& frame,
        std::size_t box_index,
        const DetectionBox& box,
        const std::optional<SelectionResult>& continuity_reference) const;
    bool IsWithinContinueGate(const DetectionBox& box, const SelectionResult& reference) const;
    bool IsSameTarget(const SelectionResult& lhs, const SelectionResult& rhs) const;
    bool HasRecentPacket() const;
    RECT GetCanvasRect(const RECT& client_rect) const;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);

private:
    Config config_;
    UdpReceiver receiver_;
    MouseController mouse_controller_;
    std::unique_ptr<FfmpegSenderService> sender_;
    std::unique_ptr<ClockOffsetClient> clock_offset_client_;
    HWND hwnd_ = nullptr;
    HFONT ui_font_ = nullptr;

    std::optional<FrameData> latest_frame_;
    std::optional<SelectionResult> latest_selection_;
    std::optional<TargetCandidate> candidate_target_;
    std::optional<TargetCandidate> switch_candidate_;
    std::optional<TrackingTarget> tracked_target_;
    std::ofstream save_json_stream_;

    std::string latest_sender_ = "无";
    std::string status_line_ = "等待 UDP 数据包";
    std::string mouse_status_ = "DLL 相对移动";
    std::string ffmpeg_sender_status_ = "已禁用";
    std::string save_json_status_ = "关闭";
    std::string hotkey_status_ = "Q";
    std::chrono::steady_clock::time_point last_packet_at_ = {};
    std::chrono::steady_clock::time_point lost_since_ = {};
    std::chrono::steady_clock::time_point last_clock_offset_request_at_ = {};
    std::chrono::steady_clock::time_point last_profiling_log_at_ = {};
    std::chrono::steady_clock::time_point last_auto_move_at_ = {};
    std::chrono::steady_clock::time_point latest_frame_received_at_ = {};
    std::chrono::steady_clock::time_point auto_move_enabled_at_ = {};
    std::optional<std::uint64_t> last_auto_move_seq_;
    bool has_received_packet_ = false;
    bool auto_move_enabled_ = false;
    bool wait_fresh_frame_after_enable_ = false;
    bool hotkey_registered_ = false;
    bool lost_since_valid_ = false;
    bool sender_exit_reported_ = false;
    HWND hotkey_target_ = nullptr;
    TrackerState tracker_state_ = TrackerState::kIdle;
    TimingProfiler timing_profiler_;
};
