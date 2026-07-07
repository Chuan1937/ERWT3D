// sb_common.cpp: Shared logic for SB-based I/O
//
// Contains:
//   - Plan builders (buildSBPlanZ/Y/X)
//   - Leaf unpacking utility
//   - Task ordering utility

#include "erwt3d/sb_plan.hpp"
#include "erwt3d/morton.hpp"
#include <algorithm>

namespace erwt3d {

using namespace detail;

namespace {

std::vector<uint32_t> buildMortonTableXY(uint64_t lpx, uint64_t lpy, uint64_t leafZ) {
    std::vector<uint32_t> table(lpx * lpy);
    for (uint64_t lyi = 0; lyi < lpy; ++lyi) {
        for (uint64_t lxi = 0; lxi < lpx; ++lxi) {
            table[lyi * lpx + lxi] = static_cast<uint32_t>(
                morton3D(static_cast<uint32_t>(lxi),
                         static_cast<uint32_t>(lyi),
                         static_cast<uint32_t>(leafZ)));
        }
    }
    return table;
}

std::vector<uint32_t> buildMortonTableXZ(uint64_t lpx, uint64_t lpz, uint64_t leafY) {
    std::vector<uint32_t> table(lpx * lpz);
    for (uint64_t lzi = 0; lzi < lpz; ++lzi) {
        for (uint64_t lxi = 0; lxi < lpx; ++lxi) {
            table[lzi * lpx + lxi] = static_cast<uint32_t>(
                morton3D(static_cast<uint32_t>(lxi),
                         static_cast<uint32_t>(leafY),
                         static_cast<uint32_t>(lzi)));
        }
    }
    return table;
}

std::vector<uint32_t> buildMortonTableYZ(uint64_t lpy, uint64_t lpz, uint64_t leafX) {
    std::vector<uint32_t> table(lpy * lpz);
    for (uint64_t lzi = 0; lzi < lpz; ++lzi) {
        for (uint64_t lyi = 0; lyi < lpy; ++lyi) {
            table[lzi * lpy + lyi] = static_cast<uint32_t>(
                morton3D(static_cast<uint32_t>(leafX),
                         static_cast<uint32_t>(lyi),
                         static_cast<uint32_t>(lzi)));
        }
    }
    return table;
}

} // namespace

// ============================================================================
// Plan Builders
// ============================================================================

SBTaskPlan buildSBPlanZ(const ERWT3DHeader& hdr, uint64_t z) {
    SBTaskPlan plan; plan.axis = 2;
    const uint64_t nx = hdr.nx, ny = hdr.ny;
    const uint64_t sx = hdr.super_x, sy = hdr.super_y, sz = hdr.super_z;
    const uint64_t lx = hdr.leaf_x, ly = hdr.leaf_y, lz = hdr.leaf_z;
    const uint64_t sbBV = sbBytes(hdr);
    const uint64_t sgX = getSuperGridX(hdr), sgY = getSuperGridY(hdr);
    const uint64_t lpx = leafsPerX(hdr), lpy = leafsPerY(hdr);
    uint64_t superZ = z / sz, leafZ = (z % sz) / lz, inLeafZ = (z % sz) % lz;

    const uint64_t maxTasks = sgY * sgX;
    const uint64_t maxLeaves = maxTasks * lpy * lpx;
    plan.tasks.reserve(maxTasks);
    plan.leaf_data.reserve(maxLeaves * 4);
    plan.leaf_out.reserve(maxLeaves * 4);
    const auto mortonXY = buildMortonTableXY(lpx, lpy, leafZ);

    for (uint64_t syi = 0; syi < sgY; ++syi) {
        for (uint64_t sxi = 0; sxi < sgX; ++sxi) {
            uint64_t sbIdx = (superZ * sgY + syi) * sgX + sxi;
            uint64_t off = hdr.data_offset + sbIdx * sbBV;

            SBTask task;
            task.file_offset = off;
            task.first_leaf = static_cast<uint32_t>(plan.leaf_out.size() / 4);

            uint32_t lcnt = 0;
            for (uint64_t lyi = 0; lyi < lpy; ++lyi) {
                for (uint64_t lxi = 0; lxi < lpx; ++lxi) {
                    uint64_t dx = sxi * sx + lxi * lx, dy = syi * sy + lyi * ly;
                    uint64_t vx = std::min(lx, dx < nx ? nx - dx : uint64_t(0));
                    uint64_t vy = std::min(ly, dy < ny ? ny - dy : uint64_t(0));
                    if (vx == 0 || vy == 0) continue;

                    uint64_t mortar = mortonXY[lyi * lpx + lxi];
                    plan.leaf_data.push_back(mortar);  // 0: mortar
                    plan.leaf_data.push_back(0);        // 1: unused
                    plan.leaf_data.push_back(0);        // 2: unused
                    plan.leaf_data.push_back(inLeafZ);  // 3: param

                    plan.leaf_out.push_back(static_cast<uint32_t>(dy * nx + dx)); // out_base
                    plan.leaf_out.push_back(static_cast<uint32_t>(nx));            // out_stride
                    plan.leaf_out.push_back(static_cast<uint32_t>(vx));            // inner (x)
                    plan.leaf_out.push_back(static_cast<uint32_t>(vy));            // outer (y)
                    ++lcnt;
                }
            }
            if (lcnt == 0) continue;
            task.leaf_count = lcnt;
            plan.tasks.push_back(task);
        }
    }

    plan.superblocks_touched = plan.tasks.size();
    plan.pread_calls = plan.tasks.size();
    plan.bytes_read = plan.tasks.size() * sbBV;
    plan.output_bytes = nx * ny * sizeof(float);
    return plan;
}

SBTaskPlan buildSBPlanY(const ERWT3DHeader& hdr, uint64_t y) {
    SBTaskPlan plan; plan.axis = 1;
    const uint64_t nx = hdr.nx, nz = hdr.nz;
    const uint64_t sx = hdr.super_x, sy = hdr.super_y, sz = hdr.super_z;
    const uint64_t lx = hdr.leaf_x, ly = hdr.leaf_y, lz = hdr.leaf_z;
    const uint64_t sbBV = sbBytes(hdr);
    const uint64_t sgX = getSuperGridX(hdr), sgY = getSuperGridY(hdr), sgZ = getSuperGridZ(hdr);
    const uint64_t lpx = leafsPerX(hdr), lpz = leafsPerZ(hdr);
    uint64_t superY = y / sy, leafY = (y % sy) / ly, inLeafY = (y % sy) % ly;

    const uint64_t maxTasks = sgZ * sgX;
    const uint64_t maxLeaves = maxTasks * lpz * lpx;
    plan.tasks.reserve(maxTasks);
    plan.leaf_data.reserve(maxLeaves * 4);
    plan.leaf_out.reserve(maxLeaves * 4);
    const auto mortonXZ = buildMortonTableXZ(lpx, lpz, leafY);

    for (uint64_t szi = 0; szi < sgZ; ++szi) {
        for (uint64_t sxi = 0; sxi < sgX; ++sxi) {
            uint64_t sbIdx = (szi * sgY + superY) * sgX + sxi;
            uint64_t off = hdr.data_offset + sbIdx * sbBV;

            SBTask task;
            task.file_offset = off;
            task.first_leaf = static_cast<uint32_t>(plan.leaf_out.size() / 4);

            uint32_t lcnt = 0;
            for (uint64_t lzi = 0; lzi < lpz; ++lzi) {
                for (uint64_t lxi = 0; lxi < lpx; ++lxi) {
                    uint64_t dx = sxi * sx + lxi * lx, dz = szi * sz + lzi * lz;
                    uint64_t vx = std::min(lx, dx < nx ? nx - dx : uint64_t(0));
                    uint64_t vz = std::min(lz, dz < nz ? nz - dz : uint64_t(0));
                    if (vx == 0 || vz == 0) continue;

                    uint64_t mortar = mortonXZ[lzi * lpx + lxi];
                    plan.leaf_data.push_back(mortar);
                    plan.leaf_data.push_back(0);
                    plan.leaf_data.push_back(0);
                    plan.leaf_data.push_back(inLeafY);

                    plan.leaf_out.push_back(static_cast<uint32_t>(dz * nx + dx)); // out_base
                    plan.leaf_out.push_back(static_cast<uint32_t>(nx));            // out_stride
                    plan.leaf_out.push_back(static_cast<uint32_t>(vx));            // inner (x)
                    plan.leaf_out.push_back(static_cast<uint32_t>(vz));            // outer (z)
                    ++lcnt;
                }
            }
            if (lcnt == 0) continue;
            task.leaf_count = lcnt;
            plan.tasks.push_back(task);
        }
    }

    plan.superblocks_touched = plan.tasks.size();
    plan.pread_calls = plan.tasks.size();
    plan.bytes_read = plan.tasks.size() * sbBV;
    plan.output_bytes = nz * nx * sizeof(float);
    return plan;
}

SBTaskPlan buildSBPlanX(const ERWT3DHeader& hdr, uint64_t x) {
    SBTaskPlan plan; plan.axis = 0;
    const uint64_t ny = hdr.ny, nz = hdr.nz;
    const uint64_t sx = hdr.super_x, sy = hdr.super_y, sz = hdr.super_z;
    const uint64_t lx = hdr.leaf_x, ly = hdr.leaf_y, lz = hdr.leaf_z;
    const uint64_t sbBV = sbBytes(hdr);
    const uint64_t sgX = getSuperGridX(hdr), sgY = getSuperGridY(hdr), sgZ = getSuperGridZ(hdr);
    const uint64_t lpy = leafsPerY(hdr), lpz = leafsPerZ(hdr);
    uint64_t superX = x / sx, leafX = (x % sx) / lx, inLeafX = (x % sx) % lx;

    const uint64_t maxTasks = sgZ * sgY;
    const uint64_t maxLeaves = maxTasks * lpz * lpy;
    plan.tasks.reserve(maxTasks);
    plan.leaf_data.reserve(maxLeaves * 4);
    plan.leaf_out.reserve(maxLeaves * 4);
    const auto mortonYZ = buildMortonTableYZ(lpy, lpz, leafX);

    for (uint64_t szi = 0; szi < sgZ; ++szi) {
        for (uint64_t syi = 0; syi < sgY; ++syi) {
            uint64_t sbIdx = (szi * sgY + syi) * sgX + superX;
            uint64_t off = hdr.data_offset + sbIdx * sbBV;

            SBTask task;
            task.file_offset = off;
            task.first_leaf = static_cast<uint32_t>(plan.leaf_out.size() / 4);

            uint32_t lcnt = 0;
            for (uint64_t lzi = 0; lzi < lpz; ++lzi) {
                for (uint64_t lyi = 0; lyi < lpy; ++lyi) {
                    uint64_t dy = syi * sy + lyi * ly, dz = szi * sz + lzi * lz;
                    uint64_t vy = std::min(ly, dy < ny ? ny - dy : uint64_t(0));
                    uint64_t vz = std::min(lz, dz < nz ? nz - dz : uint64_t(0));
                    if (vy == 0 || vz == 0) continue;

                    uint64_t mortar = mortonYZ[lzi * lpy + lyi];
                    plan.leaf_data.push_back(mortar);
                    plan.leaf_data.push_back(0);
                    plan.leaf_data.push_back(0);
                    plan.leaf_data.push_back(inLeafX);

                    plan.leaf_out.push_back(static_cast<uint32_t>(dz * ny + dy)); // out_base
                    plan.leaf_out.push_back(static_cast<uint32_t>(ny));            // out_stride
                    plan.leaf_out.push_back(static_cast<uint32_t>(vy));            // inner (y)
                    plan.leaf_out.push_back(static_cast<uint32_t>(vz));            // outer (z)
                    ++lcnt;
                }
            }
            if (lcnt == 0) continue;
            task.leaf_count = lcnt;
            plan.tasks.push_back(task);
        }
    }

    plan.superblocks_touched = plan.tasks.size();
    plan.pread_calls = plan.tasks.size();
    plan.bytes_read = plan.tasks.size() * sbBV;
    plan.output_bytes = nz * ny * sizeof(float);
    return plan;
}

// ============================================================================
// Leaf Unpacking
// ============================================================================

void unpackLeaves(const ERWT3DHeader& hdr, const SBTaskPlan& plan,
                  const SBTask& task, const uint8_t* __restrict__ sbBuf,
                  float* __restrict__ output) {
    const uint64_t lx = hdr.leaf_x, ly = hdr.leaf_y;
    const uint64_t lfBV = lfBytes(hdr);

    for (uint32_t li = 0; li < task.leaf_count; ++li) {
        uint64_t leafIdx = task.first_leaf + li;

        uint64_t ldOff = leafIdx * 4;
        uint64_t morton = plan.leaf_data[ldOff];
        uint64_t param  = plan.leaf_data[ldOff + 3];

        uint32_t loOff = static_cast<uint32_t>(leafIdx) * 4;
        uint32_t out_base   = plan.leaf_out[loOff];
        uint32_t out_stride = plan.leaf_out[loOff + 1];
        uint32_t v_inner    = plan.leaf_out[loOff + 2];
        uint32_t v_outer    = plan.leaf_out[loOff + 3];

        const float* __restrict__ leaf = reinterpret_cast<const float*>(sbBuf + morton * lfBV);

        if (plan.axis == 2) {
            uint64_t srcBase = param * ly;
            for (uint32_t outer = 0; outer < v_outer; ++outer) {
                const float* __restrict__ src = leaf + (srcBase + outer) * lx;
                float* __restrict__ dst = output + out_base + outer * out_stride;
                std::memcpy(dst, src, v_inner * sizeof(float));
            }
        } else if (plan.axis == 1) {
            for (uint32_t outer = 0; outer < v_outer; ++outer) {
                const float* __restrict__ src = leaf + (outer * ly + param) * lx;
                float* __restrict__ dst = output + out_base + outer * out_stride;
                std::memcpy(dst, src, v_inner * sizeof(float));
            }
        } else {
            for (uint32_t outer = 0; outer < v_outer; ++outer) {
                const float* __restrict__ src = leaf + (outer * ly) * lx + param;
                float* __restrict__ dst = output + out_base + outer * out_stride;
                for (uint32_t inner = 0; inner < v_inner; ++inner)
                    dst[inner] = src[inner * lx];
            }
        }
    }
}

// ============================================================================
// Task Ordering
// ============================================================================

void sortTasksByFileOffset(SBTaskPlan& plan) {
    const size_t n = plan.tasks.size();
    if (n <= 1) return;

    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return plan.tasks[a].file_offset < plan.tasks[b].file_offset;
    });

    bool alreadySorted = true;
    for (size_t i = 0; i < n; ++i)
        if (order[i] != i) { alreadySorted = false; break; }
    if (alreadySorted) return;

    std::vector<SBTask> newTasks; newTasks.reserve(n);
    std::vector<uint64_t> newLeafData;
    std::vector<uint32_t> newLeafOut;
    newLeafData.reserve(plan.leaf_data.size());
    newLeafOut.reserve(plan.leaf_out.size());

    for (size_t i = 0; i < n; ++i) {
        const auto& old = plan.tasks[order[i]];
        SBTask nt;
        nt.file_offset = old.file_offset;
        nt.first_leaf = static_cast<uint32_t>(newLeafOut.size() / 4);
        nt.leaf_count = old.leaf_count;
        uint32_t base = old.first_leaf;
        uint32_t cnt = old.leaf_count;
        for (uint32_t li = 0; li < cnt; ++li) {
            newLeafData.insert(newLeafData.end(),
                plan.leaf_data.begin() + (base + li) * 4,
                plan.leaf_data.begin() + (base + li + 1) * 4);
            newLeafOut.insert(newLeafOut.end(),
                plan.leaf_out.begin() + (base + li) * 4,
                plan.leaf_out.begin() + (base + li + 1) * 4);
        }
        newTasks.push_back(nt);
    }

    plan.tasks = std::move(newTasks);
    plan.leaf_data = std::move(newLeafData);
    plan.leaf_out = std::move(newLeafOut);
    plan.superblocks_touched = plan.tasks.size();
    plan.pread_calls = plan.tasks.size();
}

} // namespace erwt3d
