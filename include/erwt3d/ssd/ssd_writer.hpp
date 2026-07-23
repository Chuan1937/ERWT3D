#pragma once

#include "erwt3d/ssd/ssd_config.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace erwt3d {

struct SSDWriteTask {
    std::string filepath;
    std::shared_ptr<std::vector<float>> buffer;
    uint64_t bytes = 0;
    uint64_t sequence = 0;
};

struct SSDWriterStats {
    std::atomic<uint64_t> write_calls{0};
    std::atomic<uint64_t> write_bytes{0};
    std::atomic<uint64_t> queue_peak_bytes{0};
    std::atomic<uint64_t> file_create_ms{0};
    std::atomic<uint64_t> writer_time_ns{0};
    std::atomic<uint64_t> queued_count{0};
    std::atomic<uint64_t> completed_count{0};
};

class SSDWriterPipeline {
public:
    explicit SSDWriterPipeline(const SSDWriterConfig& cfg, SSDWriterStats* stats = nullptr);
    ~SSDWriterPipeline();

    bool createOutputFiles(const std::string& dir,
                           const std::vector<std::string>& filenames);

    bool enqueue(SSDWriteTask&& task);

    bool finish();

private:
    SSDWriterConfig cfg_;
    SSDWriterStats* stats_ = nullptr;

    std::vector<std::string> filenames_;
    int dirFd_ = -1;
    bool precreated_ = false;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace erwt3d
