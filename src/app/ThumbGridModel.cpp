#include "ThumbGridModel.h"

#include "db/Database.h"

ThumbGridModel::ThumbGridModel(pixet::Database &db, QObject *parent) : QAbstractListModel(parent), db_(db) {}

ThumbGridModel::Row ThumbGridModel::rowFromStatement(pixet::Statement &sel) {
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
    return row;
}

bool ThumbGridModel::loadRow(const QString &name, Row &out) const {
    if (dirId_ == 0) return false;
    auto sel = db_.prepare("SELECT id, name, fmt, state, thumb_id, size, width, height, taken_at, "
                            "duration_ms FROM files WHERE dir_id=? AND name=?");
    sel.bind(1, dirId_);
    sel.bind(2, name.toStdString());
    if (!sel.step()) return false;
    out = rowFromStatement(sel);
    return true;
}

void ThumbGridModel::accumulate(const Row &row, int sign) {
    if (pixet::kindForFormat(row.fmt) == pixet::Kind::Video) videoCount_ += sign;
    else imageCount_ += sign;
    totalBytes_ += sign * row.size;
    if (row.fmt == pixet::Format::Raw) {
        if (row.state == pixet::FileState::Done) rawRenderedCount_ += sign;
        else if (row.state == pixet::FileState::DoneNeedsRender) rawPreviewCount_ += sign;
    }
}

void ThumbGridModel::reindexLookups() {
    rowByFileId_.clear();
    rowByName_.clear();
    for (int i = 0; i < rows_.size(); ++i) {
        rowByFileId_[rows_[i].id] = i;
        rowByName_[rows_[i].name] = i;
    }
}

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
            Row row = rowFromStatement(sel);
            accumulate(row, +1);
            rows_.push_back(std::move(row));
        }
        reindexLookups();
    }

    endResetModel();
}

int ThumbGridModel::rowForFileId(qint64 fileId) const {
    auto it = rowByFileId_.find(fileId);
    return it == rowByFileId_.end() ? -1 : it.value();
}

int ThumbGridModel::insertOrUpdateFileByName(const QString &name) {
    if (dirId_ == 0) return -1;
    Row row;
    if (!loadRow(name, row)) return -1;

    auto existing = rowByName_.find(name);
    if (existing != rowByName_.end()) {
        // Already present (a second call before the first was needed, or a rename
        // landed on an already-loaded name) - update in place rather than inserting
        // a duplicate row for the same name.
        int r = existing.value();
        accumulate(rows_[r], -1);
        rows_[r] = row;
        accumulate(rows_[r], +1);
        reindexLookups(); // the file id at this name may have changed
        emit dataChanged(index(r), index(r), {Qt::DecorationRole, Qt::DisplayRole});
        return r;
    }

    // Sorted insertion position using SQLite's default BINARY (UTF-8 byte order)
    // comparator - not QString::operator< (UTF-16 code-unit order), which disagrees
    // with it on non-ASCII names. Matching it keeps this path and a full
    // setDirectory() reload in the same row order.
    int insertAt = 0;
    while (insertAt < rows_.size() && rows_[insertAt].name.toUtf8() < row.name.toUtf8()) ++insertAt;

    beginInsertRows(QModelIndex(), insertAt, insertAt);
    accumulate(row, +1);
    rows_.insert(insertAt, std::move(row));
    reindexLookups();
    endInsertRows();
    return insertAt;
}

bool ThumbGridModel::removeFileById(qint64 fileId) {
    auto it = rowByFileId_.find(fileId);
    if (it == rowByFileId_.end()) return false;
    int r = it.value();

    beginRemoveRows(QModelIndex(), r, r);
    accumulate(rows_[r], -1);
    rows_.remove(r);
    reindexLookups();
    endRemoveRows();
    return true;
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

qint64 ThumbGridModel::sizeForRows(const QList<int> &rows) const {
    qint64 total = 0;
    for (int r : rows) {
        if (r >= 0 && r < rows_.size()) total += rows_[r].size;
    }
    return total;
}

void ThumbGridModel::setThumbnail(qint64 fileId, const QPixmap &pixmap) {
    auto it = rowByFileId_.find(fileId);
    if (it == rowByFileId_.end()) return;
    int row = it.value();
    rows_[row].thumb = pixmap;
    QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {Qt::DecorationRole});
}
