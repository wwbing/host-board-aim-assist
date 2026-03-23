#include <csignal>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include "project_logging.hpp"
#include "video_infer_app.hpp"
#include "video_infer_options.hpp"

namespace {

ffmpeg_video_infer::VideoInferApp* g_app = nullptr;

std::filesystem::path DefaultModelPath() {
    return std::filesystem::path(FFMPEG_VIDEO_INFER_SOURCE_DIR) / ".." / "rknn_infer" / "model" /
           "v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn";
}

std::filesystem::path DefaultOutputDir() {
    return std::filesystem::path(FFMPEG_VIDEO_INFER_SOURCE_DIR) / "result";
}

void HandleSignal(int signal_number) {
    if ((signal_number == SIGINT || signal_number == SIGTERM) && g_app != nullptr) {
        g_app->RequestStop();
    }
}

void PrintUsage(const char* program_name) {
    std::cout
        << "用法: " << program_name
        << " [--display] [--save] [--savejson] [--disable_result_udp] [--result_udp_ip IP]"
        << " [--result_udp_port PORT] [--videofrequency HZ] [--model MODEL] [--output_dir DIR] [udp_url]\n"
        << "默认值:\n"
        << "  模型路径: " << DefaultModelPath() << "\n"
        << "  输出目录: " << DefaultOutputDir() << "\n"
        << "  结果 JSON: " << (DefaultOutputDir() / "result_udp_packets.jsonl") << "\n"
        << "  结果 UDP IP: 192.168.7.1\n"
        << "  结果 UDP 端口: 6000\n"
        << "  视频频率: 60 Hz\n"
        << "  时钟偏移端口: 45678\n"
        << "  输入 UDP URL: udp://0.0.0.0:5000?pkt_size=188&fifo_size=32768&buffer_size=8388608&overrun_nonfatal=1\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    project_logging::EnsureInitialized();
    project_logging::SetLevel(spdlog::level::info);

    ffmpeg_video_infer::VideoInferOptions options;
    options.model_path = DefaultModelPath();
    options.output_dir = DefaultOutputDir();
    options.result_json_path = options.output_dir / "result_udp_packets.jsonl";

    bool input_url_overridden = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        constexpr const char* kVideoFrequencyPrefix = "--videofrequency=";
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "--display") {
            options.enable_display = true;
            continue;
        }
        if (arg == "--no-display") {
            options.enable_display = false;
            continue;
        }
        if (arg == "--save") {
            options.save_result = true;
            continue;
        }
        if (arg == "--savejson") {
            options.save_result_json = true;
            continue;
        }
        if (arg == "--disable_result_udp") {
            options.enable_result_udp = false;
            continue;
        }
        if (arg == "--result_udp_ip" && i + 1 < argc) {
            options.result_udp_ip = argv[++i];
            options.enable_result_udp = true;
            continue;
        }
        if (arg == "--result_udp_port" && i + 1 < argc) {
            try {
                options.result_udp_port = std::stoi(argv[++i]);
            } catch (const std::exception&) {
                project_logging::Error("参数 --result_udp_port 的值无效。");
                return 1;
            }
            options.enable_result_udp = true;
            continue;
        }
        if (arg == "--videofrequency" && i + 1 < argc) {
            try {
                options.video_frequency_hz = std::stoi(argv[++i]);
            } catch (const std::exception&) {
                project_logging::Error("参数 --videofrequency 的值无效。");
                return 1;
            }
            if (options.video_frequency_hz <= 0) {
                project_logging::Error("参数 --videofrequency 必须是正整数。");
                return 1;
            }
            continue;
        }
        if (arg.rfind(kVideoFrequencyPrefix, 0) == 0) {
            try {
                options.video_frequency_hz = std::stoi(arg.substr(std::string(kVideoFrequencyPrefix).size()));
            } catch (const std::exception&) {
                project_logging::Error("参数 --videofrequency 的值无效。");
                return 1;
            }
            if (options.video_frequency_hz <= 0) {
                project_logging::Error("参数 --videofrequency 必须是正整数。");
                return 1;
            }
            continue;
        }
        if (arg == "--model" && i + 1 < argc) {
            options.model_path = argv[++i];
            continue;
        }
        if (arg == "--output_dir" && i + 1 < argc) {
            options.output_dir = argv[++i];
            options.result_json_path = options.output_dir / "result_udp_packets.jsonl";
            continue;
        }
        if (!input_url_overridden) {
            options.input_url = arg;
            input_url_overridden = true;
            continue;
        }

        project_logging::Error("未知参数: {}", arg);
        PrintUsage(argv[0]);
        return 1;
    }

    ffmpeg_video_infer::VideoInferApp app(options);
    g_app = &app;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    if (!app.Initialize()) {
        project_logging::Error("初始化失败: {}", app.LastError());
        g_app = nullptr;
        return 1;
    }

    const int exit_code = app.Run();
    if (exit_code != 0 && !app.LastError().empty()) {
        project_logging::Error("运行失败: {}", app.LastError());
    }

    g_app = nullptr;
    return exit_code;
}
