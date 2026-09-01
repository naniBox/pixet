#include "FolderListModel.h"

#include <QFileInfo>
#include <QPixmap>

#include <algorithm>

#include "Preferences.h"
#include "ThumbGridModel.h"
#include "db/Schema.h"
#include "scan/DirWalker.h"
#include "util/PathUtil.h"

namespace {

int cmpInt64(qint64 a, qint64 b) { return a < b ? -1 : (a > b ? 1 : 0); }

} // namespace

FolderListModel::FolderListModel(QObject *parent) : QAbstractListModel(parent) {}

int FolderListModel::setFile(const QString &filePath) {
    QFileInfo info(filePath);
    const QString dir =
        QString::fromStdString(pixet::normalizePath(info.absolutePath().toStdString()));
    const QString target = info.fileName();

    std::vector<pixet::DirEntry> entries;
    try {
        entries = pixet::listDir(dir.toStdString());
    } catch (const std::exception &) {
        // Unreadable directory (permissions, a volume that went away between the shell
        // resolving the path and us listing it). Reported as "no row" rather than thrown
        // on: the caller's fallback - hand the path to the full app - is the same answer
        // for every reason this can fail.
        return -1;
    }

    beginResetModel();
    directoryPath_ = dir;
    rows_.clear();
    rows_.reserve((int)entries.size());
    for (const pixet::DirEntry &entry : entries) {
        if (entry.isDir) continue;
        pixet::Format fmt = pixet::classifyFormat(entry.name);
        if (fmt == pixet::Format::Unknown) continue; // sidecars, Thumbs.db, ... - same filter the indexer applies
        Row row;
        row.name = QString::fromStdString(entry.name);
        row.fmt = (int)fmt;
        row.size = entry.size;
        row.mtime = entry.mtimeUnix;
        rows_.append(row);
    }

    // Matches ThumbGridModel::isRowBefore() so that arrowing through here visits files in
    // the same order the grid would, including the UTF-8 byte-wise name comparison (which
    // is SQLite's BINARY collation, not QString::operator<).
    //
    // TakenDate is the one order that can't be honoured: capture time lives in each file's
    // EXIF, and reading it means opening every file in the folder - seconds of I/O before
    // the first pixel, which is the entire cost this mode exists to skip. Those folders
    // fall back to name order here. The user still sees the sort they chose the moment
    // they press Enter into the folder view.
    const prefs::SortKey key = prefs::gridSortKey();
    const bool descending = prefs::gridSortDescending();
    std::sort(rows_.begin(), rows_.end(), [key, descending](const Row &a, const Row &b) {
        const QByteArray aName = a.name.toUtf8(), bName = b.name.toUtf8();
        if (key == prefs::SortKey::Name || key == prefs::SortKey::TakenDate) {
            return descending ? aName > bName : aName < bName;
        }
        int cmp = (key == prefs::SortKey::FileDate) ? cmpInt64(a.mtime, b.mtime) : cmpInt64(a.size, b.size);
        if (descending) cmp = -cmp;
        if (cmp != 0) return cmp < 0;
        return aName < bName; // tiebreak, always ascending - matches ThumbGridModel
    });
    endResetModel();

    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_.at(i).name == target) return i;
    }
    return -1;
}

void FolderListModel::setNativeSize(int row, QSize size) {
    if (row < 0 || row >= rows_.size()) return;
    if (size.width() <= 0 || size.height() <= 0) return;
    Row &r = rows_[row];
    if (r.width == size.width() && r.height == size.height()) return;
    r.width = size.width();
    r.height = size.height();
    QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ThumbGridModel::WidthRole, ThumbGridModel::HeightRole});
}

int FolderListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return rows_.size();
}

QVariant FolderListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return QVariant();
    const Row &row = rows_.at(index.row());

    switch (role) {
        case Qt::DisplayRole:
            return row.name;
        case Qt::DecorationRole:
            // No cached thumbnail to fall back on - there is no thumbs.db in this mode.
            // FullscreenViewer treats a null pixmap as "nothing to draw yet" and paints
            // black until the real decode lands, which for one image is the next frame or
            // two rather than the folder-wide wait the grid would have.
            return QPixmap();
        case ThumbGridModel::FormatRole:
            return row.fmt;
        case ThumbGridModel::StateRole:
            // Nothing here consults it, but answering Done rather than New keeps the role
            // from reading as "this file is queued for work" in a mode where no work is queued.
            return (int)pixet::FileState::Done;
        case ThumbGridModel::SizeRole:
            return row.size;
        case ThumbGridModel::WidthRole:
            return row.width;
        case ThumbGridModel::HeightRole:
            return row.height;
        case ThumbGridModel::FileIdRole:
        case ThumbGridModel::ThumbIdRole:
        case ThumbGridModel::TakenAtRole:
        case ThumbGridModel::DurationMsRole:
            // All of these are indexer products (a files row id, a thumbs blob id, parsed
            // EXIF, a probed video duration). Zero is what FullscreenViewer's info overlay
            // already reads as "unknown" and omits, so the overlay simply shows fewer
            // fields here rather than needing a mode of its own.
            return (qint64)0;
        case ThumbGridModel::HasGpsRole:
            return false;
        default:
            return QVariant();
    }
}
