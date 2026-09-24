#include "NameFilter.h"

namespace {

// True if every character of `term` appears in `text`, in order. Both already case-folded.
bool isSubsequence(const QString &term, const QString &text) {
    qsizetype t = 0;
    for (qsizetype i = 0; i < text.size() && t < term.size(); ++i) {
        if (text.at(i) == term.at(t)) ++t;
    }
    return t == term.size();
}

// `*` and `?` become regex wildcards, and everything between them is escaped so it matches
// literally - brackets, dots and all. See the class comment for why this isn't Qt's own
// wildcard conversion.
QString wildcardToRegex(const QString &pattern) {
    QString regex;
    QString literal;
    for (QChar c : pattern) {
        if (c == QLatin1Char('*') || c == QLatin1Char('?')) {
            regex += QRegularExpression::escape(literal);
            literal.clear();
            regex += c == QLatin1Char('*') ? QStringLiteral(".*") : QStringLiteral(".");
        } else {
            literal += c;
        }
    }
    regex += QRegularExpression::escape(literal);
    return QRegularExpression::anchoredPattern(regex);
}

} // namespace

NameFilter::NameFilter(Mode mode, const QString &pattern) : mode_(mode) {
    switch (mode) {
        case Mode::Wildcard: {
            pattern_ = pattern.trimmed();
            if (pattern_.isEmpty()) return;
            plainSubstring_ = !pattern_.contains(QLatin1Char('*')) && !pattern_.contains(QLatin1Char('?'));
            if (!plainSubstring_) {
                // DotMatchesEverything only so a `*` can't be stopped by a newline, which no
                // real file name has but nothing forbids.
                regex_ = QRegularExpression(wildcardToRegex(pattern_), QRegularExpression::CaseInsensitiveOption |
                                                                           QRegularExpression::DotMatchesEverythingOption);
            }
            active_ = true;
            return;
        }
        case Mode::Fuzzy:
            pattern_ = pattern.simplified();
            fuzzyTerms_ = pattern_.toCaseFolded().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            active_ = !fuzzyTerms_.isEmpty();
            return;
        case Mode::Regex:
            // Not trimmed: a space can be the whole point of a regex. Only a pattern that is
            // nothing *but* whitespace counts as blank, since that is what an empty-looking
            // field with a stray space in it is.
            pattern_ = pattern;
            if (pattern_.trimmed().isEmpty()) return;
            regex_ = QRegularExpression(pattern_, QRegularExpression::CaseInsensitiveOption |
                                                      QRegularExpression::UseUnicodePropertiesOption);
            if (!regex_.isValid()) {
                error_ = regex_.errorString();
                return;
            }
            active_ = true;
            return;
    }
}

bool NameFilter::matches(const QString &name) const {
    if (!active_) return true;
    switch (mode_) {
        case Mode::Wildcard:
            if (plainSubstring_) return name.contains(pattern_, Qt::CaseInsensitive);
            return regex_.match(name).hasMatch();
        case Mode::Fuzzy: {
            const QString folded = name.toCaseFolded();
            for (const QString &term : fuzzyTerms_) {
                if (!isSubsequence(term, folded)) return false;
            }
            return true;
        }
        case Mode::Regex:
            return regex_.match(name).hasMatch();
    }
    return true;
}

bool NameFilter::operator==(const NameFilter &other) const {
    if (!active_ || !other.active_) return active_ == other.active_;
    return mode_ == other.mode_ && pattern_ == other.pattern_;
}
