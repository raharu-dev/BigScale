// BigScale — file logging for the native bridge.
#pragma once

#include <string>

namespace bigscale
{
// Absolute directory that receives native.log. Set once from BIGSCALE_Initialize.
void SetLogDirectory(const std::wstring& directory);
// Turns native.log on or off. Disabling closes the open handle and keeps the remembered path, so a
// later enable reopens it; enabling before SetLogDirectory is remembered and applied there.
void SetLogEnabled(bool enabled);
// The same directory without its trailing separator; empty before SetLogDirectory.
std::wstring LogDirectory();
void Log(const char* format, ...);
}
