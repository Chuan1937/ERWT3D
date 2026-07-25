#include "erwt3d/ssd_cold/rzfp_cold_executor.hpp"
#include "erwt3d/ssd_cold/cold_request_plan.hpp"
#include "erwt3d/ssd_cold/cold_extent_plan.hpp"
#include "erwt3d/ssd_cold/cold_profile.hpp"
#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/format.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <iostream>
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

static uint16_t descriptorSizeVal(RzfpLeafDescriptor d) {
    return static_cast<uint16_t>(d & 0x1FFFu);
}

static RzfpLeafCodec descriptorCodecVal(RzfpLeafDescriptor d) {
    return static_cast<RzfpLeafCodec>((d >> 13) & 0x7u);
}

static void axisLeafRecordCoords(
    const RzfpFileHeader& header,
    uint64_t descriptorId,
    uint64_t& gx, uint64_t& gy, uint64_t& gz)
{
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header);
    const uint64_t physicalSb = descriptorId / leavesPerSB;
    const uint32_t morton = static_cast<uint32_t>(descriptorId % leavesPerSB);

    const uint64_t sgX = rzfpSuperGridX(header);
    const uint64_t sgY = rzfpSuperGridY(header);
    const uint64_t sgZ = rzfpSuperGridZ(header);
    const uint64_t sx = physicalSb % sgX;
    const uint64_t rem = physicalSb / sgX;
    uint64_t sy = 0, sz = 0;
    if ((header.flags & FLAG_PHYSICAL_ORDER_YZX) != 0) {
        sz = rem % sgZ; sy = rem / sgZ;
    } else {
        sy = rem % sgY; sz = rem / sgY;
    }

    uint32_t lx = 0, ly = 0, lz = 0;
    unmorton3D(morton, lx, ly, lz);
    gx = sx * header.super_x + static_cast<uint64_t>(lx) * header.leaf_x;
    gy = sy * header.super_y + static_cast<uint64_t>(ly) * header.leaf_y;
    gz = sz * header.super_z + static_cast<uint64_t>(lz) * header.leaf_z;
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
    profile->read_threads = config.read_threads;
    profile->decode_threads = config.decode_threads;
    profile->write_threads = config.write_threads;
    profile->max_gap_bytes = config.max_gap_bytes;
    profile->max_extent_bytes = config.max_extent_bytes;

    auto tPlan = Clock::now();
    ColdRequestPlanResult planResult = buildColdRequestPlan(filePath, positions);
    if (!planResult.ok) {
        std::cerr << "[RZFP-COLD] plan failed: " << planResult.error << "\n";
        return false;
    }
    const auto& plan = planResult.plan;
    const uint64_t nx = planResult.nx, ny = planResult.ny, nz = planResult.nz;
    const uint64_t leafX = planResult.leaf_x, leafY = planResult.leaf_y, leafZ = planResult.leaf_z;
    profile->plan_time_ms = msSince(tPlan);

    profile->logical_slice_requests = plan.logical_slice_requests;
    profile->unique_leaf_records = plan.unique_slabs;
    profile->duplicate_records_eliminated = plan.duplicate_slabs_eliminated;
    profile->requested_record_bytes = plan.requested_record_bytes;
    profile->main_payload_read_bytes = plan.main_payload_read_bytes;
    profile->axis_x_read_bytes = plan.axis_x_read_bytes;
    profile->axis_y_read_bytes = plan.axis_y_read_bytes;
    profile->axis_z_read_bytes = plan.axis_z_read_bytes;

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
    extentCfg.cross_fd_merge = false;

    ColdExtentPlan extentPlan = buildColdExtentPlan(plan.slab_requests, extentCfg);
    profile->extent_count = extentPlan.extents.size();
    profile->pread_calls = extentPlan.extents.size();
    profile->actual_read_bytes = extentPlan.planned_read_bytes;
    profile->merged_gap_bytes = extentPlan.gap_bytes;
    profile->read_amplification = extentPlan.planned_read_bytes > 0
        ? static_cast<double>(extentPlan.planned_read_bytes) /
          static_cast<double>(plan.requested_record_bytes)
        : 1.0;

    if (profile->read_amplification > 1.50) {
        std::cerr << "[RZFP-COLD] read amplification " << profile->read_amplification
                  << " exceeds 1.50 — probable routing error, abort\n";
        return false;
    }

    struct GroupEntry {
        SliceAxis axis;
        std::string name;
        const std::vector<uint64_t>* indices;
    };
    std::vector<GroupEntry> groups = {
        {SliceAxis::X, "x_random", &positions.x_random},
        {SliceAxis::Y, "y_random", &positions.y_random},
        {SliceAxis::Z, "z_random", &positions.z_random},
        {SliceAxis::X, "x_continuous", &positions.x_continuous},
        {SliceAxis::Y, "y_continuous", &positions.y_continuous},
        {SliceAxis::Z, "z_continuous", &positions.z_continuous},
    };

    const uint64_t largestOutputBytes = std::max({ny * nz * sizeof(float),
                                                   nx * nz * sizeof(float),
                                                   nx * ny * sizeof(float)});
    const uint64_t totalOutputBytes = 330ULL * largestOutputBytes;
    const uint64_t memLimitBytes = static_cast<uint64_t>(config.memory_limit_mb) << 20;

    bool deferredWrite = (config.write_mode == "deferred") ||
        (config.write_mode == "auto" && totalOutputBytes + extentPlan.planned_read_bytes <= memLimitBytes);

    size_t outputBatchSize = deferredWrite ? 330ULL : std::min<size_t>(330ULL,
        (memLimitBytes / 2) / largestOutputBytes);
    if (outputBatchSize == 0) outputBatchSize = 33;

    std::vector<std::vector<float>> allOutputs(outputBatchSize);
    for (auto& o : allOutputs) o.resize(largestOutputBytes / sizeof(float), 0.0f);

    std::vector<std::string> outputFiles(330);
    {
        uint32_t slot = 0;
        for (const auto& g : groups) {
            for (size_t si = 0; si < g.indices->size(); ++si) {
                char name[128];
                snprintf(name, sizeof(name), "contest_%s_%03zu.dat", g.name.c_str(), si);
                outputFiles[slot] = outputDir + "/" + name;
                int fd = open(outputFiles[slot].c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    (void)ftruncate(fd, static_cast<off_t>(largestOutputBytes));
                    close(fd);
                }
                ++slot;
            }
        }
    }

    const auto& rzfpHdr = planResult.rzfp_header;
    const auto& descriptors = planResult.descriptors;

    auto tIO = Clock::now();
    std::atomic<bool> allOk{true};
    std::atomic<uint64_t> totalDecodedLeaves{0};
    std::atomic<uint64_t> totalDecodeErrors{0};
    std::atomic<uint64_t> totalSlabReadBytes{0};
    std::atomic<uint64_t> slabReadCalls{0};

    const int decodeThreads = std::max(1, config.decode_threads);
    ThreadPool decodePool(static_cast<size_t>(decodeThreads));
    std::vector<RzfpCodec> threadCodecs(static_cast<size_t>(decodeThreads));
    std::deque<std::future<bool>> decodeFutures;

    std::vector<uint8_t> readBuf;
    readBuf.reserve(static_cast<size_t>(config.max_extent_bytes));

    for (size_t ei = 0; ei < extentPlan.extents.size() && allOk; ++ei) {
        const auto& ext = extentPlan.extents[ei];
        if (ext.size > readBuf.size()) readBuf.resize(static_cast<size_t>(ext.size));

        ssize_t nr = pread(ext.fd, readBuf.data(), static_cast<size_t>(ext.size),
                           static_cast<off_t>(ext.file_offset));
        if (nr != static_cast<ssize_t>(ext.size)) {
            std::cerr << "[RZFP-COLD] pread failed for extent at " << ext.file_offset
                      << " size " << ext.size << "\n";
            allOk = false; break;
        }
        totalSlabReadBytes += ext.size;
        slabReadCalls++;

        for (size_t si = ext.first_slab; si < ext.first_slab + ext.slab_count && allOk; ++si) {
            if (si >= plan.slab_requests.size()) continue;
            const auto& slab = plan.slab_requests[si];

            uint64_t relOff = slab.file_offset - ext.file_offset;
            if (relOff + slab.slab_bytes > ext.size) continue;

            const uint8_t* slabData = readBuf.data() + relOff;
            const uint64_t slabSize = slab.slab_bytes;

            size_t color = si % static_cast<size_t>(decodeThreads);
            decodeFutures.push_back(decodePool.submit(
                [&threadCodecs, color, slabData, slabSize, &slab, &descriptors,
                 &rzfpHdr, nx, ny, nz, leafX, leafY, leafZ, &allOutputs,
                 &groups, &allOk, &totalDecodedLeaves, &totalDecodeErrors]() -> bool {

                    RzfpCodec& codec = threadCodecs[color];
                    uint64_t offset = 0;

                    while (offset < slabSize && allOk) {
                        if (slabSize - offset < sizeof(uint32_t)) break;

                        uint32_t descriptorId = 0;
                        std::memcpy(&descriptorId, slabData + offset, sizeof(uint32_t));
                        offset += sizeof(uint32_t);

                        if (descriptorId >= descriptors.size()) {
                            totalDecodeErrors++; offset = slabSize; break;
                        }

                        const auto desc = descriptors[descriptorId];
                        const uint16_t recSize = descriptorSizeVal(desc);
                        if (recSize > slabSize - offset) {
                            totalDecodeErrors++; offset = slabSize; break;
                        }

                        uint64_t gx = 0, gy = 0, gz = 0;
                        axisLeafRecordCoords(rzfpHdr, descriptorId, gx, gy, gz);

                        const uint64_t copyX = std::min<uint64_t>(leafX, nx - gx);
                        const uint64_t copyY = std::min<uint64_t>(leafY, ny - gy);
                        const uint64_t copyZ = std::min<uint64_t>(leafZ, nz - gz);

                        float leaf[64] = {};
                        if (!codec.decodeRecord(descriptorCodecVal(desc),
                                slabData + offset, recSize, leaf)) {
                            totalDecodeErrors++;
                        } else {
                            totalDecodedLeaves++;

                            for (const auto& target : slab.targets) {
                                if (target.output_slot >= allOutputs.size()) continue;
                                float* output = allOutputs[target.output_slot].data();
                                if (!output) continue;

                                switch (slab.source) {
                                    case ColdRecordSource::RzfpAxisLeafX:
                                        if (target.local >= copyX) continue;
                                        for (uint64_t z = 0; z < copyZ; ++z)
                                            for (uint64_t y = 0; y < copyY; ++y)
                                                output[(gy + y) * nz + gz + z] =
                                                    leaf[(z * leafY + y) * leafX + target.local];
                                        break;
                                    case ColdRecordSource::RzfpAxisLeafY:
                                        if (target.local >= copyY) continue;
                                        for (uint64_t z = 0; z < copyZ; ++z)
                                            for (uint64_t x = 0; x < copyX; ++x)
                                                output[(gx + x) * nz + gz + z] =
                                                    leaf[(z * leafY + target.local) * leafX + x];
                                        break;
                                    case ColdRecordSource::RzfpAxisLeafZ:
                                        if (target.local >= copyZ) continue;
                                        for (uint64_t y = 0; y < copyY; ++y)
                                            for (uint64_t x = 0; x < copyX; ++x)
                                                output[(gx + x) * ny + gy + y] =
                                                    leaf[(target.local * leafY + y) * leafX + x];
                                        break;
                                    default: break;
                                }
                            }
                        }
                        offset += recSize;
                    }

                    return offset == slabSize;
                }));

            while (decodeFutures.size() >= static_cast<size_t>(decodeThreads * 2)) {
                if (!decodeFutures.front().get()) { allOk = false; }
                decodeFutures.pop_front();
            }
        }
    }

    while (!decodeFutures.empty()) {
        if (!decodeFutures.front().get()) allOk = false;
        decodeFutures.pop_front();
    }
    decodePool.waitAll();

    auto tDecode = Clock::now();
    profile->io_time_ms = msSince(tIO);
    profile->decode_time_ms = msSince(tDecode) - profile->io_time_ms;
    profile->decoded_leaf_count = totalDecodedLeaves;
    profile->decoder_error_count = totalDecodeErrors;
    profile->actual_read_bytes = totalSlabReadBytes;
    profile->pread_calls = slabReadCalls;

    if (!allOk) {
        std::cerr << "[RZFP-COLD] decode errors: " << totalDecodeErrors << "\n";
        return false;
    }

    auto tWrite = Clock::now();

    uint32_t slot = 0;
    size_t batchStart = 0;
    while (batchStart < 330) {
        size_t batchEnd = std::min<size_t>(330, batchStart + outputBatchSize);

        for (size_t bi = batchStart; bi < batchEnd; ++bi) {
            const std::string& fname = outputFiles[bi];
            int ofd = open(fname.c_str(), O_WRONLY);
            if (ofd >= 0) {
                (void)pwrite(ofd, allOutputs[bi - batchStart].data(), largestOutputBytes, 0);
                close(ofd);
            }
        }

        batchStart = batchEnd;
    }

    profile->write_time_ms = msSince(tWrite);

    for (int fd : planResult.section_fds) close(fd);
    if (planResult.main_fd >= 0) close(planResult.main_fd);

    profile->process_e2e_ms = msSince(tTotal);
    profile->peak_rss_mib = readPeakRss();

    std::cout << "[RZFP-COLD] process_e2e=" << profile->process_e2e_ms / 1000.0 << "s"
              << " io=" << profile->io_time_ms / 1000.0 << "s"
              << " decode=" << profile->decode_time_ms / 1000.0 << "s"
              << " write=" << profile->write_time_ms / 1000.0 << "s"
              << " read_mb=" << (profile->actual_read_bytes >> 20)
              << " amp=" << profile->read_amplification
              << " preads=" << profile->pread_calls
              << " slabs=" << profile->unique_leaf_records
              << " leaves=" << profile->decoded_leaf_count
              << " rss=" << profile->peak_rss_mib << "MiB\n";

    return true;
}

} // namespace ssd_cold
} // namespace erwt3d
