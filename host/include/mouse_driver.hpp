#pragma once

#include "logging/logger.hpp"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <string>
#include <vector>

class MouseController
{
public:
    MouseController() = default;
    ~MouseController()
    {
        if (library_ != nullptr)
        {
            FreeLibrary(library_);
        }
    }

    MouseController(const MouseController&) = delete;
    MouseController& operator=(const MouseController&) = delete;

    bool Initialize()
    {
        if (library_ != nullptr)
        {
            return move_to_ != nullptr;
        }

        std::vector<std::string> candidates;
        candidates.emplace_back("ddll64.dll");

        char exe_path[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH) > 0)
        {
            const std::string exe_dir = DirnameOf(exe_path);
            candidates.push_back(exe_dir + "\\ddll64.dll");
            candidates.push_back(exe_dir + "\\..\\runtime\\ddll64.dll");
            candidates.push_back(exe_dir + "\\..\\..\\runtime\\ddll64.dll");
        }

        for (const std::string& candidate : candidates)
        {
            library_ = LoadLibraryA(candidate.c_str());
            if (library_ != nullptr)
            {
                break;
            }
        }

        if (library_ == nullptr)
        {
            spdlog::error("[错误] 加载 ddll64.dll 失败");
            return false;
        }

        const auto open_device =
            reinterpret_cast<OpenDeviceFunctionType>(GetProcAddress(library_, "OpenDevice"));
        if (open_device == nullptr || open_device() == 0)
        {
            spdlog::error("[错误] 鼠标设备未就绪，无法打开 ddll64 设备");
            return false;
        }

        move_to_ =
            reinterpret_cast<MoveToFunctionType>(GetProcAddress(library_, "MoveTo"));
        if (move_to_ == nullptr)
        {
            spdlog::error("[错误] ddll64.dll 中缺少 MoveTo 导出");
            return false;
        }

        move_relative_ =
            reinterpret_cast<MoveRelativeFunctionType>(GetProcAddress(library_, "MoveR"));
        return true;
    }

    void MoveTo(const int x, const int y) const
    {
        if (move_to_ != nullptr)
        {
            move_to_(static_cast<unsigned short>(x), static_cast<unsigned short>(y));
        }
    }

    void MoveRelative(const int dx, const int dy) const
    {
        if (move_relative_ != nullptr)
        {
            move_relative_(dx, dy);
        }
    }

    bool SupportsRelativeMove() const
    {
        return move_relative_ != nullptr;
    }

private:
    using MoveToFunctionType = void (*)(unsigned short, unsigned short);
    using MoveRelativeFunctionType = void (*)(int, int);
    using OpenDeviceFunctionType = int (*)();

    static std::string DirnameOf(const std::string& path)
    {
        const std::size_t pos = path.find_last_of("\\/");
        return (pos == std::string::npos) ? "." : path.substr(0, pos);
    }

private:
    HMODULE library_ = nullptr;
    MoveToFunctionType move_to_ = nullptr;
    MoveRelativeFunctionType move_relative_ = nullptr;
};
