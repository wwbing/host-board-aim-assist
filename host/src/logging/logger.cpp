#include "logging/logger.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <Winsock2.h>

#include <mutex>
#include <string>

namespace
{

std::once_flag g_logging_init_flag;

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    const int required_size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required_size <= 1)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(required_size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), required_size, nullptr, nullptr);
    result.pop_back();
    return result;
}

} // namespace

namespace logging
{

void Initialize()
{
    std::call_once(g_logging_init_flag, []()
    {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        auto logger = spdlog::stdout_color_mt("console");
        logger->set_pattern("[%H:%M:%S.%e] %v");
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
    });
}

std::string FormatWin32Error(const std::wstring& action, const DWORD error_code)
{
    LPWSTR message_buffer = nullptr;
    const DWORD message_length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message_buffer),
        0,
        nullptr);

    std::string message = WideToUtf8(action) + " 失败, error_code=" + std::to_string(error_code);
    if (message_length > 0 && message_buffer != nullptr)
    {
        std::wstring windows_message(message_buffer, message_length);
        while (!windows_message.empty() &&
               (windows_message.back() == L'\r' || windows_message.back() == L'\n'))
        {
            windows_message.pop_back();
        }
        message += ", system_message=" + WideToUtf8(windows_message);
    }

    if (message_buffer != nullptr)
    {
        LocalFree(message_buffer);
    }

    return message;
}

std::string FormatSocketError(const std::string& action, const int error_code)
{
    return action + " 失败, WSA_error=" + std::to_string(error_code);
}

std::string FormatLastSocketError(const std::string& action)
{
    return FormatSocketError(action, WSAGetLastError());
}

} // namespace logging
