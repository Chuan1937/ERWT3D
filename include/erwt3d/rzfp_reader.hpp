#pragma once

#include "rzfp_format.hpp"
#include "rzfp_xplane_codec.hpp"
#include "device_profile.hpp"
#include "slice.hpp"
#include "sb_hdd.hpp"
#include "window_cache.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace erwt3d {

enum class RzfpReadStrategy {
    Auto = 0,
    SelectiveLeaf,
    WholeSuperblock,
    FullPayloadScan,
    RawXAux,
    XPlaneSidecar,
};

struct RzfpReadProfile {
    uint64_t unique_superblocks = 0;
    uint64_t unique_leaves = 0;
    uint64_t requested_record_bytes = 0;
    uint64_t actual_read_bytes = 0;
    uint64_t pread_calls = 0;
    uint64_t window_cache_hits = 0;
    uint64_t window_cache_misses = 0;
    uint64_t window_cache_resident_bytes = 0;
    uint64_t window_cache_saved_read_bytes = 0;

    double plan_time_ms = 0.0;
    double prefix_time_ms = 0.0;
    double io_time_ms = 0.0;
    double decode_time_ms = 0.0;
    double scatter_time_ms = 0.0;
    double sidecar_io_ms = 0.0;
    double sidecar_decode_ms = 0.0;

    double predicted_selective_seconds = 0.0;
    double predicted_whole_seconds = 0.0;
    double predicted_fullscan_seconds = 0.0;
    double effective_device_mb_s = 0.0;
    double pilot_observed_mb_s = 0.0;

    RzfpReadStrategy selected_strategy = RzfpReadStrategy::Auto;
    CachePolicy cache_policy = CachePolicy::StableAuto;
    std::string strategy_reason;

    std::atomic<uint64_t> scatter_ns{0};

    RzfpReadProfile() = default;
    RzfpReadProfile(const RzfpReadProfile& o)
        : unique_superblocks(o.unique_superblocks), unique_leaves(o.unique_leaves),
          requested_record_bytes(o.requested_record_bytes), actual_read_bytes(o.actual_read_bytes),
          pread_calls(o.pread_calls), window_cache_hits(o.window_cache_hits),
          window_cache_misses(o.window_cache_misses),
          window_cache_resident_bytes(o.window_cache_resident_bytes),
          window_cache_saved_read_bytes(o.window_cache_saved_read_bytes),
          plan_time_ms(o.plan_time_ms), prefix_time_ms(o.prefix_time_ms),
          io_time_ms(o.io_time_ms), decode_time_ms(o.decode_time_ms),
          scatter_time_ms(o.scatter_time_ms), sidecar_io_ms(o.sidecar_io_ms),
          sidecar_decode_ms(o.sidecar_decode_ms),
          predicted_selective_seconds(o.predicted_selective_seconds),
          predicted_whole_seconds(o.predicted_whole_seconds),
          predicted_fullscan_seconds(o.predicted_fullscan_seconds),
          effective_device_mb_s(o.effective_device_mb_s),
          pilot_observed_mb_s(o.pilot_observed_mb_s),
          selected_strategy(o.selected_strategy), cache_policy(o.cache_policy),
          strategy_reason(o.strategy_reason), scatter_ns(o.scatter_ns.load()) {}

    RzfpReadProfile& operator=(const RzfpReadProfile& o) {
        if (this == &o) return *this;
        unique_superblocks = o.unique_superblocks;
        unique_leaves = o.unique_leaves;
        requested_record_bytes = o.requested_record_bytes;
        actual_read_bytes = o.actual_read_bytes;
        pread_calls = o.pread_calls;
        window_cache_hits = o.window_cache_hits;
        window_cache_misses = o.window_cache_misses;
        window_cache_resident_bytes = o.window_cache_resident_bytes;
        window_cache_saved_read_bytes = o.window_cache_saved_read_bytes;
        plan_time_ms = o.plan_time_ms;
        prefix_time_ms = o.prefix_time_ms;
        io_time_ms = o.io_time_ms;
        decode_time_ms = o.decode_time_ms;
        scatter_time_ms = o.scatter_time_ms;
        sidecar_io_ms = o.sidecar_io_ms;
        sidecar_decode_ms = o.sidecar_decode_ms;
        predicted_selective_seconds = o.predicted_selective_seconds;
        predicted_whole_seconds = o.predicted_whole_seconds;
        predicted_fullscan_seconds = o.predicted_fullscan_seconds;
        effective_device_mb_s = o.effective_device_mb_s;
        pilot_observed_mb_s = o.pilot_observed_mb_s;
        selected_strategy = o.selected_strategy;
        cache_policy = o.cache_policy;
        strategy_reason = o.strategy_reason;
        scatter_ns.store(o.scatter_ns.load());
        return *this;
    }

    double readAmplification() const {
        return requested_record_bytes > 0
                   ? static_cast<double>(actual_read_bytes) /
                         static_cast<double>(requested_record_bytes)
                   : 1.0;
    }
};

struct RzfpAdaptiveConfig {
    bool auto_calibrate_device = true;

    CachePolicy cache_policy = CachePolicy::StableAuto;
    double strategy_switch_margin = 0.15;

    double max_fullscan_seconds = 120.0;
    double fullscan_min_advantage = 0.20;

    bool enable_strategy_probe = true;
    uint64_t strategy_probe_bytes = 256ULL * 1024 * 1024;
    double strategy_probe_max_seconds = 3.0;
};

struct RzfpReaderConfig {
    HDDReadWindowConfig hdd;
    RzfpReadStrategy strategy = RzfpReadStrategy::Auto;
    int decode_threads = 1;
    RzfpReadProfile* profile = nullptr;
    RzfpAdaptiveConfig adaptive;

    std::shared_ptr<BoundedWindowCache> window_cache;
    uint64_t window_cache_file_identity = 0;
    bool use_window_cache = true;
};

class RzfpReader {
public:
    explicit RzfpReader(const std::string& path);
    ~RzfpReader();

    bool ok() const { return fd_ >= 0; }
    const RzfpFileHeader& header() const { return header_; }
    const DeviceProfile& deviceProfile() const { return device_profile_; }
    uint64_t fileIdentity() const { return file_identity_; }
    uint64_t payloadBytes() const { return payload_bytes_; }

    const DeviceProfile& ensureDeviceProfile(
        const DeviceCalibrationConfig& config = {}
    );

    bool dropPayloadCache();

    bool readSlice(SliceAxis axis, uint64_t index, float* output,
                   int numThreads = 1, size_t memoryLimitMB = 4096,
                   const HDDReadWindowConfig& wcfg = {});

    struct SliceBatchRequest {
        SliceAxis axis;
        uint64_t index;
        float* output;
    };

    bool readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                         int numThreads = 1, size_t memoryLimitMB = 4096,
                         const HDDReadWindowConfig& wcfg = {});

    bool readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                         const RzfpReaderConfig& config);

    struct ContestRoundGroup {
        SliceAxis axis;
        std::string name;
        std::vector<uint64_t> indices;
        std::vector<float*> outputs;
    };

    struct RzfpRoundReadResult {
        double read_time_ms = 0.0;
        double io_time_ms = 0.0;
        double decode_time_ms = 0.0;
        double scatter_time_ms = 0.0;

        uint64_t unique_leaves = 0;
        uint64_t duplicate_leaf_requests = 0;
        uint64_t logical_leaf_requests = 0;

        uint64_t planned_read_bytes = 0;
        uint64_t actual_read_bytes = 0;
        uint64_t eliminated_read_bytes = 0;
        double read_reduction_ratio = 0.0;

        RzfpReadStrategy selected_strategy = RzfpReadStrategy::Auto;
        std::string strategy_reason;

        bool round_plan_built = false;
        uint64_t round_unique_superblocks = 0;
        uint64_t round_planned_preads = 0;
    };

    bool readContestRound(
        const std::vector<ContestRoundGroup>& groups,
        const RzfpReaderConfig& config,
        std::vector<RzfpRoundReadResult>* results = nullptr
    );

private:
    std::string path_;
    int fd_ = -1;
    RzfpFileHeader header_{};

    std::vector<RzfpSuperblockIndex> sb_index_;
    std::vector<RzfpLeafDescriptor> descriptors_;

    uint64_t file_size_ = 0;
    uint64_t file_identity_ = 0;
    uint64_t payload_bytes_ = 0;
    DeviceProfile device_profile_;
    bool device_profile_ready_ = false;

    // Optional 2D X-plane sidecar.
    bool has_xplane_ = false;
    int xplane_fd_ = -1;
    std::vector<uint64_t> xplane_offsets_;
    std::vector<uint32_t> xplane_sizes_;

    bool openXPlaneSidecar();
    bool readXPlaneFromSidecar(uint64_t x, float* output, RzfpReadProfile* profile);
    bool readXPlanesBatchFromSidecar(
        const std::vector<SliceBatchRequest>& requests,
        const RzfpReaderConfig& config,
        RzfpReadProfile& profile
    );

    // Raw X auxiliary (full-coverage uncompressed X-plane region)
    int rawXAuxFd_ = -1;
    bool rawXAuxAvailable_ = false;
    uint64_t rawXAuxOffset_ = 0;
    uint64_t rawXAuxPlaneBytes_ = 0;

    void initRawXAux_();
    bool tryReadBatchRawXAux_(const std::vector<SliceBatchRequest>& requests,
                              const RzfpReaderConfig& config,
                              RzfpReadProfile& profile);
    bool tryReadSliceRawXAux_(uint64_t x, float* output, RzfpReadProfile* profile);
};

} // namespace erwt3d
