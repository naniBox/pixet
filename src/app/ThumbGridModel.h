#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPixmap>
#include <QVector>

#include "NameFilter.h"
#include "Preferences.h"
#include "db/Schema.h"

namespace pixet {
class Database;
class Statement;
}

// Backs the thumbnail grid for the current folder. Rows come from a synchronous
// query against the (small, warm) files table - real disk work (thumbnail blob
// decode) is handed off to ThumbLoader via the thumbNeeded signal and filled in
// asynchronously as it arrives.
//
// The name filter (see setNameFilter()) is applied here rather than by a
// QSortFilterProxyModel in front, and that is why every row number this class takes or
// returns is a *shown* row - one the filter lets through. MainWindow, ThumbGridView and
// FullscreenViewer all address this model's rows directly (gridModel_->index(
// grid_->currentRow()) and friends, dozens of times), and a proxy would have made every
// one of those quietly index the wrong file until it was taught to map through it.
// Filtering inside keeps "row" meaning one thing everywhere, so Select All, fullscreen
// browsing, drag-out and the indexer's priority hint all act on exactly the matches with
// no extra wiring. Hidden files are still held (rows_) - which is also what lets a filter
// change keep every already-decoded thumbnail rather than dropping and re-decoding them.
class ThumbGridModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        FileIdRole = Qt::UserRole + 1,
        ThumbIdRole,
        FormatRole,
        StateRole,
        SizeRole,      // bytes, qint64
        WidthRole,     // px, int (0/invalid if unknown)
        HeightRole,    // px, int (0/invalid if unknown)
        TakenAtRole,   // unix seconds, qint64 (0 if unknown)
        DurationMsRole,// video only, qint64 (0 if unknown/not video)
        // bool - the file has EXIF GPS coordinates. Stored on the row rather than read
        // per paint: ThumbGridView asks for this for every visible cell on every repaint.
        HasGpsRole,
    };

    explicit ThumbGridModel(pixet::Database &db, QObject *parent = nullptr);

    // Reloads rows for the directory at `path` (looked up by path each time, since the
    // dir's id can change - e.g. it didn't exist yet before FolderIndexer created it).
    // Resets the model - only call when the row *set* may have changed (initial load,
    // or right after Pass A lists files for a folder). Existing cached thumbnail
    // pixmaps are discarded, so calling this repeatedly during Pass B would flicker.
    void setDirectory(const QString &path);

    // Changes sort key/direction and, if a folder is already loaded, reorders rows_ in
    // place (an in-memory std::sort + model reset) rather than re-querying - a sort
    // change doesn't touch which files exist or their data, only the order they're
    // shown in, so re-running setDirectory() would cost a DB round trip and throw away
    // every already-decoded thumbnail pixmap just to redraw them in a different order.
    // No-op (doesn't even reset the model) if `key`/`descending` already match the
    // current order. Safe to call before any folder is loaded (rows_ empty) - just
    // updates the settings so the next setDirectory() picks them up; MainWindow uses
    // this to push the persisted sort order in before the first real navigation.
    void setSortOrder(prefs::SortKey key, bool descending);

    // Shows only the files whose name passes `filter`, as a model reset - a filter change
    // rewrites which file every row number refers to, which is what a reset means, and
    // selection is carried across it by file id the same way a sort change does (see
    // MainWindow::applyNameFilter()). Persists across setDirectory(), so a filter left
    // open applies to each folder navigated into. False, and no reset, when `filter`
    // would let through exactly what the current one does (see NameFilter::operator==).
    bool setNameFilter(const NameFilter &filter);
    const NameFilter &nameFilter() const { return filter_; }
    // Files in the folder whether the filter shows them or not - rowCount() is only the
    // shown ones. Together they are the filter bar's "12 of 340".
    int unfilteredRowCount() const { return rows_.size(); }

    // Re-checks thumb_id/state for the *currently loaded* rows without resetting the
    // model, so already-displayed thumbnails aren't touched - only rows whose
    // thumb_id actually changed get a dataChanged (which lets the view re-request a
    // real thumbnail for what was a placeholder). This is what makes Pass B's
    // progress visible incrementally instead of in one final jump. No-op if
    // setDirectory hasn't been called yet.
    void refreshThumbStates();

    // Row index for the (currently loaded) file named `name`, or -1 if there's no
    // such row - used by the path bar's "paste a file path" -> select-it behavior.
    int rowForName(const QString &name) const;

    // Sum of `size` over the given rows, read directly from rows_ rather than N
    // QVariant round trips through data() - Select All on a large folder would
    // otherwise be tens of thousands of QVariant constructions per status-bar update.
    qint64 sizeForRows(const QList<int> &rows) const;

    // File ids for rows [first, last] inclusive, in row (i.e. display) order. The range
    // is clipped to what exists rather than validated, so a caller can ask for a
    // screenful past either end without doing the arithmetic itself - which is exactly
    // how MainWindow builds the indexer's priority hint (see
    // FolderIndexer::setPriorityFiles). Empty if the range doesn't overlap any row.
    QVector<qint64> fileIdsForRows(int first, int last) const;

    // True once setDirectory() has resolved a real dirs row - insertOrUpdateFileByName()/
    // removeFileById() are meaningless before that (there's nothing to query
    // against); callers should fall back to a full setDirectory() reload instead.
    bool hasDirectory() const { return dirId_ != 0; }

    // Row index for the (currently loaded) file with id `fileId`, or -1 - mirror of
    // rowForName(), used to re-derive a selection by file identity across a reload
    // (see MainWindow::reloadGridPreservingSelection).
    int rowForFileId(qint64 fileId) const;

    // Re-reads the files row for `name` in the current directory and inserts it at
    // its correct sorted-by-name position (matching SQLite's BINARY ORDER BY name,
    // not QString::operator<), or updates it in place if a row for that name already
    // exists. Emits begin/endInsertRows or dataChanged accordingly, so an already-
    // displayed grid gains/updates exactly one cell rather than resetting (and losing
    // scroll position/selection) - used after a file-move/copy/drag/paste lands a
    // file in the currently-open folder. Returns the resulting row index, or -1 if
    // there's no such files row (caller must commit it to the DB first),
    // hasDirectory() is false, or the name filter hides it - in which case the file is
    // still recorded, and appears as soon as the filter lets it through, but no row
    // signal is emitted because no row the view knows about changed.
    int insertOrUpdateFileByName(const QString &name);

    // Removes the row for `fileId`, emitting begin/endRemoveRows - the counterpart to
    // insertOrUpdateFileByName() for a file that moved *out* of the currently-open
    // folder. False if there was no such row. A file the name filter hides is removed
    // just the same, silently.
    bool removeFileById(qint64 fileId);

    // Folder-level aggregates over every row currently loaded (all states, including
    // not-yet-thumbnailed - size/kind are known from Pass A alone) - recomputed by
    // setDirectory(), not refreshThumbStates() (kind/size never change in Pass B).
    // Whole-folder regardless of the name filter, like the RAW counts below: the status
    // bar describes the folder, and the filter bar reports how much of it is on screen.
    int imageCount() const { return imageCount_; }
    int videoCount() const { return videoCount_; }
    qint64 totalBytes() const { return totalBytes_; }

    // RAW files whose current thumbnail is a full demosaic render (state=Done) vs.
    // still the fast embedded-preview one (state=DoneNeedsRender) - see
    // RawRenderer/`pixet-index --render-raws`. Unlike imageCount()/videoCount()/
    // totalBytes(), these *do* need to stay live across refreshThumbStates() too (not
    // just setDirectory()), since a RAW file's state can flip from DoneNeedsRender to
    // Done via exactly the incremental Pass-B-progress path refreshThumbStates()
    // exists for - a background render finishing while this folder is on screen.
    // Zero rawRenderedCount()+rawPreviewCount() means either no RAW files in this
    // folder, or none have reached either state yet (still New/Failed) - callers
    // wanting "are there any RAW files at all" should check imageCount() context or
    // just treat 0+0 as "nothing to report" either way.
    int rawRenderedCount() const { return rawRenderedCount_; }
    int rawPreviewCount() const { return rawPreviewCount_; }

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
        qint64 size = 0;
        qint64 mtime = 0;
        int width = 0;
        int height = 0;
        qint64 takenAt = 0;
        qint64 durationMs = 0;
        bool hasGps = false;
        QPixmap thumb;
        mutable bool requested = false;
        // Whether the name filter lets this row through - decided once, when the row is
        // created or the filter changes, rather than every time reindexLookups() rebuilds
        // shown_: that runs on every single-file insert/remove, and re-running a regex over
        // the whole folder for each file of a large paste would add up.
        bool shown = true;
        // Bytes `thumb` occupies, cached at insert time so eviction doesn't have to ask a
        // QPixmap whose backing store may already be gone. 0 when thumb is null.
        qint64 thumbBytes = 0;
    };

    // Maps the current row of a "SELECT id, name, fmt, state, thumb_id, size, width,
    // height, taken_at, duration_ms FROM files ..." statement (this exact column
    // list/order is shared by setDirectory()'s bulk query and loadRow()'s single-row
    // query) into a Row.
    static Row rowFromStatement(pixet::Statement &sel);
    // Single-row mirror of setDirectory()'s bulk query, for (dirId_, name) - false if
    // no such files row exists (caller must commit it to the DB first).
    bool loadRow(const QString &name, Row &out) const;
    // Adds (sign=+1) or removes (sign=-1) `row`'s contribution to the five running
    // aggregates below - shared by setDirectory() and the incremental insert/remove
    // paths so they can't drift apart on a future schema addition.
    void accumulate(const Row &row, int sign);
    // Rebuilds rowByFileId_/rowByName_ and shown_/shownRowOf_ from rows_ in one pass.
    // Every insert/remove invalidates every index at or after the mutation point -
    // rebuilding wholesale (cheap: a folder's worth of rows, not the whole library) is
    // simpler and less bug-prone than shifting each index in place.
    void reindexLookups();
    // rows_ index -> the row number the view knows it by, or -1 if the filter hides it.
    int shownRowOf(int rowsIndex) const { return shownRowOf_[rowsIndex]; }
    // The ORDER BY clause (no leading "ORDER BY") for the current sortKey_/
    // sortDescending_ - shared by setDirectory()'s bulk query and (indirectly, via
    // isRowBefore() mirroring the same logic in C++) insertOrUpdateFileByName()'s
    // in-memory insertion point, so the two can't disagree about row order.
    QString orderByClause() const;
    // True if `a` sorts before `b` under the current sortKey_/sortDescending_ - the
    // in-memory equivalent of orderByClause(), used to find an insertion point for a
    // single new row (insertOrUpdateFileByName()) and to reorder rows_ in place
    // (setSortOrder()) without a DB round trip. Must stay logically equivalent to
    // orderByClause()'s SQL, term for term - see setSortOrder()'s doc comment for why
    // that agreement matters.
    bool isRowBefore(const Row &a, const Row &b) const;

    pixet::Database &db_;
    int64_t dirId_ = 0;
    prefs::SortKey sortKey_ = prefs::SortKey::Name;
    bool sortDescending_ = false;
    // Every file in the folder, sorted, whether the filter shows it or not. rowByFileId_,
    // rowByName_ and thumbFifo_ all hold indices into this; none of them ever leaves the
    // class - see the class comment.
    QVector<Row> rows_;
    QHash<qint64, int> rowByFileId_;
    QHash<QString, int> rowByName_;
    NameFilter filter_;
    // shown_[r] is the rows_ index of view row r, in rows_ order; shownRowOf_ is the
    // inverse, with -1 for a hidden row. Both rebuilt by reindexLookups(). Kept even with
    // no filter active (as the identity) so there is one code path, not two.
    QVector<int> shown_;
    QVector<int> shownRowOf_;

    // Decoded thumbnails must not be allowed to accumulate in rows_ indefinitely. One QPixmap
    // per row, held until the folder changes, is fine for a few hundred files and quietly
    // ruinous past that: at a 300px icon on a 2x display each pixmap is ~1.1MB, so scrolling
    // through a 1280-file folder (a real one on this machine) parks ~1.4GB of pixmaps, and a
    // 5000-file folder would simply exhaust memory. The symptom isn't a crash, it's the whole
    // machine slowing down as it starts swapping - which reads exactly like "the app is slow".
    //
    // So the cache is now bounded by total bytes and evicted oldest-first. Eviction only
    // clears the pixmap and the `requested` flag; the row itself stays, so scrolling back up
    // re-requests a decode (~1.5ms, off the UI thread) rather than showing a hole.
    //
    // The budget is deliberately far larger than any screenful - ~230 thumbnails at 300px/2x
    // against ~18 visible - because evicting something still on screen would have the view
    // immediately re-request it, and a cache that thrashes at the working-set size is worse
    // than no cache. Bytes rather than a count, since a 480px icon costs 2.5x a 300px one.
    static constexpr qint64 kThumbCacheBudgetBytes = 256LL * 1024 * 1024;
    qint64 thumbCacheBytes_ = 0;
    // Rows in the order their thumbnails were set. FIFO rather than true LRU on purpose: the
    // access pattern here is scrolling, where insertion order and recency coincide closely
    // enough that tracking real recency would cost a touch on every paint to buy nothing.
    // May contain stale or duplicate entries (a row re-thumbnailed while still cached);
    // evictThumbsIfNeeded() tolerates both by skipping rows that no longer hold a pixmap.
    QList<int> thumbFifo_;
    void evictThumbsIfNeeded();
    int imageCount_ = 0;
    int videoCount_ = 0;
    qint64 totalBytes_ = 0;
    int rawRenderedCount_ = 0;
    int rawPreviewCount_ = 0;
};
