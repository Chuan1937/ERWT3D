#include "erwt3d/contest_round_executor.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace erwt3d {

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

bool writeFullyAt(int fd, const void* data, uint64_t bytes, uint64_t offset) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    uint64_t completed = 0;
    while (completed < bytes) {
        const ssize_t written = pwrite(
            fd, cursor + completed,
            static_cast<size_t>(bytes - completed),
            static_cast<off_t>(offset + completed)
        );
        if (written > 0) { completed += static_cast<uint64_t>(written); continue; }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

uint64_t elementsForAxis(const RzfpFileHeader& header, SliceAxis axis) {
    switch (axis) {
        case SliceAxis::X: return header.ny * header.nz;
        case SliceAxis::Y: return header.nx * header.nz;
        case SliceAxis::Z: return header.nx * header.ny;
    }
    return 0;
}

} // namespace

bool executeContestRound(
    RzfpReader& reader,
    const RzfpFileHeader& header,
    const std::vector<ContestExecutionGroup>& groups,
    const std::string& output_dir,
    const std::string& file_prefix,
    const RzfpReaderConfig& reader_config,
    const MemoryBudget& budget,
    ContestExecutionProfile* profile
) {
    if (profile) *profile = ContestExecutionProfile{};
    if (groups.empty()) return true;

    std::vector<uint64_t> outputBytes(groups.size(), 0);
    std::vector<SliceAxis> axes(groups.size());
    std::vector<std::string> modes(groups.size());
    size_t maxSlices = 0;

    for (size_t g = 0; g < groups.size(); ++g) {
        if (!groups[g].indices || groups[g].indices->empty()) continue;
        uint64_t elements = elementsForAxis(header, groups[g].axis);
        outputBytes[g] = elements * sizeof(float) * groups[g].indices->size();
        axes[g] = groups[g].axis;
        modes[g] = (groups[g].name.find("random") != std::string::npos)
            ? "random" : "continuous";
        maxSlices = std::max(maxSlices, groups[g].indices->size());
    }

    constexpr uint64_t kOverhead = 64ULL * 1024 * 1024;
    const uint64_t outputBudget = budget.output_buffer_bytes > kOverhead
        ? budget.output_buffer_bytes - kOverhead : budget.output_buffer_bytes / 2;

    auto phasePlan = buildContestPhasePlan(outputBytes, axes, modes, outputBudget);

    uint64_t batchSize = budget.output_batch_size > 0
        ? budget.output_batch_size
        : maxSlices;

    std::vector<std::vector<int>> allFds(groups.size());
    for (size_t g = 0; g < groups.size(); ++g) {
        if (!groups[g].indices || groups[g].indices->empty()) continue;
        uint64_t elements = elementsForAxis(header, groups[g].axis);
        const uint64_t outBytes = elements * sizeof(float);

        std::ostringstream prefix;
        prefix << file_prefix << "_g" << g << "_" << groups[g].name;
        auto& fds = allFds[g];
        fds.resize(groups[g].indices->size(), -1);
        for (size_t i = 0; i < groups[g].indices->size(); ++i) {
            std::ostringstream oss;
            oss << output_dir << "/" << prefix.str() << "_" << i << ".raw";
            int fd = open(oss.str().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                for (auto& afds : allFds) for (int f : afds) if (f >= 0) close(f);
                return false;
            }
            if (posix_fallocate(fd, 0, static_cast<off_t>(outBytes)) != 0 &&
                ftruncate(fd, static_cast<off_t>(outBytes)) != 0) {
                close(fd);
                return false;
            }
            fds[i] = fd;
        }
    }

    double totalReadMs = 0.0;
    double totalWriteMs = 0.0;
    uint64_t peakOutputBytes = 0;
    RzfpReadProfile aggregateProfile;

    for (const auto& phase : phasePlan.phases) {
        if (phase.group_ids.empty()) continue;

        std::vector<size_t> groupSizes(phase.group_ids.size(), 0);
        std::vector<uint64_t> groupIndices(phase.group_ids.size(), 0);

        while (true) {
            bool anyRemaining = false;
            for (size_t pi = 0; pi < phase.group_ids.size(); ++pi) {
                size_t g = phase.group_ids[pi];
                if (groupIndices[pi] < groups[g].indices->size()) {
                    anyRemaining = true;
                    break;
                }
            }
            if (!anyRemaining) break;

            std::vector<RzfpReader::ContestRoundGroup> cgroups;
            std::vector<std::vector<float>> batchBuffers(phase.group_ids.size());

            uint64_t batchOutputBytes = 0;
            for (size_t pi = 0; pi < phase.group_ids.size(); ++pi) {
                size_t g = phase.group_ids[pi];
                uint64_t elements = elementsForAxis(header, groups[g].axis);
                const uint64_t outPerSlice = elements * sizeof(float);
                const size_t remaining = groups[g].indices->size() - groupIndices[pi];
                const size_t take = std::min(remaining, static_cast<size_t>(batchSize));

                RzfpReader::ContestRoundGroup cg;
                cg.axis = groups[g].axis;
                cg.name = groups[g].name;

                if (take > 0) {
                    auto& bufs = batchBuffers[pi];
                    bufs.resize(take * static_cast<size_t>(elements));
                    for (size_t i = 0; i < take; ++i) {
                        cg.indices.push_back((*groups[g].indices)[groupIndices[pi] + i]);
                        cg.outputs.push_back(bufs.data() + i * static_cast<size_t>(elements));
                    }
                    batchOutputBytes += take * outPerSlice;
                }

                cgroups.push_back(std::move(cg));
            }

            peakOutputBytes = std::max(peakOutputBytes, batchOutputBytes);

            const auto readStart = Clock::now();
            std::vector<RzfpReader::RzfpRoundReadResult> batchResults;
            if (!reader.readContestRound(cgroups, reader_config, &batchResults)) {
                for (auto& afds : allFds) for (int f : afds) if (f >= 0) close(f);
                return false;
            }
            const double readMs = msSince(readStart);
            totalReadMs += readMs;

            if (profile) {
                aggregateProfile.selected_strategy = RzfpReadStrategy::Auto;
                for (const auto& rr : batchResults) {
                    RzfpReadProfile bp;
                    bp.logical_leaf_requests = rr.logical_leaf_requests;
                    bp.unique_leaf_requests = rr.unique_leaves;
                    bp.duplicate_leaf_requests = rr.duplicate_leaf_requests;
                    bp.actual_read_bytes = rr.actual_read_bytes;
                    bp.unique_record_bytes = rr.planned_read_bytes;
                    bp.eliminated_record_bytes = rr.eliminated_read_bytes;
                    bp.io_time_ms = rr.io_time_ms;
                    bp.decode_time_ms = rr.decode_time_ms;
                    bp.scatter_time_ms = rr.scatter_time_ms;
                    if (rr.selected_strategy != RzfpReadStrategy::Auto)
                        bp.selected_strategy = rr.selected_strategy;
                    bp.strategy_reason = rr.strategy_reason;
                    bp.unique_superblocks = rr.round_unique_superblocks;
                    bp.pread_calls = rr.round_planned_preads;
                    accumulateReadProfile(aggregateProfile, bp);
                }
            }

            const auto writeStart = Clock::now();
            for (size_t pi = 0; pi < phase.group_ids.size(); ++pi) {
                size_t g = phase.group_ids[pi];
                uint64_t elements = elementsForAxis(header, groups[g].axis);
                const uint64_t outBytes = elements * sizeof(float);
                const size_t remaining = groups[g].indices->size() - groupIndices[pi];
                const size_t take = std::min(remaining, static_cast<size_t>(batchSize));
                if (take == 0) continue;

                const size_t nOut = cgroups[pi].outputs.size();
                const size_t writeTake = (nOut >= take) ? take : nOut;
                if (nOut < take) {
                    for (auto& afds : allFds) for (int f : afds) if (f >= 0) close(f);
                    return false;
                }

                for (size_t i = 0; i < writeTake; ++i) {
                    if (writeTake > 0 && i >= cgroups[pi].outputs.size()) {
                        for (auto& afds : allFds) for (int f : afds) if (f >= 0) close(f);
                        return false;
                    }
                    if (!writeFullyAt(allFds[g][groupIndices[pi] + i],
                                      cgroups[pi].outputs[i], outBytes, 0)) {
                        for (auto& afds : allFds) for (int f : afds) if (f >= 0) close(f);
                        return false;
                    }
                }
                groupIndices[pi] += writeTake;
            }
            totalWriteMs += msSince(writeStart);
        }
    }

    for (auto& afds : allFds) for (int f : afds) if (f >= 0) close(f);

    if (profile) {
        profile->phase_count = phasePlan.phases.size();
        profile->output_buffer_bytes = peakOutputBytes;
        profile->read_time_ms = totalReadMs;
        profile->write_time_ms = totalWriteMs;
        profile->total_time_ms = totalReadMs + totalWriteMs;
        profile->all_outputs_deferred = phasePlan.all_outputs_deferred;
        profile->logical_leaf_requests = aggregateProfile.logical_leaf_requests;
        profile->duplicate_leaf_requests = aggregateProfile.duplicate_leaf_requests;
        profile->eliminated_record_bytes = aggregateProfile.eliminated_record_bytes;
        profile->actual_read_bytes = aggregateProfile.actual_read_bytes;
        profile->selected_strategy = aggregateProfile.selected_strategy;
        profile->strategy_reason = aggregateProfile.strategy_reason;
    }

    return true;
}

} // namespace erwt3d
