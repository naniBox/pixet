// Covers fileops::execute() (src/core/fileops/FileOps) - the DB-preserving smart-move
// transaction, plain copy, Replace-collision handling, partial-batch-failure behavior,
// the changes()==0 race fallback, and uniqueNameFor()'s Keep-Both naming. This is the
// single most correctness-critical piece of the whole file-operations feature (the
// app's first code path that can move/overwrite/delete a real user file), so it's
// tested against a real Database and real files on disk rather than mocked.

#include "TestHarness.h"
#include "TestPaths.h"

#include <filesystem>
#include <set>

#include "db/Database.h"
#include "db/Schema.h"
#include "fileops/FileOps.h"
#include "scan/DirRows.h"
#include "scan/Indexer.h"
#include "util/FileIO.h"
#include "util/PathUtil.h"

using namespace pixet;
using namespace pixet::fileops;

namespace {

// A source/destination directory pair on disk (siblings under the OS temp dir - same
// volume, so moveFile() takes the plain-rename fast path), each registered as a real
// `dirs` row - mirroring what navigateTo()/Indexer would already have done for any
// folder pixet has actually browsed to.
struct FileOpsFixture {
    Database db;
    std::string srcDir, dstDir;
    int64_t srcDirId = 0, dstDirId = 0;

    explicit FileOpsFixture(const std::string &tag)
        : db(testTempPath(tag + "_index.db"), testTempPath(tag + "_thumbs.db")) {
        std::filesystem::path base = std::filesystem::temp_directory_path() / "pixet_tests" / tag;
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        srcDir = (base / "src").string();
        dstDir = (base / "dst").string();
        std::filesystem::create_directories(srcDir);
        std::filesystem::create_directories(dstDir);
        srcDirId = upsertDir(db, srcDir, -1);
        dstDirId = upsertDir(db, dstDir, -1);
    }
};

// Writes a real file to disk and inserts a matching `files` row (plus a `thumbs.thumbs`
// blob if `state` is Done), as if Indexer had already scanned it - returns the new
// file id. `thumbIdOut` is 0 unless `state` is Done.
int64_t insertTestFile(Database &db, int64_t dirId, const std::string &dirPath, const std::string &name,
                        const std::vector<uint8_t> &content, FileState state, int64_t &thumbIdOut) {
    writeTestFile(joinPath(dirPath, name), content);

    thumbIdOut = 0;
    if (state == FileState::Done) {
        auto insThumb = db.prepare("INSERT INTO thumbs.thumbs(w,h,fmt,bytes) VALUES(1,1,?,?)");
        insThumb.bind(1, (int64_t)Format::Jpeg);
        insThumb.bind(2, std::vector<uint8_t>{0xFF});
        insThumb.step();
        thumbIdOut = db.lastInsertRowId();
    }

    auto ins = db.prepare("INSERT INTO files(dir_id, name, mtime, size, kind, fmt, orientation, state, thumb_id) "
                           "VALUES(?,?,111,?,?,?,1,?,?)");
    ins.bind(1, dirId);
    ins.bind(2, name);
    ins.bind(3, (int64_t)content.size());
    ins.bind(4, (int64_t)Kind::Image);
    ins.bind(5, (int64_t)Format::Jpeg);
    ins.bind(6, (int64_t)state);
    if (thumbIdOut != 0) ins.bind(7, thumbIdOut);
    else ins.bindNull(7);
    ins.step();
    return db.lastInsertRowId();
}

} // namespace

// ------------------------------------------------------------- fileops::execute

PIXET_TEST(SmartMovePreservesThumbIdAndState) {
    FileOpsFixture fx("fileops_smartmove");
    int64_t thumbId = 0;
    int64_t fileId = insertTestFile(fx.db, fx.srcDirId, fx.srcDir, "a.jpg", {1, 2, 3}, FileState::Done, thumbId);
    PIXET_CHECK(thumbId != 0);

    Plan plan;
    plan.kind = OpKind::Move;
    plan.dstDirPath = fx.dstDir;
    plan.items.push_back(PlannedItem{joinPath(fx.srcDir, "a.jpg"), "a.jpg", fileId, fx.srcDirId, false});

    Report report = execute(fx.db, plan, "test-owner");
    PIXET_CHECK(report.succeeded == 1);
    PIXET_CHECK(report.outcomes[0].thumbPreserved);
    PIXET_CHECK(report.outcomes[0].dstFileId == fileId); // same row, not a new one

    auto sel = fx.db.prepare("SELECT dir_id, name, thumb_id, state FROM files WHERE id=?");
    sel.bind(1, fileId);
    PIXET_CHECK(sel.step());
    PIXET_CHECK(sel.columnInt64(0) == fx.dstDirId);
    PIXET_CHECK(sel.columnText(1) == "a.jpg");
    PIXET_CHECK(sel.columnInt64(2) == thumbId);
    PIXET_CHECK((FileState)sel.columnInt64(3) == FileState::Done);

    PIXET_CHECK(!fileExists(joinPath(fx.srcDir, "a.jpg")));
    PIXET_CHECK(fileExists(joinPath(fx.dstDir, "a.jpg")));
}

// The single highest-value test in this file: proves the thumb-preservation optimization
// actually holds end-to-end, not just that the row looks right immediately after the
// move. If fileops::execute() wrote stale (mtime,size), this forced re-diff would
// re-thumbnail the file and this test would catch it.
PIXET_TEST(SmartMovePostOpStatPreventsRethumbnailOnNextIndex) {
    FileOpsFixture fx("fileops_poststat");
    int64_t thumbId = 0;
    int64_t fileId = insertTestFile(fx.db, fx.srcDirId, fx.srcDir, "b.jpg", {9, 9, 9, 9}, FileState::Done, thumbId);

    Plan plan;
    plan.kind = OpKind::Move;
    plan.dstDirPath = fx.dstDir;
    plan.items.push_back(PlannedItem{joinPath(fx.srcDir, "b.jpg"), "b.jpg", fileId, fx.srcDirId, false});
    Report report = execute(fx.db, plan, "test-owner");
    PIXET_CHECK(report.succeeded == 1);

    // forceRescan bypasses the directory-mtime freshness shortcut so Pass A genuinely
    // re-diffs against disk - if fileops::execute() left a stale (mtime,size) on the
    // files row, this diff would see a mismatch and re-queue the file for Pass B.
    IndexOptions opts;
    opts.recursive = false;
    opts.forceRescan = true;
    opts.owner = "test-indexer";
    Indexer indexer(fx.db, opts);
    IndexStats stats;
    indexer.run(fx.dstDir, stats);

    PIXET_CHECK(stats.thumbsDecoded == 0);
    PIXET_CHECK(stats.thumbsEmbedded == 0);
    PIXET_CHECK(stats.filesNew == 0);
    PIXET_CHECK(stats.filesRemoved == 0);

    auto sel = fx.db.prepare("SELECT thumb_id, state FROM files WHERE id=?");
    sel.bind(1, fileId);
    PIXET_CHECK(sel.step());
    PIXET_CHECK(sel.columnInt64(0) == thumbId);
    PIXET_CHECK((FileState)sel.columnInt64(1) == FileState::Done);
}

PIXET_TEST(CopyCreatesANewRowWithFreshStateAndNoThumb) {
    FileOpsFixture fx("fileops_copy");
    int64_t thumbId = 0;
    int64_t fileId = insertTestFile(fx.db, fx.srcDirId, fx.srcDir, "c.jpg", {1, 2}, FileState::Done, thumbId);

    Plan plan;
    plan.kind = OpKind::Copy;
    plan.dstDirPath = fx.dstDir;
    plan.items.push_back(PlannedItem{joinPath(fx.srcDir, "c.jpg"), "c.jpg", fileId, fx.srcDirId, false});
    Report report = execute(fx.db, plan, "test-owner");
    PIXET_CHECK(report.succeeded == 1);
    PIXET_CHECK(!report.outcomes[0].thumbPreserved);
    PIXET_CHECK(report.outcomes[0].dstFileId != fileId);

    // Source completely untouched.
    PIXET_CHECK(fileExists(joinPath(fx.srcDir, "c.jpg")));
    auto srcSel = fx.db.prepare("SELECT thumb_id FROM files WHERE id=?");
    srcSel.bind(1, fileId);
    PIXET_CHECK(srcSel.step());
    PIXET_CHECK(srcSel.columnInt64(0) == thumbId);

    auto dstSel = fx.db.prepare("SELECT state, thumb_id FROM files WHERE id=?");
    dstSel.bind(1, report.outcomes[0].dstFileId);
    PIXET_CHECK(dstSel.step());
    PIXET_CHECK((FileState)dstSel.columnInt64(0) == FileState::New);
    PIXET_CHECK(dstSel.columnIsNull(1));
}

PIXET_TEST(ReplaceDeletesTheVictimRowAndItsThumbBlob) {
    FileOpsFixture fx("fileops_replace");
    int64_t srcThumbId = 0, victimThumbId = 0;
    int64_t srcFileId = insertTestFile(fx.db, fx.srcDirId, fx.srcDir, "d.jpg", {1}, FileState::Done, srcThumbId);
    int64_t victimFileId =
        insertTestFile(fx.db, fx.dstDirId, fx.dstDir, "d.jpg", {9, 9}, FileState::Done, victimThumbId);
    PIXET_CHECK(victimThumbId != 0);

    Plan plan;
    plan.kind = OpKind::Move;
    plan.dstDirPath = fx.dstDir;
    plan.items.push_back(PlannedItem{joinPath(fx.srcDir, "d.jpg"), "d.jpg", srcFileId, fx.srcDirId, true});
    Report report = execute(fx.db, plan, "test-owner");
    PIXET_CHECK(report.succeeded == 1);

    auto victimSel = fx.db.prepare("SELECT count(*) FROM files WHERE id=?");
    victimSel.bind(1, victimFileId);
    PIXET_CHECK(victimSel.step());
    PIXET_CHECK(victimSel.columnInt64(0) == 0);
    auto blobSel = fx.db.prepare("SELECT count(*) FROM thumbs.thumbs WHERE id=?");
    blobSel.bind(1, victimThumbId);
    PIXET_CHECK(blobSel.step());
    PIXET_CHECK(blobSel.columnInt64(0) == 0);

    PIXET_CHECK(fileExists(joinPath(fx.dstDir, "d.jpg")));
    std::vector<uint8_t> data;
    PIXET_CHECK(readWholeFile(joinPath(fx.dstDir, "d.jpg"), data));
    PIXET_CHECK(data == std::vector<uint8_t>({1})); // the moved-in file's content, not the victim's
}

PIXET_TEST(MidBatchFailureLeavesEarlierItemsCommittedAndLaterOnesAttempted) {
    FileOpsFixture fx("fileops_midbatch");
    int64_t t1 = 0, t3 = 0;
    int64_t id1 = insertTestFile(fx.db, fx.srcDirId, fx.srcDir, "e1.jpg", {1}, FileState::Done, t1);
    int64_t id3 = insertTestFile(fx.db, fx.srcDirId, fx.srcDir, "e3.jpg", {3}, FileState::Done, t3);

    Plan plan;
    plan.kind = OpKind::Move;
    plan.dstDirPath = fx.dstDir;
    plan.items.push_back(PlannedItem{joinPath(fx.srcDir, "e1.jpg"), "e1.jpg", id1, fx.srcDirId, false});
    // e2 has no real file backing it on disk - simulates a source that vanished
    // between preflight and execute. moveFile() must fail cleanly here, not throw,
    // and the batch must continue to e3.
    plan.items.push_back(PlannedItem{joinPath(fx.srcDir, "e2-missing.jpg"), "e2.jpg", 0, fx.srcDirId, false});
    plan.items.push_back(PlannedItem{joinPath(fx.srcDir, "e3.jpg"), "e3.jpg", id3, fx.srcDirId, false});

    Report report = execute(fx.db, plan, "test-owner");
    PIXET_CHECK(report.succeeded == 2);
    PIXET_CHECK(report.failed == 1);
    PIXET_CHECK(report.outcomes.size() == 3);
    PIXET_CHECK(report.outcomes[0].ok);
    PIXET_CHECK(!report.outcomes[1].ok);
    PIXET_CHECK(report.outcomes[1].fsResult == FsResult::SourceMissing);
    PIXET_CHECK(report.outcomes[2].ok);

    PIXET_CHECK(fileExists(joinPath(fx.dstDir, "e1.jpg")));
    PIXET_CHECK(!fileExists(joinPath(fx.dstDir, "e2.jpg")));
    PIXET_CHECK(fileExists(joinPath(fx.dstDir, "e3.jpg")));
}

PIXET_TEST(SourceRowRaceFallsBackToInsertInsteadOfLosingTheFile) {
    FileOpsFixture fx("fileops_racefallback");
    // No files row at all for this id - simulates a concurrent indexer diff having
    // already deleted the source row out from under this move (the one documented
    // race from best-effort claims: the contract is "one unnecessary re-thumbnail",
    // never a lost file).
    writeTestFile(joinPath(fx.srcDir, "f.jpg"), {7, 7});

    Plan plan;
    plan.kind = OpKind::Move;
    plan.dstDirPath = fx.dstDir;
    plan.items.push_back(PlannedItem{joinPath(fx.srcDir, "f.jpg"), "f.jpg", 999999, fx.srcDirId, false});

    Report report = execute(fx.db, plan, "test-owner");
    PIXET_CHECK(report.succeeded == 1);
    PIXET_CHECK(!report.outcomes[0].thumbPreserved);
    PIXET_CHECK(report.outcomes[0].dstFileId != 0);
    PIXET_CHECK(report.outcomes[0].dstFileId != 999999);
    PIXET_CHECK(fileExists(joinPath(fx.dstDir, "f.jpg")));

    auto sel = fx.db.prepare("SELECT state FROM files WHERE id=?");
    sel.bind(1, report.outcomes[0].dstFileId);
    PIXET_CHECK(sel.step());
    PIXET_CHECK((FileState)sel.columnInt64(0) == FileState::New);
}

PIXET_TEST(MoveWithinSameDirToSameNameIsANoOp) {
    FileOpsFixture fx("fileops_samedirnoop");
    int64_t thumbId = 0;
    int64_t fileId = insertTestFile(fx.db, fx.srcDirId, fx.srcDir, "g.jpg", {1, 1, 1}, FileState::Done, thumbId);

    Plan plan;
    plan.kind = OpKind::Move;
    plan.dstDirPath = fx.srcDir; // same directory the file is already in
    plan.items.push_back(PlannedItem{joinPath(fx.srcDir, "g.jpg"), "g.jpg", fileId, fx.srcDirId, false});

    Report report = execute(fx.db, plan, "test-owner");
    PIXET_CHECK(report.succeeded == 1);
    PIXET_CHECK(report.outcomes[0].dstFileId == fileId);
    PIXET_CHECK(fileExists(joinPath(fx.srcDir, "g.jpg")));

    auto sel = fx.db.prepare("SELECT thumb_id, state FROM files WHERE id=?");
    sel.bind(1, fileId);
    PIXET_CHECK(sel.step());
    PIXET_CHECK(sel.columnInt64(0) == thumbId);
    PIXET_CHECK((FileState)sel.columnInt64(1) == FileState::Done);
}

// --------------------------------------------------------------- fileops::executeDelete

PIXET_TEST(ExecuteDeleteRemovesFileRowAndThumbBlob) {
    FileOpsFixture fx("fileops_delete");
    int64_t thumbId = 0;
    int64_t fileId = insertTestFile(fx.db, fx.srcDirId, fx.srcDir, "h.jpg", {1, 2, 3}, FileState::Done, thumbId);
    PIXET_CHECK(thumbId != 0);
    PIXET_CHECK(fileExists(joinPath(fx.srcDir, "h.jpg")));

    std::vector<DeleteItem> items{DeleteItem{joinPath(fx.srcDir, "h.jpg"), fileId, fx.srcDirId}};
    Report report = executeDelete(fx.db, items, "test-owner");
    PIXET_CHECK(report.succeeded == 1);
    PIXET_CHECK(report.failed == 0);
    PIXET_CHECK(report.outcomes[0].ok);

    // moveToTrash() actually sends it to the real Recycle Bin/Trash, not a hard
    // delete - either way it's gone from its original path, which is what the DB
    // row's absence needs to agree with.
    PIXET_CHECK(!fileExists(joinPath(fx.srcDir, "h.jpg")));

    auto sel = fx.db.prepare("SELECT count(*) FROM files WHERE id=?");
    sel.bind(1, fileId);
    PIXET_CHECK(sel.step());
    PIXET_CHECK(sel.columnInt64(0) == 0);

    auto thumbSel = fx.db.prepare("SELECT count(*) FROM thumbs.thumbs WHERE id=?");
    thumbSel.bind(1, thumbId);
    PIXET_CHECK(thumbSel.step());
    PIXET_CHECK(thumbSel.columnInt64(0) == 0);
}

PIXET_TEST(ExecuteDeleteOfRowWithoutThumbIdSucceeds) {
    FileOpsFixture fx("fileops_delete_nothumb");
    int64_t thumbId = 0;
    // FileState::New - never got as far as a thumbnail, so thumb_id is NULL.
    int64_t fileId = insertTestFile(fx.db, fx.srcDirId, fx.srcDir, "i.jpg", {1}, FileState::New, thumbId);
    PIXET_CHECK(thumbId == 0);

    std::vector<DeleteItem> items{DeleteItem{joinPath(fx.srcDir, "i.jpg"), fileId, fx.srcDirId}};
    Report report = executeDelete(fx.db, items, "test-owner");
    PIXET_CHECK(report.succeeded == 1);
    PIXET_CHECK(!fileExists(joinPath(fx.srcDir, "i.jpg")));

    auto sel = fx.db.prepare("SELECT count(*) FROM files WHERE id=?");
    sel.bind(1, fileId);
    PIXET_CHECK(sel.step());
    PIXET_CHECK(sel.columnInt64(0) == 0);
}

PIXET_TEST(ExecuteDeleteFailsGracefullyOnMissingFileWithoutAbortingBatch) {
    FileOpsFixture fx("fileops_delete_missing");
    int64_t thumbId = 0;
    int64_t survivorId = insertTestFile(fx.db, fx.srcDirId, fx.srcDir, "j.jpg", {1}, FileState::Done, thumbId);

    std::vector<DeleteItem> items{
        DeleteItem{joinPath(fx.srcDir, "does-not-exist.jpg"), 999999, fx.srcDirId},
        DeleteItem{joinPath(fx.srcDir, "j.jpg"), survivorId, fx.srcDirId},
    };
    Report report = executeDelete(fx.db, items, "test-owner");
    PIXET_CHECK(report.succeeded == 1);
    PIXET_CHECK(report.failed == 1);
    PIXET_CHECK(!report.outcomes[0].ok);
    PIXET_CHECK(report.outcomes[1].ok);

    // The batch's second, valid item still went through despite the first failing.
    PIXET_CHECK(!fileExists(joinPath(fx.srcDir, "j.jpg")));
    auto sel = fx.db.prepare("SELECT count(*) FROM files WHERE id=?");
    sel.bind(1, survivorId);
    PIXET_CHECK(sel.step());
    PIXET_CHECK(sel.columnInt64(0) == 0);
}

// --------------------------------------------------------- fileops::uniqueNameFor

PIXET_TEST(UniqueNameForAppendsAnIncrementingSuffix) {
    std::string dir = testTempPath("fileops_unique_dir");
    std::filesystem::create_directories(dir);
    writeTestFile(joinPath(dir, "IMG_1234 (2).jpg"), {1}); // pre-occupy "(2)"

    std::set<std::string, CaseInsensitiveLess> none;
    PIXET_CHECK(uniqueNameFor(dir, "IMG_1234.jpg", none) == "IMG_1234 (3).jpg");
}

PIXET_TEST(UniqueNameForKeepsALeadingDotDotfileNameIntactAsTheStem) {
    std::string dir = testTempPath("fileops_unique_dotfile_dir");
    std::filesystem::create_directories(dir);
    std::set<std::string, CaseInsensitiveLess> none;
    PIXET_CHECK(uniqueNameFor(dir, ".DS_Store", none) == ".DS_Store (2)");
}

PIXET_TEST(UniqueNameForSplitsOnTheLastDotForMultiDotExtensions) {
    std::string dir = testTempPath("fileops_unique_multidot_dir");
    std::filesystem::create_directories(dir);
    std::set<std::string, CaseInsensitiveLess> none;
    PIXET_CHECK(uniqueNameFor(dir, "a.tar.gz", none) == "a.tar (2).gz");
}

PIXET_TEST(UniqueNameForAvoidsNamesAlreadyClaimedInTheSameBatch) {
    std::string dir = testTempPath("fileops_unique_batch_dir");
    std::filesystem::create_directories(dir);
    std::set<std::string, CaseInsensitiveLess> taken = {"photo (2).jpg"};
    PIXET_CHECK(uniqueNameFor(dir, "photo.jpg", taken) == "photo (3).jpg");
}

PIXET_TEST(UniqueNameForIsCaseInsensitiveAgainstAlsoTaken) {
    std::string dir = testTempPath("fileops_unique_case_dir");
    std::filesystem::create_directories(dir);
    std::set<std::string, CaseInsensitiveLess> taken = {"PHOTO (2).JPG"};
    PIXET_CHECK(uniqueNameFor(dir, "photo.jpg", taken) == "photo (3).jpg");
}
