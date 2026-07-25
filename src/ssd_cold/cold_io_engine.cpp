#include "erwt3d/ssd_cold/cold_io_engine.hpp"
#include "erwt3d/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace erwt3d {
namespace ssd_cold {

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

struct ColdIOEngine::Impl {
    ColdIOConfig config;
    ColdBufferPool* pool = nullptr;
    bool running = false;

    std::unique_ptr<ThreadPool> readPool;

    std::deque<ColdCompleteCallback> pending;

    std::atomic<uint64_t> totalBytesRead{0};
    std::atomic<uint64_t> preadCalls{0};

    bool init(const ColdIOConfig& cfg, ColdBufferPool* bp) {
        config = cfg;
        pool = bp;
        int rt = std::max(1, config.read_threads);
        readPool = std::make_unique<ThreadPool>(static_cast<size_t>(rt));
        running = true;
        return true;
    }

    void shutdown() {
        running = false;
        if (readPool) {
            readPool->waitAll();
            readPool.reset();
        }
    }

    bool submit(const ColdIOTask& task, ColdCompleteCallback cb) {
        if (!running || !readPool) return false;

        pending.push_back(std::move(cb));

        readPool->submit([this, task]() {
            ColdIOResult result;
            auto t0 = Clock::now();

            uint64_t totalSize = task.size;
            const uint64_t kAlignment = 4096;
            uint8_t* buf = pool ? pool->allocate(totalSize) :
                static_cast<uint8_t*>(std::aligned_alloc(kAlignment,
                    (totalSize + kAlignment - 1) & ~(kAlignment - 1)));

            if (!buf) {
                result.ok = false;
                {
                    ColdCompleteCallback cb;
                    cb = std::move(pending.front());
                    pending.pop_front();
                    cb(result);
                }
                return;
            }

            uint64_t bytesRead = 0;
            while (bytesRead < totalSize) {
                uint64_t remain = totalSize - bytesRead;
                ssize_t nr = pread(task.fd, buf + bytesRead,
                                    static_cast<size_t>(remain),
                                    static_cast<off_t>(task.offset + bytesRead));
                if (nr <= 0) {
                    if (pool) pool->deallocate(buf, totalSize);
                    else std::free(buf);
                    result.ok = false;
                    {
                        ColdCompleteCallback cb;
                        cb = std::move(pending.front());
                        pending.pop_front();
                        cb(result);
                    }
                    return;
                }
                bytesRead += static_cast<uint64_t>(nr);
            }

            result.ok = true;
            result.bytes_read = bytesRead;
            result.io_time_ms = msSince(t0);

            totalBytesRead += bytesRead;
            preadCalls++;

            {
                ColdCompleteCallback cb;
                cb = std::move(pending.front());
                pending.pop_front();
                cb(result);
            }

            if (pool) pool->deallocate(buf, totalSize);
            else std::free(buf);
        });

        return true;
    }

    bool drain() {
        if (readPool) {
            readPool->waitAll();
        }
        return true;
    }
};

ColdIOEngine::~ColdIOEngine() {
    shutdown();
}

bool ColdIOEngine::init(const ColdIOConfig& cfg, ColdBufferPool* bufferPool) {
    if (impl_) {
        delete impl_;
        impl_ = nullptr;
    }

    if (cfg.backend == ColdIOBackend::IOUring) {
        impl_ = new Impl();
        return impl_->init(cfg, bufferPool);
    }

    impl_ = new Impl();
    return impl_->init(cfg, bufferPool);
}

bool ColdIOEngine::submit(const ColdIOTask& task, ColdCompleteCallback cb) {
    if (!impl_) return false;
    return impl_->submit(task, std::move(cb));
}

bool ColdIOEngine::drain() {
    if (!impl_) return false;
    return impl_->drain();
}

void ColdIOEngine::shutdown() {
    if (impl_) {
        impl_->shutdown();
        delete impl_;
        impl_ = nullptr;
    }
}

} // namespace ssd_cold
} // namespace erwt3d
