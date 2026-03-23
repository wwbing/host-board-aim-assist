#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct Config
{
    std::string listen_ip = "0.0.0.0";
    int listen_port = 6000;
    bool enable_display = false;
    bool save_json = false;
    bool enable_sender = true;
    bool enable_clock_offset = true;
    bool enable_tracker_state_machine = false;
    bool enable_aim_motion_filters = false;
    double aim_active_radius_px = 100.0;
    double aim_gain = 0.35;
    int screen_width = 0;
    int screen_height = 0;
    int roi_width = 640;
    int roi_height = 640;
    int roi_offset_x = 0;
    int roi_offset_y = 0;
    std::string ffmpeg_path;
    std::string sender_output_ip = "192.168.7.2";
    int sender_output_port = 5000;
    int sender_output_idx = 0;
    int sender_framerate = 60;
    int sender_crop_width = 640;
    int sender_crop_height = 640;
    int sender_offset_x = 0;
    int sender_offset_y = 0;
    std::string sender_bitrate = "4M";
    std::string sender_maxrate = "4M";
    std::string sender_bufsize = "150k";
    int sender_gop = 10;
    int sender_pkt_size = 188;
    int sender_udp_buffer_size = 1048576;
    std::string clock_offset_ip = "192.168.7.2";
    int clock_offset_port = 45678;
    int clock_offset_interval_ms = 1000;
    int clock_offset_timeout_ms = 250;
    double sender_pts_per_second = 90000.0;
    int profiling_log_interval_ms = 1000;
    std::string save_json_path = "saved_udp_packets.jsonl";
    double aim_smooth_factor = 1.0;
    double aim_max_step_px = 35.0;
    double aim_deadzone_px = 1.0;
    int aim_update_interval_ms = 0;
    double min_score = 0.45;
    int acquire_confirm_frames = 2;
    int lost_timeout_ms = 80;
    double continue_gate_radius = 120.0;
    double switch_margin = 0.15;
    int switch_confirm_frames = 2;
    double weight_center = 0.45;
    double weight_confidence = 0.20;
    double weight_continuity = 0.35;
    bool screen_width_overridden = false;
    bool screen_height_overridden = false;
    bool roi_offset_x_overridden = false;
    bool roi_offset_y_overridden = false;
    bool sender_crop_width_overridden = false;
    bool sender_crop_height_overridden = false;
    bool sender_offset_x_overridden = false;
    bool sender_offset_y_overridden = false;
};

struct DetectionBox
{
    int class_id = -1;
    double score = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    double cx = 0.0;
    double cy = 0.0;
};

struct FrameData
{
    std::uint64_t seq = 0;
    std::optional<std::int64_t> frame_pts;
    std::optional<std::int64_t> frame_best_effort_ts;
    std::optional<std::int64_t> frame_time_base_num;
    std::optional<std::int64_t> frame_time_base_den;
    int frame_width = 640;
    int frame_height = 640;
    int box_count = 0;
    std::vector<DetectionBox> boxes;
    std::optional<std::int64_t> board_wall_infer_done_ms;
    std::optional<double> board_preprocess_ms;
    std::optional<double> board_inference_ms;
    std::optional<double> board_postprocess_ms;
    std::optional<std::int64_t> board_result_send_start_ms;
    std::optional<std::int64_t> board_result_send_end_ms;
};

struct SenderTimingContext
{
    std::optional<std::int64_t> sender_start_wall_ms;
    std::optional<std::int64_t> sender_start_wall_ns;
    int sender_framerate = 0;
    int roi_width = 0;
    int roi_height = 0;
    int offset_x = 0;
    int offset_y = 0;
    double pts_per_second = 90000.0;
    std::optional<std::int64_t> anchor_frame_pts;
    std::optional<std::int64_t> anchor_time_base_num;
    std::optional<std::int64_t> anchor_time_base_den;
    std::optional<double> anchor_adjusted_board_infer_done_ms;
    std::optional<double> anchor_pipeline_delay_ms;
    std::optional<std::int64_t> anchor_established_host_wall_ms;
    std::optional<std::uint64_t> anchor_seq;
};

struct FrameTimingEstimate
{
    std::optional<std::int64_t> host_result_recv_wall_ms;
    std::optional<std::int64_t> nominal_send_ms;
    std::optional<std::int64_t> board_wall_infer_done_ms;
    bool anchor_ready = false;
    std::optional<std::int64_t> anchor_frame_pts;
    std::optional<std::int64_t> anchor_time_base_num;
    std::optional<std::int64_t> anchor_time_base_den;
    std::optional<double> anchor_board_ms;
    std::optional<double> anchor_pipeline_delay_ms;
    std::optional<std::int64_t> anchor_age_ms;
    std::optional<std::uint64_t> anchor_seq;
    int anchor_resets = 0;
    bool anchor_reset = false;
    std::string anchor_reset_reason;
    // Estimated using frame_pts plus a board-timed anchor. This is not a true sender wall-time E2E metric.
    std::optional<double> estimated_pipeline_to_infer_done_ms;
    std::optional<double> result_return_ms;
    std::optional<double> board_pre_ms;
    std::optional<double> board_infer_ms;
    std::optional<double> board_post_ms;
    std::optional<double> board_result_send_ms;
};

struct SelectionResult
{
    std::size_t box_index = 0;
    DetectionBox box;
    double distance_sq = 0.0;
    double center_score = 0.0;
    double confidence_score = 0.0;
    double continuity_score = 0.0;
    double total_score = 0.0;
    int screen_x = 0;
    int screen_y = 0;
    int dx = 0;
    int dy = 0;
};

enum class TrackerState
{
    kIdle,
    kCandidate,
    kTracking,
    kLost
};

struct TargetCandidate
{
    SelectionResult selection;
    int confirm_frames = 0;
};

struct TrackingTarget
{
    SelectionResult selection;
    std::uint64_t last_seen_seq = 0;
};

struct TrackerDecision
{
    TrackerState state = TrackerState::kIdle;
    std::optional<SelectionResult> selection;
    bool should_output = false;
    bool state_changed = false;
    bool target_switched = false;
};

struct TargetPoint
{
    int screen_x = 0;
    int screen_y = 0;
};

struct TargetOffset
{
    int dx = 0;
    int dy = 0;
};

struct UdpPacket
{
    std::string payload;
    std::string sender_ip;
    std::uint16_t sender_port = 0;
};
