// Covers the path/encoding/IO layer added for the macOS port: joinPath and normalizePath
// (util/PathUtil), readWholeFile's contract corners (util/FileIO), and NFC normalization
// (util/Unicode, macOS only).
//
// These are worth testing precisely because their failure mode is silent. A separator or
// normalization mismatch doesn't crash - it makes a DB lookup miss rows that are really
// there, which surfaces much later as a duplicated folder or a photo that re-thumbnails on
// every scan.

#include "TestHarness.h"
#include "TestPaths.h"

#include <filesystem>
#include <string>

#include "util/FileIO.h"
#include "util/PathUtil.h"

#ifdef __APPLE__
#include "util/Unicode.h"
#endif

using namespace pixet;

// ---------------------------------------------------------------- joinPath

PIXET_TEST(JoinPathUsesThePlatformSeparator) {
    const std::string sep(1, pathSeparator());
    PIXET_CHECK(joinPath("dir", "file.jpg") == "dir" + sep + "file.jpg");
}

PIXET_TEST(JoinPathDoesNotDoubleAnExistingSeparator) {
    // Both separators count as "already separated", not just the native one: a caller can
    // hand us a directory that came from Qt, which uses forward slashes on every platform.
    PIXET_CHECK(joinPath("dir/", "file.jpg") == "dir/file.jpg");
    PIXET_CHECK(joinPath("dir\\", "file.jpg") == "dir\\file.jpg");
}

PIXET_TEST(JoinPathHandlesEmptyOperands) {
    PIXET_CHECK(joinPath("", "file.jpg") == "file.jpg");
    PIXET_CHECK(joinPath("dir", "") == "dir");
}

PIXET_TEST(JoinPathNeverEmitsAForeignSeparator) {
    // The regression this whole helper exists for: Indexer used to hardcode '\\', so every
    // subdirectory on a non-Windows box was stored as "/Users/x/Photos\Sub".
    std::string joined = joinPath("parent", "child");
#ifdef _WIN32
    PIXET_CHECK(joined.find('/') == std::string::npos);
#else
    PIXET_CHECK(joined.find('\\') == std::string::npos);
#endif
}

// ----------------------------------------------------------- normalizePath

PIXET_TEST(NormalizePathStripsTrailingSeparators) {
    std::string once = normalizePath("/tmp/pixet_norm/");
    std::string twice = normalizePath("/tmp/pixet_norm///");
    PIXET_CHECK(once == twice);
    PIXET_CHECK(once.back() != '/');
    PIXET_CHECK(once.back() != '\\');
}

PIXET_TEST(NormalizePathLeavesEmptyInputAlone) {
    // GetFullPathNameW returns 0 for an empty input and the Windows implementation returns
    // the input unchanged; the mac one matches rather than resolving "" to the CWD.
    PIXET_CHECK(normalizePath("").empty());
}

PIXET_TEST(NormalizePathCollapsesDotAndDotDot) {
    PIXET_CHECK(normalizePath("/a/b/../c") == normalizePath("/a/c"));
    PIXET_CHECK(normalizePath("/a/./b") == normalizePath("/a/b"));
}

PIXET_TEST(NormalizePathMakesRelativePathsAbsolute) {
    std::string abs = normalizePath("some_relative_dir");
    PIXET_CHECK(abs.size() > std::string("some_relative_dir").size());
    PIXET_CHECK(std::filesystem::path(abs).is_absolute());
}

PIXET_TEST(NormalizePathIsIdempotent) {
    // The property the DB actually depends on: dirs.path is a UNIQUE lookup key, so
    // normalizing an already-normalized path must not change it, or the same folder gets
    // two identities depending on how many times it passed through here.
    std::string once = normalizePath("/tmp/pixet_norm/sub/../sub");
    PIXET_CHECK(normalizePath(once) == once);
}

PIXET_TEST(NormalizePathDoesNotResolveSymlinks) {
    // Deliberately lexical, matching GetFullPathNameW. If this ever starts resolving
    // symlinks, a symlinked photo folder silently changes identity depending on which route
    // the user navigated in by - and on macOS /tmp itself is a symlink to /private/tmp,
    // which is exactly why this assertion is written against /tmp.
    std::string normalized = normalizePath("/tmp/pixet_symlink_probe");
#ifdef _WIN32
    // There's no POSIX-style /tmp to alias via a symlink here - GetFullPathNameW just
    // drive-qualifies the leading '/' (e.g. "C:\tmp\..."), so a literal round trip isn't the
    // right check. What still matters is that it stays lexical: idempotent, and it doesn't
    // silently rewrite the path to something else the way symlink resolution would.
    PIXET_CHECK(normalizePath(normalized) == normalized);
    PIXET_CHECK(normalized.find("pixet_symlink_probe") != std::string::npos);
#else
    PIXET_CHECK(normalized == "/tmp/pixet_symlink_probe");
    PIXET_CHECK(normalized.find("/private/") == std::string::npos);
#endif
}

// ------------------------------------------------------------ readWholeFile

PIXET_TEST(ReadWholeFileRoundTripsContent) {
    std::string path = testTempPath("fileio_roundtrip.bin");
    std::vector<uint8_t> written{0x00, 0x01, 0xFF, 0x7F, 0x80};
    writeTestFile(path, written);

    std::vector<uint8_t> read;
    PIXET_CHECK(readWholeFile(path, read));
    PIXET_CHECK(read == written);
}

PIXET_TEST(ReadWholeFileTreatsEmptyFileAsSuccess) {
    // Not a quibble: callers distinguish "read nothing" from "failed", and the Windows
    // implementation returns true here because its read loop simply never runs. An empty
    // file that reported failure would be recorded as ThumbTier::Failed permanently.
    std::string path = testTempPath("fileio_empty.bin");
    writeTestFile(path, {});

    std::vector<uint8_t> read{0xAA}; // pre-populated, to prove it gets cleared
    PIXET_CHECK(readWholeFile(path, read));
    PIXET_CHECK(read.empty());
}

PIXET_TEST(ReadWholeFileRejectsADirectory) {
    // POSIX open(O_RDONLY) on a directory *succeeds* and only fails later at read() with
    // EISDIR, unlike CreateFileW which fails outright - so without an explicit
    // is_regular_file check the two platforms would diverge here.
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "pixet_tests";
    std::filesystem::create_directories(dir);

    std::vector<uint8_t> read;
    PIXET_CHECK(!readWholeFile(dir.string(), read));
}

PIXET_TEST(ReadWholeFileFailsOnMissingFileWithoutThrowing) {
    std::vector<uint8_t> read;
    PIXET_CHECK(!readWholeFile(nonexistentPath("bin"), read));
}

// ------------------------------------------------------- NFC (macOS only)

#ifdef __APPLE__

PIXET_TEST(ToNfcComposesDecomposedInput) {
    // "Noe" + U+0301 COMBINING ACUTE ACCENT -> "Noé" with a precomposed U+00E9.
    const std::string decomposed = "Noe\xCC\x81mie";
    const std::string composed = "No\xC3\xA9mie";

    PIXET_CHECK(decomposed != composed);          // genuinely different bytes to start with
    PIXET_CHECK(toNfc(decomposed) == composed);
    PIXET_CHECK(toNfc(composed) == composed);     // already-NFC input is left alone
}

PIXET_TEST(ToNfcPassesThroughAsciiAndEmptyUnchanged) {
    PIXET_CHECK(toNfc("").empty());
    PIXET_CHECK(toNfc("plain_ascii_name.jpg") == "plain_ascii_name.jpg");
}

PIXET_TEST(ToNfcPassesThroughInvalidUtf8RatherThanMangling) {
    // A filename we can't interpret is still a filename we have to be able to open, so an
    // undecodable byte sequence comes back untouched rather than replaced or truncated.
    const std::string invalid = "bad\xFF\xFEname";
    PIXET_CHECK(toNfc(invalid) == invalid);
}

PIXET_TEST(NormalizePathNormalizesToNfc) {
    // The integration property that actually protects the DB: whichever form a path arrives
    // in - decomposed from readdir, composed from Qt - both must funnel to the same bytes,
    // or dirs.path gets two rows for one folder.
    std::string fromDecomposed = normalizePath("/tmp/Noe\xCC\x81mie");
    std::string fromComposed = normalizePath("/tmp/No\xC3\xA9mie");
    PIXET_CHECK(fromDecomposed == fromComposed);
    PIXET_CHECK(fromDecomposed.find("\xC3\xA9") != std::string::npos); // and it's the NFC form
}

#endif // __APPLE__
