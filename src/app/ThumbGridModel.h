#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPixmap>
#include <QVector>

#include "db/Schema.h"

namespace pixet {
class Database;
}

// Backs the thumbnail grid for the current folder. Rows come from a synchronous
// query against the (small, warm) files table - real disk work (thumbnail blob
// decode) is handed off to ThumbLoader via the thumbNeeded signal and filled in
// asynchronously as it arrives.
class ThumbGridModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        FileIdRole = Qt::UserRole + 1,
        ThumbIdRole,
        FormatRole,
        StateRole,
    };

    explicit ThumbGridModel(pixet::Database &db, QObject *parent = nullptr);

    // Reloads rows for the directory at `path` (looked up by path each time, since the
    // dir's id can change - e.g. it didn't exist yet before FolderIndexer created it).
    // Resets the model - only call when the row *set* may have changed (initial load,
    // or right after Pass A lists files for a folder). Existing cached thumbnail
    // pixmaps are discarded, so calling this repeatedly during Pass B would flicker.
    void setDirectory(const QString &path);

    // Re-checks thumb_id/state for the *currently loaded* rows without resetting the
    // model, so already-displayed thumbnails aren't touched - only rows whose
    // thumb_id actually changed get a dataChanged (which lets the view re-request a
    // real thumbnail for what was a placeholder). This is what makes Pass B's
    // progress visible incrementally instead of in one final jump. No-op if
    // setDirectory hasn't been called yet.
    void refreshThumbStates();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

public slots:
    void setThumbnail(qint64 fileId, const QPixmap &pixmap);

signals:
    void thumbNeeded(qint64 fileId, qint64 thumbId) const;

private:
    struct Row {
        qint64 id = 0;
        QString name;
        pixet::Format fmt = pixet::Format::Unknown;
        pixet::FileState state = pixet::FileState::New;
        qint64 thumbId = 0;
        QPixmap thumb;
        mutable bool requested = false;
    };

    pixet::Database &db_;
    int64_t dirId_ = 0;
    QVector<Row> rows_;
    QHash<qint64, int> rowByFileId_;
};
