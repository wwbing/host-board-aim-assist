#pragma once

#include <Windows.h>

#include <string>

namespace logging
{

void Initialize();
std::string FormatWin32Error(const std::wstring& action, DWORD error_code = GetLastError());
std::string FormatSocketError(const std::string& action, int error_code);
std::string FormatLastSocketError(const std::string& action);

} // namespace logging
