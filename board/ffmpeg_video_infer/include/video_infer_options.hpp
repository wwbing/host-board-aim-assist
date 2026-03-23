#pragma once

#include <filesystem>
#include <string>

namespace ffmpeg_video_infer {

struct VideoInferOptions {
    std::string input_url =
        "udp://0.0.0.0:5000?pkt_size=188&fifo_size=32768&buffer_size=8388608&overrun_nonfatal=1";
    std::filesystem::path model_path;
    std::filesystem::path output_dir;
    bool enable_display = false;
    bool save_result = false;
    bool save_result_json = false;
    std::filesystem::path result_json_path;
    bool enable_result_udp = true;
    std::string result_udp_ip = "192.168.7.1";
    int result_udp_port = 6000;
    bool enable_clock_offset_server = true;
    int clock_offset_server_port = 45678;
    int video_frequency_hz = 60;
    int idle_sleep_ms = 5;
    int duplicate_sleep_ms = 1;
    int display_width = 960;
    int display_height = 960;
};

}  // namespace ffmpeg_video_infer
