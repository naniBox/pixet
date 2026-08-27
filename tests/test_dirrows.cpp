// Covers the `dirs` row lifecycle: which directories earn a row in the first place
// (Indexer), and which ones lose it again (pruneDirs).
//
// The bug these exist for: a `dirs` row used to be written for every subdirectory merely
// seen during a walk, `dirs` was BackgroundReconciler's entire worklist, and nothing ever
// removed a row. One visit to a filesystem root therefore enlisted the whole disk as
// permanent background work, one directory level per pass. See pruneDirs()'s own header.

#include "TestHarness.h"
#include "TestPaths.h"

#ifndef _WIN32
#include <sys/stat.h> // chmod, for the unreadable-directory test below
#endif

#include <filesystem>
#include <string>
#include <vector>

#include "db/Database.h"
#include "db/Schema.h"
#include "decode/RgbImage.h"
#include "scan/DirRows.h"
#include "scan/DirWalker.h"
#include "scan/Indexer.h"
#include "util/PathUtil.h"

using namespace pixet;

namespace {

std::string freshDir(const std::string &name) {
    std::filesystem::path base = std::filesystem::temp_directory_path() / "pixet_tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base);
    return base.string();
}

void writeJpeg(const std::string &path) {
    RgbImage img;
    img.w = img.h = 8;
    img.pixels.assign(8 * 8 * 3, 128);
    std::vector<uint8_t> bytes;
    encodeJpeg(img, 80, bytes);
    writeTestFile(path, bytes);
}

int64_t countDirs(Database &db) {
    auto sel = db.prepare("SELECT COUNT(*) FROM dirs");
    sel.step();
    return sel.columnInt64(0);
}

bool hasDirRow(Database &db, const std::string &path) {
    auto sel = db.prepare("SELECT 1 FROM dirs WHERE path=?");
    sel.bind(1, path);
    return sel.step();
}

void indexOnce(Database &db, const std::string &root, bool recursive, const std::string &owner) {
    IndexOptions opts;
    opts.recursive = recursive;
    opts.owner = owner;
    opts.threadCount = 1;
    Indexer indexer(db, opts);
    IndexStats stats;
    indexer.run(root, stats);
}

} // namespace

// The fix at the source. Browsing a folder must not enlist its subdirectories as
// permanent background work - and on a non-recursive run those rows were never even read
// back, since Indexer::run() ignores subdirsOut entirely when it isn't descending.
PIXET_TEST(NonRecursiveIndexDoesNotRegisterSubdirectories) {
    std::string root = freshDir("dirrows_nonrecursive");
    std::filesystem::create_directories(joinPath(root, "sub_a"));
    std::filesystem::create_directories(joinPath(joinPath(root, "sub_a"), "deeper"));
    std::filesystem::create_directories(joinPath(root, "sub_b"));
    writeJpeg(joinPath(root, "photo.jpg"));

    Database db(testTempPath("dirrows_nonrec_index.db"), testTempPath("dirrows_nonrec_thumbs.db"));
    indexOnce(db, root, /*recursive=*/false, "test");

    PIXET_CHECK(hasDirRow(db, root));                       // the folder actually indexed
    PIXET_CHECK(!hasDirRow(db, joinPath(root, "sub_a")));   // merely seen - not our business
    PIXET_CHECK(!hasDirRow(db, joinPath(root, "sub_b")));
    PIXET_CHECK(countDirs(db) == 1);
}

// ...while the whole-tree pre-warm still registers everything, because it genuinely
// indexes everything.
PIXET_TEST(RecursiveIndexStillRegistersSubdirectories) {
    std::string root = freshDir("dirrows_recursive");
    std::filesystem::create_directories(joinPath(joinPath(root, "sub_a"), "deeper"));
    std::filesystem::create_directories(joinPath(root, "sub_b"));

    Database db(testTempPath("dirrows_rec_index.db"), testTempPath("dirrows_rec_thumbs.db"));
    indexOnce(db, root, /*recursive=*/true, "test");

    PIXET_CHECK(hasDirRow(db, root));
    PIXET_CHECK(hasDirRow(db, joinPath(root, "sub_a")));
    PIXET_CHECK(hasDirRow(db, joinPath(joinPath(root, "sub_a"), "deeper")));
    PIXET_CHECK(hasDirRow(db, joinPath(root, "sub_b")));
    PIXET_CHECK(countDirs(db) == 4);
}

// The cleanup for libraries that already got crawled: a chain of directories holding no
// media collapses entirely, however deep, while anything holding media survives - along
// with the ancestors that link to it.
PIXET_TEST(PruneRemovesBarrenChainsButKeepsMediaAndItsAncestors) {
    std::string root = freshDir("dirrows_prune_barren");
    std::string keep = joinPath(root, "photos");
    std::filesystem::create_directories(keep);
    writeJpeg(joinPath(keep, "photo.jpg"));
    // A five-deep chain with nothing in it - the shape /nix/store/... took on the library
    // that prompted this.
    std::string chain = root;
    for (const char *seg : {"a", "b", "c", "d", "e"}) {
        chain = joinPath(chain, seg);
        std::filesystem::create_directories(chain);
    }

    Database db(testTempPath("dirrows_prune_index.db"), testTempPath("dirrows_prune_thumbs.db"));
    indexOnce(db, root, /*recursive=*/true, "test");
    PIXET_CHECK(countDirs(db) == 7); // root + photos + the five-deep chain

    PruneStats stats = pruneDirs(db);

    PIXET_CHECK(stats.dirsBarren == 5); // the whole chain, collapsed leaf-first
    PIXET_CHECK(stats.dirsMissing == 0);
    PIXET_CHECK(hasDirRow(db, keep));  // holds media
    PIXET_CHECK(hasDirRow(db, root));  // barren itself, but parents the one that doesn't
    PIXET_CHECK(!hasDirRow(db, joinPath(root, "a")));
    PIXET_CHECK(countDirs(db) == 2);

    // Idempotent: a second prune over an already-clean table finds nothing.
    PruneStats again = pruneDirs(db);
    PIXET_CHECK(again.dirsBarren == 0 && again.dirsMissing == 0 && again.filesRemoved == 0);
}

// A directory deleted since it was indexed takes its file rows and thumbnail blobs with
// it - those used to be kept forever.
PIXET_TEST(PruneRemovesDirectoriesThatAreGoneAlongWithTheirThumbnails) {
    std::string root = freshDir("dirrows_prune_missing");
    std::string doomed = joinPath(root, "removed_later");
    std::filesystem::create_directories(doomed);
    writeJpeg(joinPath(doomed, "photo.jpg"));

    Database db(testTempPath("dirrows_missing_index.db"), testTempPath("dirrows_missing_thumbs.db"));
    indexOnce(db, root, /*recursive=*/true, "test");

    auto thumbCount = [&db]() {
        auto sel = db.prepare("SELECT COUNT(*) FROM thumbs.thumbs");
        sel.step();
        return sel.columnInt64(0);
    };
    PIXET_CHECK(thumbCount() == 1);

    std::error_code ec;
    std::filesystem::remove_all(doomed, ec);

    PruneStats stats = pruneDirs(db);

    PIXET_CHECK(stats.dirsMissing == 1);
    PIXET_CHECK(stats.filesRemoved == 1);
    PIXET_CHECK(stats.thumbsRemoved == 1);
    PIXET_CHECK(!hasDirRow(db, doomed));
    PIXET_CHECK(thumbCount() == 0); // the blob went too, rather than being stranded in thumbs.db
}

// The one that would quietly destroy data if it went wrong. On macOS a TCC-protected
// folder the user hasn't granted access to cannot be stat'd at all - it is unreachable,
// not absent - and its index must survive untouched. Modelled here with a parent
// directory the process can't traverse, which is the same errno (EACCES) by the same
// mechanism.
//
// POSIX-only, because the mechanism is: Windows traversal rights come from ACLs, and its
// _chmod only toggles the read-only attribute, which does not stop a directory being
// opened. Reproducing this on Windows would mean denying FILE_TRAVERSE through a real ACL
// (SetNamedSecurityInfo or icacls), which tests neither dirPresence()'s logic nor
// pruneDirs(). dirPresence() answers Unreadable on Windows too - see DirWalker_win.cpp,
// where a disconnected share or an unmounted drive lands in the same branch - so the
// behaviour this guards is not Windows-specific even though this test is.
#ifndef _WIN32
PIXET_TEST(PruneKeepsDirectoriesItCannotReachRatherThanAssumingTheyAreGone) {
    std::string root = freshDir("dirrows_prune_unreadable");
    std::string locked = joinPath(root, "locked");
    std::string inside = joinPath(locked, "photos");
    std::filesystem::create_directories(inside);
    writeJpeg(joinPath(inside, "photo.jpg"));

    Database db(testTempPath("dirrows_unreadable_index.db"), testTempPath("dirrows_unreadable_thumbs.db"));
    indexOnce(db, root, /*recursive=*/true, "test");
    PIXET_CHECK(hasDirRow(db, inside));

    // Restored however this test leaves, including through a failed PIXET_CHECK - a
    // mode-000 directory in the temp tree can't be cleaned up by the next run otherwise.
    struct ModeRestorer {
        std::string path;
        ~ModeRestorer() { ::chmod(path.c_str(), 0755); }
    } restorer{locked};
    PIXET_CHECK(::chmod(locked.c_str(), 0000) == 0);

    if (dirPresence(inside) == DirPresence::Present) {
        // Running as root, where mode bits don't apply - there's nothing to test here,
        // and asserting would fail for a reason that has nothing to do with the code.
        return;
    }
    PIXET_CHECK(dirPresence(inside) == DirPresence::Unreadable);

    PruneStats stats = pruneDirs(db);

    PIXET_CHECK(stats.dirsMissing == 0); // unreachable is not absent
    PIXET_CHECK(hasDirRow(db, inside));
    auto files = db.prepare("SELECT COUNT(*) FROM files");
    files.step();
    PIXET_CHECK(files.columnInt64(0) == 1);
}
#endif // !_WIN32

// A directory some other indexer is working in right now is off limits, whatever it
// currently looks like - the claim is the existing mechanism for exactly that.
PIXET_TEST(PruneLeavesClaimedDirectoriesAlone) {
    std::string root = freshDir("dirrows_prune_claimed");
    std::string barren = joinPath(root, "empty");
    std::filesystem::create_directories(barren);

    Database db(testTempPath("dirrows_claimed_index.db"), testTempPath("dirrows_claimed_thumbs.db"));
    indexOnce(db, root, /*recursive=*/true, "test");

    int64_t barrenId = 0;
    {
        auto sel = db.prepare("SELECT id FROM dirs WHERE path=?");
        sel.bind(1, barren);
        PIXET_CHECK(sel.step());
        barrenId = sel.columnInt64(0);
    }
    auto ins = db.prepare("INSERT INTO claims(dir_id, owner, heartbeat) VALUES(?,?,?)");
    ins.bind(1, barrenId);
    ins.bind(2, std::string("someone-else"));
    ins.bind(3, (int64_t)1);
    ins.step();

    PruneStats stats = pruneDirs(db);

    PIXET_CHECK(stats.dirsBarren == 0);
    PIXET_CHECK(hasDirRow(db, barren));
}
