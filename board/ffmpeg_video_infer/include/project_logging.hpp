#pragma once

#include <mutex>
#include <string_view>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace project_logging {

inline void EnsureInitialized() {
    static std::once_flag once;
    std::call_once(once, []() {
        auto logger = spdlog::stderr_color_mt("project_console");
        logger->set_pattern("[%H:%M:%S.%e] %v");
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
    });
}

inline void SetLevel(spdlog::level::level_enum level) {
    EnsureInitialized();
    spdlog::set_level(level);
}

inline void Info(std::string_view message) {
    EnsureInitialized();
    spdlog::info("[信息] {}", message);
}

inline void Warn(std::string_view message) {
    EnsureInitialized();
    spdlog::warn("[警告] {}", message);
}

inline void Error(std::string_view message) {
    EnsureInitialized();
    spdlog::error("[错误] {}", message);
}

template <typename... Args>
inline void Info(fmt::format_string<Args...> format, Args&&... args) {
    EnsureInitialized();
    spdlog::info("[信息] {}", fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Warn(fmt::format_string<Args...> format, Args&&... args) {
    EnsureInitialized();
    spdlog::warn("[警告] {}", fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Error(fmt::format_string<Args...> format, Args&&... args) {
    EnsureInitialized();
    spdlog::error("[错误] {}", fmt::format(format, std::forward<Args>(args)...));
}

}  // namespace project_logging
