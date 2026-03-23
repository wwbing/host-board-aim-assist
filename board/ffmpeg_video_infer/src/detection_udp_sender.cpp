#include "detection_udp_sender.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "project_logging.hpp"

namespace ffmpeg_video_infer {
namespace {

std::string BuildPayload(std::uint64_t seq,
                         const DetectionFramePayload& payload,
                         const std::vector<infer_v6_cpp::Detection>& detections) {
    nlohmann::ordered_json json_payload;
    json_payload["seq"] = seq;
    json_payload["frame_pts"] = payload.metadata.frame_pts;
    json_payload["frame_best_effort_ts"] = payload.metadata.frame_best_effort_ts;
    json_payload["frame_time_base_num"] = payload.metadata.frame_time_base_num;
    json_payload["frame_time_base_den"] = payload.metadata.frame_time_base_den;
    json_payload["frame_width"] = payload.metadata.frame_width;
    json_payload["frame_height"] = payload.metadata.frame_height;
    json_payload["box_count"] = detections.size();
    json_payload["boxes"] = nlohmann::ordered_json::array();

    for (const infer_v6_cpp::Detection& detection : detections) {
        const float center_x = (detection.x1 + detection.x2) * 0.5f;
        const float center_y = (detection.y1 + detection.y2) * 0.5f;
        nlohmann::ordered_json json_box;
        json_box["class_id"] = detection.class_id;
        json_box["score"] = detection.score;
        json_box["x1"] = detection.x1;
        json_box["y1"] = detection.y1;
        json_box["x2"] = detection.x2;
        json_box["y2"] = detection.y2;
        json_box["cx"] = center_x;
        json_box["cy"] = center_y;
        json_payload["boxes"].push_back(std::move(json_box));
    }

    nlohmann::ordered_json json_timing;
    json_timing["board_wall_infer_done_ms"] = payload.timing.board_wall_infer_done_ms;
    json_timing["board_preprocess_ms"] = payload.timing.board_preprocess_ms;
    json_timing["board_inference_ms"] = payload.timing.board_inference_ms;
    json_timing["board_postprocess_ms"] = payload.timing.board_postprocess_ms;
    json_timing["board_result_send_start_ms"] = payload.timing.board_result_send_start_ms;
    if (payload.timing.board_result_send_end_ms.has_value()) {
        json_timing["board_result_send_end_ms"] = *payload.timing.board_result_send_end_ms;
    } else {
        json_timing["board_result_send_end_ms"] = nullptr;
    }
    json_payload["timing"] = std::move(json_timing);

    return json_payload.dump();
}

}  // namespace

DetectionUdpSender::~DetectionUdpSender() {
    Close();
}

bool DetectionUdpSender::Initialize(const std::string& target_ip, int target_port) {
    Close();

    if (target_ip.empty()) {
        SetError("结果 UDP 目标 IP 为空。");
        return false;
    }
    if (target_port <= 0 || target_port > 65535) {
        SetError("结果 UDP 端口超出范围。");
        return false;
    }

    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        SetError("创建结果 UDP socket 失败: " + std::string(std::strerror(errno)));
        return false;
    }

    destination_ = {};
    destination_.sin_family = AF_INET;
    destination_.sin_port = htons(static_cast<std::uint16_t>(target_port));
    if (::inet_pton(AF_INET, target_ip.c_str(), &destination_.sin_addr) != 1) {
        SetError("解析结果 UDP 目标 IP 失败: " + target_ip);
        Close();
        return false;
    }

    ready_ = true;
    next_seq_ = 0;
    last_error_.clear();
    return true;
}

bool DetectionUdpSender::ConfigureJsonDump(
    bool enable_json_dump,
    const std::filesystem::path& json_dump_path) {
    save_json_enabled_ = false;
    json_dump_path_.clear();

    if (!enable_json_dump) {
        last_error_.clear();
        return true;
    }
    if (json_dump_path.empty()) {
        SetError("结果 JSON 落盘路径为空。");
        return false;
    }

    save_json_enabled_ = true;
    json_dump_path_ = json_dump_path;
    last_error_.clear();
    return true;
}

void DetectionUdpSender::Close() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    ready_ = false;
}

bool DetectionUdpSender::IsReady() const {
    return ready_;
}

std::uint64_t DetectionUdpSender::NextSequence() const {
    return next_seq_;
}

bool DetectionUdpSender::SendFrameDetections(
    const DetectionFramePayload& payload,
    const std::vector<infer_v6_cpp::Detection>& detections) {
    if (!ready_) {
        SetError("结果 UDP 发送器尚未初始化。");
        return false;
    }

    const std::string payload_json = BuildPayload(next_seq_, payload, detections);
    ++next_seq_;

    const ssize_t sent_bytes = ::sendto(
        socket_fd_,
        payload_json.data(),
        payload_json.size(),
        0,
        reinterpret_cast<const sockaddr*>(&destination_),
        sizeof(destination_));
    if (sent_bytes < 0) {
        SetError("发送 UDP 数据失败: " + std::string(std::strerror(errno)));
        return false;
    }
    if (sent_bytes != static_cast<ssize_t>(payload_json.size())) {
        SetError("发送 UDP 数据时发生了非完整写入。");
        return false;
    }
    if (!AppendJsonLine(payload_json)) {
        project_logging::Warn(
            "将已发送 JSON 追加写入文件失败: {}",
            json_dump_path_.string());
    }

    last_error_.clear();
    return true;
}

const std::string& DetectionUdpSender::LastError() const {
    return last_error_;
}

void DetectionUdpSender::SetError(const std::string& message) {
    last_error_ = message;
}

bool DetectionUdpSender::AppendJsonLine(const std::string& payload_json) const {
    if (!save_json_enabled_) {
        return true;
    }

    std::ofstream output(json_dump_path_, std::ios::app);
    if (!output.is_open()) {
        return false;
    }

    output << payload_json << '\n';
    return output.good();
}

}  // namespace ffmpeg_video_infer
