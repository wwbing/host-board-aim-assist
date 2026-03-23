#include "sender/ffmpeg_process.hpp"
#include "logging/logger.hpp"

#include <string>
#include <vector>

namespace
{

std::string FormatWindowsErrorMessage(const std::wstring& context)
{
    return logging::FormatWin32Error(context);
}

bool CreateChildPipe(HANDLE& read_handle, HANDLE& write_handle, std::string& error_message)
{
    SECURITY_ATTRIBUTES security_attributes = {};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    if (!CreatePipe(&read_handle, &write_handle, &security_attributes, 0))
    {
        error_message = FormatWindowsErrorMessage(L"创建 ffmpeg 子进程管道(CreatePipe)");
        return false;
    }

    if (!SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0))
    {
        error_message = FormatWindowsErrorMessage(L"设置管道句柄继承属性(SetHandleInformation)");
        FfmpegProcess::CloseHandleIfValid(read_handle);
        FfmpegProcess::CloseHandleIfValid(write_handle);
        return false;
    }

    return true;
}

HANDLE OpenNullInputHandle(std::string& error_message)
{
    SECURITY_ATTRIBUTES security_attributes = {};
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

    if (null_handle == INVALID_HANDLE_VALUE)
    {
        error_message = FormatWindowsErrorMessage(L"打开空输入句柄(CreateFileW)");
        return nullptr;
    }

    return null_handle;
}

void DiscardLogChunk(const char* prefix, const std::string& chunk)
{
    (void)prefix;
    (void)chunk;
}

} // namespace

FfmpegProcess::~FfmpegProcess()
{
    Stop();
}

bool FfmpegProcess::Start(const std::wstring& command_line, std::string& error_message)
{
    Stop();

    HANDLE stdout_write = nullptr;
    HANDLE stderr_write = nullptr;
    std::string local_error;

    if (!CreateChildPipe(stdout_read_, stdout_write, local_error))
    {
        last_error_ = local_error;
        error_message = last_error_;
        return false;
    }

    if (!CreateChildPipe(stderr_read_, stderr_write, local_error))
    {
        last_error_ = local_error;
        error_message = last_error_;
        CloseHandleIfValid(stdout_read_);
        CloseHandleIfValid(stdout_write);
        return false;
    }

    HANDLE null_input = OpenNullInputHandle(local_error);
    if (null_input == nullptr)
    {
        last_error_ = local_error;
        error_message = last_error_;
        CloseHandleIfValid(stdout_read_);
        CloseHandleIfValid(stdout_write);
        CloseHandleIfValid(stderr_read_);
        CloseHandleIfValid(stderr_write);
        return false;
    }

    STARTUPINFOW startup_info = {};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = null_input;
    startup_info.hStdOutput = stdout_write;
    startup_info.hStdError = stderr_write;

    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    const BOOL created = CreateProcessW(
        nullptr,
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NEW_PROCESS_GROUP,
        nullptr,
        nullptr,
        &startup_info,
        &process_info_);

    CloseHandleIfValid(stdout_write);
    CloseHandleIfValid(stderr_write);
    CloseHandleIfValid(null_input);

    if (!created)
    {
        last_error_ = FormatWindowsErrorMessage(L"启动 ffmpeg 进程(CreateProcessW)");
        error_message = last_error_;
        CleanupHandles();
        return false;
    }

    exit_code_ = STILL_ACTIVE;
    last_error_.clear();
    stdout_thread_ = std::thread(&FfmpegProcess::ReadPipeLoop, this, stdout_read_, "[ffmpeg stdout] ");
    stderr_thread_ = std::thread(&FfmpegProcess::ReadPipeLoop, this, stderr_read_, "[ffmpeg stderr] ");
    return true;
}

void FfmpegProcess::Stop(const DWORD graceful_timeout_ms)
{
    if (process_info_.hProcess == nullptr)
    {
        JoinPipeThreads();
        CleanupHandles();
        return;
    }

    if (IsRunning())
    {
        if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process_info_.dwProcessId))
        {
            last_error_ = FormatWindowsErrorMessage(L"向 ffmpeg 发送 CTRL_BREAK(GenerateConsoleCtrlEvent)");
            if (!TerminateProcess(process_info_.hProcess, 1))
            {
                last_error_ = FormatWindowsErrorMessage(L"强制结束 ffmpeg 进程(TerminateProcess)");
            }
            else
            {
                WaitForSingleObject(process_info_.hProcess, 5000);
            }
        }
        else
        {
            const DWORD wait_result = WaitForSingleObject(process_info_.hProcess, graceful_timeout_ms);
            if (wait_result == WAIT_TIMEOUT)
            {
                if (!TerminateProcess(process_info_.hProcess, 1))
                {
                    last_error_ = FormatWindowsErrorMessage(L"强制结束 ffmpeg 进程(TerminateProcess)");
                }
                else
                {
                    WaitForSingleObject(process_info_.hProcess, 5000);
                }
            }
            else if (wait_result == WAIT_FAILED)
            {
                last_error_ = FormatWindowsErrorMessage(L"等待 ffmpeg 进程退出(WaitForSingleObject)");
                if (!TerminateProcess(process_info_.hProcess, 1))
                {
                    last_error_ = FormatWindowsErrorMessage(L"强制结束 ffmpeg 进程(TerminateProcess)");
                }
                else
                {
                    WaitForSingleObject(process_info_.hProcess, 5000);
                }
            }
        }
    }

    UpdateExitCode();
    JoinPipeThreads();
    CleanupHandles();
}

bool FfmpegProcess::IsRunning()
{
    if (process_info_.hProcess == nullptr)
    {
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(process_info_.hProcess, 0);
    if (wait_result == WAIT_TIMEOUT)
    {
        return true;
    }
    if (wait_result == WAIT_OBJECT_0)
    {
        UpdateExitCode();
        return false;
    }

    last_error_ = FormatWindowsErrorMessage(L"查询 ffmpeg 进程状态(WaitForSingleObject)");
    return false;
}

DWORD FfmpegProcess::ExitCode()
{
    UpdateExitCode();
    return exit_code_;
}

const std::string& FfmpegProcess::LastError() const
{
    return last_error_;
}

void FfmpegProcess::CloseHandleIfValid(HANDLE& handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle);
        handle = nullptr;
    }
}

void FfmpegProcess::ReadPipeLoop(HANDLE pipe, const char* prefix)
{
    constexpr DWORD kBufferSize = 4096;
    char buffer[kBufferSize];
    std::string pending;

    for (;;)
    {
        DWORD bytes_read = 0;
        const BOOL ok = ReadFile(pipe, buffer, kBufferSize, &bytes_read, nullptr);
        if (!ok || bytes_read == 0)
        {
            break;
        }

        pending.append(buffer, buffer + bytes_read);
        std::size_t newline_pos = std::string::npos;
        while ((newline_pos = pending.find('\n')) != std::string::npos)
        {
            DiscardLogChunk(prefix, pending.substr(0, newline_pos + 1));
            pending.erase(0, newline_pos + 1);
        }
    }

    if (!pending.empty())
    {
        DiscardLogChunk(prefix, pending);
    }
}

void FfmpegProcess::JoinPipeThreads()
{
    if (stdout_thread_.joinable())
    {
        stdout_thread_.join();
    }
    if (stderr_thread_.joinable())
    {
        stderr_thread_.join();
    }
}

void FfmpegProcess::CleanupHandles()
{
    CloseHandleIfValid(stdout_read_);
    CloseHandleIfValid(stderr_read_);
    CloseHandleIfValid(process_info_.hThread);
    CloseHandleIfValid(process_info_.hProcess);
}

void FfmpegProcess::UpdateExitCode()
{
    if (process_info_.hProcess == nullptr)
    {
        return;
    }

    DWORD exit_code = exit_code_;
    if (GetExitCodeProcess(process_info_.hProcess, &exit_code))
    {
        exit_code_ = exit_code;
    }
    else
    {
        last_error_ = FormatWindowsErrorMessage(L"读取 ffmpeg 退出码(GetExitCodeProcess)");
    }
}
