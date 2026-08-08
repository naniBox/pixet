#include "ThumbGridModel.h"

#include "db/Database.h"
#include "util/StringUtil.h"

ThumbGridModel::ThumbGridModel(pixet::Database &db, QObject *parent) : QAbstractListModel(parent), db_(db) {}

void ThumbGridModel::setDirectory(const QString &path) {
    beginResetModel();
    rows_.clear();
    rowByFileId_.clear();
    dirId_ = 0;

    std::string pathUtf8 = pixet::toUtf8(path.toStdWString());
    auto dirSel = db_.prepare("SELECT id FROM dirs WHERE path=?");
    dirSel.bind(1, pathUtf8);
    if (dirSel.step()) {
        dirId_ = dirSel.columnInt64(0);
        auto sel = db_.prepare("SELECT id, name, fmt, state, thumb_id FROM files WHERE dir_id=? ORDER BY name");
        sel.bind(1, dirId_);
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

void ThumbGridModel::refreshThumbStates() {
    if (dirId_ == 0 || rows_.isEmpty()) return;

    // Batched into one dataChanged covering the full changed range, rather than one
    // signal per row - IconMode + setUniformItemSizes has shown flaky partial repaints
    // (some cells not redrawn until an unrelated interaction like a hover) under many
    // small individual dataChanged emissions in quick succession. One range-covering
    // signal is both more efficient and more reliably triggers a full repaint.
    int minChanged = -1, maxChanged = -1;

    auto sel = db_.prepare("SELECT id, state, thumb_id FROM files WHERE dir_id=?");
    sel.bind(1, dirId_);
    while (sel.step()) {
        qint64 fileId = sel.columnInt64(0);
        auto it = rowByFileId_.find(fileId);
        if (it == rowByFileId_.end()) continue; // new file mid-Pass-B shouldn't happen; ignore defensively

        int row = it.value();
        auto state = (pixet::FileState)sel.columnInt64(1);
        qint64 thumbId = sel.columnIsNull(2) ? 0 : sel.columnInt64(2);

        if (thumbId != rows_[row].thumbId || state != rows_[row].state) {
            rows_[row].thumbId = thumbId;
            rows_[row].state = state;
            rows_[row].requested = false; // allow re-request now that a real thumb may exist
            if (minChanged == -1 || row < minChanged) minChanged = row;
            if (row > maxChanged) maxChanged = row;
        }
    }

    if (minChanged != -1) {
        emit dataChanged(index(minChanged), index(maxChanged), {Qt::DecorationRole});
    }
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
