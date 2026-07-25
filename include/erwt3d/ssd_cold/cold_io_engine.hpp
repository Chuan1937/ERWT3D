#pragma once

#include "erwt3d/ssd_cold/cold_extent_plan.hpp"
#include "erwt3d/ssd_cold/cold_buffer_pool.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace erwt3d {
namespace ssd_cold {

enum class ColdIOBackend { Pread, IOUring, Auto };

struct ColdIOConfig {
    ColdIOBackend backend = ColdIOBackend::Auto;
    int read_threads = 4;
    int queue_depth = 8;
    bool use_fadvise = true;
    bool use_direct_io = false;
};

struct ColdIOTask {
    int fd = -1;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t base_offset = 0;
    size_t first_record = 0;
    size_t record_count = 0;
    const std::vector<ColdLeafRecord>* records = nullptr;
};

struct ColdIOResult {
    bool ok = false;
    uint64_t bytes_read = 0;
    double io_time_ms = 0.0;
};

using ColdCompleteCallback = std::function<void(const ColdIOResult&)>;

struct ColdIOEngine {
    ColdIOConfig config;
    ColdBufferPool* pool = nullptr;

    ColdIOEngine() = default;
    ~ColdIOEngine();

    ColdIOEngine(const ColdIOEngine&) = delete;
    ColdIOEngine& operator=(const ColdIOEngine&) = delete;

    bool init(const ColdIOConfig& cfg, ColdBufferPool* bufferPool);
    bool submit(const ColdIOTask& task, ColdCompleteCallback cb);
    bool drain();
    void shutdown();

    uint64_t totalBytesRead() const { return totalBytesRead_; }
    uint64_t preadCalls() const { return preadCalls_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    uint64_t totalBytesRead_ = 0;
    uint64_t preadCalls_ = 0;
};

} // namespace ssd_cold
} // namespace erwt3d
