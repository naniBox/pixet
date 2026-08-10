#include "ThumbGridModel.h"

#include "db/Database.h"

ThumbGridModel::ThumbGridModel(pixet::Database &db, QObject *parent) : QAbstractListModel(parent), db_(db) {}

void ThumbGridModel::setDirectory(const QString &path) {
    beginResetModel();
    rows_.clear();
    rowByFileId_.clear();
    rowByName_.clear();
    imageCount_ = 0;
    videoCount_ = 0;
    totalBytes_ = 0;
    rawRenderedCount_ = 0;
    rawPreviewCount_ = 0;
    dirId_ = 0;

    std::string pathUtf8 = path.toStdString();
    auto dirSel = db_.prepare("SELECT id FROM dirs WHERE path=?");
    dirSel.bind(1, pathUtf8);
    if (dirSel.step()) {
        dirId_ = dirSel.columnInt64(0);
        auto sel = db_.prepare("SELECT id, name, fmt, state, thumb_id, size, width, height, taken_at, "
                                "duration_ms FROM files WHERE dir_id=? ORDER BY name");
        sel.bind(1, dirId_);
        while (sel.step()) {
            Row row;
            row.id = sel.columnInt64(0);
            row.name = QString::fromStdString(sel.columnText(1));
            row.fmt = (pixet::Format)sel.columnInt64(2);
            row.state = (pixet::FileState)sel.columnInt64(3);
            row.thumbId = sel.columnIsNull(4) ? 0 : sel.columnInt64(4);
            row.size = sel.columnInt64(5);
            row.width = sel.columnIsNull(6) ? 0 : (int)sel.columnInt64(6);
            row.height = sel.columnIsNull(7) ? 0 : (int)sel.columnInt64(7);
            row.takenAt = sel.columnIsNull(8) ? 0 : sel.columnInt64(8);
            row.durationMs = sel.columnIsNull(9) ? 0 : sel.columnInt64(9);
            if (pixet::kindForFormat(row.fmt) == pixet::Kind::Video) videoCount_++;
            else imageCount_++;
            totalBytes_ += row.size;
            if (row.fmt == pixet::Format::Raw) {
                if (row.state == pixet::FileState::Done) rawRenderedCount_++;
                else if (row.state == pixet::FileState::DoneNeedsRender) rawPreviewCount_++;
            }
            rowByFileId_[row.id] = rows_.size();
            rowByName_[row.name] = rows_.size();
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

    auto sel = db_.prepare("SELECT id, state, thumb_id, width, height, taken_at, duration_ms "
                            "FROM files WHERE dir_id=?");
    sel.bind(1, dirId_);
    while (sel.step()) {
        qint64 fileId = sel.columnInt64(0);
        auto it = rowByFileId_.find(fileId);
        if (it == rowByFileId_.end()) continue; // new file mid-Pass-B shouldn't happen; ignore defensively

        int row = it.value();
        auto state = (pixet::FileState)sel.columnInt64(1);
        qint64 thumbId = sel.columnIsNull(2) ? 0 : sel.columnInt64(2);
        // width/height/taken_at/duration_ms are only known once Pass B has actually
        // decoded the file (not at Pass A insert time) - pick them up here too, same
        // as thumb_id/state, rather than only ever refreshing them at setDirectory().
        int width = sel.columnIsNull(3) ? 0 : (int)sel.columnInt64(3);
        int height = sel.columnIsNull(4) ? 0 : (int)sel.columnInt64(4);
        qint64 takenAt = sel.columnIsNull(5) ? 0 : sel.columnInt64(5);
        qint64 durationMs = sel.columnIsNull(6) ? 0 : sel.columnInt64(6);

        if (thumbId != rows_[row].thumbId || state != rows_[row].state || width != rows_[row].width ||
            height != rows_[row].height || takenAt != rows_[row].takenAt || durationMs != rows_[row].durationMs) {
            if (rows_[row].fmt == pixet::Format::Raw && state != rows_[row].state) {
                // Keep rawRenderedCount_/rawPreviewCount_ live across this incremental
                // path too, not just a full setDirectory() reload - a RAW file
                // finishing a background render is exactly the kind of change that
                // arrives here (see the class comment on why these two counts can't
                // just be setDirectory()-only like imageCount_/videoCount_).
                if (rows_[row].state == pixet::FileState::Done) rawRenderedCount_--;
                else if (rows_[row].state == pixet::FileState::DoneNeedsRender) rawPreviewCount_--;
                if (state == pixet::FileState::Done) rawRenderedCount_++;
                else if (state == pixet::FileState::DoneNeedsRender) rawPreviewCount_++;
            }
            rows_[row].thumbId = thumbId;
            rows_[row].state = state;
            rows_[row].width = width;
            rows_[row].height = height;
            rows_[row].takenAt = takenAt;
            rows_[row].durationMs = durationMs;
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
        case SizeRole:
            return (qlonglong)row.size;
        case WidthRole:
            return row.width;
        case HeightRole:
            return row.height;
        case TakenAtRole:
            return (qlonglong)row.takenAt;
        case DurationMsRole:
            return (qlonglong)row.durationMs;
        default:
            return {};
    }
}

int ThumbGridModel::rowForName(const QString &name) const {
    auto it = rowByName_.find(name);
    return it == rowByName_.end() ? -1 : it.value();
}

void ThumbGridModel::setThumbnail(qint64 fileId, const QPixmap &pixmap) {
    auto it = rowByFileId_.find(fileId);
    if (it == rowByFileId_.end()) return;
    int row = it.value();
    rows_[row].thumb = pixmap;
    QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {Qt::DecorationRole});
}
