#include "erwt3d/memory_budget.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace erwt3d {

namespace {

constexpr uint64_t KiB = 1024ULL;
constexpr uint64_t MiB = 1024ULL * KiB;
constexpr uint64_t GiB = 1024ULL * MiB;

static bool checkedAdd(uint64_t a, uint64_t b, uint64_t& out) {
    if (a > std::numeric_limits<uint64_t>::max() - b) return false;
    out = a + b;
    return true;
}

static bool checkedMul(uint64_t a, uint64_t b, uint64_t& out) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return false;
    out = a * b;
    return true;
}

static bool parseExplicitMiB(const std::string& value, uint64_t& bytes) {
    if (value.empty()) return false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    }

    try {
        const unsigned long long mib = std::stoull(value);
        return checkedMul(static_cast<uint64_t>(mib), MiB, bytes);
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

uint64_t readLinuxMemAvailableBytes() {
    std::ifstream input("/proc/meminfo");
    if (!input) return 0;

    std::string line;
    while (std::getline(input, line)) {
        if (line.compare(0, 13, "MemAvailable:") != 0) continue;

        std::istringstream stream(line.substr(13));
        uint64_t kib = 0;
        std::string unit;
        stream >> kib >> unit;
        if (!stream || kib == 0) return 0;

        uint64_t bytes = 0;
        if (!checkedMul(kib, KiB, bytes)) return 0;
        return bytes;
    }

    return 0;
}

MemoryBudget makeMemoryBudget(
    const std::string& value,
    uint64_t payload_bytes,
    uint64_t bytes_per_output_slice,
    uint64_t requested_slice_count
) {
    MemoryBudget budget;

    if (bytes_per_output_slice == 0 || requested_slice_count == 0) {
        budget.error = "output slice size and requested slice count must be positive";
        return budget;
    }

    if (value == "auto") {
        budget.automatic = true;

        const uint64_t memAvailable = readLinuxMemAvailableBytes();
        if (memAvailable == 0) {
            budget.error = "cannot read MemAvailable from /proc/meminfo";
            return budget;
        }

        uint64_t payloadPlusWorkspace = 0;
        if (!checkedAdd(payload_bytes, 4ULL * GiB, payloadPlusWorkspace)) {
            payloadPlusWorkspace = std::numeric_limits<uint64_t>::max();
        }

        budget.total_bytes = std::min<uint64_t>({
            32ULL * GiB,
            memAvailable / 2,
            payloadPlusWorkspace
        });

        // Use 4 GiB when possible, but never exceed half of MemAvailable.
        if (budget.total_bytes < 4ULL * GiB && memAvailable / 2 >= 4ULL * GiB) {
            budget.total_bytes = 4ULL * GiB;
        }
    } else {
        if (!parseExplicitMiB(value, budget.total_bytes)) {
            budget.error = "memory limit must be 'auto' or a positive integer MiB value";
            return budget;
        }
    }

    if (budget.total_bytes < 512ULL * MiB) {
        budget.error = "memory limit is too small; at least 512 MiB is required";
        return budget;
    }

    budget.reserve_bytes = budget.automatic
        ? std::min<uint64_t>(1ULL * GiB, budget.total_bytes / 8)
        : std::min<uint64_t>(256ULL * MiB, budget.total_bytes / 8);
    budget.reserve_bytes = std::max<uint64_t>(64ULL * MiB, budget.reserve_bytes);

    uint64_t metadataFromRequests = 0;
    if (!checkedMul(requested_slice_count, 64ULL * KiB, metadataFromRequests)) {
        budget.error = "request metadata size overflow";
        return budget;
    }
    budget.estimated_metadata_bytes = std::min<uint64_t>(
        256ULL * MiB,
        std::max<uint64_t>(32ULL * MiB, metadataFromRequests)
    );

    budget.io_buffer_bytes = std::min<uint64_t>(
        1ULL * GiB,
        std::max<uint64_t>(128ULL * MiB, budget.total_bytes / 8)
    );

    uint64_t fixed = 0;
    if (!checkedAdd(budget.reserve_bytes, budget.estimated_metadata_bytes, fixed) ||
        !checkedAdd(fixed, budget.io_buffer_bytes, fixed) ||
        fixed >= budget.total_bytes) {
        budget.error = "memory limit cannot hold required fixed buffers";
        return budget;
    }

    const uint64_t remaining = budget.total_bytes - fixed;
    if (bytes_per_output_slice > remaining) {
        budget.error = "memory limit cannot hold one output slice";
        return budget;
    }

    uint64_t totalOutputBytes = 0;
    if (!checkedMul(
            bytes_per_output_slice,
            requested_slice_count,
            totalOutputBytes)) {
        budget.error = "total output size overflow";
        return budget;
    }

    // Preserve at least half of the variable budget for compressed payload
    // reuse whenever the output group does not itself require all memory.
    const uint64_t outputAllowance = std::max<uint64_t>(
        bytes_per_output_slice,
        remaining / 2
    );
    budget.output_buffer_bytes = std::min<uint64_t>(
        totalOutputBytes,
        outputAllowance
    );

    budget.output_batch_size = std::max<uint64_t>(
        1,
        budget.output_buffer_bytes / bytes_per_output_slice
    );
    budget.output_batch_size = std::min<uint64_t>(
        budget.output_batch_size,
        requested_slice_count
    );
    budget.output_buffer_bytes =
        budget.output_batch_size * bytes_per_output_slice;

    const uint64_t variableUsed = budget.output_buffer_bytes;
    budget.window_cache_bytes = remaining > variableUsed
        ? remaining - variableUsed
        : 0;
    budget.window_cache_bytes = std::min<uint64_t>(
        budget.window_cache_bytes,
        payload_bytes
    );

    // Any capacity removed by the payload cap remains intentionally unused;
    // it is safer than silently reallocating it to output buffers and violating
    // the expected balance between working memory and kernel page cache.
    if (budget.accountedBytes() > budget.total_bytes) {
        budget.error = "internal memory budget accounting exceeded the limit";
        return budget;
    }

    budget.valid = true;
    return budget;
}

} // namespace erwt3d
