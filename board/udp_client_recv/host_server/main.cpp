#include <windows.h>

#include <conio.h>

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct FfmpegConfig {
    // Default to the ffmpeg build already installed on this machine.
    std::wstring ffmpeg_path =
        L"C:\\Users\\jiahao\\AppData\\Local\\Microsoft\\WinGet\\Packages\\"
        L"Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\\"
        L"ffmpeg-8.1-full_build\\bin\\ffmpeg.exe";
    std::wstring output_ip = L"192.168.7.2";
    uint16_t output_port = 5000;
    int output_idx = 0;
    int framerate = 60;
    int crop_width = 640;
    int crop_height = 640;
    int offset_x = 960;
    int offset_y = 400;
    std::wstring bitrate = L"4M";
    std::wstring maxrate = L"4M";
    std::wstring bufsize = L"150k";
    int gop = 10;
    int pkt_size = 188;
    int udp_buffer_size = 1048576;
};

struct ChildProcess {
    PROCESS_INFORMATION process_info{};
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;
};

namespace {

volatile LONG g_stop_requested = 0;
CRITICAL_SECTION g_log_lock;

void RequestStop() {
    InterlockedExchange(&g_stop_requested, 1);
}

bool IsStopRequested() {
    return InterlockedCompareExchange(&g_stop_requested, 0, 0) != 0;
}

void PrintWindowsError(const std::wstring& context) {
    const DWORD error_code = GetLastError();
    LPWSTR message_buffer = nullptr;
    const DWORD message_length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message_buffer),
        0,
        nullptr);

    std::wcerr << L"[error] " << context << L" failed, code=" << error_code;
    if (message_length > 0 && message_buffer != nullptr) {
        std::wcerr << L", message=" << message_buffer;
    } else {
        std::wcerr << L'\n';
    }

    if (message_buffer != nullptr) {
        LocalFree(message_buffer);
    }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD control_type) {
    switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        RequestStop();
        return TRUE;
    default:
        return FALSE;
    }
}

void CloseHandleIfValid(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

std::wstring BuildFilterComplex(const FfmpegConfig& config) {
    std::wostringstream stream;
    stream << L"ddagrab=output_idx=" << config.output_idx
           << L":framerate=" << config.framerate
           << L":video_size=" << config.crop_width << L"x" << config.crop_height
           << L":offset_x=" << config.offset_x
           << L":offset_y=" << config.offset_y;
    return stream.str();
}

std::wstring BuildUdpUrl(const FfmpegConfig& config) {
    std::wostringstream stream;
    stream << L"udp://" << config.output_ip
           << L":" << config.output_port
           << L"?pkt_size=" << config.pkt_size
           << L"&buffer_size=" << config.udp_buffer_size;
    return stream.str();
}

std::wstring QuoteCommandLineArg(const std::wstring& value, bool force_quote = false) {
    const bool needs_quotes = force_quote || value.empty() ||
        (value.find_first_of(L" \t\n\v\"") != std::wstring::npos);
    if (!needs_quotes) {
        return value;
    }

    std::wstring quoted;
    quoted.push_back(L'"');

    size_t backslash_count = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslash_count;
            continue;
        }

        if (ch == L'"') {
            quoted.append(backslash_count * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslash_count = 0;
            continue;
        }

        if (backslash_count > 0) {
            quoted.append(backslash_count, L'\\');
            backslash_count = 0;
        }

        quoted.push_back(ch);
    }

    if (backslash_count > 0) {
        quoted.append(backslash_count * 2, L'\\');
    }

    quoted.push_back(L'"');
    return quoted;
}

std::wstring BuildCommandLine(const FfmpegConfig& config) {
    const std::wstring filter_complex = BuildFilterComplex(config);
    const std::wstring udp_url = BuildUdpUrl(config);
    std::wostringstream gop_stream;
    gop_stream << config.gop;

    struct CommandArg {
        std::wstring value;
        bool force_quote = false;
    };

    const std::vector<CommandArg> args = {
        {config.ffmpeg_path, true},
        {L"-hide_banner"},
        {L"-loglevel"},
        {L"warning"},
        {L"-filter_complex"},
        {filter_complex, true},
        {L"-an"},
        {L"-c:v"},
        {L"h264_nvenc"},
        {L"-rc"},
        {L"cbr"},
        {L"-tune"},
        {L"ll"},
        {L"-multipass"},
        {L"disabled"},
        {L"-b:v"},
        {config.bitrate},
        {L"-maxrate"},
        {config.maxrate},
        {L"-bufsize"},
        {config.bufsize},
        {L"-g"},
        {gop_stream.str()},
        // For late-join UDP TS receivers, make keyframes IDR and repeat SPS/PPS on them.
        {L"-forced-idr"},
        {L"1"},
        {L"-bf"},
        {L"0"},
        {L"-rc-lookahead"},
        {L"0"},
        {L"-delay"},
        {L"0"},
        {L"-zerolatency"},
        {L"1"},
        {L"-flush_packets"},
        {L"1"},
        {L"-muxdelay"},
        {L"0"},
        {L"-muxpreload"},
        {L"0"},
        {L"-bsf:v"},
        {L"dump_extra"},
        {L"-f"},
        {L"mpegts"},
        {udp_url, true},
    };

    std::wostringstream command_line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            command_line << L' ';
        }
        command_line << QuoteCommandLineArg(args[i].value, args[i].force_quote);
    }

    return command_line.str();
}

bool CreateChildPipe(HANDLE& read_handle, HANDLE& write_handle) {
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    if (!CreatePipe(&read_handle, &write_handle, &security_attributes, 0)) {
        PrintWindowsError(L"CreatePipe");
        return false;
    }

    if (!SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0)) {
        PrintWindowsError(L"SetHandleInformation");
        CloseHandleIfValid(read_handle);
        CloseHandleIfValid(write_handle);
        return false;
    }

    return true;
}

HANDLE OpenNullInputHandle() {
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE null_handle = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security_attributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (null_handle == INVALID_HANDLE_VALUE) {
        PrintWindowsError(L"CreateFileW(NUL)");
        return nullptr;
    }

    return null_handle;
}

void PrintLogChunk(const char* prefix, const std::string& chunk) {
    if (chunk.empty()) {
        return;
    }

    EnterCriticalSection(&g_log_lock);
    std::cout << prefix << chunk;
    if (chunk.back() != '\n') {
        std::cout << '\n';
    }
    std::cout.flush();
    LeaveCriticalSection(&g_log_lock);
}

void ReadPipeToConsole(HANDLE pipe, const char* prefix) {
    constexpr DWORD kBufferSize = 4096;
    char buffer[kBufferSize];
    std::string pending;

    for (;;) {
        DWORD bytes_read = 0;
        const BOOL ok = ReadFile(pipe, buffer, kBufferSize, &bytes_read, nullptr);
        if (!ok || bytes_read == 0) {
            break;
        }

        pending.append(buffer, buffer + bytes_read);

        size_t newline_pos = std::string::npos;
        while ((newline_pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline_pos + 1);
            pending.erase(0, newline_pos + 1);
            PrintLogChunk(prefix, line);
        }
    }

    if (!pending.empty()) {
        PrintLogChunk(prefix, pending);
    }
}

struct PipeReaderContext {
    HANDLE pipe = nullptr;
    const char* prefix = nullptr;
};

DWORD WINAPI PipeReaderThreadProc(LPVOID parameter) {
    PipeReaderContext* context = static_cast<PipeReaderContext*>(parameter);
    if (context != nullptr) {
        ReadPipeToConsole(context->pipe, context->prefix);
    }
    return 0;
}

bool StartFfmpegProcess(const FfmpegConfig& config, ChildProcess& child, std::wstring& command_line) {
    HANDLE stdout_write = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE null_input = nullptr;

    if (!CreateChildPipe(child.stdout_read, stdout_write)) {
        return false;
    }

    if (!CreateChildPipe(child.stderr_read, stderr_write)) {
        CloseHandleIfValid(child.stdout_read);
        CloseHandleIfValid(stdout_write);
        return false;
    }

    null_input = OpenNullInputHandle();
    if (null_input == nullptr) {
        CloseHandleIfValid(child.stdout_read);
        CloseHandleIfValid(stdout_write);
        CloseHandleIfValid(child.stderr_read);
        CloseHandleIfValid(stderr_write);
        return false;
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = null_input;
    startup_info.hStdOutput = stdout_write;
    startup_info.hStdError = stderr_write;

    command_line = BuildCommandLine(config);
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    // Create a new process group so CTRL_BREAK_EVENT can target ffmpeg for graceful shutdown.
    const DWORD creation_flags = CREATE_NEW_PROCESS_GROUP;
    const BOOL created = CreateProcessW(
        nullptr,
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        creation_flags,
        nullptr,
        nullptr,
        &startup_info,
        &child.process_info);

    CloseHandleIfValid(stdout_write);
    CloseHandleIfValid(stderr_write);
    CloseHandleIfValid(null_input);

    if (!created) {
        PrintWindowsError(L"CreateProcessW");
        CloseHandleIfValid(child.stdout_read);
        CloseHandleIfValid(child.stderr_read);
        return false;
    }

    return true;
}

bool WaitForProcessExit(HANDLE process_handle, DWORD timeout_ms) {
    const DWORD wait_result = WaitForSingleObject(process_handle, timeout_ms);
    if (wait_result == WAIT_OBJECT_0) {
        return true;
    }
    if (wait_result == WAIT_TIMEOUT) {
        return false;
    }

    PrintWindowsError(L"WaitForSingleObject");
    return false;
}

void StopFfmpegProcess(ChildProcess& child, DWORD graceful_timeout_ms) {
    if (child.process_info.hProcess == nullptr) {
        return;
    }

    if (WaitForProcessExit(child.process_info.hProcess, 0)) {
        return;
    }

    // Ask ffmpeg to exit cleanly first, then fall back to hard termination if needed.
    if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, child.process_info.dwProcessId)) {
        PrintWindowsError(L"GenerateConsoleCtrlEvent");
    } else if (WaitForProcessExit(child.process_info.hProcess, graceful_timeout_ms)) {
        return;
    }

    std::wcerr << L"[warning] ffmpeg did not exit after CTRL_BREAK_EVENT, forcing termination.\n";
    if (!TerminateProcess(child.process_info.hProcess, 1)) {
        PrintWindowsError(L"TerminateProcess");
        return;
    }

    WaitForProcessExit(child.process_info.hProcess, 5000);
}

void CleanupChildProcess(ChildProcess& child) {
    CloseHandleIfValid(child.stdout_read);
    CloseHandleIfValid(child.stderr_read);
    CloseHandleIfValid(child.process_info.hThread);
    CloseHandleIfValid(child.process_info.hProcess);
}

void PrintConfigSummary(const FfmpegConfig& config) {
    std::wcout << L"Capture: " << config.crop_width << L"x" << config.crop_height
               << L", framerate=" << config.framerate
               << L", offset=(" << config.offset_x << L"," << config.offset_y << L")\n";
    std::wcout << L"Output : udp://" << config.output_ip << L":" << config.output_port
               << L", pkt_size=" << config.pkt_size
               << L", buffer_size=" << config.udp_buffer_size << L"\n";
    std::wcout << L"Encoder: h264_nvenc, bitrate=" << config.bitrate
               << L", maxrate=" << config.maxrate
               << L", bufsize=" << config.bufsize
               << L", gop=" << config.gop << L"\n";
}

} // namespace

int wmain() {
    FfmpegConfig config;
    InitializeCriticalSection(&g_log_lock);

    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        PrintWindowsError(L"SetConsoleCtrlHandler");
        DeleteCriticalSection(&g_log_lock);
        return 1;
    }

    PrintConfigSummary(config);

    ChildProcess child;
    std::wstring command_line;
    if (!StartFfmpegProcess(config, child, command_line)) {
        DeleteCriticalSection(&g_log_lock);
        return 1;
    }

    std::wcout << L"Command: " << command_line << L"\n";
    std::wcout << L"ffmpeg started. Press 'q' to stop, or Ctrl+C to stop.\n";

    PipeReaderContext stdout_context{child.stdout_read, "[ffmpeg stdout] "};
    PipeReaderContext stderr_context{child.stderr_read, "[ffmpeg stderr] "};
    HANDLE stdout_thread = CreateThread(nullptr, 0, PipeReaderThreadProc, &stdout_context, 0, nullptr);
    HANDLE stderr_thread = CreateThread(nullptr, 0, PipeReaderThreadProc, &stderr_context, 0, nullptr);
    if (stdout_thread == nullptr || stderr_thread == nullptr) {
        PrintWindowsError(L"CreateThread");
        RequestStop();
    }

    bool stop_sent = false;
    DWORD process_exit_code = STILL_ACTIVE;

    for (;;) {
        const DWORD wait_result = WaitForSingleObject(child.process_info.hProcess, 100);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (wait_result == WAIT_FAILED) {
            PrintWindowsError(L"WaitForSingleObject");
            RequestStop();
        }

        if (_kbhit()) {
            const int key = _getch();
            if (key == 'q' || key == 'Q') {
                RequestStop();
            }
        }

        // A stop request can come from q, Ctrl+C, console close, or an internal failure path.
        if (IsStopRequested() && !stop_sent) {
            std::wcout << L"Stopping ffmpeg...\n";
            StopFfmpegProcess(child, 5000);
            stop_sent = true;
        }
    }

    if (!GetExitCodeProcess(child.process_info.hProcess, &process_exit_code)) {
        PrintWindowsError(L"GetExitCodeProcess");
        process_exit_code = 1;
    }

    if (stdout_thread != nullptr) {
        WaitForSingleObject(stdout_thread, INFINITE);
        CloseHandle(stdout_thread);
    }
    if (stderr_thread != nullptr) {
        WaitForSingleObject(stderr_thread, INFINITE);
        CloseHandle(stderr_thread);
    }

    CleanupChildProcess(child);
    DeleteCriticalSection(&g_log_lock);

    std::wcout << L"ffmpeg exited with code " << process_exit_code << L".\n";
    return static_cast<int>(process_exit_code);
}
