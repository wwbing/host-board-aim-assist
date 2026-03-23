#include "profiling/timing_profiler.hpp"

#include <cmath>

namespace
{

constexpr std::int64_t kStreamGapResetMs = 500;
constexpr double kPtsDiscontinuityResetMs = 2000.0;

bool HasValidTimeBase(const FrameData& frame)
{
    return frame.frame_pts.has_value() &&
        frame.frame_time_base_num.has_value() &&
        frame.frame_time_base_den.has_value() &&
        *frame.frame_time_base_num > 0 &&
        *frame.frame_time_base_den > 0;
}

std::optional<double> ComputeAdjustedBoardInferDoneMs(
    const FrameData& frame,
    const std::optional<double>& clock_offset_ms)
{
    if (!frame.board_wall_infer_done_ms.has_value() || !clock_offset_ms.has_value())
    {
        return std::nullopt;
    }

    return static_cast<double>(*frame.board_wall_infer_done_ms) - *clock_offset_ms;
}

std::optional<double> ComputeBoardPipelineDelayMs(const FrameData& frame)
{
    if (!frame.board_preprocess_ms.has_value() ||
        !frame.board_inference_ms.has_value() ||
        !frame.board_postprocess_ms.has_value())
    {
        return std::nullopt;
    }

    return *frame.board_preprocess_ms + *frame.board_inference_ms + *frame.board_postprocess_ms;
}

bool HasAnchor(const SenderTimingContext& context)
{
    return context.anchor_frame_pts.has_value() &&
        context.anchor_time_base_num.has_value() &&
        context.anchor_time_base_den.has_value() &&
        context.anchor_adjusted_board_infer_done_ms.has_value() &&
        context.anchor_pipeline_delay_ms.has_value() &&
        context.anchor_established_host_wall_ms.has_value();
}

void ClearAnchorState(SenderTimingContext& context)
{
    context.anchor_frame_pts.reset();
    context.anchor_time_base_num.reset();
    context.anchor_time_base_den.reset();
    context.anchor_adjusted_board_infer_done_ms.reset();
    context.anchor_pipeline_delay_ms.reset();
    context.anchor_established_host_wall_ms.reset();
    context.anchor_seq.reset();
}

double ComputeDeltaMs(
    const std::int64_t frame_pts,
    const std::int64_t reference_frame_pts,
    const std::int64_t time_base_num,
    const std::int64_t time_base_den)
{
    const double delta_pts = static_cast<double>(frame_pts - reference_frame_pts);
    return delta_pts * static_cast<double>(time_base_num) * 1000.0 /
        static_cast<double>(time_base_den);
}

} // namespace

void TimingProfiler::ResetAnchor(const char* reason)
{
    if (HasAnchor(sender_timing_))
    {
        ++anchor_reset_count_;
    }
    ClearAnchorState(sender_timing_);
    last_anchor_reset_reason_ = reason;
}

void TimingProfiler::SetSenderTimingContext(const SenderTimingContext& context)
{
    const std::optional<std::int64_t> preserved_anchor_frame_pts = sender_timing_.anchor_frame_pts;
    const std::optional<std::int64_t> preserved_anchor_time_base_num = sender_timing_.anchor_time_base_num;
    const std::optional<std::int64_t> preserved_anchor_time_base_den = sender_timing_.anchor_time_base_den;
    const std::optional<double> preserved_anchor_adjusted_board_infer_done_ms =
        sender_timing_.anchor_adjusted_board_infer_done_ms;
    const std::optional<double> preserved_anchor_pipeline_delay_ms =
        sender_timing_.anchor_pipeline_delay_ms;
    const std::optional<std::int64_t> preserved_anchor_established_host_wall_ms =
        sender_timing_.anchor_established_host_wall_ms;
    const std::optional<std::uint64_t> preserved_anchor_seq = sender_timing_.anchor_seq;
    const bool sender_restarted =
        sender_timing_.sender_start_wall_ns.has_value() &&
        context.sender_start_wall_ns.has_value() &&
        sender_timing_.sender_start_wall_ns != context.sender_start_wall_ns;

    sender_timing_ = context;
    if (sender_restarted)
    {
        ResetAnchor("sender_restart");
        last_frame_pts_.reset();
        last_frame_host_recv_wall_ms_.reset();
    }
    else
    {
        sender_timing_.anchor_frame_pts = preserved_anchor_frame_pts;
        sender_timing_.anchor_time_base_num = preserved_anchor_time_base_num;
        sender_timing_.anchor_time_base_den = preserved_anchor_time_base_den;
        sender_timing_.anchor_adjusted_board_infer_done_ms =
            preserved_anchor_adjusted_board_infer_done_ms;
        sender_timing_.anchor_pipeline_delay_ms = preserved_anchor_pipeline_delay_ms;
        sender_timing_.anchor_established_host_wall_ms = preserved_anchor_established_host_wall_ms;
        sender_timing_.anchor_seq = preserved_anchor_seq;
    }
}

void TimingProfiler::SetSenderRunning(const bool sender_running)
{
    sender_running_ = sender_running;
}

FrameTimingEstimate TimingProfiler::UpdateFrame(
    const FrameData& frame,
    const std::int64_t host_result_recv_wall_ms,
    const std::optional<double>& clock_offset_ms)
{
    latest_seq_ = frame.seq;
    latest_frame_pts_ = frame.frame_pts;
    latest_estimate_ = {};
    latest_estimate_.host_result_recv_wall_ms = host_result_recv_wall_ms;
    latest_estimate_.board_wall_infer_done_ms = frame.board_wall_infer_done_ms;
    latest_estimate_.anchor_resets = anchor_reset_count_;
    const std::optional<double> adjusted_board_infer_done_ms =
        ComputeAdjustedBoardInferDoneMs(frame, clock_offset_ms);
    const std::optional<double> board_pipeline_delay_ms = ComputeBoardPipelineDelayMs(frame);

    if (frame.board_preprocess_ms.has_value())
    {
        latest_estimate_.board_pre_ms = *frame.board_preprocess_ms;
    }
    if (frame.board_inference_ms.has_value())
    {
        latest_estimate_.board_infer_ms = *frame.board_inference_ms;
    }
    if (frame.board_postprocess_ms.has_value())
    {
        latest_estimate_.board_post_ms = *frame.board_postprocess_ms;
    }
    if (frame.board_result_send_start_ms.has_value() &&
        frame.board_result_send_end_ms.has_value())
    {
        const std::int64_t board_result_send_delta_ms =
            *frame.board_result_send_end_ms - *frame.board_result_send_start_ms;
        if (board_result_send_delta_ms >= 0)
        {
            latest_estimate_.board_result_send_ms = static_cast<double>(board_result_send_delta_ms);
        }
    }

    const bool has_valid_time_base = HasValidTimeBase(frame);

    if (HasAnchor(sender_timing_) && has_valid_time_base &&
        (*frame.frame_time_base_num != *sender_timing_.anchor_time_base_num ||
         *frame.frame_time_base_den != *sender_timing_.anchor_time_base_den))
    {
        ResetAnchor("time_base_changed");
    }

    if (HasAnchor(sender_timing_) &&
        frame.frame_pts.has_value() &&
        last_frame_pts_.has_value() &&
        *frame.frame_pts < *last_frame_pts_)
    {
        ResetAnchor("pts_regression");
    }

    if (HasAnchor(sender_timing_) &&
        last_frame_host_recv_wall_ms_.has_value() &&
        (host_result_recv_wall_ms - *last_frame_host_recv_wall_ms_) > kStreamGapResetMs)
    {
        ResetAnchor("stream_gap");
    }

    if (HasAnchor(sender_timing_) &&
        has_valid_time_base &&
        last_frame_pts_.has_value())
    {
        const double delta_ms =
            ComputeDeltaMs(*frame.frame_pts, *last_frame_pts_, *frame.frame_time_base_num, *frame.frame_time_base_den);
        if (delta_ms > kPtsDiscontinuityResetMs)
        {
            ResetAnchor("pts_discontinuity");
        }
    }

    if (!HasAnchor(sender_timing_) &&
        has_valid_time_base &&
        adjusted_board_infer_done_ms.has_value() &&
        board_pipeline_delay_ms.has_value())
    {
        sender_timing_.anchor_frame_pts = frame.frame_pts;
        sender_timing_.anchor_time_base_num = frame.frame_time_base_num;
        sender_timing_.anchor_time_base_den = frame.frame_time_base_den;
        sender_timing_.anchor_adjusted_board_infer_done_ms = adjusted_board_infer_done_ms;
        sender_timing_.anchor_pipeline_delay_ms = board_pipeline_delay_ms;
        sender_timing_.anchor_established_host_wall_ms = host_result_recv_wall_ms;
        sender_timing_.anchor_seq = frame.seq;
    }

    latest_estimate_.anchor_ready = HasAnchor(sender_timing_);
    latest_estimate_.anchor_frame_pts = sender_timing_.anchor_frame_pts;
    latest_estimate_.anchor_time_base_num = sender_timing_.anchor_time_base_num;
    latest_estimate_.anchor_time_base_den = sender_timing_.anchor_time_base_den;
    latest_estimate_.anchor_board_ms = sender_timing_.anchor_adjusted_board_infer_done_ms;
    latest_estimate_.anchor_pipeline_delay_ms = sender_timing_.anchor_pipeline_delay_ms;
    latest_estimate_.anchor_seq = sender_timing_.anchor_seq;
    latest_estimate_.anchor_resets = anchor_reset_count_;
    if (!last_anchor_reset_reason_.empty())
    {
        latest_estimate_.anchor_reset = true;
        latest_estimate_.anchor_reset_reason = last_anchor_reset_reason_;
        last_anchor_reset_reason_.clear();
    }
    if (sender_timing_.anchor_established_host_wall_ms.has_value())
    {
        latest_estimate_.anchor_age_ms =
            host_result_recv_wall_ms - *sender_timing_.anchor_established_host_wall_ms;
    }

    if (latest_estimate_.anchor_ready && has_valid_time_base)
    {
        const double delta_ms =
            ComputeDeltaMs(
                *frame.frame_pts,
                *sender_timing_.anchor_frame_pts,
                *sender_timing_.anchor_time_base_num,
                *sender_timing_.anchor_time_base_den);
        // This anchor is derived from board-timed infer completion minus board-side pipeline time.
        // It is useful for relative profiling, but it is not the true sender wall-clock emission time.
        const double anchor_nominal_send_ms =
            *sender_timing_.anchor_adjusted_board_infer_done_ms -
            *sender_timing_.anchor_pipeline_delay_ms;
        latest_estimate_.nominal_send_ms =
            static_cast<std::int64_t>(std::llround(anchor_nominal_send_ms + delta_ms));
    }

    if (adjusted_board_infer_done_ms.has_value())
    {
        if (latest_estimate_.nominal_send_ms.has_value())
        {
            latest_estimate_.estimated_pipeline_to_infer_done_ms =
                *adjusted_board_infer_done_ms - static_cast<double>(*latest_estimate_.nominal_send_ms);
        }

        if (latest_estimate_.host_result_recv_wall_ms.has_value())
        {
            latest_estimate_.result_return_ms =
                static_cast<double>(*latest_estimate_.host_result_recv_wall_ms) -
                *adjusted_board_infer_done_ms;
        }
    }

    last_frame_pts_ = frame.frame_pts;
    last_frame_host_recv_wall_ms_ = host_result_recv_wall_ms;

    return latest_estimate_;
}

const SenderTimingContext& TimingProfiler::SenderTiming() const
{
    return sender_timing_;
}

const FrameTimingEstimate& TimingProfiler::LatestEstimate() const
{
    return latest_estimate_;
}

bool TimingProfiler::SenderRunning() const
{
    return sender_running_;
}

std::optional<std::uint64_t> TimingProfiler::LatestSeq() const
{
    return latest_seq_;
}

std::optional<std::int64_t> TimingProfiler::LatestFramePts() const
{
    return latest_frame_pts_;
}
