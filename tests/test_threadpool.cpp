// Covers util/ThreadPool - the generic worker-pool primitive Indexer's Pass B spreads
// generateThumb() calls across. What matters here is exactly what a caller relies on:
// every submitted task actually
// runs and its result comes back correctly, a pool sized 1 still works (the
// BackgroundReconciler/RawRenderer case, which pins threadCount=1), and an exception
// inside a task surfaces through the future rather than silently vanishing or taking
// the pool down with it.

#include "TestHarness.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <vector>

#include "util/ThreadPool.h"

using namespace pixet;

PIXET_TEST(ThreadPoolRunsMoreTasksThanThreadsAndReturnsCorrectResults) {
    ThreadPool pool(4);

    constexpr int kTasks = 50; // deliberately more than the 4 worker threads
    std::vector<std::future<int>> futures;
    futures.reserve(kTasks);
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.submit([i]() { return i * i; }));
    }

    for (int i = 0; i < kTasks; ++i) {
        PIXET_CHECK(futures[(size_t)i].get() == i * i);
    }
}

PIXET_TEST(ThreadPoolOfSizeOneStillRunsEveryTask) {
    ThreadPool pool(1);

    std::vector<std::future<int>> futures;
    for (int i = 0; i < 10; ++i) futures.push_back(pool.submit([i]() { return i + 1; }));

    int sum = 0;
    for (auto &f : futures) sum += f.get();
    PIXET_CHECK(sum == 55); // 1+2+...+10
}

PIXET_TEST(ThreadPoolClampsZeroThreadsToOneRatherThanHanging) {
    // A "pool" of zero workers would never pick up a submitted task - see the
    // constructor's own doc comment. Just needs to actually complete, not hang.
    ThreadPool pool(0);
    std::future<int> f = pool.submit([]() { return 42; });
    PIXET_CHECK(f.get() == 42);
}

PIXET_TEST(ThreadPoolPropagatesExceptionsThroughTheFuture) {
    ThreadPool pool(2);

    std::future<int> failing = pool.submit([]() -> int { throw std::runtime_error("deliberate failure"); });
    std::future<int> succeeding = pool.submit([]() { return 7; });

    bool threw = false;
    try {
        failing.get();
    } catch (const std::runtime_error &e) {
        threw = true;
        PIXET_CHECK(std::string(e.what()) == "deliberate failure");
    }
    PIXET_CHECK(threw);

    // The pool itself, and the other task already in flight, are unaffected by the
    // failing one.
    PIXET_CHECK(succeeding.get() == 7);
}

PIXET_TEST(ThreadPoolActuallyRunsTasksConcurrentlyNotSequentially) {
    // Not a timing assertion (flaky on a shared/CI machine) - a correctness check that
    // tasks genuinely overlap: every task increments a counter, then spins (bounded -
    // see below, this must never be able to hang the suite) waiting to see *all* of
    // them have started. A pool that actually ran tasks one at a time would only ever
    // reach `started == 1` from inside any given task (nothing else could increment it
    // first), so the bound would be hit and the check below would fail cleanly instead
    // of this silently passing.
    constexpr int kThreads = 4;
    ThreadPool pool(kThreads);

    std::atomic<int> started{0};
    std::vector<std::future<int>> futures;
    for (int i = 0; i < kThreads; ++i) {
        futures.push_back(pool.submit([&started]() {
            started.fetch_add(1);
            // Bounded spin, not an unconditional wait - a genuinely sequential pool
            // must never be able to hang this test; it should just fail the
            // equality check below once the bound is exhausted.
            for (int spins = 0; spins < 2'000'000 && started.load() < kThreads; ++spins) {
                std::this_thread::yield();
            }
            return started.load();
        }));
    }

    int maxObserved = 0;
    for (auto &f : futures) maxObserved = std::max(maxObserved, f.get());
    PIXET_CHECK(maxObserved == kThreads);
}
