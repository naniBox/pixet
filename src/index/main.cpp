#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "db/Database.h"
#include "scan/Indexer.h"
#include "util/AppPaths.h"
#include "util/PathUtil.h"
#include "util/ProcessId.h"
#include "version.h"

using namespace pixet;
using Clock = std::chrono::steady_clock;

namespace {

void printStats(const IndexStats &s, double elapsedSec) {
    int64_t thumbed = s.thumbsEmbedded + s.thumbsDecoded + s.thumbsUnsupported + s.thumbsFailed;
    double rate = elapsedSec > 0 ? thumbed / elapsedSec : 0.0;
    std::printf(
        "\r  %lld dirs | %lld files (%lld new) | thumbs: %lld embedded, %lld decoded, %lld unsupported, %lld "
        "failed | %.0f files/s   ",
        (long long)s.dirsVisited, (long long)thumbed, (long long)s.filesNew, (long long)s.thumbsEmbedded,
        (long long)s.thumbsDecoded, (long long)s.thumbsUnsupported, (long long)s.thumbsFailed, rate);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char *argv[]) {
    // --help/-h explicitly, because argv[1] is otherwise taken as the root path
    // unconditionally: without this, `pixet-index --help` prints the banner and then starts
    // indexing a folder literally named "--help", which normalizePath() happily turns into
    // <cwd>/--help. Harmless but baffling, and it is the first thing anyone types at an
    // unfamiliar command.
    bool wantsHelp = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") wantsHelp = true;
    }
    if (argc < 2 || wantsHelp) {
        std::printf("pixet-index %s\n", pixet::version());
        std::printf(
            "usage: pixet-index <root-path> [--force] [--force-rethumbnail] [--no-recurse] [--render-raws] [-j N]\n");
        std::printf("  --force             ignore the per-folder freshness cache, rescan everything\n");
        std::printf(
            "  --force-rethumbnail unconditionally re-thumbnail every file, even ones whose (mtime, size)\n"
            "                      haven't changed since the last scan - the CLI equivalent of the GUI's\n"
            "                      \"Force Re-thumbnail This Folder\". Implies --force.\n");
        std::printf("  --no-recurse        index only the given folder, not its subfolders\n");
        std::printf(
            "  --render-raws       replace every RAW file's fast embedded-preview thumbnail with a full\n"
            "                      demosaic render of the actual sensor data. Much slower (this is exactly\n"
            "                      the expensive decode normal indexing avoids by default) - meant to be run\n"
            "                      as a separate, deliberate pass after a normal (fast) index, not routinely.\n"
            "                      Safe to re-run: only RAW files still on the fast preview are touched.\n");
        std::printf(
            "  -j N, --jobs N      how many files to thumbnail concurrently (default: 0, auto-detect from\n"
            "                      the machine's core count). Directory walking/DB writes stay\n"
            "                      single-threaded regardless - only the actual image decode work (the slow\n"
            "                      part, especially for RAW/HEIC) is spread across threads.\n");
        // 0 when help was asked for, 1 when it's being shown because the invocation was wrong -
        // the difference matters to anything calling this from a script.
        return wantsHelp ? 0 : 1;
    }

    IndexOptions opts;
    opts.owner = "pid:" + std::to_string(currentProcessId());
    std::string rootArg = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--force") opts.forceRescan = true;
        // Bypasses the freshness check on its own (see IndexOptions::forceRethumbnail
        // and Indexer.cpp's freshEnough check) - --force isn't also required.
        else if (arg == "--force-rethumbnail") opts.forceRethumbnail = true;
        else if (arg == "--no-recurse") opts.recursive = false;
        else if (arg == "--render-raws") opts.renderRaws = true;
        else if ((arg == "-j" || arg == "--jobs") && i + 1 < argc) opts.threadCount = std::atoi(argv[++i]);
    }

    std::string rootPath = normalizePath(rootArg);
    std::printf("pixet-index %s\n", pixet::version());
    std::printf("root:    %s\n", rootArg.c_str());
    std::printf("cache:   %s\n", appDataDir().c_str());
    // opts.threadCount itself stays 0 (auto) if -j wasn't passed - Indexer resolves
    // that internally, but this banner's whole purpose is to say exactly what a run
    // is about to do, so it resolves the same way here just for the printout.
    unsigned resolvedThreads =
        opts.threadCount > 0 ? (unsigned)opts.threadCount : std::max(1u, std::thread::hardware_concurrency());
    std::printf("options: recursive=%s force=%s render-raws=%s jobs=%u\n\n", opts.recursive ? "yes" : "no",
                 opts.forceRescan ? "yes" : "no", opts.renderRaws ? "yes" : "no", resolvedThreads);

    Database db(indexDbPath(), thumbsDbPath());
    Indexer indexer(db, opts);

    auto start = Clock::now();
    auto lastPrint = start;

    IndexStats stats;
    IndexCallbacks callbacks;
    callbacks.onProgress = [&](const IndexStats &s) {
        auto now = Clock::now();
        if (std::chrono::duration<double>(now - lastPrint).count() >= 0.5) {
            printStats(s, std::chrono::duration<double>(now - start).count());
            lastPrint = now;
        }
    };
    // The GUI's three indexing threads all guard this call because an escaping exception
    // there reaches std::terminate() and kills the app (see BackgroundReconciler::sweepNext).
    // Here the stakes are only an ugly abort with a non-obvious exit status, but this is also
    // the tool you reach for to reproduce a database problem - so print the message plainly
    // and still show the stats for the work that did land.
    bool runFailed = false;
    try {
        indexer.run(rootPath, stats, callbacks);
    } catch (const std::exception &e) {
        runFailed = true;
        std::fprintf(stderr, "\n\nindexing aborted: %s\n", e.what());
    }

    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    printStats(stats, elapsed);
    std::printf("\n\ndone in %.1fs\n", elapsed);
    std::printf("dirs visited:        %lld (%lld fresh-skipped, %lld claimed by another process, %lld unreadable)\n",
                (long long)stats.dirsVisited, (long long)stats.dirsSkippedFresh, (long long)stats.dirsSkippedClaimed,
                (long long)stats.dirsSkippedUnreadable);
    std::printf("files: %lld new, %lld removed, %lld gained GPS coordinates\n", (long long)stats.filesNew,
                (long long)stats.filesRemoved, (long long)stats.gpsBackfilled);
    std::printf("thumbnails: %lld embedded-preview, %lld decoded, %lld unsupported-format, %lld failed\n",
                (long long)stats.thumbsEmbedded, (long long)stats.thumbsDecoded, (long long)stats.thumbsUnsupported,
                (long long)stats.thumbsFailed);
    // Deliberately on stderr and only when non-zero, unlike the unconditional lines above:
    // this one means the *database* misbehaved, not that a folder was unreadable, and it
    // shouldn't be something you have to notice a zero in a wall of output to spot.
    if (stats.dirsFailed > 0) {
        std::fprintf(stderr, "\n%lld directory(ies) failed with a database error; first: %s\n",
                     (long long)stats.dirsFailed, stats.firstFailure.c_str());
    }

    return (runFailed || stats.dirsFailed > 0) ? 1 : 0;
}
