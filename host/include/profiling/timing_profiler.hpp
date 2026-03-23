#pragma once

#include "types.hpp"

#include <cstdint>
#include <optional>
#include <string>

class TimingProfiler
{
public:
    void SetSenderTimingContext(const SenderTimingContext& context);
    void SetSenderRunning(bool sender_running);
    FrameTimingEstimate UpdateFrame(
        const FrameData& frame,
        std::int64_t host_result_recv_wall_ms,
        const std::optional<double>& clock_offset_ms);

    const SenderTimingContext& SenderTiming() const;
    const FrameTimingEstimate& LatestEstimate() const;
    bool SenderRunning() const;
    std::optional<std::uint64_t> LatestSeq() const;
    std::optional<std::int64_t> LatestFramePts() const;

private:
    void ResetAnchor(const char* reason);

    SenderTimingContext sender_timing_;
    FrameTimingEstimate latest_estimate_;
    bool sender_running_ = false;
    std::optional<std::uint64_t> latest_seq_;
    std::optional<std::int64_t> latest_frame_pts_;
    std::optional<std::int64_t> last_frame_pts_;
    std::optional<std::int64_t> last_frame_host_recv_wall_ms_;
    int anchor_reset_count_ = 0;
    std::string last_anchor_reset_reason_;
};
