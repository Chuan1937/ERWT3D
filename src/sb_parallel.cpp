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
                                SBSchedule schedule, bool pinThreads) {
    const uint64_t sbBV = sbBytes(hdr);
    size_t n = plan.tasks.size();
    if (n == 0) return true;
    if (numThreads <= 1) return executeSBPlanSerial(fd, plan, hdr, output, profile);

    if (schedule == SBSchedule::Dynamic) {
        const size_t chunkSize = 4;
        auto nextIdx = std::make_shared<std::atomic<size_t>>(0);
        ThreadPool pool(static_cast<size_t>(numThreads), pinThreads);
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
    ThreadPool pool(static_cast<size_t>(numThreads), pinThreads);
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

bool executeSBPlanRunBatch(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                            float* output, int numThreads, size_t memoryLimitMB,
                            IOProfile* profile, bool pinThreads) {
    const uint64_t sbBV = sbBytes(hdr);
    size_t n = plan.tasks.size();
    if (n == 0) return true;
    if (numThreads <= 1) numThreads = 1;

    // Build runs: group contiguous superblock reads
    struct Run { uint64_t file_offset; uint64_t bytes; size_t first_task; size_t task_count; };
    std::vector<Run> runs;
    for (size_t i = 0; i < n; ) {
        Run r;
        r.file_offset = plan.tasks[i].file_offset;
        r.first_task = i;
        r.task_count = 1;
        r.bytes = sbBV;
        while (i + r.task_count < n &&
               plan.tasks[i + r.task_count].file_offset == r.file_offset + r.bytes) {
            r.bytes += sbBV;
            ++r.task_count;
        }
        runs.push_back(r);
        i += r.task_count;
    }

    size_t maxRunBytes = 0;
    for (const auto& r : runs) maxRunBytes = std::max(maxRunBytes, r.bytes);
    size_t maxBufPerThread = memoryLimitMB * 1024ULL * 1024ULL / static_cast<size_t>(numThreads);
    if (maxBufPerThread < sbBV) return false; // memory limit too small for one superblock
    maxBufPerThread = std::min(maxBufPerThread, std::max(maxRunBytes, sbBV * 4));

    auto processRuns = [&](size_t startR, size_t endR) -> bool {
        std::vector<uint8_t> buf(maxBufPerThread);
        for (size_t ri = startR; ri < endR; ++ri) {
            const auto& run = runs[ri];
            if (run.bytes > maxBufPerThread) {
                // Split oversized run into aligned superblock chunks
                uint64_t runOff = run.file_offset;
                uint64_t remaining = run.bytes;
                size_t ti = run.first_task;
                size_t maxTasksPerChunk = maxBufPerThread / sbBV;
                if (maxTasksPerChunk == 0) return false; // memory too small
                while (remaining > 0) {
                    size_t tasksThisChunk = std::min(static_cast<size_t>(run.task_count - (ti - run.first_task)), maxTasksPerChunk);
                    uint64_t chunk = tasksThisChunk * sbBV;
                    if (pread(fd, buf.data(), chunk, runOff) != static_cast<ssize_t>(chunk))
                        return false;
                    for (size_t j = 0; j < tasksThisChunk; ++j)
                        unpackLeaves(hdr, plan, plan.tasks[ti + j], buf.data() + j * sbBV, output);
                    ti += tasksThisChunk;
                    runOff += chunk;
                    remaining -= chunk;
                }
            } else {
                if (pread(fd, buf.data(), run.bytes, run.file_offset) != static_cast<ssize_t>(run.bytes))
                    return false;
                uint64_t off = 0;
                for (size_t j = 0; j < run.task_count; ++j) {
                    unpackLeaves(hdr, plan, plan.tasks[run.first_task + j], buf.data() + off, output);
                    off += sbBV;
                }
            }
        }
        return true;
    };

    if (numThreads == 1) {
        if (!processRuns(0, runs.size())) return false;
    } else {
        ThreadPool pool(static_cast<size_t>(numThreads), pinThreads);
        std::vector<std::future<bool>> futures;
        size_t nr = runs.size();
        for (int t = 0; t < numThreads; ++t) {
            futures.push_back(pool.submit([&, t]() -> bool {
                size_t start = t * nr / numThreads;
                size_t end = (t + 1) * nr / numThreads;
                return processRuns(start, end);
            }));
        }
        pool.waitAll();
        for (auto& f : futures) if (!f.get()) return false;
    }

    if (profile) {
        uint64_t totalRead = 0;
        for (const auto& r : runs) totalRead += r.bytes;
        profile->superblocks_touched = plan.superblocks_touched;
        profile->pread_calls = runs.size();
        profile->bytes_read = totalRead;
        profile->output_bytes = plan.output_bytes;
    }
    return true;
}

bool executeSBPlanLeafIndex(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                             float* output, int numThreads, size_t memoryLimitMB,
                             size_t leafMergeBytes, IOProfile* profile, bool pinThreads) {
    const uint64_t sbBV = sbBytes(hdr);
    const uint64_t lfBV = lfBytes(hdr);
    size_t memLimit = memoryLimitMB * 1024ULL * 1024ULL;
    if (leafMergeBytes < lfBV*2) leafMergeBytes = lfBV * 16;
    if (leafMergeBytes > memLimit / 2) leafMergeBytes = memLimit / 2;
    if (leafMergeBytes < lfBV * 4) return false; // memory too small

    // Build leaf offset list from the plan
    struct LeafOff { uint64_t off; const SBTask* task; uint16_t leafIdx; };
    std::vector<LeafOff> leafOffs;
    for (const auto& task : plan.tasks) {
        for (uint16_t li = 0; li < task.leaf_count; ++li) {
            uint64_t ldOff = task.first_leaf + li; // leaf_data index (not byte offset)
            leafOffs.push_back({task.file_offset + plan.leaf_data[ldOff*4] * lfBV, &task, li});
        }
    }
    if (leafOffs.empty()) return true;

    // Sort by file offset
    std::sort(leafOffs.begin(), leafOffs.end(), [](const LeafOff& a, const LeafOff& b) { return a.off < b.off; });

    // Build merged extents
    struct Ext { uint64_t off, size; size_t firstLeaf, leafCount; };
    std::vector<Ext> extents;
    for (size_t i = 0; i < leafOffs.size(); ) {
        Ext e; e.off = leafOffs[i].off; e.size = lfBV; e.firstLeaf = i; e.leafCount = 1;
        while (i + e.leafCount < leafOffs.size() &&
               leafOffs[i+e.leafCount].off <= e.off + e.size &&
               e.size + lfBV <= leafMergeBytes) {
            e.size += lfBV; ++e.leafCount;
        }
        extents.push_back(e); i += e.leafCount;
    }

    // Execute
    uint64_t totalRead = 0, totalCalls = extents.size();
    auto processExtents = [&](size_t start, size_t end) -> bool {
        std::vector<uint8_t> buf(leafMergeBytes * 2);
        for (size_t ei = start; ei < end; ++ei) {
            const auto& ext = extents[ei];
            if (ext.size > buf.size()) buf.resize(ext.size);
            if (pread(fd, buf.data(), ext.size, ext.off) != static_cast<ssize_t>(ext.size))
                return false;
            for (size_t li = 0; li < ext.leafCount; ++li) {
                const auto& lo = leafOffs[ext.firstLeaf + li];
                // Create single-leaf task to avoid unpacking all leaves
                SBTask oneLeaf = *lo.task;
                oneLeaf.first_leaf = static_cast<uint32_t>(lo.task->first_leaf + lo.leafIdx);
                oneLeaf.leaf_count = 1;
                uint64_t leafOffInBuf = lo.off - ext.off;
                // The leaf data at this offset: the buffer starts at the leaf block
                // unpackLeaves expects the buffer to contain the full superblock
                // We have only the leaf block. Use manual extraction instead.
                uint64_t ldOff = oneLeaf.first_leaf * 4;
                uint64_t morton = plan.leaf_data[ldOff];
                uint64_t param  = plan.leaf_data[ldOff + 3];
                uint32_t loOff = static_cast<uint32_t>(oneLeaf.first_leaf) * 4;
                uint32_t out_base = plan.leaf_out[loOff];
                uint32_t out_stride = plan.leaf_out[loOff + 1];
                uint32_t v_inner = plan.leaf_out[loOff + 2];
                uint32_t v_outer = plan.leaf_out[loOff + 3];
                const float* leaf = reinterpret_cast<const float*>(buf.data() + leafOffInBuf);
                const uint64_t lx=hdr.leaf_x, ly=hdr.leaf_y;
                if (plan.axis == 2) { // Z
                    uint64_t srcBase = param * ly;
                    for (uint32_t v = 0; v < v_outer; ++v)
                        for (uint32_t u = 0; u < v_inner; ++u)
                            output[out_base + v*out_stride + u] = leaf[(srcBase+v)*lx + u];
                } else if (plan.axis == 1) { // Y
                    for (uint32_t v = 0; v < v_outer; ++v)
                        for (uint32_t u = 0; u < v_inner; ++u)
                            output[out_base + v*out_stride + u] = leaf[(v*ly+param)*lx + u];
                } else { // X
                    for (uint32_t v = 0; v < v_outer; ++v)
                        for (uint32_t u = 0; u < v_inner; ++u)
                            output[out_base + v*out_stride + u] = leaf[(v*ly)*lx + param + u*lx];
                }
            }
        }
        return true;
    };

    if (numThreads <= 1) {
        if (!processExtents(0, extents.size())) return false;
    } else {
        ThreadPool pool(static_cast<size_t>(numThreads), pinThreads);
        std::vector<std::future<bool>> futures;
        size_t ne = extents.size();
        for (int t = 0; t < numThreads; ++t) {
            futures.push_back(pool.submit([&, t]() -> bool {
                size_t s = t * ne / numThreads, e = (t+1) * ne / numThreads;
                return processExtents(s, e);
            }));
        }
        pool.waitAll();
        for (auto& f : futures) if (!f.get()) return false;
    }

    if (profile) {
        profile->superblocks_touched = plan.superblocks_touched;
        profile->pread_calls = totalCalls;
        profile->bytes_read = 0; for (auto&e:extents) profile->bytes_read += e.size;
        profile->output_bytes = plan.output_bytes;
    }
    return true;
}

// --- SBTaskOrder: sort tasks by physical file offset ---
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

// --- HDD Read Window: merge tasks into large contiguous reads with gap tolerance ---
bool executeSBPlanHDDReadWindow(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                                float* output, int numThreads, size_t memoryLimitMB,
                                const HDDReadWindowConfig& cfg, IOProfile* profile,
                                bool pinThreads) {
    const uint64_t sbBV = sbBytes(hdr);
    const size_t n = plan.tasks.size();
    if (n == 0) return true;
    if (numThreads <= 1) numThreads = 1;

    const uint64_t rwBytes = cfg.read_window_bytes > 0 ? cfg.read_window_bytes : sbBV * 256;
    const uint64_t gapBytes = cfg.max_gap_bytes;

    struct Window {
        uint64_t file_offset;
        uint64_t read_bytes;
        size_t first_task;
        size_t task_count;
    };
    std::vector<Window> windows;

    for (size_t i = 0; i < n; ) {
        Window w;
        w.file_offset = plan.tasks[i].file_offset;
        w.first_task = i;
        w.task_count = 1;
        w.read_bytes = sbBV;
        while (i + w.task_count < n) {
            uint64_t nextOff = plan.tasks[i + w.task_count].file_offset;
            uint64_t curEnd = w.file_offset + w.read_bytes;
            if (nextOff < curEnd) { ++w.task_count; continue; }
            uint64_t gap = nextOff - curEnd;
            uint64_t extended = curEnd + gap + sbBV - w.file_offset;
            if (gap == 0 && extended <= rwBytes) {
                w.read_bytes += sbBV;
                ++w.task_count;
            } else if (gap > 0 && gap <= gapBytes && extended <= rwBytes) {
                w.read_bytes += gap + sbBV;
                ++w.task_count;
            } else {
                break;
            }
        }
        windows.push_back(w);
        i += w.task_count;
    }

    size_t maxWinBytes = 0;
    for (const auto& w : windows) maxWinBytes = std::max(maxWinBytes, w.read_bytes);
    size_t maxBufPerThread = memoryLimitMB * 1024ULL * 1024ULL / static_cast<size_t>(numThreads);
    if (maxBufPerThread < sbBV) return false;
    maxBufPerThread = std::min(maxBufPerThread, std::max(maxWinBytes, sbBV * 4));

    auto processWindows = [&](size_t startW, size_t endW) -> bool {
        std::vector<uint8_t> buf(maxBufPerThread);
        for (size_t wi = startW; wi < endW; ++wi) {
            const auto& win = windows[wi];
            if (win.read_bytes > maxBufPerThread) {
                uint64_t winOff = win.file_offset;
                uint64_t remaining = win.read_bytes;
                size_t ti = win.first_task;
                size_t maxTasksPerChunk = maxBufPerThread / sbBV;
                if (maxTasksPerChunk == 0) return false;
                while (remaining > 0) {
                    size_t tasksThisChunk = std::min(
                        static_cast<size_t>(win.task_count - (ti - win.first_task)),
                        maxTasksPerChunk);
                    uint64_t chunk = tasksThisChunk * sbBV;
                    uint64_t chunkOff = winOff + (ti - win.first_task) * sbBV;
                    if (pread(fd, buf.data(), chunk, chunkOff) != static_cast<ssize_t>(chunk))
                        return false;
                    for (size_t j = 0; j < tasksThisChunk; ++j)
                        unpackLeaves(hdr, plan, plan.tasks[ti + j],
                                     buf.data() + j * sbBV, output);
                    ti += tasksThisChunk;
                    remaining -= chunk;
                }
            } else {
                if (pread(fd, buf.data(), win.read_bytes, win.file_offset) !=
                    static_cast<ssize_t>(win.read_bytes))
                    return false;
                size_t ti = win.first_task;
                for (size_t j = 0; j < win.task_count; ++j) {
                    uint64_t taskOffInBuf = plan.tasks[ti + j].file_offset - win.file_offset;
                    unpackLeaves(hdr, plan, plan.tasks[ti + j],
                                 buf.data() + taskOffInBuf, output);
                }
            }
        }
        return true;
    };

    double rd = 0;
    if (numThreads == 1) {
        auto tr0 = std::chrono::high_resolution_clock::now();
        if (!processWindows(0, windows.size())) return false;
        auto tr1 = std::chrono::high_resolution_clock::now();
        rd = std::chrono::duration<double, std::milli>(tr1 - tr0).count();
    } else {
        auto tr0 = std::chrono::high_resolution_clock::now();
        ThreadPool pool(static_cast<size_t>(numThreads), pinThreads);
        std::vector<std::future<bool>> futures;
        size_t nw = windows.size();
        for (int t = 0; t < numThreads; ++t) {
            futures.push_back(pool.submit([&, t]() -> bool {
                size_t start = t * nw / numThreads;
                size_t end = (t + 1) * nw / numThreads;
                return processWindows(start, end);
            }));
        }
        pool.waitAll();
        for (auto& f : futures) if (!f.get()) return false;
        auto tr1 = std::chrono::high_resolution_clock::now();
        rd = std::chrono::duration<double, std::milli>(tr1 - tr0).count();
    }

    if (profile) {
        uint64_t totalRead = 0, totalCalls = windows.size();
        for (const auto& w : windows) totalRead += w.read_bytes;
        profile->superblocks_touched = plan.superblocks_touched;
        profile->pread_calls = totalCalls;
        profile->bytes_read = totalRead;
        profile->output_bytes = plan.output_bytes;
        profile->read_time_ms = rd;
        profile->read_time_sum_ms = rd * numThreads;
    }
    return true;
}

// --- Batch planner: global task sort + merge across slice boundaries ---
SBBatchPlan buildSBBatchPlan(const std::vector<const SBTaskPlan*>& plans) {
    SBBatchPlan bp; bp.plans = plans;
    for (uint32_t pid = 0; pid < plans.size(); ++pid) {
        bp.total_sb_touched += plans[pid]->tasks.size();
        for (const auto& t : plans[pid]->tasks)
            bp.batch_tasks.push_back({t.file_offset, t.first_leaf, t.leaf_count, pid, plans[pid]});
    }
    std::sort(bp.batch_tasks.begin(), bp.batch_tasks.end(),
        [](const SBBatchTask& a, const SBBatchTask& b) { return a.file_offset < b.file_offset; });
    return bp;
}

bool executeSBBatchHDD(int fd, const SBBatchPlan& batch, const ERWT3DHeader& hdr,
                       float* const* outputs, int numThreads, size_t memoryLimitMB,
                       const HDDReadWindowConfig& wcfg, bool pinThreads) {
    const uint64_t sbBV = sbBytes(hdr);
    const size_t n = batch.batch_tasks.size();
    if (n == 0) return true;
    if (numThreads <= 1) numThreads = 1;
    const uint64_t rwB = wcfg.read_window_bytes > 0 ? wcfg.read_window_bytes : sbBV * 256;
    const uint64_t gapB = wcfg.max_gap_bytes;

    struct Win { uint64_t fo, rb; size_t ft, tc; };
    std::vector<Win> wins;
    for (size_t i = 0; i < n; ) {
        Win w; w.fo = batch.batch_tasks[i].file_offset; w.ft = i; w.tc = 1; w.rb = sbBV;
        while (i + w.tc < n) {
            uint64_t no = batch.batch_tasks[i + w.tc].file_offset;
            uint64_t ce = w.fo + w.rb;
            if (no < ce) { ++w.tc; continue; }
            uint64_t g = no - ce;
            uint64_t ext = ce + g + sbBV - w.fo;
            if (g == 0 && ext <= rwB) { w.rb += sbBV; ++w.tc; }
            else if (g > 0 && g <= gapB && ext <= rwB) { w.rb += g + sbBV; ++w.tc; }
            else break;
        }
        wins.push_back(w); i += w.tc;
    }
    size_t mwb = 0; for (auto& w : wins) mwb = std::max(mwb, w.rb);
    size_t mbpt = memoryLimitMB * 1024ULL * 1024ULL / static_cast<size_t>(numThreads);
    if (mbpt < sbBV) return false;
    mbpt = std::min(mbpt, std::max(mwb, sbBV * 4));

    auto pw = [&](size_t sw, size_t ew) -> bool {
        std::vector<uint8_t> buf(mbpt);
        for (size_t wi = sw; wi < ew; ++wi) {
            const auto& win = wins[wi];
            if (win.rb > mbpt) {
                uint64_t wo = win.fo, rem = win.rb; size_t ti = win.ft;
                size_t mtpc = mbpt / sbBV; if (mtpc == 0) return false;
                while (rem > 0) {
                    size_t ttc = std::min(static_cast<size_t>(win.tc - (ti - win.ft)), mtpc);
                    uint64_t chunk = ttc * sbBV;
                    if (pread(fd, buf.data(), chunk, wo + (ti - win.ft) * sbBV) != static_cast<ssize_t>(chunk))
                        return false;
                    for (size_t j = 0; j < ttc; ++j) {
                        const auto& bt = batch.batch_tasks[ti + j];
                        SBTask t{bt.file_offset, bt.first_leaf, bt.leaf_count};
                        unpackLeaves(hdr, *bt.plan, t, buf.data() + j * sbBV, outputs[bt.output_id]);
                    }
                    ti += ttc; rem -= chunk;
                }
            } else {
                if (pread(fd, buf.data(), win.rb, win.fo) != static_cast<ssize_t>(win.rb)) return false;
                for (size_t j = 0; j < win.tc; ++j) {
                    const auto& bt = batch.batch_tasks[win.ft + j];
                    uint64_t toff = bt.file_offset - win.fo;
                    SBTask t{bt.file_offset, bt.first_leaf, bt.leaf_count};
                    unpackLeaves(hdr, *bt.plan, t, buf.data() + toff, outputs[bt.output_id]);
                }
            }
        }
        return true;
    };
    if (numThreads == 1) return pw(0, wins.size());
    ThreadPool pool(static_cast<size_t>(numThreads), pinThreads);
    std::vector<std::future<bool>> futs;
    size_t nw = wins.size();
    for (int t = 0; t < numThreads; ++t)
        futs.push_back(pool.submit([&, t]() -> bool { return pw(t*nw/numThreads, (t+1)*nw/numThreads); }));
    pool.waitAll();
    for (auto& f : futs) if (!f.get()) return false;
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
