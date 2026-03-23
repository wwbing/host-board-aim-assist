#include "sender/ffmpeg_sender_service.hpp"

#include "sender/ffmpeg_command_builder.hpp"

#include <chrono>
#include <sstream>
#include <utility>

namespace
{

std::int64_t CurrentWallTimeNs()
{
    const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now());
    return now.time_since_epoch().count();
}

} // namespace

FfmpegSenderService::FfmpegSenderService(FfmpegSenderOptions options)
    : options_(std::move(options))
{
    timing_context_.sender_framerate = options_.framerate;
    timing_context_.roi_width = options_.crop_width;
    timing_context_.roi_height = options_.crop_height;
    timing_context_.offset_x = options_.offset_x;
    timing_context_.offset_y = options_.offset_y;
    timing_context_.pts_per_second = options_.pts_per_second;
}

FfmpegSenderService::~FfmpegSenderService()
{
    Stop();
}

bool FfmpegSenderService::Start()
{
    command_line_ = BuildFfmpegCommandLine(options_);
    last_error_.clear();

    if (!process_.Start(command_line_, last_error_))
    {
        return false;
    }

    const std::int64_t wall_ns = CurrentWallTimeNs();
    timing_context_.sender_start_wall_ns = wall_ns;
    timing_context_.sender_start_wall_ms = wall_ns / 1000000;
    timing_context_.anchor_frame_pts.reset();
    timing_context_.anchor_time_base_num.reset();
    timing_context_.anchor_time_base_den.reset();
    timing_context_.anchor_adjusted_board_infer_done_ms.reset();
    timing_context_.anchor_pipeline_delay_ms.reset();
    return true;
}

void FfmpegSenderService::Stop()
{
    process_.Stop();
    if (last_error_.empty() && !process_.LastError().empty())
    {
        last_error_ = process_.LastError();
    }
}

bool FfmpegSenderService::IsRunning()
{
    const bool running = process_.IsRunning();
    if (!running && last_error_.empty() && !command_line_.empty())
    {
        std::ostringstream oss;
        oss << "ffmpeg 已退出，退出码=" << process_.ExitCode();
        last_error_ = oss.str();
    }
    return running;
}

DWORD FfmpegSenderService::ExitCode()
{
    return process_.ExitCode();
}

const std::string& FfmpegSenderService::LastError() const
{
    if (!last_error_.empty())
    {
        return last_error_;
    }

    return process_.LastError();
}

const std::wstring& FfmpegSenderService::CommandLine() const
{
    return command_line_;
}

const FfmpegSenderOptions& FfmpegSenderService::Options() const
{
    return options_;
}

const SenderTimingContext& FfmpegSenderService::TimingContext() const
{
    return timing_context_;
}
