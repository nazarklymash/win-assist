#pragma once

#include <cstdint>
#include <string>

namespace wa {

struct GpuMemory {
    bool ok = false;
    std::string adapter;
    // Dedicated VRAM on the adapter.
    uint64_t total_mb = 0;
    // What this process may currently use, and what is already committed.
    uint64_t budget_mb = 0;
    uint64_t used_mb = 0;
    uint64_t free_mb = 0;
};

// Queries the largest discrete adapter through DXGI. Results are cached for a
// couple of seconds because the model router asks on every turn.
GpuMemory query_gpu_memory();

}  // namespace wa
