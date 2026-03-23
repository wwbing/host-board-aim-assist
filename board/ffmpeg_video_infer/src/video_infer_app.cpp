#include "video_infer_app.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "clock_offset_server.hpp"
#include "detection_udp_sender.hpp"
#include "project_logging.hpp"
#include "receiver.h"
#include "yolov6_rknn.hpp"

namespace ffmpeg_video_infer {
namespace {

constexpr const char* kWindowTitle = "ffmpeg_video_infer";

std::int64_t NowWallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

cv::Mat BuildWaitingFrame(int width, int height) {
    cv::Mat canvas(height, width, CV_8UC3, cv::Scalar(18, 18, 18));
    cv::putText(
        canvas,
        "Waiting for UDP latest frame...",
        cv::Point(24, height / 2 - 12),
        cv::FONT_HERSHEY_SIMPLEX,
        0.8,
        cv::Scalar(0, 220, 255),
        2,
        cv::LINE_AA);
    cv::putText(
        canvas,
        "udp_client_recv -> infer_v6_cpp -> result_udp",
        cv::Point(24, height / 2 + 28),
        cv::FONT_HERSHEY_SIMPLEX,
        0.65,
        cv::Scalar(180, 180, 180),
        1,
        cv::LINE_AA);
    return canvas;
}

void OverlayFrameInfo(cv::Mat& image,
                      const FrameData& frame,
                      const infer_v6_cpp::InferenceStats& stats,
                      int detection_count,
                      const std::string& decoder_name) {
    cv::rectangle(image, cv::Rect(0, 0, image.cols, 120), cv::Scalar(0, 0, 0), cv::FILLED);
    cv::putText(
        image,
        "Latest-frame mode | decoder=" + decoder_name,
        cv::Point(16, 28),
        cv::FONT_HERSHEY_SIMPLEX,
        0.7,
        cv::Scalar(0, 255, 0),
        2,
        cv::LINE_AA);
    cv::putText(
        image,
        "size=" + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
            " pts=" + std::to_string(frame.pts) +
            " detections=" + std::to_string(detection_count),
        cv::Point(16, 58),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(0, 220, 255),
        2,
        cv::LINE_AA);
    cv::putText(
        image,
        cv::format(
            "prep %.1f ms | infer %.1f ms | post %.1f ms | raw %.3f",
            stats.preprocess_ms,
            stats.inference_ms,
            stats.postprocess_ms,
            stats.raw_score_max),
        cv::Point(16, 88),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(255, 255, 255),
        2,
        cv::LINE_AA);
}

}  // namespace

struct VideoInferApp::Impl {
    explicit Impl(VideoInferOptions opts)
        : options(std::move(opts)),
          receiver(BuildReceiverOptions(options)),
          waiting_frame(BuildWaitingFrame(options.display_width, options.display_height)) {}

    static ReceiverOptions BuildReceiverOptions(const VideoInferOptions& options) {
        ReceiverOptions receiver_options;
        receiver_options.input_url = options.input_url;
        receiver_options.video_frequency_hz = options.video_frequency_hz;
        return receiver_options;
    }

    bool Initialize() {
        if (options.model_path.empty()) {
            last_error = "模型路径为空。";
            return false;
        }
        if (!std::filesystem::exists(options.model_path)) {
            last_error = "找不到模型文件: " + options.model_path.string();
            return false;
        }
        if (options.save_result || options.save_result_json) {
            std::filesystem::create_directories(options.output_dir);
        }
        if (!infer.Load(options.model_path.string())) {
            last_error = infer.LastError();
            return false;
        }
        project_logging::Info("当前视频频率配置: {} Hz", options.video_frequency_hz);
        if (options.enable_clock_offset_server) {
            if (!clock_offset_server.Start(options.clock_offset_server_port)) {
                project_logging::Warn("时钟偏移服务未启用: {}", clock_offset_server.LastError());
            } else {
                project_logging::Info(
                    "时钟偏移服务已启动: udp://0.0.0.0:{}",
                    options.clock_offset_server_port);
            }
        }
        if (!result_udp_sender.ConfigureJsonDump(options.save_result_json, options.result_json_path)) {
            project_logging::Warn("结果 JSON 落盘已禁用: {}", result_udp_sender.LastError());
        } else if (options.save_result_json) {
            project_logging::Info("结果 JSON 落盘路径: {}", options.result_json_path.string());
        }
        if (options.enable_result_udp) {
            if (!result_udp_sender.Initialize(options.result_udp_ip, options.result_udp_port)) {
                project_logging::Warn("结果 UDP 发送器未启用: {}", result_udp_sender.LastError());
                result_udp_enabled = false;
            } else {
                project_logging::Info(
                    "结果 UDP 发送目标: {}:{}",
                    options.result_udp_ip,
                    options.result_udp_port);
                result_udp_enabled = true;
            }
        }
        initialized = true;
        last_error.clear();
        return true;
    }

    int Run() {
        if (!initialized) {
            last_error = "应用尚未初始化。";
            return 1;
        }

        if (options.enable_display) {
            cv::namedWindow(kWindowTitle, cv::WINDOW_NORMAL);
            cv::resizeWindow(kWindowTitle, options.display_width, options.display_height);
        }

        if (!receiver.start()) {
            last_error = "启动 UDP 接收器失败。";
            return 1;
        }

        const int exit_code = Loop();
        receiver.stop();
        clock_offset_server.Stop();
        if (options.enable_display) {
            cv::destroyAllWindows();
        }
        return exit_code;
    }

    void RequestStop() {
        stop_requested.store(true);
    }

    int Loop() {
        while (!stop_requested.load()) {
            FrameData frame;
            const bool has_frame = receiver.getLatestFrame(frame) && !frame.data.empty();
            if (!has_frame) {
                ShowWaitingFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(options.idle_sleep_ms));
                continue;
            }

            if (!IsNewFrame(frame)) {
                PollDisplayExit();
                std::this_thread::sleep_for(std::chrono::milliseconds(options.duplicate_sleep_ms));
                continue;
            }

            if (!ProcessFrame(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            UpdateLastFrameMarker(frame);
        }
        return 0;
    }

    bool ProcessFrame(const FrameData& frame) {
        cv::Mat bgr(frame.height, frame.width, CV_8UC3, const_cast<std::uint8_t*>(frame.data.data()), frame.stride);
        cv::Mat frame_copy = bgr.clone();

        std::vector<infer_v6_cpp::Detection> detections;
        infer_v6_cpp::InferenceStats stats;
        if (!infer.Infer(frame_copy, detections, &stats)) {
            last_error = "推理失败: " + infer.LastError();
            project_logging::Error(last_error);
            return false;
        }
        const std::int64_t board_wall_infer_done_ms = NowWallClockMs();

        DetectionFramePayload result_payload;
        result_payload.metadata.frame_pts = frame.pts;
        result_payload.metadata.frame_best_effort_ts = frame.best_effort_timestamp;
        result_payload.metadata.frame_time_base_num = frame.time_base_num;
        result_payload.metadata.frame_time_base_den = frame.time_base_den;
        result_payload.metadata.frame_width = frame.width;
        result_payload.metadata.frame_height = frame.height;
        result_payload.timing.board_wall_infer_done_ms = board_wall_infer_done_ms;
        result_payload.timing.board_preprocess_ms = stats.preprocess_ms;
        result_payload.timing.board_inference_ms = stats.inference_ms;
        result_payload.timing.board_postprocess_ms = stats.postprocess_ms;

        std::uint64_t result_seq = 0;
        bool has_result_seq = false;
        std::int64_t board_result_send_start_ms = 0;
        std::int64_t board_result_send_end_ms = 0;
        if (result_udp_enabled) {
            result_seq = result_udp_sender.NextSequence();
            has_result_seq = true;
            board_result_send_start_ms = NowWallClockMs();
            result_payload.timing.board_result_send_start_ms = board_result_send_start_ms;
            result_payload.timing.board_result_send_end_ms = std::nullopt;

            const bool send_ok = result_udp_sender.SendFrameDetections(result_payload, detections);
            board_result_send_end_ms = NowWallClockMs();
            if (!send_ok) {
                project_logging::Warn("发送检测结果 UDP 包失败: {}", result_udp_sender.LastError());
            }
        }

        cv::Mat visualized = infer.DrawDetections(frame_copy, detections);
        OverlayFrameInfo(visualized, frame, stats, static_cast<int>(detections.size()), receiver.currentDecoderName());

        if (options.save_result) {
            const std::filesystem::path save_path = MakeSavePath();
            if (!cv::imwrite(save_path.string(), visualized)) {
                last_error = "写入结果图片失败: " + save_path.string();
                project_logging::Error(last_error);
                return false;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_log_time >= std::chrono::seconds(1)) {
            std::ostringstream log_stream;
            log_stream
                << "最新帧推理:"
                << " 帧PTS=" << frame.pts
                << " 最佳努力时间戳=" << frame.best_effort_timestamp
                << " 时间基=" << frame.time_base_num << "/" << frame.time_base_den
                << " 分辨率=" << frame.width << "x" << frame.height
                << " 检测框数量=" << detections.size()
                << " 预处理耗时ms=" << stats.preprocess_ms
                << " 推理耗时ms=" << stats.inference_ms
                << " 后处理耗时ms=" << stats.postprocess_ms
                << " 推理完成墙钟ms=" << board_wall_infer_done_ms;
            if (has_result_seq) {
                log_stream
                    << " 结果序号=" << result_seq
                    << " 发送开始ms=" << board_result_send_start_ms
                    << " 发送结束ms=" << board_result_send_end_ms;
            } else {
                log_stream << " 结果UDP=已禁用";
            }
            project_logging::Info(log_stream.str());
            last_log_time = now;
        }

        if (options.enable_display) {
            cv::imshow(kWindowTitle, visualized);
        }
        PollDisplayExit();
        last_error.clear();
        return true;
    }

    void ShowWaitingFrame() {
        if (options.enable_display) {
            cv::imshow(kWindowTitle, waiting_frame);
        }
        PollDisplayExit();
    }

    void PollDisplayExit() {
        if (!options.enable_display) {
            return;
        }
        const int key = cv::waitKey(1) & 0xFF;
        if (key == 'q' || key == 27) {
            stop_requested.store(true);
        }
    }

    bool IsNewFrame(const FrameData& frame) const {
        const bool frame_has_no_timestamp =
            frame.pts == AV_NOPTS_VALUE && frame.best_effort_timestamp == AV_NOPTS_VALUE;
        if (frame_has_no_timestamp) {
            return true;
        }
        return frame.pts != last_pts || frame.best_effort_timestamp != last_best_effort_timestamp;
    }

    void UpdateLastFrameMarker(const FrameData& frame) {
        last_pts = frame.pts;
        last_best_effort_timestamp = frame.best_effort_timestamp;
    }

    std::filesystem::path MakeSavePath() {
        std::ostringstream oss;
        oss << "frame_" << std::setw(6) << std::setfill('0') << saved_frame_counter++ << ".jpg";
        return options.output_dir / oss.str();
    }

    VideoInferOptions options;
    UdpTsH264Receiver receiver;
    infer_v6_cpp::YoloV6RknnInfer infer;
    ClockOffsetServer clock_offset_server;
    DetectionUdpSender result_udp_sender;
    std::atomic<bool> stop_requested{false};
    bool initialized = false;
    bool result_udp_enabled = false;
    std::string last_error;
    cv::Mat waiting_frame;
    std::int64_t last_pts = AV_NOPTS_VALUE;
    std::int64_t last_best_effort_timestamp = AV_NOPTS_VALUE;
    std::uint64_t saved_frame_counter = 0;
    std::chrono::steady_clock::time_point last_log_time = std::chrono::steady_clock::now();
};

VideoInferApp::VideoInferApp(VideoInferOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

VideoInferApp::~VideoInferApp() = default;

bool VideoInferApp::Initialize() {
    return impl_->Initialize();
}

int VideoInferApp::Run() {
    return impl_->Run();
}

void VideoInferApp::RequestStop() {
    impl_->RequestStop();
}

const std::string& VideoInferApp::LastError() const {
    return impl_->last_error;
}

}  // namespace ffmpeg_video_infer
