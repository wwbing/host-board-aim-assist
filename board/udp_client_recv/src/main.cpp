#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "receiver.h"

namespace {

std::atomic<bool> g_stop_requested{false};
constexpr const char* kWindowTitle = "RK3588 UDP Latest Frame";

void handleSignal(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        g_stop_requested.store(true);
    }
}

void printUsage(const char* program_name) {
    std::cout
        << "Usage: " << program_name << " [--display] [udp_url]\n"
        << "Default udp_url:\n"
        << "  udp://0.0.0.0:5000?pkt_size=188&fifo_size=32768&buffer_size=8388608&overrun_nonfatal=1\n"
        << "Options:\n"
        << "  --display     Enable OpenCV preview window\n"
        << "  --no-display  Disable OpenCV preview window (default)\n"
        << "Controls when preview is enabled:\n"
        << "  q / ESC  Quit\n";
}

cv::Mat buildWaitingFrame(int width, int height) {
    cv::Mat canvas(height, width, CV_8UC3, cv::Scalar(18, 18, 18));
    cv::putText(
        canvas,
        "Waiting for UDP MPEG-TS H.264 stream...",
        cv::Point(24, height / 2 - 12),
        cv::FONT_HERSHEY_SIMPLEX,
        0.7,
        cv::Scalar(0, 220, 255),
        2,
        cv::LINE_AA);
    cv::putText(
        canvas,
        "Source example: 192.168.7.1 -> BOARD_IP:5000",
        cv::Point(24, height / 2 + 28),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(180, 180, 180),
        1,
        cv::LINE_AA);
    return canvas;
}

void overlayFrameInfo(cv::Mat& image, const FrameData& frame, const std::string& decoder_name) {
    cv::rectangle(image, cv::Rect(0, 0, image.cols, 86), cv::Scalar(0, 0, 0), cv::FILLED);
    cv::putText(
        image,
        "Latest-frame mode | decoder=" + decoder_name,
        cv::Point(16, 30),
        cv::FONT_HERSHEY_SIMPLEX,
        0.75,
        cv::Scalar(0, 255, 0),
        2,
        cv::LINE_AA);
    cv::putText(
        image,
        "size=" + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
            " stride=" + std::to_string(frame.stride) +
            " pts=" + std::to_string(frame.pts),
        cv::Point(16, 62),
        cv::FONT_HERSHEY_SIMPLEX,
        0.65,
        cv::Scalar(0, 220, 255),
        2,
        cv::LINE_AA);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    ReceiverOptions options;
    bool enable_display = false;
    bool input_url_overridden = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        if (arg == "--display") {
            enable_display = true;
            continue;
        }
        if (arg == "--no-display") {
            enable_display = false;
            continue;
        }
        if (!input_url_overridden) {
            options.input_url = arg;
            input_url_overridden = true;
            continue;
        }

        std::cerr << "[ERROR] Unknown extra argument: " << arg << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    std::cerr << "[INFO] Starting latest-frame UDP MPEG-TS receiver" << std::endl;
    std::cerr << "[INFO] Input URL: " << options.input_url << std::endl;
    std::cerr << "[INFO] Preview: " << (enable_display ? "enabled" : "disabled") << std::endl;
    std::cerr << "[INFO] Sender host example: 192.168.7.1 -> BOARD_IP:5000" << std::endl;

    UdpTsH264Receiver receiver(options);
    if (!receiver.start()) {
        std::cerr << "[ERROR] Failed to start receiver." << std::endl;
        return 1;
    }

    cv::Mat waiting;
    if (enable_display) {
        cv::namedWindow(kWindowTitle, cv::WINDOW_NORMAL);
        cv::resizeWindow(kWindowTitle, 960, 960);
        waiting = buildWaitingFrame(960, 960);
    }

    std::int64_t last_reported_pts = AV_NOPTS_VALUE;
    auto last_status_log = std::chrono::steady_clock::now();

    while (!g_stop_requested.load()) {
        FrameData frame;
        const bool has_frame = receiver.getLatestFrame(frame) && !frame.data.empty();
        const auto now = std::chrono::steady_clock::now();

        if (has_frame) {
            if (enable_display) {
                cv::Mat bgr(frame.height, frame.width, CV_8UC3, frame.data.data(), frame.stride);
                cv::Mat display = bgr.clone();
                overlayFrameInfo(display, frame, receiver.currentDecoderName());
                cv::imshow(kWindowTitle, display);
            }

            if (frame.pts != last_reported_pts && now - last_status_log >= std::chrono::seconds(1)) {
                last_reported_pts = frame.pts;
                std::cerr
                    << "[INFO] Latest frame: "
                    << frame.width << "x" << frame.height
                    << " stride=" << frame.stride
                    << " pts=" << frame.pts
                    << " fmt=" << frame.pixel_format
                    << " bytes=" << frame.data.size()
                    << " decoder=" << receiver.currentDecoderName()
                    << std::endl;
                last_status_log = now;
            }
        } else {
            if (enable_display) {
                cv::imshow(kWindowTitle, waiting);
            }
            if (now - last_status_log >= std::chrono::seconds(1)) {
                std::cerr << "[INFO] Waiting for latest frame..." << std::endl;
                last_status_log = now;
            }
        }

        if (enable_display) {
            const int key = cv::waitKey(1) & 0xFF;
            if (key == 'q' || key == 27) {
                g_stop_requested.store(true);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cerr << "[INFO] Stopping receiver..." << std::endl;
    receiver.stop();
    if (enable_display) {
        cv::destroyAllWindows();
    }
    return 0;
}
