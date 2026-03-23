#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

class LatestFrameBuffer;

struct FrameData {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::string pixel_format = "bgr24";
    std::int64_t pts = AV_NOPTS_VALUE;
    std::int64_t best_effort_timestamp = AV_NOPTS_VALUE;
    int time_base_num = 0;
    int time_base_den = 1;
    std::vector<std::uint8_t> data;
};

struct ReceiverOptions {
    std::string input_url =
        "udp://0.0.0.0:5000?pkt_size=188&fifo_size=32768&buffer_size=8388608&overrun_nonfatal=1";
    std::string input_format = "mpegts";
    int probesize = 2000000;
    int analyzeduration = 2000000;
    int max_delay = 0;
    int reconnect_delay_ms = 1000;
    int io_timeout_ms = 3000;
    int video_frequency_hz = 60;
    bool prefer_hardware_decoder = true;
};

class UdpTsH264Receiver {
public:
    explicit UdpTsH264Receiver(ReceiverOptions options = {});
    ~UdpTsH264Receiver();

    bool start();
    void stop();
    bool getLatestFrame(FrameData& out);

    bool isRunning() const;
    std::string currentDecoderName() const;

private:
    static int interruptCallback(void* opaque);

    void workerLoop();
    bool runSingleSession();
    bool openInput();
    bool selectVideoStream();
    bool trySelectVideoStream(int stream_index);
    bool isSupportedVideoStream(const AVStream* stream) const;
    bool ensureDecoderReadyForPacket(const AVPacket* packet);
    bool packetIsDecoderConfigReady(const AVPacket* packet) const;
    bool openDecoder();
    void closeSession();
    void closeDecoderOnly();

    bool readPacketsLoop();
    bool decodePacket(const AVPacket* packet);
    bool flushDecoder();
    bool handleDecodedFrame(AVFrame* decoded_frame);
    bool convertFrameToBgr(AVFrame* source_frame, FrameData& out_frame);
    bool transferHardwareFrame(AVFrame* hardware_frame, AVFrame* software_frame);

    void resetSwsContext();
    bool shouldInterruptForTimeout() const;
    void touchIoActivity();
    std::int64_t nowMs() const;
    void noteWaitingForDecoderConfig();

    void logInfo(const std::string& message) const;
    void logWarn(const std::string& message) const;
    void logError(const std::string& message) const;
    std::string ffmpegErrorToString(int error_code) const;

    ReceiverOptions options_;
    LatestFrameBuffer* latest_frame_buffer_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::int64_t> last_io_activity_ms_{0};
    std::thread worker_thread_;

    AVFormatContext* format_context_ = nullptr;
    AVCodecContext* codec_context_ = nullptr;
    const AVCodec* codec_ = nullptr;
    AVStream* video_stream_ = nullptr;
    int video_stream_index_ = -1;
    bool opened_with_hardware_decoder_ = false;
    bool has_decoded_frame_ = false;
    bool waiting_for_stream_logged_ = false;
    bool waiting_for_keyframe_logged_ = false;
    std::string decoder_name_;

    SwsContext* sws_context_ = nullptr;
};
