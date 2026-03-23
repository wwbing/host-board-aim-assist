#include "app.hpp"
#include "logging/logger.hpp"
#include "types.hpp"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <exception>
#include <iostream>
#include <limits>
#include <string>

namespace
{

void PrintUsage()
{
    std::cout
        << "Usage: remote_box_receiver [options]\n"
        << "Options:\n"
        << "  --listen_ip <ip>\n"
        << "  --listen_port <port>\n"
        << "  --display\n"
        << "  --savejson\n"
        << "  --enable_tracker_state_machine\n"
        << "  --disable_tracker_state_machine\n"
        << "  --enable_aim_motion_filters\n"
        << "  --screen_width <value>\n"
        << "  --screen_height <value>\n"
        << "  --roi_width <value>\n"
        << "  --roi_height <value>\n"
        << "  --roi_offset_x <value>\n"
        << "  --roi_offset_y <value>\n"
        << "  --disable_sender\n"
        << "  --ffmpeg_path <path>\n"
        << "  --sender_output_ip <ip>\n"
        << "  --sender_output_port <port>\n"
        << "  --sender_output_idx <index>\n"
        << "  --sender_framerate <value>\n"
        << "  --videofrequency <value>\n"
        << "  videofrequency=<value>\n"
        << "  --sender_crop_width <value>\n"
        << "  --sender_crop_height <value>\n"
        << "  --sender_offset_x <value>\n"
        << "  --sender_offset_y <value>\n"
        << "  --sender_bitrate <value>\n"
        << "  --sender_maxrate <value>\n"
        << "  --sender_bufsize <value>\n"
        << "  --sender_gop <value>\n"
        << "  --sender_pkt_size <value>\n"
        << "  --sender_udp_buffer_size <value>\n"
        << "  --aim_gain <value>\n"
        << "  --aim_max_step_px <value>\n"
        << "  --aim_update_interval_ms <value>\n"
        << "Hotkey:\n"
        << "  Q Toggle auto-move\n"
        << "  --help\n";
}

bool ParseIntValue(const std::string& text, int& value)
{
    try
    {
        std::size_t parsed_length = 0;
        const long long parsed = std::stoll(text, &parsed_length, 10);
        if (parsed_length != text.size() ||
            parsed < std::numeric_limits<int>::min() ||
            parsed > std::numeric_limits<int>::max())
        {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool ParseDoubleValue(const std::string& text, double& value)
{
    try
    {
        std::size_t parsed_length = 0;
        const double parsed = std::stod(text, &parsed_length);
        if (parsed_length != text.size())
        {
            return false;
        }

        value = parsed;
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool TryParseInlineIntOption(const std::string& argument, const std::string& key, int& destination)
{
    const std::string prefix = key + "=";
    if (argument.rfind(prefix, 0) != 0)
    {
        return false;
    }

    return ParseIntValue(argument.substr(prefix.size()), destination);
}

bool ReadIntOption(const int argc, char* argv[], int& index, int& destination)
{
    if (index + 1 >= argc)
    {
        return false;
    }

    ++index;
    return ParseIntValue(argv[index], destination);
}

bool ReadStringOption(const int argc, char* argv[], int& index, std::string& destination)
{
    if (index + 1 >= argc)
    {
        return false;
    }

    destination = argv[++index];
    return true;
}

bool ReadDoubleOption(const int argc, char* argv[], int& index, double& destination)
{
    if (index + 1 >= argc)
    {
        return false;
    }

    ++index;
    return ParseDoubleValue(argv[index], destination);
}

void EnableDpiAwareness()
{
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    using SetProcessDPIAwareFn = BOOL(WINAPI*)();

    HMODULE user32_module = GetModuleHandleA("user32.dll");
    if (user32_module != nullptr)
    {
        const auto set_process_dpi_awareness_context =
            reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                GetProcAddress(user32_module, "SetProcessDpiAwarenessContext"));
        if (set_process_dpi_awareness_context != nullptr)
        {
            if (set_process_dpi_awareness_context(reinterpret_cast<HANDLE>(-4)))
            {
                return;
            }
        }

        const auto set_process_dpi_aware =
            reinterpret_cast<SetProcessDPIAwareFn>(GetProcAddress(user32_module, "SetProcessDPIAware"));
        if (set_process_dpi_aware != nullptr)
        {
            set_process_dpi_aware();
        }
    }
}

void DetectPrimaryScreenSize(int& width, int& height)
{
    HDC screen_dc = GetDC(nullptr);
    if (screen_dc != nullptr)
    {
        const int desktop_width = GetDeviceCaps(screen_dc, DESKTOPHORZRES);
        const int desktop_height = GetDeviceCaps(screen_dc, DESKTOPVERTRES);
        const int fallback_width = GetDeviceCaps(screen_dc, HORZRES);
        const int fallback_height = GetDeviceCaps(screen_dc, VERTRES);
        ReleaseDC(nullptr, screen_dc);

        width = (desktop_width > 0) ? desktop_width : fallback_width;
        height = (desktop_height > 0) ? desktop_height : fallback_height;
        return;
    }

    width = GetSystemMetrics(SM_CXSCREEN);
    height = GetSystemMetrics(SM_CYSCREEN);
}

void ApplyDetectedScreenDefaults(Config& config)
{
    int detected_width = 0;
    int detected_height = 0;
    DetectPrimaryScreenSize(detected_width, detected_height);

    if (!config.screen_width_overridden)
    {
        config.screen_width = detected_width;
    }
    if (!config.screen_height_overridden)
    {
        config.screen_height = detected_height;
    }

    if (!config.roi_offset_x_overridden)
    {
        config.roi_offset_x = (config.screen_width - config.roi_width) / 2;
    }
    if (!config.roi_offset_y_overridden)
    {
        config.roi_offset_y = (config.screen_height - config.roi_height) / 2;
    }
    if (!config.sender_crop_width_overridden)
    {
        config.sender_crop_width = config.roi_width;
    }
    if (!config.sender_crop_height_overridden)
    {
        config.sender_crop_height = config.roi_height;
    }
    if (!config.sender_offset_x_overridden)
    {
        config.sender_offset_x = config.roi_offset_x;
    }
    if (!config.sender_offset_y_overridden)
    {
        config.sender_offset_y = config.roi_offset_y;
    }
}

enum class ParseResult
{
    kOk,
    kHelp,
    kError
};

ParseResult ParseArguments(const int argc, char* argv[], Config& config)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--help")
        {
            PrintUsage();
            return ParseResult::kHelp;
        }
        if (arg == "--listen_ip")
        {
            if (i + 1 >= argc)
            {
                spdlog::error("[错误] 缺少 --listen_ip 的取值");
                return ParseResult::kError;
            }
            config.listen_ip = argv[++i];
            continue;
        }
        if (arg == "--display")
        {
            config.enable_display = true;
            continue;
        }
        if (arg == "--savejson")
        {
            config.save_json = true;
            continue;
        }
        if (arg == "--enable_tracker_state_machine")
        {
            config.enable_tracker_state_machine = true;
            continue;
        }
        if (arg == "--disable_tracker_state_machine")
        {
            config.enable_tracker_state_machine = false;
            continue;
        }
        if (arg == "--enable_aim_motion_filters")
        {
            config.enable_aim_motion_filters = true;
            continue;
        }
        if (arg == "--disable_sender")
        {
            config.enable_sender = false;
            continue;
        }
        if (arg == "--listen_port")
        {
            if (!ReadIntOption(argc, argv, i, config.listen_port))
            {
                spdlog::error("[错误] --listen_port 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--screen_width")
        {
            if (!ReadIntOption(argc, argv, i, config.screen_width))
            {
                spdlog::error("[错误] --screen_width 取值无效");
                return ParseResult::kError;
            }
            config.screen_width_overridden = true;
            continue;
        }
        if (arg == "--screen_height")
        {
            if (!ReadIntOption(argc, argv, i, config.screen_height))
            {
                spdlog::error("[错误] --screen_height 取值无效");
                return ParseResult::kError;
            }
            config.screen_height_overridden = true;
            continue;
        }
        if (arg == "--roi_width")
        {
            if (!ReadIntOption(argc, argv, i, config.roi_width))
            {
                spdlog::error("[错误] --roi_width 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--roi_height")
        {
            if (!ReadIntOption(argc, argv, i, config.roi_height))
            {
                spdlog::error("[错误] --roi_height 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--roi_offset_x")
        {
            if (!ReadIntOption(argc, argv, i, config.roi_offset_x))
            {
                spdlog::error("[错误] --roi_offset_x 取值无效");
                return ParseResult::kError;
            }
            config.roi_offset_x_overridden = true;
            continue;
        }
        if (arg == "--roi_offset_y")
        {
            if (!ReadIntOption(argc, argv, i, config.roi_offset_y))
            {
                spdlog::error("[错误] --roi_offset_y 取值无效");
                return ParseResult::kError;
            }
            config.roi_offset_y_overridden = true;
            continue;
        }
        if (arg == "--ffmpeg_path")
        {
            if (!ReadStringOption(argc, argv, i, config.ffmpeg_path))
            {
                spdlog::error("[错误] --ffmpeg_path 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--sender_output_ip")
        {
            if (!ReadStringOption(argc, argv, i, config.sender_output_ip))
            {
                spdlog::error("[错误] --sender_output_ip 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--sender_output_port")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_output_port))
            {
                spdlog::error("[错误] --sender_output_port 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--sender_output_idx")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_output_idx))
            {
                spdlog::error("[错误] --sender_output_idx 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--sender_framerate")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_framerate))
            {
                spdlog::error("[错误] --sender_framerate 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--videofrequency")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_framerate))
            {
                spdlog::error("[错误] --videofrequency 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (TryParseInlineIntOption(arg, "videofrequency", config.sender_framerate) ||
            TryParseInlineIntOption(arg, "--videofrequency", config.sender_framerate))
        {
            continue;
        }
        if (arg == "--sender_crop_width")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_crop_width))
            {
                spdlog::error("[错误] --sender_crop_width 取值无效");
                return ParseResult::kError;
            }
            config.sender_crop_width_overridden = true;
            continue;
        }
        if (arg == "--sender_crop_height")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_crop_height))
            {
                spdlog::error("[错误] --sender_crop_height 取值无效");
                return ParseResult::kError;
            }
            config.sender_crop_height_overridden = true;
            continue;
        }
        if (arg == "--sender_offset_x")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_offset_x))
            {
                spdlog::error("[错误] --sender_offset_x 取值无效");
                return ParseResult::kError;
            }
            config.sender_offset_x_overridden = true;
            continue;
        }
        if (arg == "--sender_offset_y")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_offset_y))
            {
                spdlog::error("[错误] --sender_offset_y 取值无效");
                return ParseResult::kError;
            }
            config.sender_offset_y_overridden = true;
            continue;
        }
        if (arg == "--sender_bitrate")
        {
            if (!ReadStringOption(argc, argv, i, config.sender_bitrate))
            {
                spdlog::error("[错误] --sender_bitrate 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--sender_maxrate")
        {
            if (!ReadStringOption(argc, argv, i, config.sender_maxrate))
            {
                spdlog::error("[错误] --sender_maxrate 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--sender_bufsize")
        {
            if (!ReadStringOption(argc, argv, i, config.sender_bufsize))
            {
                spdlog::error("[错误] --sender_bufsize 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--sender_gop")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_gop))
            {
                spdlog::error("[错误] --sender_gop 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--sender_pkt_size")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_pkt_size))
            {
                spdlog::error("[错误] --sender_pkt_size 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--sender_udp_buffer_size")
        {
            if (!ReadIntOption(argc, argv, i, config.sender_udp_buffer_size))
            {
                spdlog::error("[错误] --sender_udp_buffer_size 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--aim_gain")
        {
            if (!ReadDoubleOption(argc, argv, i, config.aim_gain))
            {
                spdlog::error("[错误] --aim_gain 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--aim_max_step_px")
        {
            if (!ReadDoubleOption(argc, argv, i, config.aim_max_step_px))
            {
                spdlog::error("[错误] --aim_max_step_px 取值无效");
                return ParseResult::kError;
            }
            continue;
        }
        if (arg == "--aim_update_interval_ms")
        {
            if (!ReadIntOption(argc, argv, i, config.aim_update_interval_ms))
            {
                spdlog::error("[错误] --aim_update_interval_ms 取值无效");
                return ParseResult::kError;
            }
            continue;
        }

        spdlog::error("[错误] 未知参数: {}", arg);
        return ParseResult::kError;
    }

    return ParseResult::kOk;
}

} // namespace

int main(int argc, char* argv[])
{
    EnableDpiAwareness();
    logging::Initialize();

    Config config;
    const ParseResult parse_result = ParseArguments(argc, argv, config);
    if (parse_result == ParseResult::kHelp)
    {
        return 0;
    }
    if (parse_result != ParseResult::kOk)
    {
        return 1;
    }

    ApplyDetectedScreenDefaults(config);

    if (config.listen_port <= 0 || config.listen_port > 65535)
    {
        spdlog::error("[错误] listen_port 必须在 [1, 65535] 范围内");
        return 1;
    }
    if (config.screen_width <= 0 || config.screen_height <= 0 ||
        config.roi_width <= 0 || config.roi_height <= 0)
    {
        spdlog::error("[错误] screen 和 ROI 尺寸必须为正数");
        return 1;
    }
    if (config.enable_sender &&
        (config.sender_output_port <= 0 || config.sender_output_port > 65535))
    {
        spdlog::error("[错误] sender_output_port 必须在 [1, 65535] 范围内");
        return 1;
    }
    if (config.enable_sender &&
        (config.sender_crop_width <= 0 || config.sender_crop_height <= 0 ||
         config.sender_framerate <= 0))
    {
        spdlog::error("[错误] sender crop 尺寸和 framerate 必须为正数");
        return 1;
    }
    if (config.enable_clock_offset &&
        (config.clock_offset_port <= 0 || config.clock_offset_port > 65535))
    {
        spdlog::error("[错误] clock_offset_port 必须在 [1, 65535] 范围内");
        return 1;
    }
    if (config.sender_pts_per_second <= 0.0)
    {
        spdlog::error("[错误] sender_pts_per_second 必须为正数");
        return 1;
    }
    if (config.aim_update_interval_ms < 0)
    {
        spdlog::error("[错误] aim_update_interval_ms 不能小于 0");
        return 1;
    }

    App app(config);
    return app.Run();
}
