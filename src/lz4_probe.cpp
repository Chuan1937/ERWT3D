#include "erwt3d/lz4_probe.hpp"
#include "erwt3d/raw_layout.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/raw_x_aux.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <unistd.h>
#include <vector>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

namespace erwt3d {

namespace {

static void packSuperblockFromSlab(
    const float* slab, float* sb,
    uint64_t slabXStart, uint64_t slabXCount,
    uint64_t nx, uint64_t ny, uint64_t nz,
    uint64_t sbSX, uint64_t sbSY, uint64_t sbSZ,
    uint64_t superX, uint64_t superY, uint64_t superZ
) {
    std::memset(sb, 0, superX * superY * superZ * sizeof(float));
    uint64_t startX = sbSX * superX;
    uint64_t localXStart = (startX >= slabXStart) ? (startX - slabXStart) : 0;
    uint64_t localXEnd = std::min(slabXStart + slabXCount - startX, superX);
    if (localXStart >= slabXCount || localXEnd == 0) return;

    uint64_t startY = sbSY * superY;
    uint64_t startZ = sbSZ * superZ;
    for (uint64_t lz = 0; lz < superZ; ++lz) {
        uint64_t gz = startZ + lz; if (gz >= nz) break;
        for (uint64_t ly = 0; ly < superY; ++ly) {
            uint64_t gy = startY + ly; if (gy >= ny) break;
            for (uint64_t lx = localXStart; lx < localXEnd; ++lx) {
                uint64_t gx = startX + lx;
                uint64_t slabOff = (lx * ny + gy) * nz + gz;
                sb[(lz * superY + ly) * superX + lx] = slab[slabOff];
            }
        }
    }
}

static void reorderLeaves(const float* sb, uint8_t* leafBuf,
                           uint32_t superX, uint32_t superY, uint32_t superZ,
                           uint32_t leafX, uint32_t leafY, uint32_t leafZ) {
    uint64_t leafBytes = leafX * leafY * leafZ * sizeof(float);
    uint64_t lpsX = superX / leafX, lpsY = superY / leafY, lpsZ = superZ / leafZ;
    uint64_t totalLeafs = lpsX * lpsY * lpsZ;
    for (uint64_t j = 0; j < totalLeafs; ++j) {
        uint32_t lx, ly, lz; unmorton3D(j, lx, ly, lz);
        if (lx >= lpsX || ly >= lpsY || lz >= lpsZ) continue;
        uint64_t bx = lx * leafX, by = ly * leafY, bz = lz * leafZ;
        float* leaf = reinterpret_cast<float*>(leafBuf + j * leafBytes);
        for (uint32_t z = 0; z < leafZ; ++z)
            for (uint32_t y = 0; y < leafY; ++y)
                for (uint32_t x = 0; x < leafX; ++x)
                    leaf[(z * leafY + y) * leafX + x] =
                        sb[((bz + z) * superY + (by + y)) * superX + (bx + x)];
    }
}

struct ProbeResult {
    bool compressed = false;
    uint64_t raw_size = 0;
    uint64_t comp_size = 0;
};

} // namespace

Lz4ProbeResult probeLz4Compression(const std::string& raw_path,
                                    const Lz4ProbeConfig& config) {
    Lz4ProbeResult result;
#ifndef ERWT3D_HAVE_LZ4
    result.skipped = true;
    result.skip_reason = "LZ4 not compiled";
    return result;
#else
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();

    int fd = open(raw_path.c_str(), O_RDONLY);
    if (fd < 0) {
        result.skipped = true;
        result.skip_reason = "cannot open raw file";
        return result;
    }

    const uint64_t yzFloats = config.ny * config.nz;
    const uint64_t yzBytes = yzFloats * sizeof(float);
    const uint64_t slabX = config.super_x;
    const uint64_t slabFloats = slabX * yzFloats;
    const uint64_t slabBytes = slabFloats * sizeof(float);

    std::vector<float> slab(slabFloats);
    std::vector<float> sb(config.super_x * config.super_y * config.super_z);
    const uint64_t sbBytes = config.super_x * config.super_y * config.super_z * sizeof(float);
    std::vector<uint8_t> leafBuf(sbBytes);
    const int compBound = LZ4_compressBound(static_cast<int>(sbBytes));

    uint64_t sgX = (config.nx + config.super_x - 1) / config.super_x;

    // Stratify: pick slabs evenly across the X dimension
    uint32_t slabCount = std::min(config.slabs_to_sample,
                                   static_cast<uint32_t>((config.nx + slabX - 1) / slabX));
    std::vector<uint64_t> slabStarts;
    for (uint32_t s = 0; s < slabCount; ++s) {
        uint64_t xStart = (static_cast<uint64_t>(s) * config.nx) / slabCount;
        xStart = (xStart / slabX) * slabX;
        if (xStart >= config.nx) xStart = config.nx - slabX;
        slabStarts.push_back(xStart);
    }
    std::sort(slabStarts.begin(), slabStarts.end());
    slabStarts.erase(std::unique(slabStarts.begin(), slabStarts.end()), slabStarts.end());

    uint64_t totalRawSampled = 0;
    uint64_t totalCompSampled = 0;
    uint64_t compressedCount = 0;
    uint64_t superblockCount = 0;

    ThreadPool pool(static_cast<size_t>(std::max(1, config.threads)));

    for (uint64_t xStart : slabStarts) {
        uint64_t readX = std::min(slabX, config.nx - xStart);
        if (!readFullyAt(fd, slab.data(), readX * yzBytes, xStart * yzBytes)) {
            result.skipped = true;
            result.skip_reason = "read error at x=" + std::to_string(xStart);
            close(fd);
            return result;
        }

        uint64_t sxStart = xStart / config.super_x;

        // Pick superblocks evenly across YZ
        uint64_t sgY = (config.ny + config.super_y - 1) / config.super_y;
        uint64_t sgZ = (config.nz + config.super_z - 1) / config.super_z;
        uint32_t sbPerSlab = std::min(config.superblocks_per_slab,
                                       static_cast<uint32_t>(sgY * sgZ));

        std::vector<std::future<ProbeResult>> futures;
        for (uint32_t si = 0; si < sbPerSlab; ++si) {
            uint64_t idx = (static_cast<uint64_t>(si) * sgY * sgZ) / sbPerSlab;
            uint64_t sy = (idx / sgZ) % sgY;
            uint64_t sz = idx % sgZ;
            if (sy >= sgY || sz >= sgZ) continue;

            futures.push_back(pool.submit([&, sxStart, sy, sz]() -> ProbeResult {
                std::vector<float> localSb(config.super_x * config.super_y * config.super_z);
                packSuperblockFromSlab(
                    slab.data(), localSb.data(),
                    xStart, readX,
                    config.nx, config.ny, config.nz,
                    sxStart, sy, sz,
                    config.super_x, config.super_y, config.super_z);

                std::vector<uint8_t> localLeaf(sbBytes);
                reorderLeaves(localSb.data(), localLeaf.data(),
                              config.super_x, config.super_y, config.super_z,
                              config.leaf_x, config.leaf_y, config.leaf_z);

                ProbeResult pr;
                pr.raw_size = sbBytes;
                std::vector<char> comp(compBound);
                int cs = LZ4_compress_default(
                    reinterpret_cast<const char*>(localLeaf.data()),
                    comp.data(), static_cast<int>(sbBytes), compBound);
                if (cs > 0 && static_cast<uint64_t>(cs) < sbBytes * 95 / 100) {
                    pr.compressed = true;
                    pr.comp_size = static_cast<uint64_t>(cs);
                } else {
                    pr.compressed = false;
                    pr.comp_size = sbBytes;
                }
                return pr;
            }));
        }

        for (auto& f : futures) {
            auto pr = f.get();
            totalRawSampled += pr.raw_size;
            totalCompSampled += pr.comp_size;
            if (pr.compressed) ++compressedCount;
            ++superblockCount;
            result.ratios.push_back(static_cast<double>(pr.comp_size) / static_cast<double>(pr.raw_size));
        }
    }

    pool.waitAll();
    close(fd);

    if (superblockCount == 0) {
        result.skipped = true;
        result.skip_reason = "no superblocks sampled";
        return result;
    }

    // Compute real statistics from per-superblock ratios
    result.main_ratio_estimate = static_cast<double>(totalCompSampled) / static_cast<double>(totalRawSampled);
    result.compressed_block_fraction = static_cast<double>(compressedCount) / static_cast<double>(superblockCount);
    result.sampled_superblocks = superblockCount;
    result.sampled_raw_bytes = totalRawSampled;
    result.slabs_sampled = slabStarts.size();

    if (!result.ratios.empty()) {
        std::sort(result.ratios.begin(), result.ratios.end());
        size_t n = result.ratios.size();
        result.main_ratio_median = result.ratios[n / 2];
        result.main_ratio_p10 = result.ratios[std::max(size_t(0), n * 10 / 100)];
        result.main_ratio_p90 = result.ratios[std::min(n - 1, n * 90 / 100)];

        double sumSq = 0.0;
        for (auto r : result.ratios) {
            double d = r - result.main_ratio_estimate;
            sumSq += d * d;
        }
        result.main_ratio_stddev = std::sqrt(sumSq / static_cast<double>(n));
        double ci = 1.96 * result.main_ratio_stddev / std::sqrt(static_cast<double>(n));
        result.main_ratio_lower = std::max(0.0, result.main_ratio_estimate - ci);
        result.main_ratio_upper = result.main_ratio_estimate + ci;
    }

    auto t1 = Clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

    if (result.main_ratio_estimate > config.skip_threshold) {
        result.skipped = true;
        result.skip_reason = "estimated ratio " + std::to_string(result.main_ratio_estimate) +
                             " > threshold " + std::to_string(config.skip_threshold);
    }

    return result;
#endif
}

} // namespace erwt3d
