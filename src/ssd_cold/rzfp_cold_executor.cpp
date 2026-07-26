#include "erwt3d/ssd_cold/rzfp_cold_executor.hpp"
#include "erwt3d/ssd_cold/cold_request_plan.hpp"
#include "erwt3d/ssd_cold/cold_extent_plan.hpp"
#include "erwt3d/ssd_cold/cold_profile.hpp"
#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/format.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace erwt3d {
namespace ssd_cold {

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
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

static uint64_t sliceElements(SliceAxis axis, uint64_t nx, uint64_t ny, uint64_t nz) {
    switch (axis) {
        case SliceAxis::X: return ny * nz;
        case SliceAxis::Y: return nx * nz;
        case SliceAxis::Z: return nx * ny;
    }
    return 0;
}

struct SlabDecodeResult {
    bool ok = true;
    int fail_reason = 0;
    uint64_t decoded_leaves = 0;
    uint64_t slab_index = 0;
    double elapsed_ms = 0.0;
};

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

    {
        uint64_t xSlabs = 0, ySlabs = 0, zSlabs = 0;
        for (const auto& s : plan.slab_requests) {
            switch (s.source) {
                case ColdRecordSource::RzfpAxisLeafX: ++xSlabs; break;
                case ColdRecordSource::RzfpAxisLeafY: ++ySlabs; break;
                case ColdRecordSource::RzfpAxisLeafZ: ++zSlabs; break;
                default: break;
            }
        }
        if (xSlabs == 0 || ySlabs == 0 || zSlabs == 0) {
            std::cerr << "[RZFP-COLD] missing axis slabs: X=" << xSlabs
                      << " Y=" << ySlabs << " Z=" << zSlabs << "\n";
            return false;
        }
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

    {
        std::string covErr;
        if (!validateExtentCoverage(plan.slab_requests, extentPlan, covErr)) {
            std::cerr << "[RZFP-COLD] extent coverage: " << covErr << "\n";
            return false;
        }
    }

    if (extentPlan.planned_read_bytes < plan.requested_record_bytes) {
        std::cerr << "[RZFP-COLD] invalid plan: " << extentPlan.planned_read_bytes
                  << " < requested " << plan.requested_record_bytes << "\n";
        return false;
    }

    if (profile->read_amplification > 1.50) {
        std::cerr << "[RZFP-COLD] read amplification " << profile->read_amplification
                  << " exceeds 1.50\n";
        return false;
    }

    const size_t outputCount = plan.slice_requests.size();
    std::vector<std::vector<float>> allOutputs(outputCount);
    std::vector<uint64_t> outputSizes(outputCount);
    for (size_t slot = 0; slot < outputCount; ++slot) {
        const uint64_t elem = sliceElements(plan.slice_requests[slot].axis, nx, ny, nz);
        outputSizes[slot] = elem * sizeof(float);
        allOutputs[slot].resize(elem, 0.0f);
    }

    struct GroupNames {
        SliceAxis axis;
        std::string name;
        const std::vector<uint64_t>* indices;
    };
    auto buildGroupList = [&positions]() -> std::vector<GroupNames> {
        return {
            {SliceAxis::X, "x_random", &positions.x_random},
            {SliceAxis::Y, "y_random", &positions.y_random},
            {SliceAxis::Z, "z_random", &positions.z_random},
            {SliceAxis::X, "x_continuous", &positions.x_continuous},
            {SliceAxis::Y, "y_continuous", &positions.y_continuous},
            {SliceAxis::Z, "z_continuous", &positions.z_continuous},
        };
    };
    const auto groups = buildGroupList();

    std::vector<std::string> outputFiles(outputCount);
    {
        uint32_t slot = 0;
        for (const auto& g : groups) {
            for (size_t si = 0; si < g.indices->size(); ++si) {
                char name[128];
                snprintf(name, sizeof(name), "contest_%s_%03zu.dat", g.name.c_str(), si);
                outputFiles[slot] = outputDir + "/" + name;
                int fd = open(outputFiles[slot].c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    auto osize = static_cast<off_t>(outputSizes[slot]);
                    if (ftruncate(fd, osize) != 0) {
                        close(fd); return false;
                    }
                    close(fd);
                }
                ++slot;
            }
        }
    }

    const auto& rzfpHdr = planResult.rzfp_header;
    const auto& descriptors = planResult.descriptors;

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> totalReadBytes{0};
    std::atomic<uint64_t> totalReadCalls{0};
    double totalIOMs = 0.0;
    uint64_t totalDecodedLeaves = 0;
    uint64_t totalDecodeErrors = 0;
    double totalDecodeMs = 0.0;
    uint64_t totalProcessedSlabs = 0;

    const int decodeThreads = std::max(1, config.decode_threads);
    ThreadPool decodePool(static_cast<size_t>(decodeThreads));
    std::deque<std::future<SlabDecodeResult>> decodeFutures;

    for (size_t ei = 0; ei < extentPlan.extents.size() && !stop; ++ei) {
        const auto& ext = extentPlan.extents[ei];

        uint64_t allocSize = (ext.size + 4095) & ~static_cast<uint64_t>(4095);
        uint8_t* extBufRaw = static_cast<uint8_t*>(std::aligned_alloc(4096, static_cast<size_t>(allocSize)));
        if (!extBufRaw) { stop = true; break; }
        auto extBuf = std::shared_ptr<uint8_t>(extBufRaw, std::free);

        auto tPread = Clock::now();
        ssize_t nr = pread(ext.fd, extBuf.get(), static_cast<size_t>(ext.size),
                           static_cast<off_t>(ext.file_offset));
        totalIOMs += msSince(tPread);

        if (nr != static_cast<ssize_t>(ext.size)) {
            std::cerr << "[RZFP-COLD] pread failed\n";
            stop = true; break;
        }
        totalReadBytes += ext.size;
        totalReadCalls++;

        for (size_t slabIdx : ext.slab_indices) {
            if (slabIdx >= plan.slab_requests.size()) { stop = true; break; }
            const auto& slab = plan.slab_requests[slabIdx];

            if (slab.file_offset < ext.file_offset) {
                std::cerr << "[RZFP-COLD] slab before extent: " << slabIdx << "\n";
                stop = true; break;
            }
            uint64_t relOff = slab.file_offset - ext.file_offset;
            if (relOff > ext.size || slab.slab_bytes > ext.size - relOff) {
                std::cerr << "[RZFP-COLD] slab over extent: " << slabIdx << "\n";
                stop = true; break;
            }

            const uint8_t* slabData = extBuf.get() + relOff;
            const uint64_t slabSize = slab.slab_bytes;

            decodeFutures.push_back(decodePool.submit(
                [extBuf, slabIdx, slabData, slabSize, &plan, &descriptors,
                 &rzfpHdr, nx, ny, nz, leafX, leafY, leafZ, &allOutputs,
                 &stop]() -> SlabDecodeResult {

                    SlabDecodeResult result;
                    result.slab_index = slabIdx;
                    auto tDecStart = Clock::now();
                    const auto& slab = plan.slab_requests[slabIdx];
                    RzfpCodec codec;
                    uint64_t offset = 0;
                    uint64_t recordCount = 0;

                    while (offset < slabSize) {
                        if (slabSize - offset < sizeof(uint32_t)) break;
                        uint32_t descriptorId = 0;
                        std::memcpy(&descriptorId, slabData + offset, sizeof(uint32_t));
                        offset += sizeof(uint32_t);

                        if (descriptorId >= descriptors.size()) {
                            result.fail_reason = 1; result.ok = false; return result;
                        }
                        const auto desc = descriptors[descriptorId];
                        const uint16_t recSize = descriptorSizeVal(desc);
                        if (recSize > slabSize - offset) {
                            result.fail_reason = 2; result.ok = false; return result;
                        }
                        uint64_t gx = 0, gy = 0, gz = 0;
                        axisLeafRecordCoords(rzfpHdr, descriptorId, gx, gy, gz);

                        uint64_t recordSlab = 0;
                        switch (slab.source) {
                            case ColdRecordSource::RzfpAxisLeafX:
                                recordSlab = gx / leafX; break;
                            case ColdRecordSource::RzfpAxisLeafY:
                                recordSlab = gy / leafY; break;
                            case ColdRecordSource::RzfpAxisLeafZ:
                                recordSlab = gz / leafZ; break;
                            default: result.fail_reason = 3; result.ok = false; return result;
                        }
                        if (recordSlab != slab.slab_id) {
                            result.fail_reason = 4; result.ok = false; return result;
                        }
                        if (gx >= nx || gy >= ny || gz >= nz) {
                            offset += recSize; continue;
                        }
                        const uint64_t copyX = std::min<uint64_t>(leafX, nx - gx);
                        const uint64_t copyY = std::min<uint64_t>(leafY, ny - gy);
                        const uint64_t copyZ = std::min<uint64_t>(leafZ, nz - gz);

                        alignas(64) float leaf[64];
                        if (!codec.decodeRecord(descriptorCodecVal(desc),
                                slabData + offset, recSize, leaf)) {
                            result.fail_reason = 5; result.ok = false; return result;
                        }
                        ++result.decoded_leaves;

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
                        offset += recSize;
                        if ((++recordCount & 4095u) == 0 &&
                            stop.load(std::memory_order_relaxed))
                            return result;
                    }
                    result.elapsed_ms = msSince(tDecStart);
                    return result;
                }));

            while (decodeFutures.size() >= static_cast<size_t>(decodeThreads * 2)) {
                auto r = decodeFutures.front().get();
                decodeFutures.pop_front();
                ++totalProcessedSlabs;
                if (!r.ok) {
                    std::cerr << "[RZFP] slab decode fail reason=" << r.fail_reason << "\n";
                    stop = true; break;
                }
                totalDecodedLeaves += r.decoded_leaves;
                totalDecodeMs += r.elapsed_ms;
            }
            if (stop) break;
        }
    }

    while (!decodeFutures.empty()) {
        auto r = decodeFutures.front().get();
        decodeFutures.pop_front();
        ++totalProcessedSlabs;
        if (!r.ok) { stop = true; ++totalDecodeErrors; }
        totalDecodedLeaves += r.decoded_leaves;
        totalDecodeMs += r.elapsed_ms;
    }
    decodePool.waitAll();

    profile->io_time_ms = totalIOMs;
    profile->decode_time_ms = totalDecodeMs;
    profile->decoded_leaf_count = totalDecodedLeaves;
    profile->decoder_error_count = totalDecodeErrors;
    profile->actual_read_bytes = totalReadBytes;
    profile->pread_calls = totalReadCalls;

    if (stop) {
        std::cerr << "[RZFP-COLD] decode errors: " << totalDecodeErrors << "\n";
        return false;
    }

    if (totalProcessedSlabs != plan.slab_requests.size()) {
        std::cerr << "[RZFP-COLD] slab mismatch: " << totalProcessedSlabs
                  << "/" << plan.slab_requests.size() << "\n";
        return false;
    }

    auto tWrite = Clock::now();
    uint32_t slot = 0;
    for (const auto& g : groups) {
        for (size_t si = 0; si < g.indices->size(); ++si) {
            int ofd = open(outputFiles[slot].c_str(), O_WRONLY);
            if (ofd >= 0) {
                auto osize = outputSizes[slot];
                uint64_t written = 0;
                const uint8_t* src = reinterpret_cast<const uint8_t*>(allOutputs[slot].data());
                while (written < osize) {
                    ssize_t n = pwrite(ofd, src + written, static_cast<size_t>(osize - written),
                                       static_cast<off_t>(written));
                    if (n < 0) { if (errno == EINTR) continue; break; }
                    if (n == 0) break;
                    written += static_cast<uint64_t>(n);
                }
                if (written != osize) { close(ofd); return false; }
                close(ofd);
            }
            ++slot;
        }
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
              << " preads=" << profile->pread_calls
              << " slabs=" << totalProcessedSlabs
              << " leaves=" << profile->decoded_leaf_count
              << " errors=" << profile->decoder_error_count
              << " rss=" << profile->peak_rss_mib << "MiB\n";

    return true;
}

} // namespace ssd_cold
} // namespace erwt3d
