#pragma once

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

namespace erwt3d {

enum class MemoryLimitMode {
    Explicit,
    Auto
};

struct MemoryBudget {
    uint64_t total_bytes = 0;
    uint64_t io_buffer_bytes = 0;
    uint64_t output_buffer_bytes = 0;
    uint64_t window_cache_bytes = 0;
    uint64_t reserve_bytes = 0;
    uint64_t estimated_metadata_bytes = 0;
    uint64_t output_batch_size = 0;
    bool automatic = false;
    bool valid = false;
    std::string error;
    uint64_t auto_mem_available = 0;
    uint64_t auto_reserve = 0;

    uint64_t accountedBytes() const {
        return io_buffer_bytes + output_buffer_bytes +
               window_cache_bytes + reserve_bytes +
               estimated_metadata_bytes;
    }
};

uint64_t readLinuxMemAvailableBytes();

MemoryBudget makeMemoryBudget(
    const std::string& value,
    uint64_t payload_bytes,
    uint64_t bytes_per_output_slice,
    uint64_t requested_slice_count
);

} // namespace erwt3d
