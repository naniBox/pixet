#pragma once

#include <QDialog>

class QLabel;
class QPushButton;

namespace pixet {
class Database;
}

// Read-only view of what pixet's index actually contains, plus the one maintenance action
// that view can make obvious: compaction.
//
// Compaction is a real need here, not a formality. SQLite never shrinks a database file on
// its own - deleting rows just marks their pages free for reuse. Re-thumbnailing is
// precisely the operation that churns them: every regenerated thumbnail deletes a blob and
// inserts a larger one, so a folder re-rendered at a bigger size leaves its old blobs behind
// as free pages. Raise the thumbnail size across a whole library and thumbs.db can end up
// substantially larger than the data in it. VACUUM rewrites the file without the free pages.
//
// Opened by MainWindow rather than PreferencesDialog (which only emits the request), because
// MainWindow owns the live Database connection.
class DatabaseStatsDialog : public QDialog {
    Q_OBJECT

public:
    explicit DatabaseStatsDialog(pixet::Database &db, QWidget *parent = nullptr);

private slots:
    // VACUUMs both schemas under a wait cursor, then re-reads the stats so the reclaimed
    // space is visible immediately rather than needing the dialog reopened.
    void onCompact();

private:
    void refresh();

    pixet::Database &db_;
    QLabel *statsLabel_;
    QPushButton *compactButton_;
};
