#include "Profile.h"

#ifdef PIXET_PROFILE

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pixet::profile {
namespace {

using Clock = std::chrono::steady_clock;

uint64_t nowNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

struct Stat {
    uint64_t count = 0;
    uint64_t totalNs = 0;
    uint64_t minNs = UINT64_MAX;
    uint64_t maxNs = 0;
    int64_t counter = 0; // for PIXET_PROF_COUNT; unused by scopes
};

struct Mark {
    const char *name;
    uint64_t ns;
    std::thread::id thread;
};

// One table per thread, so the hot path (scope exit, counter bump) touches only thread-local
// memory. Merging happens in report(), which is rare. Without this, four decode threads would
// serialize on a single mutex inside the very code being measured - the classic way a profiler
// reports the cost of itself.
struct ThreadTable {
    std::unordered_map<std::string, Stat> stats;
    std::string label;
};

// Every one of these is an intentionally-leaked heap object rather than a function-local
// static, and the reason is specific: the PIXET_PROFILE_ATEXIT dump runs from an atexit
// handler registered during static initialisation, so it runs *after* function-local statics
// constructed later have already been destroyed. The first version used plain
// `static std::mutex m;` and killed the very process it was measuring on the way out -
// "libc++abi: terminating due to uncaught exception ... mutex lock failed: Invalid argument",
// after a clean 8.8s indexing run. A profiler that can crash the program is worse than no
// profiler, so these simply never die.
std::mutex &registryMutex() {
    static std::mutex *m = new std::mutex();
    return *m;
}

std::vector<ThreadTable *> &registry() {
    static auto *v = new std::vector<ThreadTable *>();
    return *v;
}

// Never freed, deliberately: a worker thread can exit while a report is being generated from
// another thread, and a few hundred bytes per thread for the life of the process is a better
// trade than the lifetime dance required to make that safe. Thread tables are reused across
// reset() rather than reallocated.
ThreadTable &table() {
    static thread_local ThreadTable *t = [] {
        auto *fresh = new ThreadTable();
        std::lock_guard<std::mutex> lock(registryMutex());
        registry().push_back(fresh);
        return fresh;
    }();
    return *t;
}

std::mutex &markMutex() {
    static std::mutex *m = new std::mutex();
    return *m;
}

std::vector<Mark> &marks() {
    static auto *v = new std::vector<Mark>();
    return *v;
}

uint64_t &originNs() {
    static auto *origin = new uint64_t(nowNs());
    return *origin;
}

std::string fmtNs(uint64_t ns) {
    char buf[64];
    if (ns < 1000ull) std::snprintf(buf, sizeof(buf), "%lluns", (unsigned long long)ns);
    else if (ns < 1000000ull) std::snprintf(buf, sizeof(buf), "%.1fus", ns / 1e3);
    else if (ns < 1000000000ull) std::snprintf(buf, sizeof(buf), "%.2fms", ns / 1e6);
    else std::snprintf(buf, sizeof(buf), "%.3fs", ns / 1e9);
    return buf;
}

std::string fmtCount(int64_t n) {
    char buf[64];
    if (n < 10000) std::snprintf(buf, sizeof(buf), "%lld", (long long)n);
    else if (n < 10000000) std::snprintf(buf, sizeof(buf), "%.1fk", n / 1e3);
    else std::snprintf(buf, sizeof(buf), "%.2fM", n / 1e6);
    return buf;
}

} // namespace

void addScope(const char *name, uint64_t elapsedNs) {
    Stat &s = table().stats[name];
    s.count++;
    s.totalNs += elapsedNs;
    s.minNs = std::min(s.minNs, elapsedNs);
    s.maxNs = std::max(s.maxNs, elapsedNs);
}

void addCount(const char *name, int64_t n) {
    Stat &s = table().stats[name];
    s.count++;
    s.counter += n;
}

void addMark(const char *name) {
    uint64_t t = nowNs();
    std::lock_guard<std::mutex> lock(markMutex());
    (void)originNs(); // ensure the origin is established before the first mark
    marks().push_back({name, t, std::this_thread::get_id()});
}

void reset() {
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        for (ThreadTable *t : registry()) t->stats.clear();
    }
    std::lock_guard<std::mutex> lock(markMutex());
    marks().clear();
    originNs() = nowNs();
}

ScopeTimer::ScopeTimer(const char *name) : name_(name), startNs_(nowNs()) {}
ScopeTimer::~ScopeTimer() { addScope(name_, nowNs() - startNs_); }

std::string report() {
    // Merge every thread's table. A scope that ran on four decode threads shows up once, with
    // its counts summed - which is what you want for "how much total work was this", while the
    // thread count column keeps the concurrency visible.
    struct Merged {
        Stat stat;
        int threads = 0;
    };
    std::unordered_map<std::string, Merged> merged;
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        for (ThreadTable *t : registry()) {
            for (const auto &[name, s] : t->stats) {
                Merged &m = merged[name];
                m.threads++;
                m.stat.count += s.count;
                m.stat.totalNs += s.totalNs;
                m.stat.counter += s.counter;
                m.stat.minNs = std::min(m.stat.minNs, s.minNs);
                m.stat.maxNs = std::max(m.stat.maxNs, s.maxNs);
            }
        }
    }

    std::ostringstream out;
    out << "==== pixet profile ====\n";

    {
        std::vector<Mark> snapshot;
        {
            std::lock_guard<std::mutex> lock(markMutex());
            snapshot = marks();
        }
        if (!snapshot.empty()) {
            uint64_t origin = originNs();
            std::stable_sort(snapshot.begin(), snapshot.end(),
                             [](const Mark &a, const Mark &b) { return a.ns < b.ns; });
            out << "\n-- timeline (from reset) --\n";
            uint64_t prev = origin;
            for (const Mark &m : snapshot) {
                out << "  " << fmtNs(m.ns - origin) << "  (+" << fmtNs(m.ns - prev) << ")  " << m.name << "\n";
                prev = m.ns;
            }
        }
    }

    std::vector<std::pair<std::string, Merged>> scopes, counters;
    for (const auto &kv : merged) {
        // A counter entry never recorded any elapsed time; that's what distinguishes the two.
        if (kv.second.stat.totalNs == 0 && kv.second.stat.counter != 0) counters.push_back(kv);
        else scopes.push_back(kv);
    }
    std::sort(scopes.begin(), scopes.end(),
              [](const auto &a, const auto &b) { return a.second.stat.totalNs > b.second.stat.totalNs; });
    std::sort(counters.begin(), counters.end(),
              [](const auto &a, const auto &b) { return a.second.stat.counter > b.second.stat.counter; });

    if (!scopes.empty()) {
        out << "\n-- scopes (by total time) --\n";
        char line[256];
        std::snprintf(line, sizeof(line), "  %-42s %8s %10s %10s %10s %5s\n", "name", "calls", "total", "avg",
                      "max", "thr");
        out << line;
        for (const auto &[name, m] : scopes) {
            const Stat &s = m.stat;
            std::snprintf(line, sizeof(line), "  %-42s %8llu %10s %10s %10s %5d\n", name.c_str(),
                          (unsigned long long)s.count, fmtNs(s.totalNs).c_str(),
                          fmtNs(s.count ? s.totalNs / s.count : 0).c_str(), fmtNs(s.maxNs).c_str(), m.threads);
            out << line;
        }
    }

    if (!counters.empty()) {
        out << "\n-- counters --\n";
        char line[256];
        for (const auto &[name, m] : counters) {
            std::snprintf(line, sizeof(line), "  %-42s %12s  (%llu events)\n", name.c_str(),
                          fmtCount(m.stat.counter).c_str(), (unsigned long long)m.stat.count);
            out << line;
        }
    }

    return out.str();
}

void dumpToStderr() {
    std::string s = report();
    std::fwrite(s.data(), 1, s.size(), stderr);
    std::fflush(stderr);
}

namespace {
// Opt-in rather than automatic: a report on every pixet-index run would be noise, but when
// you're chasing something it's the difference between needing a UI hook and not.
struct AtExitInstaller {
    AtExitInstaller() {
        const char *v = std::getenv("PIXET_PROFILE_ATEXIT");
        if (v && std::strcmp(v, "0") != 0) std::atexit([] { dumpToStderr(); });
    }
} g_atExitInstaller;
} // namespace

} // namespace pixet::profile

#endif // PIXET_PROFILE
