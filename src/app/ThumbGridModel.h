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
    void setDirectory(const QString &path);

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
    QVector<Row> rows_;
    QHash<qint64, int> rowByFileId_;
};
