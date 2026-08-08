#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "db/Database.h"
#include "scan/Indexer.h"
#include "util/AppPaths.h"
#include "util/PathUtil.h"
#include "util/StringUtil.h"
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
    if (argc < 2) {
        std::printf("pixet-index %s\n", pixet::version());
        std::printf("usage: pixet-index <root-path> [--force] [--no-recurse]\n");
        std::printf("  --force       ignore the per-folder freshness cache, rescan everything\n");
        std::printf("  --no-recurse  index only the given folder, not its subfolders\n");
        return 1;
    }

    IndexOptions opts;
    opts.owner = "pid:" + std::to_string(GetCurrentProcessId());
    std::string rootArg = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--force") opts.forceRescan = true;
        if (arg == "--no-recurse") opts.recursive = false;
    }

    std::wstring rootPath = normalizePath(toUtf16(rootArg));
    std::printf("pixet-index %s\n", pixet::version());
    std::printf("root:    %s\n", rootArg.c_str());
    std::printf("cache:   %s\n", toUtf8(appDataDir()).c_str());
    std::printf("options: recursive=%s force=%s\n\n", opts.recursive ? "yes" : "no", opts.forceRescan ? "yes" : "no");

    Database db(indexDbPath(), thumbsDbPath());
    Indexer indexer(db, opts);

    auto start = Clock::now();
    auto lastPrint = start;

    IndexStats stats;
    indexer.run(rootPath, stats, [&](const IndexStats &s) {
        auto now = Clock::now();
        if (std::chrono::duration<double>(now - lastPrint).count() >= 0.5) {
            printStats(s, std::chrono::duration<double>(now - start).count());
            lastPrint = now;
        }
    });

    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    printStats(stats, elapsed);
    std::printf("\n\ndone in %.1fs\n", elapsed);
    std::printf("dirs visited:        %lld (%lld fresh-skipped, %lld claimed by another process)\n",
                (long long)stats.dirsVisited, (long long)stats.dirsSkippedFresh, (long long)stats.dirsSkippedClaimed);
    std::printf("files: %lld new, %lld removed\n", (long long)stats.filesNew, (long long)stats.filesRemoved);
    std::printf("thumbnails: %lld embedded-preview, %lld decoded, %lld unsupported-format, %lld failed\n",
                (long long)stats.thumbsEmbedded, (long long)stats.thumbsDecoded, (long long)stats.thumbsUnsupported,
                (long long)stats.thumbsFailed);

    return 0;
}
