#include "erwt3d/memory_budget.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/window_cache.hpp"
#include "erwt3d/contest_round_executor.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include "erwt3d/platform_io.hpp"
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t MiB = 1024ULL * 1024ULL;

struct GroupSpec {
    erwt3d::SliceAxis axis = erwt3d::SliceAxis::X;
    std::string axis_name;
    std::string mode;
    const std::vector<uint64_t>* indices = nullptr;
};

struct GroupResult {
    int round = 0;
    int group_index = 0;
    std::string axis;
    std::string mode;
    std::string benchmark_cache_mode;
    int slice_count = 0;
    uint64_t output_bytes_per_slice = 0;
    uint64_t output_batch_size = 0;
    uint64_t memory_limit_bytes = 0;
    uint64_t window_cache_capacity_bytes = 0;
    double group_time_ms = 0.0;
    double read_time_ms = 0.0;
    double write_time_ms = 0.0;
    double device_seq_mb_s = 0.0;
    double device_seek_ms = 0.0;
    std::string selected_strategy;
    std::string strategy_reason;
    uint64_t logical_leaf_requests = 0;
    uint64_t duplicate_leaf_requests = 0;
    uint64_t eliminated_read_bytes = 0;
    double read_reduction_ratio = 0.0;
    uint64_t round_unique_superblocks = 0;
    uint64_t round_planned_preads = 0;
    bool round_plan_built = false;
    erwt3d::RzfpReadProfile profile;
};

struct RoundResult {
    int round = 0;
    std::vector<GroupResult> groups;
    double total_ms = 0.0;
    double composite_ms = 0.0;
};

enum class BenchmarkCacheMode {
    StableAuto,
    ColdRound,
    ColdGroup,
    Warm
};

enum class ExecutionMode {
    P4Groups,
    P5Round
};

static const char* executionModeName(ExecutionMode mode) {
    switch (mode) {
        case ExecutionMode::P4Groups: return "p4-groups";
        case ExecutionMode::P5Round: return "p5-round";
    }
    return "unknown";
}

static const char* benchmarkModeName(BenchmarkCacheMode mode) {
    switch (mode) {
        case BenchmarkCacheMode::StableAuto: return "stable-auto";
        case BenchmarkCacheMode::ColdRound: return "cold-round";
        case BenchmarkCacheMode::ColdGroup: return "cold-group";
        case BenchmarkCacheMode::Warm: return "warm";
    }
    return "unknown";
}

static const char* strategyName(erwt3d::RzfpReadStrategy strategy) {
    switch (strategy) {
        case erwt3d::RzfpReadStrategy::Auto: return "auto";
        case erwt3d::RzfpReadStrategy::SelectiveLeaf: return "selective";
        case erwt3d::RzfpReadStrategy::WholeSuperblock: return "whole";
        case erwt3d::RzfpReadStrategy::FullPayloadScan: return "fullscan";
        case erwt3d::RzfpReadStrategy::RawXAux: return "raw-x-aux";
        case erwt3d::RzfpReadStrategy::XPlaneSidecar: return "xplane-sidecar";
    }
    return "unknown";
}

static bool mkdirOne(const std::string& path) {
    if (path.empty() || path == ".") return true;
    if (mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

static bool mkdirP(const std::string& path) {
    if (path.empty()) return false;
    std::string current;
    if (path.front() == '/') current = "/";

    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(
            start,
            slash == std::string::npos ? std::string::npos : slash - start
        );
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') current.push_back('/');
            current += part;
            if (!mkdirOne(current)) return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

static uint64_t sliceElements(
    const erwt3d::RzfpFileHeader& header,
    erwt3d::SliceAxis axis
) {
    switch (axis) {
        case erwt3d::SliceAxis::X: return header.ny * header.nz;
        case erwt3d::SliceAxis::Y: return header.nx * header.nz;
        case erwt3d::SliceAxis::Z: return header.nx * header.ny;
    }
    return 0;
}

static void accumulateProfile(
    erwt3d::RzfpReadProfile& destination,
    const erwt3d::RzfpReadProfile& source
) {
    destination.unique_superblocks += source.unique_superblocks;
    destination.unique_leaves += source.unique_leaves;
    destination.requested_record_bytes += source.requested_record_bytes;
    destination.actual_read_bytes += source.actual_read_bytes;
    destination.pread_calls += source.pread_calls;
    destination.window_cache_hits += source.window_cache_hits;
    destination.window_cache_misses += source.window_cache_misses;
    destination.window_cache_saved_read_bytes +=
        source.window_cache_saved_read_bytes;
    destination.window_cache_resident_bytes =
        source.window_cache_resident_bytes;
    destination.plan_time_ms += source.plan_time_ms;
    destination.prefix_time_ms += source.prefix_time_ms;
    destination.io_time_ms += source.io_time_ms;
    destination.decode_time_ms += source.decode_time_ms;
    destination.scatter_time_ms += source.scatter_time_ms;
    destination.sidecar_io_ms += source.sidecar_io_ms;
    destination.sidecar_decode_ms += source.sidecar_decode_ms;
    destination.predicted_selective_seconds =
        source.predicted_selective_seconds;
    destination.predicted_whole_seconds = source.predicted_whole_seconds;
    destination.predicted_fullscan_seconds =
        source.predicted_fullscan_seconds;
    destination.effective_device_mb_s = source.effective_device_mb_s;
    destination.pilot_observed_mb_s = source.pilot_observed_mb_s;
    destination.cache_policy = source.cache_policy;
    destination.strategy_reason = source.strategy_reason;

    if (destination.selected_strategy == erwt3d::RzfpReadStrategy::Auto) {
        destination.selected_strategy = source.selected_strategy;
    } else if (destination.selected_strategy != source.selected_strategy) {
        destination.strategy_reason =
            "multiple batches selected different strategies; last: " +
            source.strategy_reason;
    }
}

static bool precreateOutputs(
    const std::string& output_dir,
    const std::string& prefix,
    size_t count,
    uint64_t bytes,
    std::vector<int>& fds
) {
    fds.assign(count, -1);
    for (size_t i = 0; i < count; ++i) {
        const std::string path = output_dir + "/" + prefix + "_" +
                                 std::to_string(i) + ".raw";
        const int fd = io_open(
            path.c_str(),
            O_RDWR | O_CREAT | O_TRUNC,
            0644
        );
        if (fd < 0) {
            std::cerr << "Error: cannot create output " << path << "\n";
            return false;
        }
        if (posix_fallocate(fd, 0, static_cast<int64_t>(bytes)) != 0 &&
            ftruncate(fd, static_cast<int64_t>(bytes)) != 0) {
            std::cerr << "Error: cannot preallocate output " << path << "\n";
            io_close(fd);
            return false;
        }
        fds[i] = fd;
    }
    return true;
}

static void closeOutputs(std::vector<int>& fds) {
    for (int& fd : fds) {
        if (fd >= 0) io_close(fd);
        fd = -1;
    }
}

static bool writeFullyAtLocal(
    int fd,
    const void* data,
    uint64_t bytes,
    uint64_t offset
) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    uint64_t completed = 0;
    while (completed < bytes) {
        const ssize_t written = pwrite(
            fd,
            cursor + completed,
            static_cast<size_t>(bytes - completed),
            static_cast<int64_t>(offset + completed)
        );
        if (written > 0) {
            completed += static_cast<uint64_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool runGroup(
    erwt3d::RzfpReader& reader,
    const erwt3d::RzfpFileHeader& header,
    const GroupSpec& spec,
    int round,
    int group_index,
    const std::string& output_dir,
    const erwt3d::MemoryBudget& budget,
    const erwt3d::RzfpReaderConfig& base_config,
    BenchmarkCacheMode benchmark_mode,
    GroupResult& result
) {
    if (!spec.indices || spec.indices->empty()) return false;

    const uint64_t elements = sliceElements(header, spec.axis);
    const uint64_t outputBytes = elements * sizeof(float);
    const size_t batchSize = static_cast<size_t>(
        std::max<uint64_t>(1, std::min<uint64_t>(
            budget.output_batch_size,
            spec.indices->size()
        ))
    );

    result.round = round;
    result.group_index = group_index;
    result.axis = spec.axis_name;
    result.mode = spec.mode;
    result.benchmark_cache_mode = benchmarkModeName(benchmark_mode);
    result.slice_count = static_cast<int>(spec.indices->size());
    result.output_bytes_per_slice = outputBytes;
    result.output_batch_size = batchSize;
    result.memory_limit_bytes = budget.total_bytes;
    result.window_cache_capacity_bytes = budget.window_cache_bytes;

    std::vector<int> outputFds;
    const std::string prefix =
        "r" + std::to_string(round) + "_g" +
        std::to_string(group_index) + "_" +
        spec.axis_name + "_" + spec.mode;
    if (!precreateOutputs(
            output_dir,
            prefix,
            spec.indices->size(),
            outputBytes,
            outputFds)) {
        closeOutputs(outputFds);
        return false;
    }

    const auto groupStart = Clock::now();
    erwt3d::RzfpReadProfile accumulated;
    double totalReadMs = 0.0;
    double totalWriteMs = 0.0;

    for (size_t batchStart = 0;
         batchStart < spec.indices->size();
         batchStart += batchSize) {
        const size_t batchEnd = std::min(
            batchStart + batchSize,
            spec.indices->size()
        );
        const size_t batchLength = batchEnd - batchStart;

        // Read/decode phase. No output-file writes are issued while the HDD
        // input phase for this batch is active.
        std::vector<std::vector<float>> buffers(
            batchLength,
            std::vector<float>(static_cast<size_t>(elements))
        );
        std::vector<erwt3d::RzfpReader::SliceBatchRequest> requests;
        requests.reserve(batchLength);
        for (size_t i = 0; i < batchLength; ++i) {
            requests.push_back({
                spec.axis,
                (*spec.indices)[batchStart + i],
                buffers[i].data()
            });
        }

        erwt3d::RzfpReadProfile batchProfile;
        erwt3d::RzfpReaderConfig config = base_config;
        config.profile = &batchProfile;

        const auto readStart = Clock::now();
        if (!reader.readSlicesBatch(requests, config)) {
            std::cerr << "Error: batch read failed for "
                      << spec.axis_name << " " << spec.mode << "\n";
            closeOutputs(outputFds);
            return false;
        }
        totalReadMs += std::chrono::duration<double, std::milli>(
            Clock::now() - readStart
        ).count();
        accumulateProfile(accumulated, batchProfile);

        // Sequential output phase after the batch read/decode has finished.
        const auto writeStart = Clock::now();
        for (size_t i = 0; i < batchLength; ++i) {
            if (!writeFullyAtLocal(
                    outputFds[batchStart + i],
                    buffers[i].data(),
                    outputBytes,
                    0)) {
                std::cerr << "Error: output write failed for "
                          << spec.axis_name << " index "
                          << (batchStart + i) << "\n";
                closeOutputs(outputFds);
                return false;
            }
        }
        totalWriteMs += std::chrono::duration<double, std::milli>(
            Clock::now() - writeStart
        ).count();
    }

    closeOutputs(outputFds);
    result.group_time_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - groupStart
    ).count();
    result.read_time_ms = totalReadMs;
    result.write_time_ms = totalWriteMs;
    result.profile = accumulated;
    result.selected_strategy = strategyName(accumulated.selected_strategy);
    result.strategy_reason = accumulated.strategy_reason;
    result.device_seq_mb_s = reader.deviceProfile().sequential_mb_s;
    result.device_seek_ms = reader.deviceProfile().random_seek_ms;
    return true;
}

static void printUsage(const char* program) {
    std::cerr
        << "Usage: " << program
        << " --input PATH --output-dir DIR [options]\n\n"
        << "Options:\n"
        << "  --random-count N              Random slices per axis (default: 100)\n"
        << "  --continuous-count N          Continuous slices per axis (default: 10)\n"
        << "  --decode-threads N            Decode threads (default: 8)\n"
        << "  --memory-limit-mb auto|N      Strict memory budget (default: auto)\n"
        << "  --window-cache-mb auto|N|0    Compressed-window cache cap (default: auto)\n"
        << "  --read-window-bytes N         Maximum merged read window\n"
        << "  --max-gap-bytes N             Maximum merged-window gap\n"
        << "  --read-strategy STR           auto|selective|whole|fullscan\n"
        << "  --benchmark-cache-mode STR    stable-auto|cold-round|cold-group|warm\n"
        << "  --cache-policy STR            Backward-compatible alias\n"
        << "  --group-order STR             official|hdd-optimized\n"
        << "  --execution-mode STR          p4-groups|p5-round (default: p4-groups)\n"
        << "  --rounds N                    Measured rounds (default: 3)\n"
        << "  --seed N                      Request seed (default: 20260511)\n"
        << "  --hdd                         Apply HDD defaults\n";
}

static double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if (values.size() % 2 == 1) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

static double coefficientOfVariation(const std::vector<double>& values) {
    if (values.size() < 2) return 0.0;
    const double mean = std::accumulate(
        values.begin(), values.end(), 0.0
    ) / static_cast<double>(values.size());
    if (mean == 0.0) return 0.0;
    double squared = 0.0;
    for (double value : values) {
        const double difference = value - mean;
        squared += difference * difference;
    }
    const double variance = squared /
        static_cast<double>(values.size() - 1);
    return std::sqrt(variance) / mean;
}

static bool clearForRound(
    erwt3d::RzfpReader& reader,
    const std::shared_ptr<erwt3d::BoundedWindowCache>& cache
) {
    const bool dropped = reader.dropPayloadCache();
    if (cache) cache->clear();
    return dropped;
}

static bool runP5Round(
    erwt3d::RzfpReader& reader,
    const erwt3d::RzfpFileHeader& header,
    const std::vector<GroupSpec>& groups,
    int round,
    const std::string& output_dir,
    const erwt3d::MemoryBudget& budget,
    const erwt3d::RzfpReaderConfig& base_config,
    BenchmarkCacheMode benchmark_mode,
    std::vector<GroupResult>& results
) {
    results.clear();
    results.resize(groups.size());

    std::vector<erwt3d::ContestExecutionGroup> execGroups;
    for (size_t g = 0; g < groups.size(); ++g) {
        const auto& spec = groups[g];
        if (!spec.indices || spec.indices->empty()) continue;
        erwt3d::ContestExecutionGroup eg;
        eg.axis = spec.axis;
        eg.name = spec.axis_name + "_" + spec.mode;
        eg.indices = spec.indices;
        execGroups.push_back(eg);
    }

    std::ostringstream prefix;
    prefix << "r" << round;

    erwt3d::ContestExecutionProfile execProf;
    if (!erwt3d::executeContestRound(
            reader, header, execGroups, output_dir, prefix.str(),
            base_config, budget, &execProf)) {
        return false;
    }

    for (size_t g = 0; g < groups.size(); ++g) {
        GroupResult& result = results[g];
        result.round = round;
        result.group_index = static_cast<int>(g);
        result.axis = groups[g].axis_name;
        result.mode = groups[g].mode;
        result.benchmark_cache_mode = benchmarkModeName(benchmark_mode);
        result.slice_count = static_cast<int>(groups[g].indices->size());
        result.memory_limit_bytes = budget.total_bytes;
        result.window_cache_capacity_bytes = budget.window_cache_bytes;

        uint64_t elements = sliceElements(header, groups[g].axis);
        result.output_bytes_per_slice = elements * sizeof(float);
        result.output_batch_size = static_cast<uint64_t>(groups[g].indices->size());

        result.read_time_ms = execProf.read_time_ms;
        result.write_time_ms = execProf.write_time_ms;
        result.group_time_ms = execProf.total_time_ms / static_cast<double>(groups.size());
        result.selected_strategy = strategyName(execProf.selected_strategy);
        result.strategy_reason = execProf.strategy_reason;
        result.device_seq_mb_s = reader.deviceProfile().sequential_mb_s;
        result.device_seek_ms = reader.deviceProfile().random_seek_ms;

        result.profile.actual_read_bytes = execProf.actual_read_bytes;
        result.logical_leaf_requests = execProf.logical_leaf_requests;
        result.duplicate_leaf_requests = execProf.duplicate_leaf_requests;
        result.eliminated_read_bytes = execProf.eliminated_record_bytes;
        result.round_plan_built = false;
    }

    return true;
}
} // namespace

int main(int argc, char* argv[]) {
    std::string inputPath;
    std::string outputDir;
    int randomCount = 100;
    int continuousCount = 10;
    int decodeThreads = 8;
    std::string memoryLimit = "auto";
    std::string windowCacheLimit = "auto";
    uint64_t readWindowBytes = 512ULL * MiB;
    uint64_t maxGapBytes = 8ULL * MiB;
    uint32_t seed = 20260511;
    int rounds = 3;
    bool hddMode = false;
    std::string strategyText = "auto";
    std::string benchmarkModeText = "stable-auto";
    std::string groupOrder = "official";
    std::string executionModeText = "p4-groups";

    for (int i = 1; i < argc; ++i) {
        const auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << argv[i]
                          << " requires a value\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (std::strcmp(argv[i], "--input") == 0 ||
            std::strcmp(argv[i], "-i") == 0) {
            inputPath = next();
        } else if (std::strcmp(argv[i], "--output-dir") == 0 ||
                   std::strcmp(argv[i], "-o") == 0) {
            outputDir = next();
        } else if (std::strcmp(argv[i], "--random-count") == 0) {
            randomCount = std::stoi(next());
        } else if (std::strcmp(argv[i], "--continuous-count") == 0) {
            continuousCount = std::stoi(next());
        } else if (std::strcmp(argv[i], "--decode-threads") == 0 ||
                   std::strcmp(argv[i], "--threads") == 0) {
            decodeThreads = std::stoi(next());
        } else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 ||
                   std::strcmp(argv[i], "-m") == 0) {
            memoryLimit = next();
        } else if (std::strcmp(argv[i], "--window-cache-mb") == 0) {
            windowCacheLimit = next();
        } else if (std::strcmp(argv[i], "--read-window-bytes") == 0) {
            readWindowBytes = std::stoull(next());
        } else if (std::strcmp(argv[i], "--max-gap-bytes") == 0) {
            maxGapBytes = std::stoull(next());
        } else if (std::strcmp(argv[i], "--read-strategy") == 0) {
            strategyText = next();
        } else if (std::strcmp(argv[i], "--benchmark-cache-mode") == 0) {
            benchmarkModeText = next();
        } else if (std::strcmp(argv[i], "--cache-policy") == 0) {
            const std::string alias = next();
            if (alias == "cold") benchmarkModeText = "cold-group";
            else benchmarkModeText = alias;
        } else if (std::strcmp(argv[i], "--group-order") == 0) {
            groupOrder = next();
        } else if (std::strcmp(argv[i], "--execution-mode") == 0) {
            executionModeText = next();
        } else if (std::strcmp(argv[i], "--rounds") == 0) {
            rounds = std::stoi(next());
        } else if (std::strcmp(argv[i], "--seed") == 0) {
            seed = static_cast<uint32_t>(std::stoul(next()));
        } else if (std::strcmp(argv[i], "--hdd") == 0) {
            hddMode = true;
            decodeThreads = 8;
            readWindowBytes = 512ULL * MiB;
            maxGapBytes = 8ULL * MiB;
        } else if (std::strcmp(argv[i], "--help") == 0 ||
                   std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (inputPath.empty() || outputDir.empty() ||
        randomCount <= 0 || continuousCount <= 0 ||
        decodeThreads <= 0 || rounds <= 0) {
        std::cerr << "Error: invalid or missing required arguments\n";
        printUsage(argv[0]);
        return 1;
    }

    erwt3d::RzfpReadStrategy strategy = erwt3d::RzfpReadStrategy::Auto;
    if (strategyText == "selective") {
        strategy = erwt3d::RzfpReadStrategy::SelectiveLeaf;
    } else if (strategyText == "whole") {
        strategy = erwt3d::RzfpReadStrategy::WholeSuperblock;
    } else if (strategyText == "fullscan") {
        strategy = erwt3d::RzfpReadStrategy::FullPayloadScan;
    } else if (strategyText != "auto") {
        std::cerr << "Error: invalid strategy " << strategyText << "\n";
        return 1;
    }

    BenchmarkCacheMode benchmarkMode = BenchmarkCacheMode::StableAuto;
    if (benchmarkModeText == "cold-round") {
        benchmarkMode = BenchmarkCacheMode::ColdRound;
    } else if (benchmarkModeText == "cold-group") {
        benchmarkMode = BenchmarkCacheMode::ColdGroup;
    } else if (benchmarkModeText == "warm") {
        benchmarkMode = BenchmarkCacheMode::Warm;
    } else if (benchmarkModeText != "stable-auto") {
        std::cerr << "Error: invalid benchmark cache mode "
                  << benchmarkModeText << "\n";
        return 1;
    }

    if (groupOrder != "official" && groupOrder != "hdd-optimized") {
        std::cerr << "Error: group order must be official or hdd-optimized\n";
        return 1;
    }

    ExecutionMode executionMode = ExecutionMode::P4Groups;
    if (executionModeText == "p5-round") {
        executionMode = ExecutionMode::P5Round;
    } else if (executionModeText != "p4-groups") {
        std::cerr << "Error: execution mode must be p4-groups or p5-round\n";
        return 1;
    }

    std::string effectiveExecutionModeText = executionModeText;
    if (executionMode == ExecutionMode::P5Round &&
        benchmarkMode == BenchmarkCacheMode::ColdGroup) {
        executionMode = ExecutionMode::P4Groups;
        effectiveExecutionModeText = "p4-groups";
        std::cout
            << "Note: p5-round is incompatible with cold-group cache mode; "
            << "falling back to p4-groups.\n";
    }

    if (!mkdirP(outputDir)) {
        std::cerr << "Error: cannot create output directory "
                  << outputDir << "\n";
        return 1;
    }

    erwt3d::RzfpReader reader(inputPath);
    if (!reader.ok()) {
        std::cerr << "Error: cannot open RZFP file " << inputPath << "\n";
        return 1;
    }
    const auto& header = reader.header();

    // Calibrate before X random. X may use Raw X Aux and would otherwise
    // bypass the main payload strategy and leave device fields at zero.
    const auto& device = reader.ensureDeviceProfile();

    uint64_t largestOutputBytes = 0;
    largestOutputBytes = std::max(
        largestOutputBytes,
        header.ny * header.nz * sizeof(float)
    );
    largestOutputBytes = std::max(
        largestOutputBytes,
        header.nx * header.nz * sizeof(float)
    );
    largestOutputBytes = std::max(
        largestOutputBytes,
        header.nx * header.ny * sizeof(float)
    );

    erwt3d::MemoryBudget budget = erwt3d::makeMemoryBudget(
        memoryLimit,
        reader.payloadBytes(),
        largestOutputBytes,
        static_cast<uint64_t>(std::max(randomCount, continuousCount))
    );
    if (!budget.valid) {
        std::cerr << "Error: memory budget: " << budget.error << "\n";
        return 1;
    }

    uint64_t cacheCapacity = budget.window_cache_bytes;
    if (windowCacheLimit != "auto") {
        try {
            const uint64_t requestedMiB = std::stoull(windowCacheLimit);
            cacheCapacity = std::min<uint64_t>(
                cacheCapacity,
                requestedMiB * MiB
            );
        } catch (const std::exception&) {
            std::cerr << "Error: --window-cache-mb must be auto or N\n";
            return 1;
        }
    }
    budget.window_cache_bytes = cacheCapacity;

    // Two active merged windows must fit inside the dedicated I/O allowance.
    const uint64_t maximumWindow = std::max<uint64_t>(
        16ULL * MiB,
        budget.io_buffer_bytes / 2
    );
    readWindowBytes = std::min(readWindowBytes, maximumWindow);
    maxGapBytes = std::min(maxGapBytes, readWindowBytes / 8);

    auto windowCache = std::make_shared<erwt3d::BoundedWindowCache>(
        cacheCapacity
    );

    struct _stat64 st{};
    uint64_t fileBytes = 0;
    if (stat(inputPath.c_str(), &st) == 0) {
        fileBytes = static_cast<uint64_t>(st.st_size);
    }
    const std::string sidecarPath = inputPath + ".xp";
    if (stat(sidecarPath.c_str(), &st) == 0) {
        fileBytes += static_cast<uint64_t>(st.st_size);
    }
    const uint64_t rawBytes = erwt3d::rzfpRawSize(header);
    const double storageRatio = rawBytes > 0
        ? static_cast<double>(fileBytes) / static_cast<double>(rawBytes)
        : 0.0;

    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint64_t> xDistribution(0, header.nx - 1);
    std::uniform_int_distribution<uint64_t> yDistribution(0, header.ny - 1);
    std::uniform_int_distribution<uint64_t> zDistribution(0, header.nz - 1);

    std::vector<uint64_t> randomX(randomCount);
    std::vector<uint64_t> randomY(randomCount);
    std::vector<uint64_t> randomZ(randomCount);
    for (int i = 0; i < randomCount; ++i) {
        randomX[i] = xDistribution(rng);
        randomY[i] = yDistribution(rng);
        randomZ[i] = zDistribution(rng);
    }

    const auto makeContinuous = [](uint64_t dimension, int count) {
        const int actual = std::min<int>(
            count,
            static_cast<int>(dimension)
        );
        std::vector<uint64_t> result(static_cast<size_t>(actual));
        const uint64_t start = static_cast<uint64_t>(actual) >= dimension
            ? 0
            : dimension / 2 - static_cast<uint64_t>(actual) / 2;
        for (int i = 0; i < actual; ++i) {
            result[static_cast<size_t>(i)] = start + static_cast<uint64_t>(i);
        }
        return result;
    };

    const auto continuousX = makeContinuous(header.nx, continuousCount);
    const auto continuousY = makeContinuous(header.ny, continuousCount);
    const auto continuousZ = makeContinuous(header.nz, continuousCount);

    std::vector<GroupSpec> groups;
    if (groupOrder == "hdd-optimized") {
        groups = {
            {erwt3d::SliceAxis::X, "x", "random", &randomX},
            {erwt3d::SliceAxis::X, "x", "continuous", &continuousX},
            {erwt3d::SliceAxis::Y, "y", "random", &randomY},
            {erwt3d::SliceAxis::Z, "z", "random", &randomZ},
            {erwt3d::SliceAxis::Y, "y", "continuous", &continuousY},
            {erwt3d::SliceAxis::Z, "z", "continuous", &continuousZ}
        };
    } else {
        groups = {
            {erwt3d::SliceAxis::X, "x", "random", &randomX},
            {erwt3d::SliceAxis::Y, "y", "random", &randomY},
            {erwt3d::SliceAxis::Z, "z", "random", &randomZ},
            {erwt3d::SliceAxis::X, "x", "continuous", &continuousX},
            {erwt3d::SliceAxis::Y, "y", "continuous", &continuousY},
            {erwt3d::SliceAxis::Z, "z", "continuous", &continuousZ}
        };
    }

    erwt3d::RzfpReaderConfig baseConfig;
    baseConfig.hdd = erwt3d::HDDReadWindowConfig{
        readWindowBytes,
        maxGapBytes
    };
    baseConfig.strategy = strategy;
    baseConfig.decode_threads = decodeThreads;
    baseConfig.window_cache = windowCache;
    baseConfig.window_cache_file_identity = reader.fileIdentity();
    baseConfig.use_window_cache = cacheCapacity > 0;
    baseConfig.adaptive.auto_calibrate_device = true;
    baseConfig.adaptive.cache_policy = benchmarkMode == BenchmarkCacheMode::Warm
        ? erwt3d::CachePolicy::WarmAllowed
        : erwt3d::CachePolicy::StableAuto;

    std::cout
        << "============================================================\n"
        << "  ERWT3D RZFP HDD Benchmark\n"
        << "============================================================\n"
        << "  File:          " << inputPath << "\n"
        << "  Dims:          " << header.nx << " x "
        << header.ny << " x " << header.nz << "\n"
        << "  Device:        " << std::fixed << std::setprecision(1)
        << device.sequential_mb_s << " MB/s, "
        << device.random_seek_ms << " ms\n"
        << "  Cache mode:    " << benchmarkModeText << "\n"
        << "  Group order:   " << groupOrder << "\n"
        << "  Exec mode:     " << effectiveExecutionModeText << "\n"
        << "  Rounds:        " << rounds << "\n"
        << "  Memory limit:  " << budget.total_bytes / MiB << " MiB"
        << (budget.automatic ? " (auto)" : " (explicit)") << "\n"
        << "  Output batch:  " << budget.output_batch_size << "\n"
        << "  Window cache:  " << cacheCapacity / MiB << " MiB\n"
        << "  Read window:   " << readWindowBytes / MiB << " MiB\n"
        << "  Strategy:      " << strategyText << "\n"
        << "  Storage ratio: " << std::setprecision(3)
        << storageRatio << "x\n"
        << "  HDD defaults:  " << (hddMode ? "true" : "false") << "\n"
        << "============================================================\n";

    // Warm mode uses one full unmeasured round. Write work is retained so the
    // access sequence matches a real round, but warm-up files use round 0.
    if (benchmarkMode == BenchmarkCacheMode::Warm) {
        std::cout << "Warm-up round...\n";
        if (executionMode == ExecutionMode::P5Round) {
            std::vector<GroupResult> ignored;
            if (!runP5Round(
                    reader, header, groups, 0,
                    outputDir + "/warmup", budget, baseConfig,
                    benchmarkMode, ignored)) {
                return 1;
            }
        } else {
            for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
                GroupResult ignored;
                if (!runGroup(
                        reader, header, groups[groupIndex], 0,
                        static_cast<int>(groupIndex), outputDir,
                        budget, baseConfig, benchmarkMode, ignored)) {
                    return 1;
                }
            }
        }
    }

    std::vector<RoundResult> roundResults;
    roundResults.reserve(static_cast<size_t>(rounds));

    for (int round = 1; round <= rounds; ++round) {
        if (benchmarkMode == BenchmarkCacheMode::ColdRound) {
            (void)clearForRound(reader, windowCache);
        }

        if (executionMode == ExecutionMode::P5Round) {
            std::cout << "\nRound " << round << "/" << rounds << "\n";

            if (benchmarkMode == BenchmarkCacheMode::ColdGroup) {
                (void)clearForRound(reader, windowCache);
            }

            std::vector<GroupResult> groupResults;
            if (!runP5Round(
                    reader, header, groups, round, outputDir,
                    budget, baseConfig, benchmarkMode, groupResults)) {
                return 1;
            }

            RoundResult roundResult;
            roundResult.round = round;
            for (auto& gr : groupResults) {
                std::cout << "  [" << (gr.group_index + 1) << "/"
                          << groups.size() << "] "
                          << gr.axis << " " << gr.mode << "... "
                          << std::fixed << std::setprecision(3)
                          << gr.group_time_ms / 1000.0 << " s"
                          << " strategy=" << gr.selected_strategy
                          << " read="
                          << gr.profile.actual_read_bytes / MiB
                          << " MiB cache="
                          << gr.profile.window_cache_hits << "/"
                          << gr.profile.window_cache_misses
                          << "\n";
                roundResult.total_ms += gr.group_time_ms;
                roundResult.groups.push_back(std::move(gr));
            }
            roundResult.composite_ms =
                roundResult.total_ms / static_cast<double>(groups.size());
            roundResults.push_back(std::move(roundResult));
        } else {
        RoundResult roundResult;
        roundResult.round = round;
        roundResult.groups.reserve(groups.size());

        std::cout << "\nRound " << round << "/" << rounds << "\n";

        for (size_t groupIndex = 0;
             groupIndex < groups.size();
             ++groupIndex) {
            if (benchmarkMode == BenchmarkCacheMode::ColdGroup) {
                (void)clearForRound(reader, windowCache);
            }

            GroupResult groupResult;
            std::cout << "  [" << (groupIndex + 1) << "/"
                      << groups.size() << "] "
                      << groups[groupIndex].axis_name << " "
                      << groups[groupIndex].mode << "..." << std::flush;

            if (!runGroup(
                    reader,
                    header,
                    groups[groupIndex],
                    round,
                    static_cast<int>(groupIndex),
                    outputDir,
                    budget,
                    baseConfig,
                    benchmarkMode,
                    groupResult)) {
                return 1;
            }

            std::cout << " " << std::fixed << std::setprecision(3)
                      << groupResult.group_time_ms / 1000.0 << " s"
                      << " strategy=" << groupResult.selected_strategy
                      << " read="
                      << groupResult.profile.actual_read_bytes / MiB
                      << " MiB cache="
                      << groupResult.profile.window_cache_hits << "/"
                      << groupResult.profile.window_cache_misses
                      << "\n";

            roundResult.total_ms += groupResult.group_time_ms;
            roundResult.groups.push_back(std::move(groupResult));
        }
        roundResult.composite_ms =
            roundResult.total_ms / static_cast<double>(groups.size());
        roundResults.push_back(std::move(roundResult));
    }
    }

    const std::string summaryPath =
        outputDir + "/rzfp_hdd_summary.csv";
    {
        std::ofstream output(summaryPath);
        output
            << "round,group,axis,mode,benchmark_cache_mode,slice_count,"
            << "group_time_ms,read_time_ms,write_time_ms,output_bytes_per_slice,"
            << "output_batch_size,memory_limit_bytes,window_cache_capacity_bytes,"
            << "unique_sbs,unique_leaves,requested_bytes,actual_bytes,read_amp,"
            << "preads,cache_hits,cache_contained_hits,cache_misses,"
            << "cache_resident_bytes,cache_saved_bytes,"
            << "io_ms,decode_ms,scatter_ms,selected_strategy,strategy_reason,"
            << "pred_selective_s,pred_whole_s,pred_fullscan_s,effective_device_mb_s,"
            << "pilot_mb_s,device_seq_mb_s,device_seek_ms,seed,"
            << "logical_leaf_requests,duplicate_leaf_requests,"
            << "logical_record_bytes,eliminated_record_bytes,dedup_ratio,"
            << "eliminated_read_bytes,read_reduction_ratio,"
            << "round_plan_built,round_unique_sbs,round_planned_preads\n";

        for (const auto& round : roundResults) {
            for (const auto& result : round.groups) {
                const auto& profile = result.profile;
                std::string reason = result.strategy_reason;
                std::replace(reason.begin(), reason.end(), ',', ';');
                output
                    << result.round << ',' << result.group_index << ','
                    << result.axis << ',' << result.mode << ','
                    << result.benchmark_cache_mode << ','
                    << result.slice_count << ','
                    << std::fixed << std::setprecision(3)
                    << result.group_time_ms << ','
                    << result.read_time_ms << ','
                    << result.write_time_ms << ','
                    << result.output_bytes_per_slice << ','
                    << result.output_batch_size << ','
                    << result.memory_limit_bytes << ','
                    << result.window_cache_capacity_bytes << ','
                    << profile.unique_superblocks << ','
                    << profile.unique_leaves << ','
                    << profile.requested_record_bytes << ','
                    << profile.actual_read_bytes << ','
                    << profile.readAmplification() << ','
                    << profile.pread_calls << ','
                    << profile.window_cache_hits << ','
                    << profile.window_cache_contained_hits << ','
                    << profile.window_cache_misses << ','
                    << profile.window_cache_resident_bytes << ','
                    << profile.window_cache_saved_read_bytes << ','
                    << profile.io_time_ms << ','
                    << profile.decode_time_ms << ','
                    << profile.scatter_time_ms << ','
                    << result.selected_strategy << ','
                    << reason << ','
                    << profile.predicted_selective_seconds << ','
                    << profile.predicted_whole_seconds << ','
                    << profile.predicted_fullscan_seconds << ','
                    << profile.effective_device_mb_s << ','
                    << profile.pilot_observed_mb_s << ','
                    << result.device_seq_mb_s << ','
                    << result.device_seek_ms << ','
                    << seed << ','
                    << profile.logical_leaf_requests << ','
                    << profile.duplicate_leaf_requests << ','
                    << profile.logical_record_bytes << ','
                    << profile.eliminated_record_bytes << ','
                    << profile.dedupReductionRatio() << ','
                    << result.eliminated_read_bytes << ','
                    << result.read_reduction_ratio << ','
                    << (result.round_plan_built ? 1 : 0) << ','
                    << result.round_unique_superblocks << ','
                    << result.round_planned_preads << '\n';
            }
        }
    }

    std::vector<double> composites;
    composites.reserve(roundResults.size());
    for (const auto& round : roundResults) {
        composites.push_back(round.composite_ms);
    }
    const double meanComposite = std::accumulate(
        composites.begin(), composites.end(), 0.0
    ) / static_cast<double>(composites.size());
    const double medianComposite = median(composites);
    const auto minmax = std::minmax_element(
        composites.begin(), composites.end()
    );
    const double cv = coefficientOfVariation(composites);

    const std::string scorePath =
        outputDir + "/rzfp_hdd_score.csv";
    {
        std::ofstream output(scorePath);
        output << "metric,value\n"
               << "input_file," << inputPath << '\n'
               << "dimensions," << header.nx << 'x'
               << header.ny << 'x' << header.nz << '\n'
               << "benchmark_cache_mode," << benchmarkModeText << '\n'
               << "group_order," << groupOrder << '\n'
               << "execution_mode," << effectiveExecutionModeText << '\n'
               << "requested_execution_mode," << executionModeText << '\n'
               << "rounds," << rounds << '\n'
               << "random_count," << randomCount << '\n'
               << "continuous_count," << continuousCount << '\n'
               << "seed," << seed << '\n'
               << "strategy," << strategyText << '\n'
               << "decode_threads," << decodeThreads << '\n'
               << "memory_limit_mode,"
               << (budget.automatic ? "auto" : "explicit") << '\n'
               << "memory_limit_bytes," << budget.total_bytes << '\n'
               << "output_batch_size," << budget.output_batch_size << '\n'
               << "window_cache_capacity_bytes," << cacheCapacity << '\n'
               << "read_window_bytes," << readWindowBytes << '\n'
               << "max_gap_bytes," << maxGapBytes << '\n'
               << "device_sequential_mb_s," << device.sequential_mb_s << '\n'
               << "device_seek_ms," << device.random_seek_ms << '\n'
               << "device_cache_contamination_suspected,"
               << (device.cache_contamination_suspected ? 1 : 0) << '\n'
               << "file_bytes," << fileBytes << '\n'
               << "raw_bytes," << rawBytes << '\n'
               << "storage_ratio," << storageRatio << '\n'
               << "T_composite_mean_ms," << meanComposite << '\n'
               << "T_composite_median_ms," << medianComposite << '\n'
               << "T_composite_min_ms," << *minmax.first << '\n'
               << "T_composite_max_ms," << *minmax.second << '\n'
               << "T_composite_cv," << cv << '\n';
    }

    std::cout
        << "\n============================================================\n"
        << "  RZFP HDD SUMMARY\n"
        << "============================================================\n"
        << "  Composite mean:   " << std::fixed << std::setprecision(3)
        << meanComposite / 1000.0 << " s\n"
        << "  Composite median: " << medianComposite / 1000.0 << " s\n"
        << "  Composite min:    " << *minmax.first / 1000.0 << " s\n"
        << "  Composite max:    " << *minmax.second / 1000.0 << " s\n"
        << "  CV:               " << cv * 100.0 << "%\n"
        << "  Summary:          " << summaryPath << "\n"
        << "  Score:            " << scorePath << "\n"
        << "============================================================\n";

    return 0;
}
