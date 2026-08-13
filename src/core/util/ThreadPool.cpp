#include "ThreadPool.h"

#include <algorithm>

namespace pixet {

ThreadPool::ThreadPool(size_t numThreads) {
    numThreads = std::max<size_t>(1, numThreads);
    workers_.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) workers_.emplace_back([this]() { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (std::thread &w : workers_) w.join();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
            // Drains whatever's left in the queue even after stop_ is set, rather than
            // abandoning it - the destructor's contract is "every submitted task runs",
            // not "runs unless a shutdown happened to race it". A caller that doesn't
            // want that should stop submitting before destroying the pool, not rely on
            // in-flight work being silently dropped.
            if (tasks_.empty()) {
                if (stop_) return;
                continue;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

} // namespace pixet
