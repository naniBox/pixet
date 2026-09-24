#include "TestHarness.h"

#include "NameFilter.h"

using Mode = NameFilter::Mode;

namespace {
bool matches(Mode mode, const char *pattern, const char *name) {
    return NameFilter(mode, QString::fromUtf8(pattern)).matches(QString::fromUtf8(name));
}
} // namespace

// The example the feature was asked for with: the text between the stars has to appear
// exactly, so the same words in a different order don't count.
PIXET_TEST(namefilter_wildcard_is_exact_between_the_stars) {
    PIXET_CHECK(matches(Mode::Wildcard, "*john_is*", "today_john_is_tall.jpg"));
    PIXET_CHECK(!matches(Mode::Wildcard, "*john_is*", "today_john_tall_is.jpg"));
}

PIXET_TEST(namefilter_wildcard_is_anchored_like_a_glob) {
    PIXET_CHECK(matches(Mode::Wildcard, "IMG_*", "IMG_0001.jpg"));
    PIXET_CHECK(!matches(Mode::Wildcard, "IMG_*", "old_IMG_0001.jpg"));
    PIXET_CHECK(matches(Mode::Wildcard, "*.cr3", "DSC01234.CR3"));
    PIXET_CHECK(!matches(Mode::Wildcard, "*.cr3", "DSC01234.CR3.xmp"));
    PIXET_CHECK(matches(Mode::Wildcard, "IMG_000?.jpg", "IMG_0007.jpg"));
    PIXET_CHECK(!matches(Mode::Wildcard, "IMG_000?.jpg", "IMG_00070.jpg"));
    PIXET_CHECK(matches(Mode::Wildcard, "*", "anything.png"));
}

// No wildcard at all means "contains" - anchoring a plain word would only ever match a
// file with no extension.
PIXET_TEST(namefilter_wildcard_without_wildcards_is_a_substring) {
    PIXET_CHECK(matches(Mode::Wildcard, "john_is", "today_john_is_tall.jpg"));
    PIXET_CHECK(!matches(Mode::Wildcard, "john_is", "today_john_tall_is.jpg"));
    PIXET_CHECK(matches(Mode::Wildcard, "  john  ", "JOHN.jpg")); // trimmed, case-insensitive
}

// Everything that isn't * or ? is literal - including what would be a character class or
// a regex metacharacter somewhere else.
PIXET_TEST(namefilter_wildcard_treats_everything_else_literally) {
    PIXET_CHECK(matches(Mode::Wildcard, "*[1]*", "photo [1].jpg"));
    PIXET_CHECK(!matches(Mode::Wildcard, "*[1]*", "photo 1.jpg"));
    PIXET_CHECK(matches(Mode::Wildcard, "*a.b*", "xa.by.png"));
    PIXET_CHECK(!matches(Mode::Wildcard, "*a.b*", "xaxby.png"));
    PIXET_CHECK(matches(Mode::Wildcard, "*(2)*", "copy (2).jpg"));
}

PIXET_TEST(namefilter_fuzzy_matches_subsequences) {
    PIXET_CHECK(matches(Mode::Fuzzy, "jhntl", "today_john_is_tall.jpg"));
    PIXET_CHECK(matches(Mode::Fuzzy, "JOHN", "today_john_is_tall.jpg"));
    PIXET_CHECK(!matches(Mode::Fuzzy, "jhnz", "today_john_is_tall.jpg"));
    // Order matters within a term...
    PIXET_CHECK(!matches(Mode::Fuzzy, "nhoj", "today_john_is_tall.jpg"));
    // ...but not between terms.
    PIXET_CHECK(matches(Mode::Fuzzy, "tall john", "today_john_is_tall.jpg"));
    PIXET_CHECK(!matches(Mode::Fuzzy, "tall mary", "today_john_is_tall.jpg"));
}

PIXET_TEST(namefilter_regex_searches_unanchored) {
    PIXET_CHECK(matches(Mode::Regex, "john_(is|was)", "today_john_was_tall.jpg"));
    PIXET_CHECK(!matches(Mode::Regex, "^john", "today_john_was_tall.jpg"));
    PIXET_CHECK(matches(Mode::Regex, "\\.jpe?g$", "a.JPEG"));
    PIXET_CHECK(!matches(Mode::Regex, "(?-i)\\.jpe?g$", "a.JPEG"));
}

PIXET_TEST(namefilter_invalid_regex_is_reported_and_inactive) {
    NameFilter f(Mode::Regex, QStringLiteral("(john"));
    PIXET_CHECK(!f.isValid());
    PIXET_CHECK(!f.errorString().isEmpty());
    PIXET_CHECK(!f.isActive());
    PIXET_CHECK(f.matches(QStringLiteral("anything.jpg")));
}

PIXET_TEST(namefilter_blank_pattern_is_inactive_in_every_mode) {
    for (Mode mode : {Mode::Wildcard, Mode::Fuzzy, Mode::Regex}) {
        NameFilter f(mode, QStringLiteral("   "));
        PIXET_CHECK(!f.isActive());
        PIXET_CHECK(f.isValid());
        PIXET_CHECK(f.matches(QStringLiteral("anything.jpg")));
    }
    PIXET_CHECK(!NameFilter().isActive());
}

PIXET_TEST(namefilter_equality_is_about_effect) {
    PIXET_CHECK(NameFilter(Mode::Wildcard, QString()) == NameFilter(Mode::Regex, QString()));
    PIXET_CHECK(NameFilter(Mode::Wildcard, QStringLiteral("john ")) == NameFilter(Mode::Wildcard, QStringLiteral("john")));
    PIXET_CHECK(NameFilter(Mode::Wildcard, QStringLiteral("john")) != NameFilter(Mode::Fuzzy, QStringLiteral("john")));
    PIXET_CHECK(NameFilter(Mode::Regex, QStringLiteral("john")) != NameFilter(Mode::Regex, QStringLiteral("john ")));
}
