#include "TestHarness.h"
#include "TestPaths.h"

#include "db/Database.h"
#include "scan/Claims.h"

using namespace pixet;

PIXET_TEST(ClaimSucceedsWhenUnclaimed) {
    Database db(testTempPath("claims1_index.db"), testTempPath("claims1_thumbs.db"));
    ClaimManager claims(db);
    PIXET_CHECK(claims.tryClaim(1, "owner-a", 1000));
}

PIXET_TEST(ClaimBlocksOtherOwnerWhileFresh) {
    Database db(testTempPath("claims2_index.db"), testTempPath("claims2_thumbs.db"));
    ClaimManager claims(db);
    PIXET_CHECK(claims.tryClaim(1, "owner-a", 1000));
    PIXET_CHECK(!claims.tryClaim(1, "owner-b", 1500)); // still fresh, held by owner-a
}

PIXET_TEST(ClaimIsReentrantForSameOwner) {
    Database db(testTempPath("claims3_index.db"), testTempPath("claims3_thumbs.db"));
    ClaimManager claims(db);
    PIXET_CHECK(claims.tryClaim(1, "owner-a", 1000));
    PIXET_CHECK(claims.tryClaim(1, "owner-a", 2000)); // renewal, not a conflict
}

PIXET_TEST(StaleClaimCanBeStolen) {
    Database db(testTempPath("claims4_index.db"), testTempPath("claims4_thumbs.db"));
    ClaimManager claims(db);
    PIXET_CHECK(claims.tryClaim(1, "owner-a", 1000));
    // owner-a "died" - heartbeat never renewed. 70s later, past the 60s staleness window.
    PIXET_CHECK(claims.tryClaim(1, "owner-b", 1000 + 70'000));
}

PIXET_TEST(ReleaseAllowsImmediateReclaim) {
    Database db(testTempPath("claims5_index.db"), testTempPath("claims5_thumbs.db"));
    ClaimManager claims(db);
    PIXET_CHECK(claims.tryClaim(1, "owner-a", 1000));
    claims.release(1, "owner-a");
    PIXET_CHECK(claims.tryClaim(1, "owner-b", 1001)); // released, no staleness wait needed
}

PIXET_TEST(ClaimsAreIndependentPerDirectory) {
    Database db(testTempPath("claims6_index.db"), testTempPath("claims6_thumbs.db"));
    ClaimManager claims(db);
    PIXET_CHECK(claims.tryClaim(1, "owner-a", 1000));
    PIXET_CHECK(claims.tryClaim(2, "owner-b", 1000)); // different dir, no conflict
}
