#include "receiver.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include "latest_frame_buffer.h"
#include "project_logging.hpp"

namespace {

constexpr const char* kPreferredHardwareDecoder = "h264_rkmpp";

bool isHardwarePixelFormat(AVPixelFormat format) {
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    return descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

int findAnnexBStartCode(const std::uint8_t* data, int size, int offset) {
    for (int i = offset; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                return i;
            }
            if (i + 4 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                return i;
            }
        }
    }
    return -1;
}

int annexBHeaderSize(const std::uint8_t* data, int index, int size) {
    if (index + 3 < size && data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
        return 3;
    }
    if (index + 4 < size && data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 0 && data[index + 3] == 1) {
        return 4;
    }
    return 0;
}

}  // namespace

UdpTsH264Receiver::UdpTsH264Receiver(ReceiverOptions options)
    : options_(std::move(options)), latest_frame_buffer_(new LatestFrameBuffer()) {
    avformat_network_init();
}

UdpTsH264Receiver::~UdpTsH264Receiver() {
    stop();
    delete latest_frame_buffer_;
    latest_frame_buffer_ = nullptr;
    avformat_network_deinit();
}

bool UdpTsH264Receiver::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        logWarn("接收器已经在运行，忽略重复 start() 调用。");
        return true;
    }

    stop_requested_.store(false);
    last_io_activity_ms_.store(nowMs());
    worker_thread_ = std::thread(&UdpTsH264Receiver::workerLoop, this);
    return true;
}

void UdpTsH264Receiver::stop() {
    stop_requested_.store(true);
    running_.store(false);

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    closeSession();
}

bool UdpTsH264Receiver::getLatestFrame(FrameData& out) {
    return latest_frame_buffer_->getLatestFrame(out);
}

bool UdpTsH264Receiver::isRunning() const {
    return running_.load();
}

std::string UdpTsH264Receiver::currentDecoderName() const {
    return decoder_name_;
}

int UdpTsH264Receiver::interruptCallback(void* opaque) {
    auto* receiver = static_cast<UdpTsH264Receiver*>(opaque);
    return receiver->shouldInterruptForTimeout() ? 1 : 0;
}

void UdpTsH264Receiver::workerLoop() {
    while (!stop_requested_.load()) {
        const bool session_completed_cleanly = runSingleSession();

        closeSession();
        if (stop_requested_.load()) {
            break;
        }

        if (!session_completed_cleanly) {
            logWarn("接收会话已结束，准备重新连接。");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(options_.reconnect_delay_ms));
    }

    running_.store(false);
}

bool UdpTsH264Receiver::runSingleSession() {
    if (!openInput()) {
        return false;
    }

    selectVideoStream();
    return readPacketsLoop();
}

bool UdpTsH264Receiver::openInput() {
    closeSession();
    touchIoActivity();

    format_context_ = avformat_alloc_context();
    if (format_context_ == nullptr) {
        logError("分配 AVFormatContext 失败。");
        return false;
    }

    format_context_->flags |= AVFMT_FLAG_NOBUFFER;
    format_context_->max_delay = options_.max_delay;
    format_context_->interrupt_callback = {&UdpTsH264Receiver::interruptCallback, this};

    AVDictionary* format_options = nullptr;
    const std::string probesize = std::to_string(options_.probesize);
    const std::string analyzeduration = std::to_string(options_.analyzeduration);
    const std::string max_delay = std::to_string(options_.max_delay);
    av_dict_set(&format_options, "fflags", "nobuffer", 0);
    av_dict_set(&format_options, "probesize", probesize.c_str(), 0);
    av_dict_set(&format_options, "analyzeduration", analyzeduration.c_str(), 0);
    av_dict_set(&format_options, "max_delay", max_delay.c_str(), 0);

    AVInputFormat* input_format = av_find_input_format(options_.input_format.c_str());
    if (input_format == nullptr) {
        logError("找不到输入格式: " + options_.input_format);
        av_dict_free(&format_options);
        return false;
    }

    logInfo(
        "正在打开输入流: " + options_.input_url +
        "，目标视频频率=" + std::to_string(options_.video_frequency_hz) + "Hz");
    const int open_result = avformat_open_input(
        &format_context_,
        options_.input_url.c_str(),
        input_format,
        &format_options);
    if (open_result < 0) {
        logError("打开输入流失败: " + ffmpegErrorToString(open_result));
        av_dict_free(&format_options);
        return false;
    }

    if (format_options != nullptr) {
        AVDictionaryEntry* entry = nullptr;
        while ((entry = av_dict_get(format_options, "", entry, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
            logWarn("存在未使用的输入选项: " + std::string(entry->key) + "=" + entry->value);
        }
        av_dict_free(&format_options);
    }

    waiting_for_stream_logged_ = false;
    waiting_for_keyframe_logged_ = false;
    touchIoActivity();
    logInfo("输入流已打开，等待携带 SPS/PPS 的关键帧后再启动 h264_rkmpp。");
    return true;
}

bool UdpTsH264Receiver::selectVideoStream() {
    if (format_context_ == nullptr) {
        return false;
    }

    for (unsigned int i = 0; i < format_context_->nb_streams; ++i) {
        if (trySelectVideoStream(static_cast<int>(i))) {
            return true;
        }
    }

    if (!waiting_for_stream_logged_) {
        logInfo("正在等待从 MPEG-TS 输入中发现 H.264 视频流...");
        waiting_for_stream_logged_ = true;
    }
    return false;
}

bool UdpTsH264Receiver::trySelectVideoStream(int stream_index) {
    if (format_context_ == nullptr || stream_index < 0 ||
        stream_index >= static_cast<int>(format_context_->nb_streams)) {
        return false;
    }

    AVStream* stream = format_context_->streams[stream_index];
    if (!isSupportedVideoStream(stream)) {
        return false;
    }

    if (video_stream_index_ == stream_index && video_stream_ == stream) {
        return true;
    }

    video_stream_index_ = stream_index;
    video_stream_ = stream;
    waiting_for_stream_logged_ = false;

    logInfo(
        "已选择视频流: 索引=" + std::to_string(video_stream_index_) +
        "，分辨率=" + std::to_string(video_stream_->codecpar->width) + "x" +
        std::to_string(video_stream_->codecpar->height));
    return true;
}

bool UdpTsH264Receiver::isSupportedVideoStream(const AVStream* stream) const {
    if (stream == nullptr || stream->codecpar == nullptr) {
        return false;
    }

    return stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
           stream->codecpar->codec_id == AV_CODEC_ID_H264;
}

bool UdpTsH264Receiver::packetIsDecoderConfigReady(const AVPacket* packet) const {
    if (packet == nullptr || packet->data == nullptr || packet->size <= 0) {
        return false;
    }

    bool has_sps = false;
    bool has_pps = false;
    bool has_idr = false;

    int start = findAnnexBStartCode(packet->data, packet->size, 0);
    while (start >= 0) {
        const int header_size = annexBHeaderSize(packet->data, start, packet->size);
        if (header_size <= 0) {
            break;
        }

        const int nal_index = start + header_size;
        if (nal_index >= packet->size) {
            break;
        }

        const int nal_type = packet->data[nal_index] & 0x1F;
        if (nal_type == 7) {
            has_sps = true;
        } else if (nal_type == 8) {
            has_pps = true;
        } else if (nal_type == 5) {
            has_idr = true;
        }

        start = findAnnexBStartCode(packet->data, packet->size, nal_index + 1);
    }

    if (has_sps && has_pps) {
        return true;
    }

    if ((packet->flags & AV_PKT_FLAG_KEY) != 0 && has_idr && (has_sps || has_pps)) {
        return true;
    }

    return false;
}

bool UdpTsH264Receiver::ensureDecoderReadyForPacket(const AVPacket* packet) {
    if (packet == nullptr || format_context_ == nullptr) {
        return false;
    }

    if (packet->stream_index < 0 || packet->stream_index >= static_cast<int>(format_context_->nb_streams)) {
        return false;
    }

    if (video_stream_ == nullptr) {
        if (!trySelectVideoStream(packet->stream_index)) {
            return false;
        }
    }

    if (packet->stream_index != video_stream_index_) {
        return false;
    }

    if (codec_context_ != nullptr) {
        return true;
    }

    if (!packetIsDecoderConfigReady(packet)) {
        noteWaitingForDecoderConfig();
        return false;
    }

    return openDecoder();
}

bool UdpTsH264Receiver::openDecoder() {
    if (video_stream_ == nullptr) {
        return false;
    }

    if (codec_context_ != nullptr) {
        return true;
    }

    if (!options_.prefer_hardware_decoder) {
        logError("当前构建仅支持硬件解码，但硬件解码已被禁用。");
        return false;
    }

    const AVCodec* hardware_decoder = avcodec_find_decoder_by_name(kPreferredHardwareDecoder);
    if (hardware_decoder == nullptr) {
        logError("当前 FFmpeg 构建中没有可用的硬件解码器 h264_rkmpp。");
        return false;
    }

    AVCodecContext* context = avcodec_alloc_context3(hardware_decoder);
    if (context == nullptr) {
        logError("为 h264_rkmpp 分配 AVCodecContext 失败。");
        return false;
    }

    const int params_result = avcodec_parameters_to_context(context, video_stream_->codecpar);
    if (params_result < 0) {
        logError("复制视频流参数到解码器上下文失败: " + ffmpegErrorToString(params_result));
        avcodec_free_context(&context);
        return false;
    }

    context->pkt_timebase = video_stream_->time_base;
    context->framerate = AVRational{options_.video_frequency_hz, 1};
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->flags2 |= AV_CODEC_FLAG2_FAST;
    context->thread_count = 1;
    context->err_recognition = AV_EF_CAREFUL;

    AVDictionary* codec_options = nullptr;
    av_dict_set(&codec_options, "flags", "low_delay", 0);
    av_dict_set(&codec_options, "threads", "1", 0);

    logInfo("正在打开硬件解码器: h264_rkmpp");
    const int open_result = avcodec_open2(context, hardware_decoder, &codec_options);
    av_dict_free(&codec_options);
    if (open_result < 0) {
        logError("打开 h264_rkmpp 失败: " + ffmpegErrorToString(open_result));
        avcodec_free_context(&context);
        return false;
    }

    codec_context_ = context;
    codec_ = hardware_decoder;
    decoder_name_ = hardware_decoder->name;
    opened_with_hardware_decoder_ = true;
    has_decoded_frame_ = false;
    waiting_for_keyframe_logged_ = false;
    return true;
}

void UdpTsH264Receiver::closeDecoderOnly() {
    resetSwsContext();

    if (codec_context_ != nullptr) {
        avcodec_free_context(&codec_context_);
    }

    codec_ = nullptr;
    decoder_name_.clear();
    opened_with_hardware_decoder_ = false;
    has_decoded_frame_ = false;
    waiting_for_keyframe_logged_ = false;
}

void UdpTsH264Receiver::closeSession() {
    closeDecoderOnly();

    if (format_context_ != nullptr) {
        avformat_close_input(&format_context_);
    }

    video_stream_ = nullptr;
    video_stream_index_ = -1;
    waiting_for_stream_logged_ = false;
}

bool UdpTsH264Receiver::readPacketsLoop() {
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        logError("分配 AVPacket 失败。");
        return false;
    }

    bool keep_running = true;
    while (!stop_requested_.load()) {
        const int read_result = av_read_frame(format_context_, packet);
        if (read_result == AVERROR(EAGAIN)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (read_result == AVERROR_EOF) {
            logWarn("输入流到达 EOF，准备重新连接。");
            keep_running = false;
            break;
        }

        if (read_result == AVERROR_EXIT) {
            if (stop_requested_.load()) {
                keep_running = false;
                break;
            }

            logWarn("输入流 I/O 超时，准备重新连接。");
            keep_running = false;
            break;
        }

        if (read_result < 0) {
            logWarn("读取输入包失败: " + ffmpegErrorToString(read_result) + "，准备重新连接。");
            keep_running = false;
            break;
        }

        touchIoActivity();
        if (ensureDecoderReadyForPacket(packet)) {
            if (!decodePacket(packet)) {
                keep_running = false;
                av_packet_unref(packet);
                break;
            }
        }

        av_packet_unref(packet);
    }

    if (keep_running && !stop_requested_.load() && codec_context_ != nullptr) {
        keep_running = flushDecoder();
    }

    av_packet_free(&packet);
    return keep_running;
}

bool UdpTsH264Receiver::decodePacket(const AVPacket* packet) {
    if (codec_context_ == nullptr) {
        return true;
    }

    int result = avcodec_send_packet(codec_context_, packet);
    if (result == AVERROR(EAGAIN)) {
        return true;
    }

    if (result == AVERROR_INVALIDDATA) {
        if (!has_decoded_frame_) {
            logWarn("h264_rkmpp 拒绝了启动阶段的数据包，正在关闭解码器并等待下一帧携带 SPS/PPS 的关键帧。");
            closeDecoderOnly();
            return true;
        }
        return true;
    }

    if (result < 0) {
        logWarn("向解码器送包失败: " + ffmpegErrorToString(result));
        return true;
    }

    AVFrame* decoded_frame = av_frame_alloc();
    if (decoded_frame == nullptr) {
        logError("分配解码输出帧失败。");
        return false;
    }

    while (!stop_requested_.load()) {
        result = avcodec_receive_frame(codec_context_, decoded_frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }

        if (result == AVERROR_INVALIDDATA) {
            if (!has_decoded_frame_) {
                logWarn("h264_rkmpp 还没有收到可用的 SPS/PPS+IDR 数据包，继续等待下一帧关键帧。");
                closeDecoderOnly();
            }
            break;
        }

        if (result < 0) {
            logWarn("从解码器取帧失败: " + ffmpegErrorToString(result));
            break;
        }

        if (!handleDecodedFrame(decoded_frame)) {
            av_frame_unref(decoded_frame);
            break;
        }

        has_decoded_frame_ = true;
        waiting_for_keyframe_logged_ = false;
        av_frame_unref(decoded_frame);
    }

    av_frame_free(&decoded_frame);
    return true;
}

bool UdpTsH264Receiver::flushDecoder() {
    if (codec_context_ == nullptr) {
        return true;
    }

    const int send_result = avcodec_send_packet(codec_context_, nullptr);
    if (send_result < 0 && send_result != AVERROR_EOF && send_result != AVERROR_INVALIDDATA) {
        logWarn("刷新解码器失败: " + ffmpegErrorToString(send_result));
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        logError("分配刷新阶段的输出帧失败。");
        return false;
    }

    while (!stop_requested_.load()) {
        const int receive_result = avcodec_receive_frame(codec_context_, frame);
        if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
            break;
        }

        if (receive_result == AVERROR_INVALIDDATA) {
            break;
        }

        if (receive_result < 0) {
            logWarn("刷新阶段取帧失败: " + ffmpegErrorToString(receive_result));
            av_frame_free(&frame);
            return false;
        }

        if (handleDecodedFrame(frame)) {
            has_decoded_frame_ = true;
            waiting_for_keyframe_logged_ = false;
        }
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    return true;
}

bool UdpTsH264Receiver::handleDecodedFrame(AVFrame* decoded_frame) {
    AVFrame* source_frame = decoded_frame;
    AVFrame* transfer_frame = nullptr;

    if (isHardwarePixelFormat(static_cast<AVPixelFormat>(decoded_frame->format))) {
        transfer_frame = av_frame_alloc();
        if (transfer_frame == nullptr) {
            logError("为硬件帧转软件帧分配缓冲失败。");
            return false;
        }

        if (!transferHardwareFrame(decoded_frame, transfer_frame)) {
            av_frame_free(&transfer_frame);
            return false;
        }

        source_frame = transfer_frame;
    }

    FrameData latest_frame;
    const bool convert_ok = convertFrameToBgr(source_frame, latest_frame);
    av_frame_free(&transfer_frame);
    if (!convert_ok) {
        return false;
    }

    latest_frame_buffer_->update(std::move(latest_frame));
    return true;
}

bool UdpTsH264Receiver::convertFrameToBgr(AVFrame* source_frame, FrameData& out_frame) {
    const auto source_format = static_cast<AVPixelFormat>(source_frame->format);
    if (source_format == AV_PIX_FMT_NONE) {
        logWarn("解码帧的像素格式无效。");
        return false;
    }

    sws_context_ = sws_getCachedContext(
        sws_context_,
        source_frame->width,
        source_frame->height,
        source_format,
        source_frame->width,
        source_frame->height,
        AV_PIX_FMT_BGR24,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);

    if (sws_context_ == nullptr) {
        const char* pix_name = av_get_pix_fmt_name(source_format);
        logWarn(
            "为源像素格式创建 SwsContext 失败: " +
            std::string(pix_name != nullptr ? pix_name : "unknown"));
        return false;
    }

    const int buffer_size = av_image_get_buffer_size(
        AV_PIX_FMT_BGR24, source_frame->width, source_frame->height, 1);
    if (buffer_size <= 0) {
        logWarn("计算 BGR24 图像缓冲区大小失败。");
        return false;
    }

    out_frame.width = source_frame->width;
    out_frame.height = source_frame->height;
    out_frame.pixel_format = "bgr24";
    out_frame.pts = source_frame->pts;
    out_frame.best_effort_timestamp = source_frame->best_effort_timestamp;
    out_frame.time_base_num = video_stream_ != nullptr ? video_stream_->time_base.num : 0;
    out_frame.time_base_den = video_stream_ != nullptr ? video_stream_->time_base.den : 1;
    out_frame.data.resize(static_cast<std::size_t>(buffer_size));

    uint8_t* destination_data[4] = {nullptr, nullptr, nullptr, nullptr};
    int destination_linesize[4] = {0, 0, 0, 0};
    const int fill_result = av_image_fill_arrays(
        destination_data,
        destination_linesize,
        out_frame.data.data(),
        AV_PIX_FMT_BGR24,
        out_frame.width,
        out_frame.height,
        1);
    if (fill_result < 0) {
        logWarn("填充 BGR24 目标图像数组失败: " + ffmpegErrorToString(fill_result));
        return false;
    }

    const int scale_result = sws_scale(
        sws_context_,
        source_frame->data,
        source_frame->linesize,
        0,
        source_frame->height,
        destination_data,
        destination_linesize);
    if (scale_result <= 0) {
        logWarn("执行 sws_scale 失败。");
        return false;
    }

    out_frame.stride = destination_linesize[0];
    return true;
}

bool UdpTsH264Receiver::transferHardwareFrame(AVFrame* hardware_frame, AVFrame* software_frame) {
    av_frame_unref(software_frame);
    const int result = av_hwframe_transfer_data(software_frame, hardware_frame, 0);
    if (result < 0) {
        logWarn("硬件帧转软件帧失败: " + ffmpegErrorToString(result));
        return false;
    }

    software_frame->width = hardware_frame->width;
    software_frame->height = hardware_frame->height;
    software_frame->pts = hardware_frame->pts;
    software_frame->best_effort_timestamp = hardware_frame->best_effort_timestamp;
    return true;
}

void UdpTsH264Receiver::resetSwsContext() {
    if (sws_context_ != nullptr) {
        sws_freeContext(sws_context_);
        sws_context_ = nullptr;
    }
}

bool UdpTsH264Receiver::shouldInterruptForTimeout() const {
    if (stop_requested_.load()) {
        return true;
    }

    const int timeout_ms = options_.io_timeout_ms;
    if (timeout_ms <= 0) {
        return false;
    }

    const std::int64_t last_io = last_io_activity_ms_.load();
    return (nowMs() - last_io) > timeout_ms;
}

void UdpTsH264Receiver::touchIoActivity() {
    last_io_activity_ms_.store(nowMs());
}

std::int64_t UdpTsH264Receiver::nowMs() const {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void UdpTsH264Receiver::noteWaitingForDecoderConfig() {
    if (waiting_for_keyframe_logged_) {
        return;
    }

    logInfo("正在等待携带 SPS/PPS 的关键帧后再启动 h264_rkmpp。");
    waiting_for_keyframe_logged_ = true;
}

void UdpTsH264Receiver::logInfo(const std::string& message) const {
    project_logging::Info(message);
}

void UdpTsH264Receiver::logWarn(const std::string& message) const {
    project_logging::Warn(message);
}

void UdpTsH264Receiver::logError(const std::string& message) const {
    project_logging::Error(message);
}

std::string UdpTsH264Receiver::ffmpegErrorToString(int error_code) const {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(error_code, buffer, sizeof(buffer));
    return buffer;
}
