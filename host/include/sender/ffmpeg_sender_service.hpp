#pragma once

#include "types.hpp"
#include "sender/ffmpeg_process.hpp"
#include "sender/ffmpeg_sender_options.hpp"

#include <Windows.h>

#include <string>

class FfmpegSenderService
{
public:
    explicit FfmpegSenderService(FfmpegSenderOptions options);
    ~FfmpegSenderService();

    FfmpegSenderService(const FfmpegSenderService&) = delete;
    FfmpegSenderService& operator=(const FfmpegSenderService&) = delete;

    bool Start();
    void Stop();
    bool IsRunning();
    DWORD ExitCode();
    const std::string& LastError() const;
    const std::wstring& CommandLine() const;
    const FfmpegSenderOptions& Options() const;
    const SenderTimingContext& TimingContext() const;

private:
    FfmpegSenderOptions options_;
    FfmpegProcess process_;
    std::wstring command_line_;
    std::string last_error_;
    SenderTimingContext timing_context_;
};
