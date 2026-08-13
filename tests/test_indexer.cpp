// Covers Indexer (src/core/scan/Indexer) - previously untested directly, only exercised
// indirectly via test_fileops.cpp's smart-move-avoids-rethumbnail test. The critical
// property to prove here, added alongside multi-threaded Pass B (see devlog/plan), is
// determinism under concurrency: running the exact same set of files through Indexer
// with threadCount=1 vs. threadCount>1 must produce identical results - same file
// states, same thumbnail dimensions, same thumbnail byte length - not just "doesn't
// crash". Pass A (the directory walk/diff) and every DB write stay single-threaded
// regardless of threadCount (see IndexOptions::threadCount's own doc comment); only
// Pass B's generateThumb() calls are dispatched across the pool, so this is really a
// test that parallel dispatch-then-collect doesn't lose, duplicate, or misattribute
// results between files.

#include "TestHarness.h"
#include "TestPaths.h"

#include <filesystem>
#include <string>
#include <vector>

#include "db/Database.h"
#include "decode/RgbImage.h"
#include "scan/Indexer.h"
#include "util/PathUtil.h"

using namespace pixet;

namespace {

// Writes the same set of files (a handful of small, real, valid JPEGs of varying size
// - so their thumbnails differ meaningfully - plus one deliberately-corrupt "photo")
// into `dir`. Called once per test directory so two independently-populated
// directories can be indexed under different thread counts and their results compared.
void populateTestLibrary(const std::string &dir) {
    std::filesystem::create_directories(dir);

    for (int i = 0; i < 24; ++i) {
        RgbImage img;
        img.w = 8 + (i % 5) * 4; // a little size variety: 8, 12, 16, 20, 24
        img.h = 8 + (i % 3) * 4;
        img.pixels.assign((size_t)img.w * (size_t)img.h * 3, (uint8_t)(i * 7));
        std::vector<uint8_t> jpegBytes;
        encodeJpeg(img, 80, jpegBytes); // only fails on encoder-internal errors, not this input
        writeTestFile(joinPath(dir, "photo_" + std::to_string(i) + ".jpg"), jpegBytes);
    }

    // Deliberately corrupt - exercises ThumbTier::Failed the same way on both runs,
    // not just the successful-decode path.
    writeTestFile(joinPath(dir, "corrupt.jpg"), {0x00, 0x01, 0x02, 0x03});
}

struct FileSnapshot {
    std::string name;
    int64_t state = 0;
    int64_t width = 0, height = 0;
    int64_t orientation = 0;
    bool hasGps = false;
    bool gpsChecked = false;
    int64_t thumbBytesLength = -1; // -1 = no thumb row at all
};

std::vector<FileSnapshot> snapshotFiles(Database &db) {
    std::vector<FileSnapshot> out;
    auto sel = db.prepare(
        "SELECT name, state, width, height, orientation, gps_lat, gps_checked, thumb_id FROM files ORDER BY name");
    while (sel.step()) {
        FileSnapshot s;
        s.name = sel.columnText(0);
        s.state = sel.columnInt64(1);
        s.width = sel.columnInt64(2);
        s.height = sel.columnInt64(3);
        s.orientation = sel.columnInt64(4);
        s.hasGps = !sel.columnIsNull(5);
        s.gpsChecked = sel.columnInt64(6) != 0;
        if (!sel.columnIsNull(7)) {
            int64_t thumbId = sel.columnInt64(7);
            auto tsel = db.prepare("SELECT length(bytes) FROM thumbs.thumbs WHERE id=?");
            tsel.bind(1, thumbId);
            if (tsel.step()) s.thumbBytesLength = tsel.columnInt64(0);
        }
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace

PIXET_TEST(ParallelPassBProducesSameResultAsSequential) {
    std::filesystem::path base = std::filesystem::temp_directory_path() / "pixet_tests" / "indexer_parallel";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);

    std::string seqDir = (base / "sequential").string();
    std::string parDir = (base / "parallel").string();
    populateTestLibrary(seqDir);
    populateTestLibrary(parDir);

    Database seqDb(testTempPath("indexer_parallel_seq_index.db"), testTempPath("indexer_parallel_seq_thumbs.db"));
    Database parDb(testTempPath("indexer_parallel_par_index.db"), testTempPath("indexer_parallel_par_thumbs.db"));

    IndexOptions seqOpts;
    seqOpts.recursive = false;
    seqOpts.owner = "test-seq";
    seqOpts.threadCount = 1;
    Indexer seqIndexer(seqDb, seqOpts);
    IndexStats seqStats;
    seqIndexer.run(seqDir, seqStats);

    IndexOptions parOpts;
    parOpts.recursive = false;
    parOpts.owner = "test-par";
    parOpts.threadCount = 8; // deliberately more threads than any real machine needs for 25 tiny files
    Indexer parIndexer(parDb, parOpts);
    IndexStats parStats;
    parIndexer.run(parDir, parStats);

    // Same input, same outcome regardless of how many threads did the decoding.
    PIXET_CHECK(seqStats.filesNew == parStats.filesNew);
    PIXET_CHECK(seqStats.thumbsDecoded == parStats.thumbsDecoded);
    PIXET_CHECK(seqStats.thumbsEmbedded == parStats.thumbsEmbedded);
    PIXET_CHECK(seqStats.thumbsFailed == parStats.thumbsFailed);
    PIXET_CHECK(seqStats.thumbsUnsupported == parStats.thumbsUnsupported);
    PIXET_CHECK(seqStats.filesNew == 25); // 24 real photos + 1 corrupt

    std::vector<FileSnapshot> seqFiles = snapshotFiles(seqDb);
    std::vector<FileSnapshot> parFiles = snapshotFiles(parDb);
    PIXET_CHECK(seqFiles.size() == parFiles.size());
    for (size_t i = 0; i < seqFiles.size(); ++i) {
        const FileSnapshot &a = seqFiles[i];
        const FileSnapshot &b = parFiles[i];
        PIXET_CHECK(a.name == b.name); // both lists are ORDER BY name - a mismatch here means a lost/duplicated row
        PIXET_CHECK(a.state == b.state);
        PIXET_CHECK(a.width == b.width);
        PIXET_CHECK(a.height == b.height);
        PIXET_CHECK(a.orientation == b.orientation);
        PIXET_CHECK(a.hasGps == b.hasGps);
        PIXET_CHECK(a.gpsChecked == b.gpsChecked);
        PIXET_CHECK(a.thumbBytesLength == b.thumbBytesLength);
    }
}

PIXET_TEST(ParallelPassBHandlesAPoolLargerThanTheFileCount) {
    std::filesystem::path base = std::filesystem::temp_directory_path() / "pixet_tests" / "indexer_oversized_pool";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::string dir = base.string();
    std::filesystem::create_directories(dir);

    RgbImage img;
    img.w = 8;
    img.h = 8;
    img.pixels.assign((size_t)8 * 8 * 3, 128);
    std::vector<uint8_t> jpegBytes;
    encodeJpeg(img, 80, jpegBytes);
    writeTestFile(joinPath(dir, "only.jpg"), jpegBytes);

    Database db(testTempPath("indexer_oversized_index.db"), testTempPath("indexer_oversized_thumbs.db"));
    IndexOptions opts;
    opts.recursive = false;
    opts.owner = "test";
    opts.threadCount = 16; // far more worker threads than the single file to process
    Indexer indexer(db, opts);
    IndexStats stats;
    indexer.run(dir, stats);

    PIXET_CHECK(stats.filesNew == 1);
    PIXET_CHECK(stats.thumbsDecoded == 1);
}
