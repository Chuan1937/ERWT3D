#include "erwt3d/ssd_cold/rzfp_cold_executor.hpp"
#include "erwt3d/ssd_cold/cold_request_plan.hpp"
#include "erwt3d/ssd_cold/cold_extent_plan.hpp"
#include "erwt3d/ssd_cold/cold_buffer_pool.hpp"
#include "erwt3d/ssd_cold/cold_profile.hpp"
#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace erwt3d {
namespace ssd_cold {

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

static uint64_t alignedSize(uint64_t s) {
    return (s + 4095) & ~static_cast<uint64_t>(4095);
}

static int openPathRead(const std::string& p) {
    return open(p.c_str(), O_RDONLY | O_CLOEXEC);
}

static uint64_t readPeakRss() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    uint64_t rssKb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            const char* s = line + 6;
            while (*s == ' ' || *s == '\t') ++s;
            rssKb = strtoull(s, nullptr, 10);
            break;
        }
    }
    fclose(f);
    return rssKb / 1024;
}

static void adviseSequential(int fd) {
#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
    (void)fd;
#endif
}

bool executeRzfpAxisLeafColdSSD(
    const std::string& filePath,
    const ContestPositions& positions,
    const std::string& outputDir,
    const RzfpColdConfig& config,
    ColdProfile* profile)
{
    auto tTotal = Clock::now();
    ColdProfile localProfile;
    if (!profile) profile = &localProfile;

    profile->physical_device = "ssd";
    profile->requested_profile = "ssd";
    profile->resolved_read_strategy = "rzfp-axis-leaf-cold";

    auto tPlan = Clock::now();
    ColdRequestPlanResult planResult = buildColdRequestPlan(filePath, positions);
    if (!planResult.ok) {
        std::cerr << "[RZFP-COLD] plan failed: " << planResult.error << "\n";
        return false;
    }
    const auto& plan = planResult.plan;
    const uint64_t nx = planResult.nx;
    const uint64_t ny = planResult.ny;
    const uint64_t nz = planResult.nz;
    profile->plan_time_ms = msSince(tPlan);

    profile->logical_slice_requests = plan.logical_slice_requests;
    profile->logical_leaf_requests = plan.logical_leaf_requests;
    profile->unique_leaf_records = plan.unique_leaf_records;
    profile->duplicate_records_eliminated = plan.duplicate_records_eliminated;
    profile->requested_record_bytes = plan.requested_record_bytes;
    profile->main_payload_read_bytes = plan.main_payload_read_bytes;
    profile->axis_x_read_bytes = plan.axis_x_read_bytes;
    profile->axis_y_read_bytes = plan.axis_y_read_bytes;
    profile->axis_z_read_bytes = plan.axis_z_read_bytes;
    profile->read_threads = config.read_threads;
    profile->decode_threads = config.decode_threads;
    profile->write_threads = config.write_threads;
    profile->max_gap_bytes = config.max_gap_bytes;
    profile->max_extent_bytes = config.max_extent_bytes;

    if (plan.main_payload_read_bytes != 0) {
        std::cerr << "[RZFP-COLD] ASSERTION FAILED: main_payload_read_bytes="
                  << plan.main_payload_read_bytes << " should be 0\n";
        return false;
    }

    ColdExtentPlanConfig extentCfg;
    extentCfg.max_gap_bytes = config.max_gap_bytes;
    extentCfg.max_extent_bytes = config.max_extent_bytes;
    extentCfg.estimated_bandwidth_mb_s = 3000.0;
    extentCfg.io_submission_cost_us = 10.0;
    extentCfg.max_read_amplification = 1.30;
    extentCfg.cross_section_merge = false;

    ColdExtentPlan extentPlan = buildColdExtentPlan(plan.records, extentCfg);
    profile->extent_count = extentPlan.extents.size();
    profile->pread_calls = extentPlan.extents.size();
    profile->actual_read_bytes = extentPlan.planned_read_bytes;
    profile->merged_gap_bytes = extentPlan.gap_bytes;
    profile->read_amplification = extentPlan.planned_read_bytes > 0
        ? static_cast<double>(extentPlan.planned_read_bytes) /
          static_cast<double>(plan.requested_record_bytes)
        : 1.0;

    if (profile->read_amplification > extentCfg.max_read_amplification) {
        std::cerr << "[RZFP-COLD] read amplification " << profile->read_amplification
                  << " exceeds max " << extentCfg.max_read_amplification << "\n";
        return false;
    }

    const uint64_t largestOutputBytes = std::max({ny * nz * sizeof(float),
                                                   nx * nz * sizeof(float),
                                                   nx * ny * sizeof(float)});
    const uint64_t totalOutputBytes = 330ULL * largestOutputBytes;
    const uint64_t memoryLimitBytes = static_cast<uint64_t>(config.memory_limit_mb) << 20;

    bool deferredWrite = false;
    uint64_t writeBatchBytes = totalOutputBytes;
    if (config.write_mode == "deferred" ||
        (config.write_mode == "auto" && totalOutputBytes + extentPlan.planned_read_bytes <= memoryLimitBytes)) {
        deferredWrite = true;
    } else if (config.write_mode == "bounded" || config.write_mode == "auto") {
        deferredWrite = false;
        writeBatchBytes = std::min<uint64_t>(totalOutputBytes, memoryLimitBytes / 4);
    }

    std::vector<std::vector<float>> allOutputs;
    {
        const uint64_t maxOutputs = deferredWrite ? 330ULL : std::min<uint64_t>(330ULL,
            writeBatchBytes / largestOutputBytes);
        allOutputs.resize(maxOutputs);
        for (auto& out : allOutputs) {
            out.resize(largestOutputBytes / sizeof(float), 0.0f);
        }
    }

    struct GroupEntry {
        SliceAxis axis;
        std::string name;
        const std::vector<uint64_t>* indices;
        uint32_t group_id;
    };
    std::vector<GroupEntry> groups;
    {
        uint32_t gid = 0;
        groups.push_back({SliceAxis::X, "x_random", &positions.x_random, gid++});
        groups.push_back({SliceAxis::Y, "y_random", &positions.y_random, gid++});
        groups.push_back({SliceAxis::Z, "z_random", &positions.z_random, gid++});
        groups.push_back({SliceAxis::X, "x_continuous", &positions.x_continuous, gid++});
        groups.push_back({SliceAxis::Y, "y_continuous", &positions.y_continuous, gid++});
        groups.push_back({SliceAxis::Z, "z_continuous", &positions.z_continuous, gid++});
    }

    std::vector<std::string> outputFiles(330);
    int outputSlot = 0;
    for (const auto& g : groups) {
        for (size_t si = 0; si < g.indices->size(); ++si) {
            char name[128];
            snprintf(name, sizeof(name), "contest_%s_%03zu.dat",
                     g.name.c_str(), si);
            outputFiles[outputSlot] = outputDir + "/" + name;

            int fd = open(outputFiles[outputSlot].c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { std::cerr << "cannot create output\n"; return false; }
            if (ftruncate(fd, static_cast<off_t>(largestOutputBytes)) != 0) {
                close(fd); return false;
            }
            close(fd);
            ++outputSlot;
        }
    }

    int mainFd = openPathRead(filePath);
    if (mainFd < 0) { std::cerr << "cannot open file\n"; return false; }
    adviseSequential(mainFd);

    std::vector<int> sectionFds;
    for (const auto& s : plan.records) {
        (void)s;
    }

    auto tIO = Clock::now();

    const int readThreads = std::max(1, config.read_threads);
    const int decodeThreads = std::max(1, config.decode_threads);

    ThreadPool readPool(static_cast<size_t>(readThreads));
    ThreadPool decodePool(static_cast<size_t>(decodeThreads));

    std::vector<RzfpCodec> threadCodecs(static_cast<size_t>(decodeThreads));

    const uint64_t leafX = 4, leafY = 4, leafZ = 4;

    std::atomic<bool> allOk{true};
    std::atomic<uint64_t> totalDecodedLeaves{0};
    std::atomic<uint64_t> totalDecodeErrors{0};

    std::deque<std::future<bool>> decodeFutures;

    for (size_t ei = 0; ei < extentPlan.extents.size() && allOk; ++ei) {
        const auto& ext = extentPlan.extents[ei];
        uint64_t readSize = alignedSize(ext.size);
        uint8_t* buf = static_cast<uint8_t*>(std::aligned_alloc(4096, static_cast<size_t>(readSize)));
        if (!buf) { allOk = false; break; }

        ssize_t nr = pread(mainFd, buf, static_cast<size_t>(ext.size),
                           static_cast<off_t>(ext.file_offset));
        if (nr != static_cast<ssize_t>(ext.size)) {
            std::free(buf);
            allOk = false;
            break;
        }
        profile->pread_calls++;
        profile->actual_read_bytes += ext.size;

        size_t color = ei % static_cast<size_t>(decodeThreads);
        decodeFutures.push_back(decodePool.submit(
            [buf, &ext, &plan, &allOutputs, &groups, &threadCodecs, color,
             nx, ny, nz, leafX, leafY, leafZ, &allOk, &totalDecodedLeaves, &totalDecodeErrors]() -> bool {

                RzfpCodec& codec = threadCodecs[color];

                for (size_t ri = 0; ri < ext.record_count; ++ri) {
                    const auto& rec = plan.records[ri];

                    uint64_t relOff = rec.file_offset - ext.file_offset;
                    if (relOff + rec.record_size > ext.size) continue;

                    uint64_t copyX = std::min<uint64_t>(leafX, nx - rec.gx);
                    uint64_t copyY = std::min<uint64_t>(leafY, ny - rec.gy);
                    uint64_t copyZ = std::min<uint64_t>(leafZ, nz - rec.gz);

                    float leaf[64] = {};
                    if (!codec.decodeRecord(rec.codec, buf + relOff, rec.record_size, leaf)) {
                        totalDecodeErrors++;
                        allOk = false;
                        continue;
                    }
                    totalDecodedLeaves++;

                    for (const auto& target : rec.outputs) {
                        if (target.output_group >= groups.size()) continue;
                        const auto& group = groups[target.output_group];
                        if (target.output_slot >= allOutputs.size()) continue;
                        float* output = allOutputs[target.output_slot].data();
                        if (!output) continue;

                        const uint64_t sliceIdx = (*group.indices)[target.output_slot];
                        const uint32_t local = static_cast<uint32_t>(sliceIdx % (group.axis == SliceAxis::X ? leafX : group.axis == SliceAxis::Y ? leafY : leafZ));

                        switch (group.axis) {
                            case SliceAxis::X:
                                if (local >= copyX) continue;
                                for (uint64_t z = 0; z < copyZ; ++z) {
                                    for (uint64_t y = 0; y < copyY; ++y) {
                                        output[(rec.gy + y) * nz + rec.gz + z] =
                                            leaf[(z * leafY + y) * leafX + local];
                                    }
                                }
                                break;
                            case SliceAxis::Y:
                                if (local >= copyY) continue;
                                for (uint64_t z = 0; z < copyZ; ++z) {
                                    for (uint64_t x = 0; x < copyX; ++x) {
                                        output[(rec.gx + x) * nz + rec.gz + z] =
                                            leaf[(z * leafY + local) * leafX + x];
                                    }
                                }
                                break;
                            case SliceAxis::Z:
                                if (local >= copyZ) continue;
                                for (uint64_t y = 0; y < copyY; ++y) {
                                    for (uint64_t x = 0; x < copyX; ++x) {
                                        output[(rec.gx + x) * ny + rec.gy + y] =
                                            leaf[(local * leafY + y) * leafX + x];
                                    }
                                }
                                break;
                        }
                    }
                }
                std::free(buf);
                return true;
            }));

        while (decodeFutures.size() >= static_cast<size_t>(decodeThreads * 2)) {
            if (!decodeFutures.front().get()) allOk = false;
            decodeFutures.pop_front();
        }
    }

    while (!decodeFutures.empty()) {
        if (!decodeFutures.front().get()) allOk = false;
        decodeFutures.pop_front();
    }

    decodePool.waitAll();

    profile->decode_time_ms += msSince(tIO);
    profile->decoded_leaf_count = totalDecodedLeaves;
    profile->decoder_error_count = totalDecodeErrors;

    if (!allOk) {
        std::cerr << "[RZFP-COLD] decode failed\n";
        close(mainFd);
        return false;
    }

    auto tWrite = Clock::now();

    outputSlot = 0;
    for (const auto& g : groups) {
        for (size_t si = 0; si < g.indices->size(); ++si) {
            int fd = open(outputFiles[outputSlot].c_str(), O_WRONLY);
            if (fd >= 0) {
                (void)pwrite(fd, allOutputs[outputSlot].data(),
                            largestOutputBytes, 0);
                close(fd);
            }
            ++outputSlot;
        }
    }

    profile->write_time_ms = msSince(tWrite);

    close(mainFd);

    profile->process_e2e_ms = msSince(tTotal);
    profile->peak_rss_mib = readPeakRss();

    std::cout << "[RZFP-COLD] process_e2e=" << profile->process_e2e_ms / 1000.0 << "s"
              << " io=" << profile->io_time_ms / 1000.0 << "s"
              << " decode=" << profile->decode_time_ms / 1000.0 << "s"
              << " write=" << profile->write_time_ms / 1000.0 << "s"
              << " read_bytes=" << (profile->actual_read_bytes >> 20) << "MiB"
              << " amp=" << profile->read_amplification
              << " preads=" << profile->pread_calls
              << " records=" << profile->unique_leaf_records
              << " leaves=" << profile->decoded_leaf_count
              << " rss=" << profile->peak_rss_mib << "MiB\n";

    return true;
}

} // namespace ssd_cold
} // namespace erwt3d
