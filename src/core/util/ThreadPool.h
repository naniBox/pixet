#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace pixet {

// A small, generic, persistent worker-thread pool - built for Indexer::run() to spread
// generateThumb()'s per-file decode+resize+encode work (CPU-bound, no shared state -
// see ThumbGenerator.h) across cores, but deliberately not indexing-specific so it can
// be reused wherever independent work needs to fan out.
//
// Persistent rather than spawned per batch: workers start once in the constructor and
// live until the pool is destroyed, so a caller processing many batches (many
// directories, in Indexer's case) doesn't pay thread-creation/teardown cost per batch.
//
// Not a queue you can submit unboundedly ahead of memory - each submit() posts one task
// immediately to the shared queue; a caller wanting to bound how much work is in flight
// at once should submit a batch, collect those futures, then submit the next batch
// (exactly what Indexer does per directory's Pass B).
class ThreadPool {
public:
    // numThreads is clamped to at least 1 - a "pool" of zero workers would just hang
    // forever on the first submit().
    explicit ThreadPool(size_t numThreads);
    ~ThreadPool();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // Posts `fn` to the queue and returns a future for its result. `fn` runs on
    // whichever worker thread picks it up next - never assume which one, or that it's
    // not the thread that's about to block on the returned future (deadlock: don't
    // submit() a task from inside another task running on this same pool and then
    // wait on it). An exception thrown by `fn` is captured by the underlying
    // std::packaged_task and rethrown from future::get(), the same as std::async -
    // the pool itself keeps running regardless.
    template <typename F>
    auto submit(F &&fn) -> std::future<std::invoke_result_t<F>> {
        using ReturnType = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(std::forward<F>(fn));
        std::future<ReturnType> result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return result;
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;

    void workerLoop();
};

} // namespace pixet
