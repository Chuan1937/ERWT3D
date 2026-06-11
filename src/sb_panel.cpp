// sb_panel.cpp: Panel optimization for slice reads
//
// Panel: 预存某个轴的切片平面数据，避免读取整个superblock
// 适用于: 频繁访问单轴切片的场景
//
// 当前实现:
//   - X-Panel: 为X轴切片预存YZ平面数据
//
// TODO:
//   - Y-Panel: 为Y轴切片预存XZ平面数据
//   - Z-Panel: 为Z轴切片预存XY平面数据

#include "erwt3d/sb_panel.hpp"
#include "erwt3d/thread_pool.hpp"
#include <algorithm>
#include <vector>
#include <unistd.h>

namespace erwt3d {

// ============================================================================
// X-Panel: Pre-stored YZ planes for X-axis slices
//
// 布局: panelDataOffset + sbIdx * sbPanelBytes + panelIdx * planeBytes
// planeBytes = super_y * super_z * sizeof(float)
// ============================================================================

bool tryReadSliceXPanels(int fd, const ERWT3DHeader& hdr, uint64_t x,
                          float* output, IOProfile* profile) {
    if (!hasXPanels(hdr)) return false;
    uint32_t stride = getPanelStrideX(hdr);
    if (stride == 0) return false;
    uint64_t localX = x % hdr.super_x;
    if (localX % stride != 0) return false;

    uint64_t superX = x / hdr.super_x;
    uint64_t sgY = getSuperGridY(hdr), sgZ = getSuperGridZ(hdr);
    uint64_t sgX = getSuperGridX(hdr);
    uint64_t planeBytes = panelPlaneBytes(hdr);
    uint64_t panelIdx = localX / stride;
    uint64_t sbPanelBytes = panelBytesPerSuperblock(hdr, stride);
    uint64_t panelDataOff = getPanelDataOffset(hdr);
    uint64_t ny = hdr.ny, nz = hdr.nz;

    uint64_t totalRead = 0, sbCount = 0;
    std::vector<float> plane(hdr.super_y * hdr.super_z);

    for (uint64_t szi = 0; szi < sgZ; ++szi) {
        for (uint64_t syi = 0; syi < sgY; ++syi) {
            uint64_t sbIdx = (szi * sgY + syi) * sgX + superX;
            if (sbIdx >= sgX * sgY * sgZ) continue;
            uint64_t sbPanelOff = panelDataOff + sbIdx * sbPanelBytes + panelIdx * planeBytes;
            if (pread(fd, plane.data(), planeBytes, sbPanelOff) != static_cast<ssize_t>(planeBytes))
                return false;
            totalRead += planeBytes; ++sbCount;

            uint64_t dz = szi * hdr.super_z, dy = syi * hdr.super_y;
            uint64_t vz = std::min(static_cast<uint64_t>(hdr.super_z), nz - dz);
            uint64_t vy = std::min(static_cast<uint64_t>(hdr.super_y), ny - dy);
            for (uint64_t z = 0; z < vz; ++z)
                for (uint64_t y = 0; y < vy; ++y)
                    output[(dz + z) * ny + (dy + y)] = plane[z * hdr.super_y + y];
        }
    }

    if (profile) {
        profile->superblocks_touched = sbCount;
        profile->panel_hit = true;
        profile->pread_calls = sbCount;
        profile->bytes_read = totalRead;
        profile->output_bytes = ny * nz * sizeof(float);
    }
    return true;
}

bool tryReadSliceXPanelsParallel(int fd, const ERWT3DHeader& hdr, uint64_t x,
                                  float* output, int numThreads, IOProfile* profile) {
    if (!hasXPanels(hdr)) return false;
    uint32_t stride = getPanelStrideX(hdr);
    if (stride == 0) return false;
    uint64_t localX = x % hdr.super_x;
    if (localX % stride != 0) return false;

    uint64_t superX = x / hdr.super_x;
    uint64_t sgY = getSuperGridY(hdr), sgZ = getSuperGridZ(hdr);
    uint64_t sgX = getSuperGridX(hdr);
    uint64_t planeBytes = panelPlaneBytes(hdr);
    uint64_t panelIdx = localX / stride;
    uint64_t sbPanelBytes = panelBytesPerSuperblock(hdr, stride);
    uint64_t panelDataOff = getPanelDataOffset(hdr);
    uint64_t ny = hdr.ny, nz = hdr.nz;
    uint64_t totalSB = sgY * sgZ;
    if (totalSB == 0) return true;
    size_t nThreads = std::max(1, numThreads);

    // Build per-superblock task list for YZ plane
    struct PanelTask { uint64_t sbIdx; uint64_t dy; uint64_t dz; uint64_t vy; uint64_t vz; };
    std::vector<PanelTask> ptasks;
    for (uint64_t szi = 0; szi < sgZ; ++szi) {
        for (uint64_t syi = 0; syi < sgY; ++syi) {
            uint64_t sbIdx = (szi * sgY + syi) * sgX + superX;
            if (sbIdx >= sgX * sgY * sgZ) continue;
            uint64_t dz = szi * hdr.super_z, dy = syi * hdr.super_y;
            uint64_t vz = std::min(static_cast<uint64_t>(hdr.super_z), nz - dz);
            uint64_t vy = std::min(static_cast<uint64_t>(hdr.super_y), ny - dy);
            if (vz == 0 || vy == 0) continue;
            ptasks.push_back({sbIdx, dy, dz, vy, vz});
        }
    }

    size_t total = ptasks.size();
    uint64_t totalRead = 0;
    std::vector<std::future<bool>> futures;
    ThreadPool pool(std::min(nThreads, total));

    for (size_t t = 0; t < nThreads; ++t) {
        futures.push_back(pool.submit([&, t]() -> bool {
            size_t start = t * total / nThreads;
            size_t end = (t + 1) * total / nThreads;
            std::vector<float> plane(hdr.super_y * hdr.super_z);
            for (size_t i = start; i < end; ++i) {
                const auto& pt = ptasks[i];
                uint64_t off = panelDataOff + pt.sbIdx * sbPanelBytes + panelIdx * planeBytes;
                if (pread(fd, plane.data(), planeBytes, off) != static_cast<ssize_t>(planeBytes))
                    return false;
                for (uint64_t z = 0; z < pt.vz; ++z)
                    for (uint64_t y = 0; y < pt.vy; ++y)
                        output[(pt.dz + z) * ny + (pt.dy + y)] = plane[z * hdr.super_y + y];
            }
            return true;
        }));
    }

    pool.waitAll();
    for (auto& f : futures) if (!f.get()) return false;

    if (profile) {
        profile->superblocks_touched = total;
        profile->panel_hit = true;
        profile->pread_calls = total;
        profile->bytes_read = total * planeBytes;
        profile->output_bytes = ny * nz * sizeof(float);
    }
    return true;
}

} // namespace erwt3d
