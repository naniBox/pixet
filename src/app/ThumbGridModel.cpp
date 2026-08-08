#include "ThumbGridModel.h"

#include "db/Database.h"
#include "util/StringUtil.h"

ThumbGridModel::ThumbGridModel(pixet::Database &db, QObject *parent) : QAbstractListModel(parent), db_(db) {}

void ThumbGridModel::setDirectory(const QString &path) {
    beginResetModel();
    rows_.clear();
    rowByFileId_.clear();

    std::string pathUtf8 = pixet::toUtf8(path.toStdWString());
    auto dirSel = db_.prepare("SELECT id FROM dirs WHERE path=?");
    dirSel.bind(1, pathUtf8);
    if (dirSel.step()) {
        int64_t dirId = dirSel.columnInt64(0);
        auto sel = db_.prepare("SELECT id, name, fmt, state, thumb_id FROM files WHERE dir_id=? ORDER BY name");
        sel.bind(1, dirId);
        while (sel.step()) {
            Row row;
            row.id = sel.columnInt64(0);
            row.name = QString::fromStdWString(pixet::toUtf16(sel.columnText(1)));
            row.fmt = (pixet::Format)sel.columnInt64(2);
            row.state = (pixet::FileState)sel.columnInt64(3);
            row.thumbId = sel.columnIsNull(4) ? 0 : sel.columnInt64(4);
            rowByFileId_[row.id] = rows_.size();
            rows_.push_back(std::move(row));
        }
    }

    endResetModel();
}

int ThumbGridModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : rows_.size(); }

QVariant ThumbGridModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
    const Row &row = rows_[index.row()];

    switch (role) {
        case Qt::DisplayRole:
            return row.name;
        case Qt::DecorationRole:
            if (!row.thumb.isNull()) return row.thumb;
            if (row.thumbId != 0 && !row.requested) {
                row.requested = true;
                emit thumbNeeded(row.id, row.thumbId);
            }
            return QVariant();
        case FileIdRole:
            return (qlonglong)row.id;
        case ThumbIdRole:
            return (qlonglong)row.thumbId;
        case FormatRole:
            return (int)row.fmt;
        case StateRole:
            return (int)row.state;
        default:
            return {};
    }
}

void ThumbGridModel::setThumbnail(qint64 fileId, const QPixmap &pixmap) {
    auto it = rowByFileId_.find(fileId);
    if (it == rowByFileId_.end()) return;
    int row = it.value();
    rows_[row].thumb = pixmap;
    QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {Qt::DecorationRole});
}
