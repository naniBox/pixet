#pragma once

// Lightweight always-in-the-source, compiled-out-by-default profiler.
//
// Built for the recurring question "why is opening a big folder slow", which needs answering
// against a *real* library rather than a synthetic one - the interesting folders are the
// user's own 1280-file, 79MB-of-thumbnails ones, and those only exist on their machine. So
// the instrumentation has to be checked in and switchable, not re-added from scratch each
// time somebody wonders again. Enable with:
//
//     cmake --preset mac-release -DPIXET_PROFILE=ON
//
// With it OFF (the default) every macro below expands to nothing - not a no-op function call,
// literally no tokens - so there is no argument evaluation, no timer read, and nothing for the
// optimizer to have to prove dead. It costs the shipping build exactly zero.
//
// Three primitives, deliberately few:
//
//   PIXET_PROF_SCOPE("name")      time this scope; aggregated by name (count/total/min/max)
//   PIXET_PROF_COUNT("name", n)   add n to a named counter (bytes decoded, rows read, ...)
//   PIXET_PROF_MARK("name")       timestamp a one-off event, ordered against every other mark
//
// Scopes answer "where did the time go"; marks answer "in what order, and how long after the
// user clicked" - which for a UI stall is usually the more revealing of the two, since the
// problem is often that something correct happens at the wrong moment, or twice.
//
// Thread-safety: scope and counter data live in per-thread tables and are merged only when a
// report is generated, so profiling four decode threads doesn't serialize them on a mutex and
// change the very timings being measured. Marks are rare and take a lock.

#include <cstdint>
#include <string>

namespace pixet::profile {

#ifdef PIXET_PROFILE

// Discards all accumulated scopes, counters and marks, and restarts the clock that marks are
// timestamped against. Call this at the start of whatever you're measuring (a folder
// navigation, say) so the report covers that and not the whole session.
void reset();

// Human-readable report: marks in time order, then scopes and counters by total time
// descending. Safe to call from any thread; takes a consistent-enough snapshot without
// stopping the world (a scope still running when this is called simply isn't counted yet).
std::string report();

// report() written to stderr. Also invoked automatically at process exit when
// PIXET_PROFILE_ATEXIT=1 is set in the environment, which is how pixet-index runs get a
// report without needing a UI to ask for one.
void dumpToStderr();

// Names must outlive the call - string literals in practice, which is what the macros pass.
void addScope(const char *name, uint64_t elapsedNs);
void addCount(const char *name, int64_t n);
void addMark(const char *name);

class ScopeTimer {
public:
    explicit ScopeTimer(const char *name);
    ~ScopeTimer();
    ScopeTimer(const ScopeTimer &) = delete;
    ScopeTimer &operator=(const ScopeTimer &) = delete;

private:
    const char *name_;
    uint64_t startNs_;
};

#define PIXET_PROF_CAT_(a, b) a##b
#define PIXET_PROF_CAT(a, b) PIXET_PROF_CAT_(a, b)
#define PIXET_PROF_SCOPE(name) ::pixet::profile::ScopeTimer PIXET_PROF_CAT(pixetProfScope_, __LINE__)(name)
#define PIXET_PROF_COUNT(name, n) ::pixet::profile::addCount(name, (int64_t)(n))
#define PIXET_PROF_MARK(name) ::pixet::profile::addMark(name)
#define PIXET_PROF_RESET() ::pixet::profile::reset()
#define PIXET_PROF_REPORT() ::pixet::profile::report()
#define PIXET_PROF_DUMP() ::pixet::profile::dumpToStderr()

#else

// Deliberately expanding to nothing rather than to an empty inline function: an argument like
// PIXET_PROF_COUNT("blob", expensiveCall()) must not be evaluated when profiling is off, and
// `(void)0` in a macro that callers end with `;` keeps every call site syntactically valid.
#define PIXET_PROF_SCOPE(name) (void)0
#define PIXET_PROF_COUNT(name, n) (void)0
#define PIXET_PROF_MARK(name) (void)0
#define PIXET_PROF_RESET() (void)0
#define PIXET_PROF_REPORT() ::std::string()
#define PIXET_PROF_DUMP() (void)0

#endif

// True when this build has profiling compiled in. A real constant rather than a macro test, so
// UI code can say `if (profile::enabled())` and have both branches type-check either way.
constexpr bool enabled() {
#ifdef PIXET_PROFILE
    return true;
#else
    return false;
#endif
}

} // namespace pixet::profile
