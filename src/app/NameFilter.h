#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

// Decides whether a file name passes the grid's name filter (Ctrl+Shift+F by default - see
// NameFilterBar and MainWindow::applyNameFilter()). A value type with no Qt widget or
// settings dependency, so the matching rules can be tested on their own (tests/
// test_namefilter.cpp) rather than only by typing into the running app.
//
// Every mode is case-insensitive and tests the whole file name, extension included, so
// "*.cr3" works. Three modes, because they answer three different questions:
//
// - Wildcard: the text must appear exactly, with `*` standing for any run of characters
//   (including none) and `?` for exactly one. Anchored at both ends the way a shell glob
//   is, so "*john_is*" matches "today_john_is_tall.jpg" but not "today_john_tall_is.jpg",
//   and "IMG_*" means "starts with IMG_". A pattern with no `*` or `?` in it at all is
//   treated as "contains": anchoring it would mean a plain word could only ever match a
//   file with no extension, so typing one would hide everything - the one reading nobody
//   typing a plain word intends. Deliberately not QRegularExpression::
//   wildcardToRegularExpression(): that also treats `[...]` as a character class, and
//   "photo [1].jpg" is an ordinary file name.
// - Fuzzy: each whitespace-separated term must appear in the name as a subsequence - its
//   characters in order, with anything in between - the way fzf or VS Code's Ctrl+P match.
//   "jhntl" finds "john_is_tall". Terms are independent, so "tall john" finds it too.
//   Filters only, never re-ranks: the grid keeps the sort order the user chose.
// - Regex: a PCRE pattern (QRegularExpression) searched for anywhere in the name, the way
//   grep does - anchor with ^ and $ explicitly. `(?-i)` turns case sensitivity back on.
//
// A blank pattern is inactive and matches everything. So is an invalid one (a regex that
// doesn't compile), but that case is reported through isValid()/errorString() so the
// caller can say so rather than silently showing everything.
class NameFilter {
public:
    enum class Mode { Wildcard, Fuzzy, Regex };

    NameFilter() = default; // inactive
    NameFilter(Mode mode, const QString &pattern);

    Mode mode() const { return mode_; }
    // True when this filter actually hides anything: a non-blank pattern that compiled.
    bool isActive() const { return active_; }
    // False only for a regex that doesn't compile; errorString() then says why.
    bool isValid() const { return error_.isEmpty(); }
    QString errorString() const { return error_; }

    // Always true for an inactive (blank or invalid) filter.
    bool matches(const QString &name) const;

    // Same effect on which names pass, not same text: two inactive filters are equal
    // whatever their mode, and wildcard/fuzzy patterns compare after the whitespace
    // trimming they get anyway. Lets ThumbGridModel skip a pointless reset - switching mode
    // with an empty field, say.
    bool operator==(const NameFilter &other) const;
    bool operator!=(const NameFilter &other) const { return !(*this == other); }

private:
    Mode mode_ = Mode::Wildcard;
    bool active_ = false;
    // The pattern as matched - trimmed for wildcard and fuzzy, as typed for regex. What
    // operator== compares.
    QString pattern_;
    QString error_;
    // Wildcard with no `*`/`?`: a plain case-insensitive contains() on pattern_, which is
    // both what that pattern means and cheaper than a regex.
    bool plainSubstring_ = false;
    // Wildcard (with wildcards) and regex.
    QRegularExpression regex_;
    // Fuzzy: the terms, case-folded once here rather than per name.
    QStringList fuzzyTerms_;
};
