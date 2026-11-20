#include "Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace
{
std::mutex g_mutex;
// One open handle for the process lifetime: a per-line fopen/fclose costs two syscalls for every
// message the bridge writes on a frame path.
FILE* g_file = nullptr;
std::wstring g_logPath;
std::wstring g_directory;

// 16 MiB, matching the rotation the managed side documents.
constexpr long long kRotateBytes = 16ll * 1024 * 1024;

void CloseFile()
{
    if (g_file == nullptr) return;
    std::fclose(g_file);
    g_file = nullptr;
}

bool OpenFile(const std::wstring& path)
{
    CloseFile();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || file == nullptr) return false;
    g_file = file;
    return true;
}

// Keeps the log bounded without ever dropping the newest lines: the previous file is moved aside and
// a fresh one is opened.
void RotateIfNeeded(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0) return;
    const long long size = (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (size < kRotateBytes) return;
    CloseFile();
    const std::wstring previous = path + L".1";
    MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
}
} // namespace

namespace bigscale
{
void SetLogDirectory(const std::wstring& directory)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    CloseFile();
    g_directory = directory;
    if (!g_directory.empty() && (g_directory.back() == L'\\' || g_directory.back() == L'/'))
        g_directory.pop_back();
    if (directory.empty())
    {
        g_logPath.clear();
        return;
    }
    g_logPath = g_directory;
    g_logPath += L"\\native.log";
    RotateIfNeeded(g_logPath);
    // A failure here is silent: the manager still runs, and the next SetLogDirectory retries.
    OpenFile(g_logPath);
}

std::wstring LogDirectory()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_directory;
}

void Log(const char* format, ...)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_file == nullptr) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::fprintf(g_file, "%02u:%02u:%02u.%03u ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    va_list args;
    va_start(args, format);
    std::vfprintf(g_file, format, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}
} // namespace bigscale
