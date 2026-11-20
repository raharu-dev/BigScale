#include "FgCapture.h"

#include "../Log.h"
#include "FgReadback.h"

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{
constexpr DWORD kReadbackTimeoutMilliseconds = 5000;
// Five source frames are the diagnostic unit; a larger arm would only grow the queue.
constexpr uint32_t kMaxArmedPairs = 32;

struct Job
{
    uint32_t index = 0;
    std::wstring directory;
    std::string provider;
    uint32_t generation = 0;
    int32_t sourceFrame = 0;
    uint64_t presentId = 0;
    uint32_t flags = 0;
    bool reset = false;
    uint32_t intermediateIndex = 1;
    uint32_t intermediateCount = 1;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float deltaMilliseconds = 0.0f;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float verticalFovRadians = 0.0f;
    float aspect = 0.0f;
    float cameraPosition[3]{};
    float cameraUp[3]{};
    float cameraRight[3]{};
    float cameraForward[3]{};
    FgReadbackPlane realColor;
    FgReadbackPlane hudless;
    FgReadbackPlane generated;
    FgReadbackPlane depth;
    FgReadbackPlane motion;
    ComPtr<ID3D12Fence> fence;
    HANDLE completed = nullptr;
    uint64_t fenceValue = 0;
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<Job> g_jobs;
// Jobs whose fence never completed. They are never released: the GPU may still write them.
std::vector<Job> g_retained;
std::wstring g_directory;
std::thread g_writer;
bool g_writerRunning = false;
bool g_stopping = false;
uint32_t g_remaining = 0;
uint32_t g_batchTotal = 0;
uint32_t g_batchWritten = 0;
uint32_t g_nextIndex = 0;
std::atomic<bool> g_armed{false};
std::atomic<bool> g_shutdown{false};

std::string Narrow(const std::wstring& wide)
{
    std::string narrow;
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size > 1)
    {
        narrow.resize(static_cast<size_t>(size - 1));
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, narrow.data(), size, nullptr, nullptr);
    }
    return narrow;
}

void WriteMeta(const std::wstring& path, const Job& job)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr)
    {
        Log("FG capture: could not open the capture metadata for writing");
        return;
    }
    const char* hudlessRoute = (job.flags & BIGSCALE_FG_FLAG_BOUND_RENDER_TARGET) != 0 ? "bound" : "graph";
    std::fprintf(file, "{\n");
    std::fprintf(file, "  \"provider\": \"%s\",\n", job.provider.c_str());
    std::fprintf(file, "  \"generation\": %u,\n", job.generation);
    std::fprintf(file, "  \"sourceFrame\": %d,\n", job.sourceFrame);
    std::fprintf(file, "  \"presentId\": %llu,\n", static_cast<unsigned long long>(job.presentId));
    std::fprintf(file, "  \"flags\": %u,\n", job.flags);
    std::fprintf(file, "  \"reset\": %s,\n", job.reset ? "true" : "false");
    std::fprintf(file, "  \"intermediateIndex\": %u,\n", job.intermediateIndex);
    std::fprintf(file, "  \"intermediateCount\": %u,\n", job.intermediateCount);
    std::fprintf(file, "  \"outputWidth\": %u,\n", job.outputWidth);
    std::fprintf(file, "  \"outputHeight\": %u,\n", job.outputHeight);
    std::fprintf(file, "  \"renderWidth\": %u,\n", job.renderWidth);
    std::fprintf(file, "  \"renderHeight\": %u,\n", job.renderHeight);
    std::fprintf(file, "  \"motionScale\": [%d, %d],\n", -static_cast<int>(job.renderWidth),
        -static_cast<int>(job.renderHeight));
    std::fprintf(file, "  \"jitter\": [%.6f, %.6f],\n", job.jitterX, job.jitterY);
    std::fprintf(file, "  \"deltaMilliseconds\": %.6f,\n", job.deltaMilliseconds);
    std::fprintf(file, "  \"cameraNear\": %.6f,\n", job.cameraNear);
    std::fprintf(file, "  \"cameraFar\": %.6f,\n", job.cameraFar);
    std::fprintf(file, "  \"verticalFovRadians\": %.6f,\n", job.verticalFovRadians);
    std::fprintf(file, "  \"aspect\": %.6f,\n", job.aspect);
    std::fprintf(file, "  \"cameraPosition\": [%.6f, %.6f, %.6f],\n", job.cameraPosition[0],
        job.cameraPosition[1], job.cameraPosition[2]);
    std::fprintf(file, "  \"cameraUp\": [%.6f, %.6f, %.6f],\n", job.cameraUp[0], job.cameraUp[1],
        job.cameraUp[2]);
    std::fprintf(file, "  \"cameraRight\": [%.6f, %.6f, %.6f],\n", job.cameraRight[0], job.cameraRight[1],
        job.cameraRight[2]);
    std::fprintf(file, "  \"cameraForward\": [%.6f, %.6f, %.6f],\n", job.cameraForward[0],
        job.cameraForward[1], job.cameraForward[2]);
    std::fprintf(file, "  \"hudlessRoute\": \"%s\"\n", hudlessRoute);
    std::fprintf(file, "}\n");
    std::fclose(file);
}

bool WritePlane(const Job& job, uint32_t index, const wchar_t* suffix, const FgReadbackPlane& plane)
{
    if (!plane.buffer) return false;
    std::vector<uint8_t> bytes;
    std::string reason;
    if (!FgMapTightRows(plane, bytes, reason))
    {
        Log("FG capture: pair %u plane %ls skipped: %s", index, suffix, reason.c_str());
        return false;
    }
    wchar_t stem[32]{};
    swprintf_s(stem, L"%02u", index);
    const std::wstring path = job.directory + L"\\" + stem + L"-" + suffix;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr)
    {
        Log("FG capture: pair %u plane %ls could not be opened for writing", index, suffix);
        return false;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return true;
}

void WriteJob(const Job& job)
{
    bool ok = true;
    ok = WritePlane(job, job.index, L"real.rgba8", job.realColor) && ok;
    if (job.hudless.buffer) ok = WritePlane(job, job.index, L"hudless.rgba8", job.hudless) && ok;
    if (job.generated.buffer) ok = WritePlane(job, job.index, L"generated.rgba8", job.generated) && ok;
    ok = WritePlane(job, job.index, L"depth.r32f", job.depth) && ok;
    ok = WritePlane(job, job.index, L"motion.rg16f", job.motion) && ok;
    wchar_t stem[32]{};
    swprintf_s(stem, L"%02u", job.index);
    WriteMeta(job.directory + L"\\" + stem + L"-meta.json", job);
    if (ok) Log("FG capture: pair %u written", job.index);
}

// Counts one finished job and reports the batch once every armed pair has been written.
void FinishJob()
{
    bool complete = false;
    uint32_t total = 0;
    std::string directory;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_batchWritten++;
        if (g_batchTotal != 0 && g_batchWritten >= g_batchTotal)
        {
            complete = true;
            total = g_batchTotal;
            directory = Narrow(g_directory);
            g_batchTotal = 0;
            g_batchWritten = 0;
        }
    }
    if (complete) Log("FG capture: complete, %u pairs written to %s", total, directory.c_str());
}

void WriterLoop()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return g_stopping || !g_jobs.empty(); });
            if (g_jobs.empty())
            {
                if (g_stopping) return;
                continue;
            }
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        if (job.completed != nullptr && job.fence != nullptr)
        {
            if (job.fence->GetCompletedValue() < job.fenceValue)
                job.fence->SetEventOnCompletion(job.fenceValue, job.completed);
            if (job.fence->GetCompletedValue() < job.fenceValue &&
                WaitForSingleObject(job.completed, kReadbackTimeoutMilliseconds) != WAIT_OBJECT_0)
            {
                Log("FG capture: pair %u readback timed out", job.index);
                // The GPU may still be writing this memory, so the planes are never released.
                CloseHandle(job.completed);
                job.completed = nullptr;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_retained.push_back(std::move(job));
                }
                FinishJob();
                continue;
            }
        }
        WriteJob(job);
        if (job.completed != nullptr) CloseHandle(job.completed);
        FinishJob();
    }
}

void EnsureWriter()
{
    if (g_writerRunning) return;
    g_writerRunning = true;
    g_writer = std::thread(WriterLoop);
}
}

bool FgCaptureArm(uint32_t pairs)
{
    if (pairs == 0 || pairs > kMaxArmedPairs) pairs = 5;
    if (g_shutdown.load()) return false;
    const std::wstring root = LogDirectory();
    if (root.empty())
    {
        Log("FG capture: refused, the diagnostics directory is unavailable");
        return false;
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stamp[32]{};
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay, time.wHour,
        time.wMinute, time.wSecond);
    const std::wstring parent = root + L"\\fg-capture";
    const std::wstring directory = parent + L"\\" + stamp;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_remaining != 0)
    {
        Log("FG capture: refused, a capture is already in progress");
        return false;
    }
    // Two levels: CreateDirectoryW does not create intermediate directories.
    CreateDirectoryW(parent.c_str(), nullptr);
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        Log("FG capture: refused, the capture directory could not be created");
        return false;
    }
    g_directory = directory;
    g_remaining = pairs;
    g_batchTotal = pairs;
    g_batchWritten = 0;
    g_nextIndex = 0;
    EnsureWriter();
    g_armed.store(true);
    const std::string narrow = Narrow(directory);
    Log("FG capture: armed for %u pairs in %s", pairs, narrow.c_str());
    return true;
}

void FgCaptureObserve(const FgTestGeneratedImage& image)
{
    if (!g_armed.load() || g_shutdown.load()) return;
    const bool hasQueue = image.queue != nullptr && image.frame != nullptr;
    Job job;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_remaining == 0)
        {
            g_armed.store(false);
            return;
        }
        if (!hasQueue)
        {
            g_remaining--;
            if (g_remaining == 0) g_armed.store(false);
        }
        else
        {
            job.index = g_nextIndex++;
            job.directory = g_directory;
        }
    }
    if (!hasQueue)
    {
        Log("FG capture: pair skipped, the provider published no queue or metadata");
        return;
    }
    job.provider = image.provider != nullptr ? image.provider : "unknown";
    job.generation = image.frame->generation;
    job.sourceFrame = image.frame->frameIndex;
    job.presentId = image.frame->presentId;
    job.flags = image.frame->flags;
    job.reset = image.reset;
    job.intermediateIndex = image.intermediateIndex;
    job.intermediateCount = image.intermediateCount;
    job.renderWidth = image.renderWidth;
    job.renderHeight = image.renderHeight;
    job.outputWidth = image.frame->outputWidth > 0 ? static_cast<uint32_t>(image.frame->outputWidth)
                                                   : image.width;
    job.outputHeight = image.frame->outputHeight > 0 ? static_cast<uint32_t>(image.frame->outputHeight)
                                                     : image.height;
    job.jitterX = image.frame->jitterX;
    job.jitterY = image.frame->jitterY;
    job.deltaMilliseconds = image.frame->deltaMilliseconds;
    job.cameraNear = image.frame->cameraNear;
    job.cameraFar = image.frame->cameraFar;
    job.verticalFovRadians = image.frame->verticalFovRadians;
    job.aspect = image.frame->aspect;
    std::memcpy(job.cameraPosition, image.frame->cameraPosition, sizeof(job.cameraPosition));
    std::memcpy(job.cameraUp, image.frame->cameraUp, sizeof(job.cameraUp));
    std::memcpy(job.cameraRight, image.frame->cameraRight, sizeof(job.cameraRight));
    std::memcpy(job.cameraForward, image.frame->cameraForward, sizeof(job.cameraForward));

    ComPtr<ID3D12Device> device;
    image.queue->GetDevice(IID_PPV_ARGS(&device));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::string reason;
    bool ok = device != nullptr;
    if (!ok) reason = "the frame-generation device is unavailable";
    if (ok &&
        (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            !allocator ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                IID_PPV_ARGS(&list))) ||
            !list))
    {
        reason = "the readback command list could not be created";
        ok = false;
    }
    if (ok)
    {
        struct PlaneRequest
        {
            ID3D12Resource* source;
            D3D12_RESOURCE_STATES state;
            FgReadbackPlane* plane;
        };
        const PlaneRequest requests[5] = {
            {image.realColor, image.realColorState, &job.realColor},
            {image.evaluationHudless, image.evaluationHudlessState, &job.hudless},
            {image.reset ? nullptr : image.generated, image.generatedState, &job.generated},
            {image.depth, image.depthState, &job.depth},
            {image.motion, image.motionState, &job.motion},
        };
        for (const PlaneRequest& request : requests)
        {
            if (request.source == nullptr) continue;
            if (!FgRecordReadback(device.Get(), list.Get(), request.source, request.state, *request.plane,
                    reason))
            {
                ok = false;
                break;
            }
        }
    }
    if (ok && FAILED(list->Close()))
    {
        reason = "the readback command list could not be closed";
        ok = false;
    }
    ComPtr<ID3D12Fence> fence;
    HANDLE completed = nullptr;
    if (ok)
    {
        completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (completed == nullptr ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || !fence)
        {
            reason = "the readback fence could not be created";
            ok = false;
        }
    }
    if (ok)
    {
        ID3D12CommandList* lists[1] = {list.Get()};
        image.queue->ExecuteCommandLists(1, lists);
        if (FAILED(image.queue->Signal(fence.Get(), 1)))
        {
            reason = "the readback fence could not be signalled";
            ok = false;
        }
    }
    if (!ok)
    {
        if (completed != nullptr) CloseHandle(completed);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_remaining--;
        if (g_remaining == 0) g_armed.store(false);
        Log("FG capture: pair %u skipped: %s", job.index, reason.c_str());
        return;
    }
    job.fence = fence;
    job.completed = completed;
    job.fenceValue = 1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_jobs.push_back(std::move(job));
        g_remaining--;
        if (g_remaining == 0) g_armed.store(false);
    }
    g_wake.notify_one();
}

void FgCaptureShutdown()
{
    if (g_shutdown.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_armed.store(false);
        g_stopping = true;
    }
    g_wake.notify_all();
    if (g_writer.joinable()) g_writer.join();
    g_writerRunning = false;
}
}
