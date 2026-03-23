#include "sender/ffmpeg_command_builder.hpp"

#include <sstream>
#include <vector>

namespace
{

std::wstring QuoteCommandLineArg(const std::wstring& value, const bool force_quote = false)
{
    const bool needs_quotes = force_quote || value.empty() ||
        (value.find_first_of(L" \t\n\v\"") != std::wstring::npos);
    if (!needs_quotes)
    {
        return value;
    }

    std::wstring quoted;
    quoted.push_back(L'"');

    std::size_t backslash_count = 0;
    for (const wchar_t ch : value)
    {
        if (ch == L'\\')
        {
            ++backslash_count;
            continue;
        }

        if (ch == L'"')
        {
            quoted.append(backslash_count * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslash_count = 0;
            continue;
        }

        if (backslash_count > 0)
        {
            quoted.append(backslash_count, L'\\');
            backslash_count = 0;
        }

        quoted.push_back(ch);
    }

    if (backslash_count > 0)
    {
        quoted.append(backslash_count * 2, L'\\');
    }

    quoted.push_back(L'"');
    return quoted;
}

} // namespace

std::wstring BuildFfmpegFilterComplex(const FfmpegSenderOptions& options)
{
    std::wostringstream stream;
    stream << L"ddagrab=output_idx=" << options.output_idx
           << L":framerate=" << options.framerate
           << L":video_size=" << options.crop_width << L"x" << options.crop_height
           << L":offset_x=" << options.offset_x
           << L":offset_y=" << options.offset_y;
    return stream.str();
}

std::wstring BuildFfmpegUdpUrl(const FfmpegSenderOptions& options)
{
    std::wostringstream stream;
    stream << L"udp://" << options.output_ip
           << L":" << options.output_port
           << L"?pkt_size=" << options.pkt_size
           << L"&buffer_size=" << options.udp_buffer_size;
    return stream.str();
}

std::wstring BuildFfmpegCommandLine(const FfmpegSenderOptions& options)
{
    const std::wstring filter_complex = BuildFfmpegFilterComplex(options);
    const std::wstring udp_url = BuildFfmpegUdpUrl(options);
    std::wostringstream gop_stream;
    gop_stream << options.gop;

    struct CommandArg
    {
        std::wstring value;
        bool force_quote = false;
    };

    const std::vector<CommandArg> args = {
        { options.ffmpeg_path, true },
        { L"-hide_banner" },
        { L"-loglevel" },
        { L"warning" },
        { L"-filter_complex" },
        { filter_complex, true },
        { L"-an" },
        { L"-c:v" },
        { L"h264_nvenc" },
        { L"-rc" },
        { L"cbr" },
        { L"-tune" },
        { L"ll" },
        { L"-multipass" },
        { L"disabled" },
        { L"-b:v" },
        { options.bitrate },
        { L"-maxrate" },
        { options.maxrate },
        { L"-bufsize" },
        { options.bufsize },
        { L"-g" },
        { gop_stream.str() },
        { L"-forced-idr" },
        { L"1" },
        { L"-bf" },
        { L"0" },
        { L"-rc-lookahead" },
        { L"0" },
        { L"-delay" },
        { L"0" },
        { L"-zerolatency" },
        { L"1" },
        { L"-flush_packets" },
        { L"1" },
        { L"-muxdelay" },
        { L"0" },
        { L"-muxpreload" },
        { L"0" },
        { L"-bsf:v" },
        { L"dump_extra" },
        { L"-f" },
        { L"mpegts" },
        { udp_url, true },
    };

    std::wostringstream command_line;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        if (i != 0)
        {
            command_line << L' ';
        }
        command_line << QuoteCommandLineArg(args[i].value, args[i].force_quote);
    }

    return command_line.str();
}
