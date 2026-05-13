#include "erwt3d/sb_task.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/thread_pool.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <unistd.h>

namespace erwt3d {

static uint64_t leafsPerX(const ERWT3DHeader& h) { return h.super_x / h.leaf_x; }
static uint64_t leafsPerY(const ERWT3DHeader& h) { return h.super_y / h.leaf_y; }
static uint64_t leafsPerZ(const ERWT3DHeader& h) { return h.super_z / h.leaf_z; }
static uint64_t sbBytes(const ERWT3DHeader& h) { return getSuperblockBytes(h); }
static uint64_t lfBytes(const ERWT3DHeader& h) { return getLeafBytes(h); }

SBTaskPlan buildSBPlanZ(const ERWT3DHeader& hdr, uint64_t z) {
    SBTaskPlan plan; plan.axis = 2;
    const uint64_t nx = hdr.nx, ny = hdr.ny;
    const uint64_t sx = hdr.super_x, sy = hdr.super_y, sz = hdr.super_z;
    const uint64_t lx = hdr.leaf_x, ly = hdr.leaf_y, lz = hdr.leaf_z;
    const uint64_t sbBV = sbBytes(hdr), lfBV = lfBytes(hdr);
    const uint64_t sgX = getSuperGridX(hdr), sgY = getSuperGridY(hdr);
    const uint64_t lpx = leafsPerX(hdr), lpy = leafsPerY(hdr);
    uint64_t superZ = z / sz, leafZ = (z % sz) / lz, inLeafZ = (z % sz) % lz;

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

                    uint64_t mortar = morton3D(static_cast<uint32_t>(lxi),
                                               static_cast<uint32_t>(lyi),
                                               static_cast<uint32_t>(leafZ));
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
    const uint64_t sbBV = sbBytes(hdr), lfBV = lfBytes(hdr);
    const uint64_t sgX = getSuperGridX(hdr), sgY = getSuperGridY(hdr), sgZ = getSuperGridZ(hdr);
    const uint64_t lpx = leafsPerX(hdr), lpz = leafsPerZ(hdr);
    uint64_t superY = y / sy, leafY = (y % sy) / ly, inLeafY = (y % sy) % ly;

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

                    uint64_t mortar = morton3D(static_cast<uint32_t>(lxi),
                                               static_cast<uint32_t>(leafY),
                                               static_cast<uint32_t>(lzi));
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
    const uint64_t sbBV = sbBytes(hdr), lfBV = lfBytes(hdr);
    const uint64_t sgX = getSuperGridX(hdr), sgY = getSuperGridY(hdr), sgZ = getSuperGridZ(hdr);
    const uint64_t lpy = leafsPerY(hdr), lpz = leafsPerZ(hdr);
    uint64_t superX = x / sx, leafX = (x % sx) / lx, inLeafX = (x % sx) % lx;

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

                    uint64_t mortar = morton3D(static_cast<uint32_t>(leafX),
                                               static_cast<uint32_t>(lyi),
                                               static_cast<uint32_t>(lzi));
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

static void unpackLeaves(const ERWT3DHeader& hdr, const SBTaskPlan& plan,
                         const SBTask& task, const uint8_t* sbBuf, float* output) {
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

        const float* leaf = reinterpret_cast<const float*>(sbBuf + morton * lfBV);

        if (plan.axis == 2) {
            // Z-slice: param=inLeafZ, outer=y, inner=x
            uint64_t srcBase = param * ly;
            for (uint32_t outer = 0; outer < v_outer; ++outer) {
                const float* src = leaf + (srcBase + outer) * lx;
                for (uint32_t inner = 0; inner < v_inner; ++inner)
                    output[out_base + outer * out_stride + inner] = src[inner];
            }
        } else if (plan.axis == 1) {
            // Y-slice: param=inLeafY, outer=z, inner=x
            for (uint32_t outer = 0; outer < v_outer; ++outer) {
                const float* src = leaf + (outer * ly + param) * lx;
                for (uint32_t inner = 0; inner < v_inner; ++inner)
                    output[out_base + outer * out_stride + inner] = src[inner];
            }
        } else {
            // X-slice: param=inLeafX, outer=z, inner=y
            for (uint32_t outer = 0; outer < v_outer; ++outer) {
                const float* src = leaf + (outer * ly) * lx + param;
                for (uint32_t inner = 0; inner < v_inner; ++inner)
                    output[out_base + outer * out_stride + inner] = src[inner * lx];
            }
        }
    }
}

bool executeSBPlanSerial(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                         float* output, IOProfile* profile) {
    const uint64_t sbBV = sbBytes(hdr);
    std::vector<uint8_t> buf(sbBV);

    double rd = 0, up = 0;
    for (const auto& task : plan.tasks) {
        auto tr0 = std::chrono::high_resolution_clock::now();
        if (pread(fd, buf.data(), sbBV, task.file_offset) != static_cast<ssize_t>(sbBV))
            return false;
        auto tr1 = std::chrono::high_resolution_clock::now();
        rd += std::chrono::duration<double, std::milli>(tr1 - tr0).count();

        auto tu0 = std::chrono::high_resolution_clock::now();
        unpackLeaves(hdr, plan, task, buf.data(), output);
        auto tu1 = std::chrono::high_resolution_clock::now();
        up += std::chrono::duration<double, std::milli>(tu1 - tu0).count();
    }

    if (profile) {
        profile->read_time_ms = rd;
        profile->unpack_time_ms = up;
        profile->read_time_sum_ms = rd;
        profile->unpack_time_sum_ms = up;
        profile->superblocks_touched = plan.superblocks_touched;
        profile->pread_calls = plan.pread_calls;
        profile->bytes_read = plan.bytes_read;
        profile->output_bytes = plan.output_bytes;
    }
    return true;
}

bool executeSBPlanParallelRead(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                                float* output, int numThreads, IOProfile* profile,
                                SBSchedule schedule) {
    const uint64_t sbBV = sbBytes(hdr);
    size_t n = plan.tasks.size();
    if (n == 0) return true;
    if (numThreads <= 1) return executeSBPlanSerial(fd, plan, hdr, output, profile);

    if (schedule == SBSchedule::Dynamic) {
        const size_t chunkSize = 4;
        auto nextIdx = std::make_shared<std::atomic<size_t>>(0);
        ThreadPool pool(static_cast<size_t>(numThreads));
        std::vector<std::future<bool>> futures;
        std::vector<double> dynReadMs(numThreads, 0);
        std::vector<double> dynUnpackMs(numThreads, 0);
        for (int t = 0; t < numThreads; ++t) {
            futures.push_back(pool.submit([&, t, nextIdx]() -> bool {
                std::vector<uint8_t> buf(sbBV);
                double lr = 0, lu = 0;
                while (true) {
                    size_t i = nextIdx->fetch_add(chunkSize);
                    if (i >= n) break;
                    size_t end = std::min(i + chunkSize, n);
                    for (; i < end; ++i) {
                        const auto& task = plan.tasks[i];
                        auto tr0 = std::chrono::high_resolution_clock::now();
                        if (pread(fd, buf.data(), sbBV, task.file_offset) != static_cast<ssize_t>(sbBV))
                            return false;
                        auto tr1 = std::chrono::high_resolution_clock::now();
                        lr += std::chrono::duration<double, std::milli>(tr1 - tr0).count();
                        auto tu0 = std::chrono::high_resolution_clock::now();
                        unpackLeaves(hdr, plan, task, buf.data(), output);
                        auto tu1 = std::chrono::high_resolution_clock::now();
                        lu += std::chrono::duration<double, std::milli>(tu1 - tu0).count();
                    }
                }
                dynReadMs[t] = lr; dynUnpackMs[t] = lu;
                return true;
            }));
        }
        pool.waitAll();
        for (auto& f : futures) if (!f.get()) return false;
        if (profile) {
            double mr=0,mu=0,sr=0,su=0;
            for (int t=0;t<numThreads;++t){mr=std::max(mr,dynReadMs[t]);mu=std::max(mu,dynUnpackMs[t]);sr+=dynReadMs[t];su+=dynUnpackMs[t];}
            profile->read_time_ms=mr;profile->unpack_time_ms=mu;profile->read_time_sum_ms=sr;profile->unpack_time_sum_ms=su;
            profile->superblocks_touched=plan.superblocks_touched;profile->pread_calls=plan.pread_calls;profile->bytes_read=plan.bytes_read;profile->output_bytes=plan.output_bytes;
        }
        return true;
    }

    // Static: original partitioned execution
    ThreadPool pool(static_cast<size_t>(numThreads));
    std::vector<std::future<bool>> futures;
    std::vector<double> threadReadMs(numThreads, 0);
    std::vector<double> threadUnpackMs(numThreads, 0);
    for (int t = 0; t < numThreads; ++t) {
        futures.push_back(pool.submit([&, t]() -> bool {
            size_t start = t * n / numThreads;
            size_t end = (t + 1) * n / numThreads;
            if (start >= end) return true;
            std::vector<uint8_t> buf(sbBV);
            double lr = 0, lu = 0;
            for (size_t i = start; i < end; ++i) {
                const auto& task = plan.tasks[i];
                auto tr0 = std::chrono::high_resolution_clock::now();
                if (pread(fd, buf.data(), sbBV, task.file_offset) != static_cast<ssize_t>(sbBV)) return false;
                auto tr1 = std::chrono::high_resolution_clock::now();
                lr += std::chrono::duration<double, std::milli>(tr1 - tr0).count();
                auto tu0 = std::chrono::high_resolution_clock::now();
                unpackLeaves(hdr, plan, task, buf.data(), output);
                auto tu1 = std::chrono::high_resolution_clock::now();
                lu += std::chrono::duration<double, std::milli>(tu1 - tu0).count();
            }
            threadReadMs[t] = lr; threadUnpackMs[t] = lu;
            return true;
        }));
    }
    pool.waitAll();
    for (auto& f : futures) if (!f.get()) return false;
    if (profile) {
        double mr = 0, mu = 0, sr = 0, su = 0;
        for (int t=0; t<numThreads; ++t) { mr=std::max(mr,threadReadMs[t]); mu=std::max(mu,threadUnpackMs[t]); sr+=threadReadMs[t]; su+=threadUnpackMs[t]; }
        profile->read_time_ms=mr; profile->unpack_time_ms=mu; profile->read_time_sum_ms=sr; profile->unpack_time_sum_ms=su;
        profile->superblocks_touched=plan.superblocks_touched; profile->pread_calls=plan.pread_calls; profile->bytes_read=plan.bytes_read; profile->output_bytes=plan.output_bytes;
    }
    return true;
}

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
        profile->superblocks_touched = sbCount; profile->panel_hit = true;
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
        profile->superblocks_touched = total; profile->panel_hit = true;
        profile->pread_calls = total;
        profile->bytes_read = total * planeBytes;
        profile->output_bytes = ny * nz * sizeof(float);
    }
    return true;
}

} // namespace erwt3d
