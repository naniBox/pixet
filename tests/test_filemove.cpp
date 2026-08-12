// Covers util/FileMove (the no-overwrite move/copy/rename primitives fileops::execute
// is built on) - the app's first code path that ever mutates a real file on disk, so
// "never silently overwrites" and "fails cleanly rather than throwing" are the two
// properties worth nailing down here before anything is built on top of them.
//
// Cross-volume behavior (the copy-then-delete-source fallback inside moveFile()) can't
// be exercised portably by this suite - it needs two actual volumes (a USB stick, a
// second partition) and is manual-only; see the devlog/plan for that verification step.

#include "TestHarness.h"
#include "TestPaths.h"

#include <filesystem>

#include "util/FileIO.h"
#include "util/FileMove.h"

using namespace pixet;

PIXET_TEST(MoveFileMovesContentAndRemovesSource) {
    std::string src = testTempPath("filemove_move_src.bin");
    std::string dst = testTempPath("filemove_move_dst.bin");
    writeTestFile(src, {1, 2, 3, 4});

    PIXET_CHECK(moveFile(src, dst) == FsResult::Ok);
    PIXET_CHECK(!fileExists(src));
    PIXET_CHECK(fileExists(dst));

    std::vector<uint8_t> data;
    PIXET_CHECK(readWholeFile(dst, data));
    PIXET_CHECK(data == std::vector<uint8_t>({1, 2, 3, 4}));
}

PIXET_TEST(MoveFileNeverOverwritesAnExistingDestination) {
    std::string src = testTempPath("filemove_noclobber_src.bin");
    std::string dst = testTempPath("filemove_noclobber_dst.bin");
    writeTestFile(src, {1});
    writeTestFile(dst, {9, 9, 9});

    PIXET_CHECK(moveFile(src, dst) == FsResult::DestExists);
    // Neither side touched - this is the property the whole primitive exists for.
    PIXET_CHECK(fileExists(src));
    std::vector<uint8_t> data;
    PIXET_CHECK(readWholeFile(dst, data));
    PIXET_CHECK(data == std::vector<uint8_t>({9, 9, 9}));
}

PIXET_TEST(MoveFileFailsCleanlyOnMissingSource) {
    PIXET_CHECK(moveFile(nonexistentPath("bin"), testTempPath("filemove_missing_dst.bin")) == FsResult::SourceMissing);
}

PIXET_TEST(CopyFilePreservesSourceAndDuplicatesContent) {
    std::string src = testTempPath("filemove_copy_src.bin");
    std::string dst = testTempPath("filemove_copy_dst.bin");
    writeTestFile(src, {5, 6, 7});

    PIXET_CHECK(copyFile(src, dst) == FsResult::Ok);
    PIXET_CHECK(fileExists(src));
    PIXET_CHECK(fileExists(dst));

    std::vector<uint8_t> srcData, dstData;
    PIXET_CHECK(readWholeFile(src, srcData));
    PIXET_CHECK(readWholeFile(dst, dstData));
    PIXET_CHECK(srcData == dstData);
}

PIXET_TEST(CopyFileNeverOverwritesAnExistingDestination) {
    std::string src = testTempPath("filemove_copy_noclobber_src.bin");
    std::string dst = testTempPath("filemove_copy_noclobber_dst.bin");
    writeTestFile(src, {1});
    writeTestFile(dst, {2});

    PIXET_CHECK(copyFile(src, dst) == FsResult::DestExists);
    std::vector<uint8_t> data;
    PIXET_CHECK(readWholeFile(dst, data));
    PIXET_CHECK(data == std::vector<uint8_t>({2}));
}

PIXET_TEST(CopyFileFailsCleanlyOnMissingSource) {
    PIXET_CHECK(copyFile(nonexistentPath("bin"), testTempPath("filemove_copy_missing_dst.bin")) ==
                FsResult::SourceMissing);
}

PIXET_TEST(RenameWithinDirRenamesAndNeverOverwrites) {
    std::string a = testTempPath("filemove_rename_a.bin");
    std::string b = testTempPath("filemove_rename_b.bin");
    writeTestFile(a, {1});

    PIXET_CHECK(renameWithinDir(a, b) == FsResult::Ok);
    PIXET_CHECK(!fileExists(a));
    PIXET_CHECK(fileExists(b));

    writeTestFile(a, {2}); // a fresh file to attempt renaming onto the occupied name
    PIXET_CHECK(renameWithinDir(a, b) == FsResult::DestExists);
    std::vector<uint8_t> data;
    PIXET_CHECK(readWholeFile(b, data));
    PIXET_CHECK(data == std::vector<uint8_t>({1})); // untouched
}

PIXET_TEST(RemoveFileDeletesAndReportsAlreadyMissing) {
    std::string path = testTempPath("filemove_remove.bin");
    writeTestFile(path, {1});

    PIXET_CHECK(removeFile(path) == FsResult::Ok);
    PIXET_CHECK(!fileExists(path));
    PIXET_CHECK(removeFile(path) == FsResult::SourceMissing);
}

PIXET_TEST(StatFileRoundTripsSizeAndAPositiveMtime) {
    std::string path = testTempPath("filemove_stat.bin");
    writeTestFile(path, {1, 2, 3, 4, 5});

    int64_t size = -1, mtime = -1;
    PIXET_CHECK(statFile(path, &size, &mtime));
    PIXET_CHECK(size == 5);
    PIXET_CHECK(mtime > 0);
}

PIXET_TEST(StatFileFailsOnMissingFile) {
    int64_t size = 0, mtime = 0;
    PIXET_CHECK(!statFile(nonexistentPath("bin"), &size, &mtime));
}

PIXET_TEST(FileExistsAndIsDirectoryDistinguishCorrectly) {
    std::string dirPath = testTempPath("filemove_dir_probe");
    std::filesystem::create_directories(dirPath);
    PIXET_CHECK(isDirectory(dirPath));
    PIXET_CHECK(!fileExists(dirPath));

    std::string filePath = testTempPath("filemove_file_probe.bin");
    writeTestFile(filePath, {1});
    PIXET_CHECK(fileExists(filePath));
    PIXET_CHECK(!isDirectory(filePath));
}

PIXET_TEST(StatFileRejectsADirectory) {
    std::string dirPath = testTempPath("filemove_stat_dir_probe");
    std::filesystem::create_directories(dirPath);
    int64_t size = 0, mtime = 0;
    PIXET_CHECK(!statFile(dirPath, &size, &mtime));
}
