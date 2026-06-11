// sb_ssd.cpp: SSD-optimized I/O execution
//
// SSD特点: 随机读性能好，IOPS高
// 策略: 多线程并行读取，每个线程独立pread，无需合并读窗口
//
// 包含:
//   - executeSBPlanSerial: 单线程baseline
//   - executeSBPlanParallelRead: 多线程并行 (Static/Dynamic schedule)

#include "erwt3d/sb_ssd.hpp"
#include "erwt3d/thread_pool.hpp"
#include <atomic>
#include <chrono>
#include <unistd.h>

namespace erwt3d {

using namespace detail;

// ============================================================================
// Serial: Single-threaded baseline
// ============================================================================

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

// ============================================================================
// ParallelRead: Multi-threaded parallel pread (SSD optimal)
//
// 每个线程独立读取分配的superblocks，无需共享锁
// - Static: 均匀分配，适合任务均匀的场景
// - Dynamic: 原子计数器，适合任务不均匀的场景
// ============================================================================

bool executeSBPlanParallelRead(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                                float* output, int numThreads, IOProfile* profile,
                                SBSchedule schedule, bool pinThreads) {
    const uint64_t sbBV = sbBytes(hdr);
    size_t n = plan.tasks.size();
    if (n == 0) return true;
    if (numThreads <= 1) return executeSBPlanSerial(fd, plan, hdr, output, profile);

    // Dynamic schedule: atomic counter, threads grab chunks
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
                dynReadMs[t] = lr;
                dynUnpackMs[t] = lu;
                return true;
            }));
        }

        pool.waitAll();
        for (auto& f : futures) if (!f.get()) return false;

        if (profile) {
            double mr = 0, mu = 0, sr = 0, su = 0;
            for (int t = 0; t < numThreads; ++t) {
                mr = std::max(mr, dynReadMs[t]);
                mu = std::max(mu, dynUnpackMs[t]);
                sr += dynReadMs[t];
                su += dynUnpackMs[t];
            }
            profile->read_time_ms = mr;
            profile->unpack_time_ms = mu;
            profile->read_time_sum_ms = sr;
            profile->unpack_time_sum_ms = su;
            profile->superblocks_touched = plan.superblocks_touched;
            profile->pread_calls = plan.pread_calls;
            profile->bytes_read = plan.bytes_read;
            profile->output_bytes = plan.output_bytes;
        }
        return true;
    }

    // Static schedule: evenly partitioned execution
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
                if (pread(fd, buf.data(), sbBV, task.file_offset) != static_cast<ssize_t>(sbBV))
                    return false;
                auto tr1 = std::chrono::high_resolution_clock::now();
                lr += std::chrono::duration<double, std::milli>(tr1 - tr0).count();

                auto tu0 = std::chrono::high_resolution_clock::now();
                unpackLeaves(hdr, plan, task, buf.data(), output);
                auto tu1 = std::chrono::high_resolution_clock::now();
                lu += std::chrono::duration<double, std::milli>(tu1 - tu0).count();
            }

            threadReadMs[t] = lr;
            threadUnpackMs[t] = lu;
            return true;
        }));
    }

    pool.waitAll();
    for (auto& f : futures) if (!f.get()) return false;

    if (profile) {
        double mr = 0, mu = 0, sr = 0, su = 0;
        for (int t = 0; t < numThreads; ++t) {
            mr = std::max(mr, threadReadMs[t]);
            mu = std::max(mu, threadUnpackMs[t]);
            sr += threadReadMs[t];
            su += threadUnpackMs[t];
        }
        profile->read_time_ms = mr;
        profile->unpack_time_ms = mu;
        profile->read_time_sum_ms = sr;
        profile->unpack_time_sum_ms = su;
        profile->superblocks_touched = plan.superblocks_touched;
        profile->pread_calls = plan.pread_calls;
        profile->bytes_read = plan.bytes_read;
        profile->output_bytes = plan.output_bytes;
    }
    return true;
}

} // namespace erwt3d
