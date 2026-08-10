#include "TestHarness.h"
#include "TestPaths.h"

#include "db/Database.h"
#include "db/Schema.h"

using namespace pixet;

PIXET_TEST(SchemaCreatesExpectedTables) {
    Database db(testTempPath("schema_index.db"), testTempPath("schema_thumbs.db"));

    for (const char *table : {"dirs", "files", "claims", "journal", "bookmarks"}) {
        auto stmt = db.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name=?");
        stmt.bind(1, std::string(table));
        PIXET_CHECK(stmt.step());
    }

    auto thumbsStmt = db.prepare("SELECT name FROM thumbs.sqlite_master WHERE type='table' AND name='thumbs'");
    PIXET_CHECK(thumbsStmt.step());

    // Regression guard: unqualified CREATE TABLE targets `main`, not the attached
    // `thumbs` database - make sure the thumbs table didn't leak into index.db.
    auto mainStmt = db.prepare("SELECT name FROM main.sqlite_master WHERE type='table' AND name='thumbs'");
    PIXET_CHECK(!mainStmt.step());
}

PIXET_TEST(SchemaIsIdempotent) {
    // Opening the same DB twice must not fail (CREATE TABLE IF NOT EXISTS).
    auto indexPath = testTempPath("schema_idempotent_index.db");
    auto thumbsPath = testTempPath("schema_idempotent_thumbs.db");
    { Database db1(indexPath, thumbsPath); }
    { Database db2(indexPath, thumbsPath); }
}

PIXET_TEST(ClassifyFormatMapsExtensions) {
    PIXET_CHECK(classifyFormat("IMG_0001.JPG") == Format::Jpeg);
    PIXET_CHECK(classifyFormat("photo.jpeg") == Format::Jpeg);
    PIXET_CHECK(classifyFormat("scan.png") == Format::Png);
    PIXET_CHECK(classifyFormat("phone.HEIC") == Format::Heic);
    PIXET_CHECK(classifyFormat("DSC001.CR2") == Format::Raw);
    PIXET_CHECK(classifyFormat("scan.tiff") == Format::Tiff);
    PIXET_CHECK(classifyFormat("clip.mov") == Format::Video);
    PIXET_CHECK(kindForFormat(Format::Video) == Kind::Video);
    PIXET_CHECK(kindForFormat(Format::Jpeg) == Kind::Image);
    PIXET_CHECK(classifyFormat("Thumbs.db") == Format::Unknown);
    PIXET_CHECK(classifyFormat("notes.txt") == Format::Unknown);
    PIXET_CHECK(classifyFormat("no_extension") == Format::Unknown);
}
