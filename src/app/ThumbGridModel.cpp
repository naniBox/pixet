#include "ThumbGridModel.h"

#include <algorithm>

#include "db/Database.h"
#include "util/Profile.h"

namespace {
int cmpInt64(qint64 a, qint64 b) { return (a > b) - (a < b); }
} // namespace

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
    // Presence of a latitude is the flag; gps_checked only matters to the backfill sweep,
    // not to painting - an unchecked file simply has no marker yet, same as one with no
    // coordinates.
    row.hasGps = !sel.columnIsNull(10);
    // Appended after gps_lat rather than inserted earlier in the list, so every
    // existing positional index above stays correct - see the column-list comment on
    // setDirectory()'s query.
    row.mtime = sel.columnInt64(11);
    return row;
}

bool ThumbGridModel::loadRow(const QString &name, Row &out) const {
    if (dirId_ == 0) return false;
    auto sel = db_.prepare("SELECT id, name, fmt, state, thumb_id, size, width, height, taken_at, "
                            "duration_ms, gps_lat, mtime FROM files WHERE dir_id=? AND name=?");
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

QString ThumbGridModel::orderByClause() const {
    const QString dir = sortDescending_ ? QStringLiteral("DESC") : QStringLiteral("ASC");
    switch (sortKey_) {
        case prefs::SortKey::FileDate:
            return QStringLiteral("mtime %1, name ASC").arg(dir);
        case prefs::SortKey::TakenDate:
            // The IS NULL term is always ASC on its own, regardless of `dir` - files
            // with no known taken date (0/NULL) sort after every file that has one
            // either way; only the real values within each group flip with direction.
            return QStringLiteral("(taken_at IS NULL) ASC, taken_at %1, name ASC").arg(dir);
        case prefs::SortKey::Size:
            return QStringLiteral("size %1, name ASC").arg(dir);
        case prefs::SortKey::Name:
        default:
            return QStringLiteral("name %1").arg(dir);
    }
}

bool ThumbGridModel::isRowBefore(const Row &a, const Row &b) const {
    if (sortKey_ == prefs::SortKey::Name) {
        // No tiebreak needed - UNIQUE(dir_id, name) means two rows never share a name.
        return sortDescending_ ? a.name.toUtf8() > b.name.toUtf8() : a.name.toUtf8() < b.name.toUtf8();
    }

    int cmp = 0;
    if (sortKey_ == prefs::SortKey::TakenDate) {
        bool aNull = a.takenAt == 0, bNull = b.takenAt == 0;
        if (aNull != bNull) return bNull; // the non-null one always sorts first
        cmp = aNull ? 0 : cmpInt64(a.takenAt, b.takenAt);
    } else if (sortKey_ == prefs::SortKey::FileDate) {
        cmp = cmpInt64(a.mtime, b.mtime);
    } else { // Size
        cmp = cmpInt64(a.size, b.size);
    }
    if (sortDescending_) cmp = -cmp;
    if (cmp != 0) return cmp < 0;
    return a.name.toUtf8() < b.name.toUtf8(); // tiebreak, always ascending - matches orderByClause()
}

void ThumbGridModel::setSortOrder(prefs::SortKey key, bool descending) {
    if (key == sortKey_ && descending == sortDescending_) return;
    sortKey_ = key;
    sortDescending_ = descending;
    if (rows_.isEmpty()) return; // nothing to reorder yet - the next setDirectory() will use the new order

    beginResetModel();
    std::sort(rows_.begin(), rows_.end(), [this](const Row &a, const Row &b) { return isRowBefore(a, b); });
    reindexLookups();
    endResetModel();
}

void ThumbGridModel::setDirectory(const QString &path) {
    PIXET_PROF_SCOPE("model.setDirectory");
    PIXET_PROF_MARK("model.setDirectory");
    beginResetModel();
    rows_.clear();
    rowByFileId_.clear();
    rowByName_.clear();
    // Every cached pixmap died with rows_, so the byte accounting and the eviction queue have
    // to go with it - otherwise the budget stays "full" against pixmaps that no longer exist
    // and the next folder evicts itself immediately.
    thumbFifo_.clear();
    thumbCacheBytes_ = 0;
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
        // Column list must stay in step with loadRow()'s and with rowFromStatement(), which
        // reads them positionally.
        auto sel = db_.prepare("SELECT id, name, fmt, state, thumb_id, size, width, height, taken_at, "
                                "duration_ms, gps_lat, mtime FROM files WHERE dir_id=? ORDER BY " +
                                orderByClause().toStdString());
        sel.bind(1, dirId_);
        {
            PIXET_PROF_SCOPE("model.rowSelect");
            while (sel.step()) {
                Row row = rowFromStatement(sel);
                accumulate(row, +1);
                rows_.push_back(std::move(row));
            }
        }
        PIXET_PROF_COUNT("model.rowsLoaded", rows_.size());
        PIXET_PROF_SCOPE("model.reindexLookups");
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

    // Sorted insertion position under the current sort order - see isRowBefore()'s
    // doc comment for why this has to stay logically equivalent to orderByClause()'s
    // SQL (what a full setDirectory() reload would produce).
    int insertAt = 0;
    while (insertAt < rows_.size() && isRowBefore(rows_[insertAt], row)) ++insertAt;

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

    auto sel = db_.prepare("SELECT id, state, thumb_id, width, height, taken_at, duration_ms, gps_lat "
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
        // Also only known after Pass B (or the backfill sweep) has looked at the file, so
        // the geotag marker appears incrementally like the thumbnails do rather than
        // waiting for the next full reload.
        bool hasGps = !sel.columnIsNull(7);

        if (thumbId != rows_[row].thumbId || state != rows_[row].state || width != rows_[row].width ||
            height != rows_[row].height || takenAt != rows_[row].takenAt || durationMs != rows_[row].durationMs ||
            hasGps != rows_[row].hasGps) {
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
            rows_[row].hasGps = hasGps;
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
            // Asked for *before* returning whatever is already held, not after. A row whose
            // thumb_id changed under it still has the previous pixmap in hand - most visibly
            // a RAW that has just finished its full demosaic render, where the old thumbnail
            // is the camera's embedded preview and the new one is the real thing. Returning
            // early on a non-null pixmap meant that replacement was never requested, so the
            // grid kept showing the superseded thumbnail until the folder was reloaded from
            // scratch.
            //
            // refreshThumbStates() is what clears `requested` when thumb_id moves, and
            // eviction clears the pixmap and the flag together - so the only way to arrive
            // here holding a pixmap with requested == false is a genuinely superseded
            // thumbnail, which is exactly when a re-request is wanted.
            if (row.thumbId != 0 && !row.requested) {
                row.requested = true;
                emit thumbNeeded(row.id, row.thumbId);
            }
            // The superseded pixmap stays on screen until its replacement lands, so the cell
            // visibly turns from the B&W preview into the rendered colour rather than
            // blanking out and filling back in.
            if (!row.thumb.isNull()) return row.thumb;
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
        case HasGpsRole:
            return row.hasGps;
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

QVector<qint64> ThumbGridModel::fileIdsForRows(int first, int last) const {
    first = qMax(0, first);
    last = qMin(last, rows_.size() - 1);
    QVector<qint64> ids;
    if (first > last) return ids;
    ids.reserve(last - first + 1);
    for (int row = first; row <= last; ++row) ids.push_back(rows_[row].id);
    return ids;
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

    // Replacing an existing pixmap (a re-thumbnail landing while this folder is on screen)
    // has to give back the old bytes first, or the accounting drifts up and the cache
    // slowly evicts itself down to nothing.
    thumbCacheBytes_ -= rows_[row].thumbBytes;
    rows_[row].thumb = pixmap;
    // depth() is bits per pixel of the actual backing store (32 here, but 8 for a mask and
    // potentially other values on other backends), so this measures the real cost rather
    // than assuming ARGB32.
    rows_[row].thumbBytes =
        pixmap.isNull() ? 0 : (qint64)pixmap.width() * pixmap.height() * pixmap.depth() / 8;
    thumbCacheBytes_ += rows_[row].thumbBytes;
    thumbFifo_.push_back(row);

    QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {Qt::DecorationRole});

    // After the dataChanged for this row, so the view has already been told about the arrival
    // it was waiting for before anything else is taken away from it.
    evictThumbsIfNeeded();
}

void ThumbGridModel::evictThumbsIfNeeded() {
    while (thumbCacheBytes_ > kThumbCacheBudgetBytes && !thumbFifo_.isEmpty()) {
        int row = thumbFifo_.takeFirst();
        if (row < 0 || row >= rows_.size()) continue; // rows shifted under us (insert/remove)
        Row &r = rows_[row];
        if (r.thumb.isNull()) continue; // already evicted, or a duplicate FIFO entry
        thumbCacheBytes_ -= r.thumbBytes;
        r.thumb = QPixmap();
        r.thumbBytes = 0;
        // Cleared so data() will re-emit thumbNeeded if this row scrolls back into view -
        // without this the row would be permanently blank, having been "requested" once.
        r.requested = false;
        QModelIndex idx = index(row);
        emit dataChanged(idx, idx, {Qt::DecorationRole});
    }
}
