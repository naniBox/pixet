// Covers Indexer (src/core/scan/Indexer) directly; test_fileops.cpp only reaches it
// incidentally, through its smart-move-avoids-rethumbnail test. The critical property to
// prove here is determinism under concurrency: running the exact same set of files
// with threadCount=1 vs. threadCount>1 must produce identical results - same file
// states, same thumbnail dimensions, same thumbnail byte length - not just "doesn't
// crash". Pass A (the directory walk/diff) and every DB write stay single-threaded
// regardless of threadCount (see IndexOptions::threadCount's own doc comment); only
// Pass B's generateThumb() calls are dispatched across the pool, so this is really a
// test that parallel dispatch-then-collect doesn't lose, duplicate, or misattribute
// results between files.

#include "TestHarness.h"
#include "TestPaths.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "db/Database.h"
#include "db/Schema.h"
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

// IndexCallbacks::onWantFirst - the hook that lets the GUI put the screenful the user is
// actually looking at at the front of Pass B's queue (Pass B's own order is readdir
// order, which has nothing to do with the grid's sort). Two properties matter and both
// are checked here: the wanted ids really do jump the queue, in the order asked for, and
// nothing is lost doing it - the rest of the folder still gets thumbnailed, in its
// original relative order, and ids that match no pending file are simply ignored.
//
// threadCount=1 is what makes this observable rather than a race: with the hook installed
// a Pass B wave is the pool's own size (see Indexer.cpp), so one thread means one file
// per wave and therefore one onWantFirst call per file, with `remaining` front()
// naming the file about to be generated.
PIXET_TEST(PassBGeneratesWantedFilesFirst) {
    std::filesystem::path base = std::filesystem::temp_directory_path() / "pixet_tests" / "indexer_want_first";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::string dir = base.string();
    populateTestLibrary(dir);

    Database db(testTempPath("indexer_want_first_index.db"), testTempPath("indexer_want_first_thumbs.db"));
    IndexOptions opts;
    opts.recursive = false;
    opts.owner = "test";
    opts.threadCount = 1;
    Indexer indexer(db, opts);
    IndexStats stats;

    std::vector<int64_t> originalOrder; // the queue as it stood before any reordering
    std::vector<int64_t> nextUp;        // remaining.front() at each wave - i.e. the actual work order
    std::vector<int64_t> wanted;        // what we asked for, fixed after the first call

    IndexCallbacks callbacks;
    callbacks.onWantFirst = [&](int64_t, const std::string &,
                                 const std::vector<int64_t> &remaining) -> std::vector<int64_t> {
        if (remaining.empty()) return {};
        nextUp.push_back(remaining.front());
        if (originalOrder.empty()) {
            originalOrder = remaining;
            // A file id that can't exist, ahead of the three files that would otherwise
            // be generated *last* - the bogus one must be ignored without displacing or
            // dropping anything.
            wanted.push_back(-1);
            wanted.insert(wanted.end(), remaining.end() - 3, remaining.end());
        }
        return wanted;
    };

    indexer.run(dir, stats, callbacks);

    PIXET_CHECK(originalOrder.size() == 25); // 24 real photos + 1 corrupt
    PIXET_CHECK(wanted.size() == 4);         // one bogus id + the three real ones
    PIXET_CHECK(nextUp.size() == originalOrder.size()); // one ask per file

    // What each entry of nextUp means, precisely, because it isn't uniform: the hook is
    // asked *before* the reorder it triggers, so nextUp[0] is still the untouched
    // queue's head. From then on the answer never changes, so no further reordering
    // happens and nextUp[k] is exactly the file wave k generates.
    PIXET_CHECK(nextUp[0] == originalOrder[0]);

    // wanted[1] was dead last in the queue and is generated in wave 0 - it's gone from
    // the queue before the hook is ever asked again, which is the whole property: last
    // in line to first, in one step.
    PIXET_CHECK(wanted[1] == originalOrder[originalOrder.size() - 3]);
    PIXET_CHECK(std::find(nextUp.begin() + 1, nextUp.end(), wanted[1]) == nextUp.end());

    // The other two follow it, in the order asked for...
    PIXET_CHECK(nextUp[1] == wanted[2]);
    PIXET_CHECK(nextUp[2] == wanted[3]);
    // ...and then the rest of the folder resumes at its own head, in its own order -
    // reordering must not shuffle anything it wasn't asked about. (wanted[0] is a file
    // id that can't exist; had it displaced anything, originalOrder[0] wouldn't be here.)
    PIXET_CHECK(nextUp[3] == originalOrder[0]);
    PIXET_CHECK(nextUp[4] == originalOrder[1]);

    // Nothing skipped - a reorder must not lose work. FileState::New is what Pass A
    // writes and Pass B clears, so a row still sitting at 0 is one that never got looked
    // at, whatever the counters say.
    auto sel = db.prepare("SELECT COUNT(*) FROM files WHERE state=?");
    sel.bind(1, (int64_t)FileState::New);
    PIXET_CHECK(sel.step());
    PIXET_CHECK(sel.columnInt64(0) == 0);
    PIXET_CHECK(stats.filesNew == 25);
    PIXET_CHECK(stats.thumbsDecoded == 24);
    PIXET_CHECK(stats.thumbsFailed == 1);
}
