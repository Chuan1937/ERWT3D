#pragma once

#include <functional>
#include <future>
#include <queue>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>

namespace erwt3d {

class ThreadPool {
public:
    ThreadPool(size_t numThreads);
    ~ThreadPool();
    
    // Submit a task and get a future
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>;
    
    // Wait for all tasks to complete
    void waitAll();
    
    // Get number of threads
    size_t size() const { return workers_.size(); }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable finished_;
    bool stop_;
    size_t activeTasks_;
};

template<typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
    using return_type = typename std::result_of<F(Args...)>::type;
    
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    
    std::future<return_type> res = task->get_future();
    
    {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (stop_) {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        
        tasks_.emplace([task]() { (*task)(); });
        ++activeTasks_;
    }
    
    condition_.notify_one();
    return res;
}

} // namespace erwt3d