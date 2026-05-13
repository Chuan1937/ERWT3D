#include "erwt3d/thread_pool.hpp"
#ifdef __linux__
#include <pthread.h>
#endif
#include <iostream>

namespace erwt3d {

ThreadPool::ThreadPool(size_t numThreads, bool pinThreads) : stop_(false), activeTasks_(0) {
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this, i, pinThreads] {
#ifdef __linux__
            if (pinThreads) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(static_cast<int>(i), &cpuset);
                int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
                if (rc != 0)
                    std::cerr << "Warning: pthread_setaffinity_np failed for thread " << i << " (err=" << rc << ")" << std::endl;
            }
#else
            if (pinThreads)
                std::cerr << "Warning: --pin-threads not supported on this platform" << std::endl;
#endif
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    --activeTasks_;
                    if (tasks_.empty() && activeTasks_ == 0)
                        finished_.notify_all();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) {
        worker.join();
    }
}

void ThreadPool::waitAll() {
    std::unique_lock<std::mutex> lock(mutex_);
    finished_.wait(lock, [this] { return tasks_.empty() && activeTasks_ == 0; });
}

} // namespace erwt3d
