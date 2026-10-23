#include "daemon/ai/gpu.h"

#include <windows.h>
#include <dxgi1_4.h>

#include <chrono>
#include <mutex>

#include "common/strings.h"

namespace wa {
namespace {

constexpr uint64_t kMb = 1024ull * 1024ull;

std::mutex g_mutex;
GpuMemory g_cached;
std::chrono::steady_clock::time_point g_cached_at{};

GpuMemory query_now() {
    GpuMemory out;
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) return out;

    IDXGIAdapter1* best = nullptr;
    DXGI_ADAPTER_DESC1 best_desc{};
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (!software && desc.DedicatedVideoMemory > best_desc.DedicatedVideoMemory) {
            if (best) best->Release();
            best = adapter;
            best_desc = desc;
            continue;
        }
        adapter->Release();
    }
    if (!best) {
        factory->Release();
        return out;
    }

    IDXGIAdapter3* adapter3 = nullptr;
    if (SUCCEEDED(best->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            out.ok = true;
            out.adapter = to_utf8(best_desc.Description);
            out.total_mb = best_desc.DedicatedVideoMemory / kMb;
            out.budget_mb = info.Budget / kMb;
            out.used_mb = info.CurrentUsage / kMb;
            out.free_mb = info.Budget > info.CurrentUsage ? (info.Budget - info.CurrentUsage) / kMb : 0;
        }
        adapter3->Release();
    }
    best->Release();
    factory->Release();
    return out;
}

}  // namespace

GpuMemory query_gpu_memory() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(g_mutex);
    if (g_cached.ok && now - g_cached_at < std::chrono::seconds(2)) return g_cached;
    g_cached = query_now();
    g_cached_at = now;
    return g_cached;
}

}  // namespace wa
