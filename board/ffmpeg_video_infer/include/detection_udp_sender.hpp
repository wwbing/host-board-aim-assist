#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <netinet/in.h>

#include "yolov6_rknn.hpp"

namespace ffmpeg_video_infer {

struct DetectionFrameMetadata {
    std::int64_t frame_pts = 0;
    std::int64_t frame_best_effort_ts = 0;
    int frame_time_base_num = 0;
    int frame_time_base_den = 1;
    int frame_width = 0;
    int frame_height = 0;
};

struct DetectionFrameTimingInfo {
    std::int64_t board_wall_infer_done_ms = 0;
    double board_preprocess_ms = 0.0;
    double board_inference_ms = 0.0;
    double board_postprocess_ms = 0.0;
    std::int64_t board_result_send_start_ms = 0;
    std::optional<std::int64_t> board_result_send_end_ms;
};

struct DetectionFramePayload {
    DetectionFrameMetadata metadata;
    DetectionFrameTimingInfo timing;
};

class DetectionUdpSender {
public:
    DetectionUdpSender() = default;
    ~DetectionUdpSender();

    DetectionUdpSender(const DetectionUdpSender&) = delete;
    DetectionUdpSender& operator=(const DetectionUdpSender&) = delete;

    bool Initialize(const std::string& target_ip, int target_port);
    bool ConfigureJsonDump(bool enable_json_dump, const std::filesystem::path& json_dump_path);
    void Close();

    bool IsReady() const;
    std::uint64_t NextSequence() const;
    bool SendFrameDetections(
        const DetectionFramePayload& payload,
        const std::vector<infer_v6_cpp::Detection>& detections);

    const std::string& LastError() const;

private:
    bool AppendJsonLine(const std::string& payload_json) const;
    void SetError(const std::string& message);

    int socket_fd_ = -1;
    sockaddr_in destination_{};
    bool ready_ = false;
    bool save_json_enabled_ = false;
    std::uint64_t next_seq_ = 0;
    std::filesystem::path json_dump_path_;
    std::string last_error_;
};

}  // namespace ffmpeg_video_infer
