// BigScale — file logging for the native bridge.
#pragma once

#include <string>

namespace bigscale
{
// Absolute directory that receives native.log. Set once from BIGSCALE_Initialize.
void SetLogDirectory(const std::wstring& directory);
// The same directory without its trailing separator; empty before SetLogDirectory.
std::wstring LogDirectory();
void Log(const char* format, ...);
}
