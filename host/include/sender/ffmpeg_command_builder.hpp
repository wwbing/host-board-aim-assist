#pragma once

#include "sender/ffmpeg_sender_options.hpp"

#include <string>

std::wstring BuildFfmpegFilterComplex(const FfmpegSenderOptions& options);
std::wstring BuildFfmpegUdpUrl(const FfmpegSenderOptions& options);
std::wstring BuildFfmpegCommandLine(const FfmpegSenderOptions& options);
