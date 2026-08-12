#include "DatabaseStatsDialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

#include "db/Database.h"
#include "util/AppPaths.h"

namespace {

int64_t scalar(pixet::Database &db, const std::string &sql) {
    auto st = db.prepare(sql);
    return st.step() ? st.columnInt64(0) : 0;
}

QString bytes(int64_t n) { return QLocale().formattedDataSize(n); }

QString row(const QString &label, const QString &value) {
    return QStringLiteral("<tr><td style='padding-right:18px'>%1</td>"
                           "<td align='right'><b>%2</b></td></tr>")
        .arg(label, value);
}

// Free pages exist because SQLite never shrinks a file on delete - it keeps the pages for
// reuse. Only worth flagging when it's both a meaningful fraction *and* a meaningful
// absolute amount: 60% of a 200KB index.db is not worth anyone's attention.
bool worthCompacting(int64_t freeBytes, int64_t totalBytes) {
    if (freeBytes < 20LL * 1024 * 1024) return false;
    return totalBytes > 0 && (freeBytes * 100 / totalBytes) >= 20;
}

} // namespace

DatabaseStatsDialog::DatabaseStatsDialog(pixet::Database &db, QWidget *parent)
    : QDialog(parent), db_(db) {
    setWindowTitle(QStringLiteral("Database Statistics"));

    auto *layout = new QVBoxLayout(this);
    statsLabel_ = new QLabel(this);
    statsLabel_->setTextFormat(Qt::RichText);
    statsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(statsLabel_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    compactButton_ = buttons->addButton(QStringLiteral("Compact Now"), QDialogButtonBox::ActionRole);
    connect(compactButton_, &QPushButton::clicked, this, &DatabaseStatsDialog::onCompact);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    refresh();
}

void DatabaseStatsDialog::refresh() {
    const int64_t dirs = scalar(db_, "SELECT COUNT(*) FROM dirs");
    const int64_t files = scalar(db_, "SELECT COUNT(*) FROM files");
    const int64_t bookmarks = scalar(db_, "SELECT COUNT(*) FROM bookmarks");
    const int64_t claims = scalar(db_, "SELECT COUNT(*) FROM claims");
    const int64_t thumbs = scalar(db_, "SELECT COUNT(*) FROM thumbs.thumbs");
    const int64_t thumbBytes = scalar(db_, "SELECT COALESCE(SUM(LENGTH(bytes)),0) FROM thumbs.thumbs");
    // Files with no thumbnail yet, excluding the ones that will never get one - a format
    // with no decoder, or one that already failed. Otherwise this reads as a permanent
    // backlog that never clears.
    const int64_t pending = scalar(db_, "SELECT COUNT(*) FROM files WHERE thumb_id IS NULL AND state = 0");

    // PRAGMAs are per-schema: unqualified is index.db (main), `thumbs.` is the attached one.
    const int64_t idxPages = scalar(db_, "PRAGMA page_count");
    const int64_t idxFree = scalar(db_, "PRAGMA freelist_count");
    const int64_t idxPageSize = scalar(db_, "PRAGMA page_size");
    const int64_t thPages = scalar(db_, "PRAGMA thumbs.page_count");
    const int64_t thFree = scalar(db_, "PRAGMA thumbs.freelist_count");
    const int64_t thPageSize = scalar(db_, "PRAGMA thumbs.page_size");

    const int64_t idxOnDisk = QFileInfo(QString::fromStdString(pixet::indexDbPath())).size();
    const int64_t thOnDisk = QFileInfo(QString::fromStdString(pixet::thumbsDbPath())).size();
    const int64_t idxFreeBytes = idxFree * idxPageSize;
    const int64_t thFreeBytes = thFree * thPageSize;
    const int64_t totalFree = idxFreeBytes + thFreeBytes;

    const bool recommend = worthCompacting(totalFree, idxOnDisk + thOnDisk);
    compactButton_->setEnabled(totalFree > 0);

    QString html = QStringLiteral("<table cellspacing='2'>");
    html += QStringLiteral("<tr><td colspan='2'><b>Contents</b></td></tr>");
    html += row(QStringLiteral("Folders indexed"), QLocale().toString(dirs));
    html += row(QStringLiteral("Files indexed"), QLocale().toString(files));
    html += row(QStringLiteral("Thumbnails stored"), QLocale().toString(thumbs));
    html += row(QStringLiteral("Awaiting a thumbnail"), QLocale().toString(pending));
    html += row(QStringLiteral("Bookmarks"), QLocale().toString(bookmarks));
    html += row(QStringLiteral("Active scan claims"), QLocale().toString(claims));

    html += QStringLiteral("<tr><td colspan='2'>&nbsp;</td></tr>");
    html += QStringLiteral("<tr><td colspan='2'><b>On disk</b></td></tr>");
    html += row(QStringLiteral("index.db"), bytes(idxOnDisk));
    html += row(QStringLiteral("thumbs.db"), bytes(thOnDisk));
    html += row(QStringLiteral("Thumbnail image data"), bytes(thumbBytes));
    html += row(QStringLiteral("Total"), bytes(idxOnDisk + thOnDisk));

    html += QStringLiteral("<tr><td colspan='2'>&nbsp;</td></tr>");
    html += QStringLiteral("<tr><td colspan='2'><b>Reclaimable by compacting</b></td></tr>");
    html += row(QStringLiteral("index.db"),
                 QStringLiteral("%1  (%2 of %3 pages)")
                     .arg(bytes(idxFreeBytes), QLocale().toString(idxFree), QLocale().toString(idxPages)));
    html += row(QStringLiteral("thumbs.db"),
                 QStringLiteral("%1  (%2 of %3 pages)")
                     .arg(bytes(thFreeBytes), QLocale().toString(thFree), QLocale().toString(thPages)));
    html += QStringLiteral("</table>");

    if (totalFree == 0) {
        html += QStringLiteral("<p>Nothing to reclaim - the files are already compact.</p>");
    } else if (recommend) {
        html += QStringLiteral("<p><b>Compacting is worthwhile:</b> %1 is being held as free pages. "
                                "SQLite reuses that space rather than returning it to the filesystem, so it "
                                "only shrinks when compacted.</p>")
                    .arg(bytes(totalFree));
    } else {
        html += QStringLiteral("<p>%1 could be reclaimed, but that's a small enough share of the total that "
                                "compacting isn't worth the rewrite - SQLite will reuse those pages anyway.</p>")
                    .arg(bytes(totalFree));
    }
    html += QStringLiteral("<p><i>Regenerating thumbnails is what produces free pages: each one deletes a blob "
                            "and writes a bigger one. Raising the thumbnail size across a large library is the "
                            "case where this adds up.</i></p>");

    statsLabel_->setText(html);
    adjustSize();
}

void DatabaseStatsDialog::onCompact() {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    // Both schemas: VACUUM only ever applies to the one named, and unqualified means main.
    // Can't run inside a transaction, which is why this is a bare exec rather than being
    // wrapped like the batch writes elsewhere.
    db_.exec("VACUUM;");
    db_.exec("VACUUM thumbs;");
    QApplication::restoreOverrideCursor();
    refresh();
}
