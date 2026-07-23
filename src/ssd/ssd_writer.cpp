#include "erwt3d/ssd/ssd_writer.hpp"
#include "erwt3d/thread_pool.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace erwt3d {

struct SSDWriterPipeline::Impl {
    SSDWriterConfig cfg;
    SSDWriterStats* stats;

    std::mutex mtx;
    std::condition_variable cv;
    bool stopped = false;

    struct Queued {
        SSDWriteTask task;
    };

    std::deque<Queued> queue;
    uint64_t queueBytes = 0;

    std::vector<std::thread> workers;
    std::vector<uint64_t> workerWriteNS;
};

SSDWriterPipeline::SSDWriterPipeline(const SSDWriterConfig& cfg, SSDWriterStats* stats)
    : cfg_(cfg), stats_(stats), impl_(std::make_unique<Impl>())
{
    impl_->cfg = cfg;
    impl_->stats = stats;
    impl_->workerWriteNS.resize(
        static_cast<size_t>(std::max(1, cfg.writer_threads)), 0);
}

SSDWriterPipeline::~SSDWriterPipeline() {
    finish();
    if (dirFd_ >= 0) close(dirFd_);
}

bool SSDWriterPipeline::createOutputFiles(
    const std::string& dir, const std::vector<std::string>& filenames)
{
    auto t0 = std::chrono::steady_clock::now();

    filenames_ = filenames;

    dirFd_ = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd_ < 0) {
        dirFd_ = open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (dirFd_ < 0) return false;

    if (cfg_.writer_threads > 1 && filenames_.size() > 1) {
        ThreadPool pool(static_cast<size_t>(cfg_.writer_threads));
        std::vector<std::future<bool>> futures;
        for (const auto& fn : filenames_) {
            futures.push_back(pool.submit([this, &fn]() -> bool {
                int fd = openat(dirFd_, fn.c_str(),
                                O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
                if (fd < 0) return false;
                if (cfg_.use_ftruncate && !cfg_.use_posix_fallocate) {
                    close(fd);
                }
                return true;
            }));
        }
        pool.waitAll();
        for (auto& f : futures) if (!f.get()) return false;
    } else {
        for (const auto& fn : filenames_) {
            int fd = openat(dirFd_, fn.c_str(),
                            O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
            if (fd < 0) return false;
            if (cfg_.use_ftruncate && !cfg_.use_posix_fallocate) {
                close(fd);
            }
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    if (stats_) {
        stats_->file_create_ms.store(
            static_cast<uint64_t>(std::chrono::duration<double, std::milli>(t1 - t0).count()));
    }

    precreated_ = true;

    impl_->workers.reserve(static_cast<size_t>(std::max(1, cfg_.writer_threads)));
    for (int i = 0; i < std::max(1, cfg_.writer_threads); ++i) {
            impl_->workers.emplace_back([this, i]() {
            while (true) {
                SSDWriteTask task;
                {
                    std::unique_lock<std::mutex> lk(impl_->mtx);
                    impl_->cv.wait(lk, [this] {
                        return impl_->stopped || !impl_->queue.empty();
                    });
                    if (impl_->stopped && impl_->queue.empty()) return;
                    task = std::move(impl_->queue.front().task);
                    impl_->queue.pop_front();
                    impl_->queueBytes -= task.bytes;
                }

                int fd = openat(dirFd_, task.filepath.c_str(),
                                O_WRONLY | O_CLOEXEC);
                if (fd < 0) continue;

                auto wt0 = std::chrono::steady_clock::now();
                ssize_t nw = pwrite(fd, task.buffer->data(),
                                     static_cast<size_t>(task.bytes), 0);
                (void)nw;
                auto wt1 = std::chrono::steady_clock::now();
                close(fd);

                impl_->workerWriteNS[static_cast<size_t>(i)] +=
                    static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(wt1 - wt0).count());

                if (stats_) {
                    stats_->write_calls.fetch_add(1);
                    stats_->write_bytes.fetch_add(task.bytes);
                    stats_->completed_count.fetch_add(1);
                }
            }
        });
    }

    return true;
}

bool SSDWriterPipeline::enqueue(SSDWriteTask&& task) {
    uint64_t qb;
    {
        std::unique_lock<std::mutex> lk(impl_->mtx);
        while (impl_->queueBytes + task.bytes > cfg_.queue_high_water_bytes &&
               cfg_.queue_high_water_bytes > 0) {
            if (impl_->stopped) return false;
            impl_->cv.wait(lk);
        }
        impl_->queueBytes += task.bytes;
        qb = impl_->queueBytes;
        impl_->queue.push_back(Impl::Queued{std::move(task)});
        if (stats_) {
            stats_->queued_count.fetch_add(1);
            uint64_t peak = stats_->queue_peak_bytes.load();
            while (qb > peak && !stats_->queue_peak_bytes.compare_exchange_weak(peak, qb)) {}
        }
    }
    impl_->cv.notify_one();
    return true;
}

bool SSDWriterPipeline::finish() {
    {
        std::unique_lock<std::mutex> lk(impl_->mtx);
        while (impl_->queueBytes > 0) {
            impl_->cv.wait(lk);
        }
        impl_->stopped = true;
    }
    impl_->cv.notify_all();
    for (auto& w : impl_->workers) {
        if (w.joinable()) w.join();
    }
    impl_->workers.clear();

    if (stats_) {
        uint64_t total_ns = 0;
        for (auto ns : impl_->workerWriteNS) total_ns += ns;
        stats_->writer_time_ns.store(total_ns);
    }
    return true;
}

} // namespace erwt3d
