#pragma once

#include <Windows.h>

#include <string>
#include <thread>

class FfmpegProcess
{
public:
    FfmpegProcess() = default;
    ~FfmpegProcess();

    FfmpegProcess(const FfmpegProcess&) = delete;
    FfmpegProcess& operator=(const FfmpegProcess&) = delete;

    bool Start(const std::wstring& command_line, std::string& error_message);
    void Stop(DWORD graceful_timeout_ms = 5000);
    bool IsRunning();
    DWORD ExitCode();
    const std::string& LastError() const;

    static void CloseHandleIfValid(HANDLE& handle);

private:
    void ReadPipeLoop(HANDLE pipe, const char* prefix);
    void JoinPipeThreads();
    void CleanupHandles();
    void UpdateExitCode();

private:
    PROCESS_INFORMATION process_info_ = {};
    HANDLE stdout_read_ = nullptr;
    HANDLE stderr_read_ = nullptr;
    std::thread stdout_thread_;
    std::thread stderr_thread_;
    std::string last_error_;
    DWORD exit_code_ = STILL_ACTIVE;
};
