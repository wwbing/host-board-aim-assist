#pragma once

#include <cstdint>
#include <string>

struct FfmpegSenderOptions
{
    std::wstring ffmpeg_path;
    std::wstring output_ip;
    std::uint16_t output_port = 5000;
    int output_idx = 0;
    int framerate = 60;
    int crop_width = 640;
    int crop_height = 640;
    int offset_x = 0;
    int offset_y = 0;
    std::wstring bitrate = L"4M";
    std::wstring maxrate = L"4M";
    std::wstring bufsize = L"150k";
    int gop = 10;
    int pkt_size = 188;
    int udp_buffer_size = 1048576;
    double pts_per_second = 90000.0;
};
