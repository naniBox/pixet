#pragma once

#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QThread>
#include <QVector>

#include <memory>

namespace pixet {
class Database;
}

// Decodes cached thumbnail blobs off the UI thread. Requests are a LIFO stack
// (deduped by file id) so the most recently requested cell - typically wherever the
// user just scrolled to - is served first, per the plan's "distance from viewport,
// newest wins" intent. No explicit cancellation of off-screen requests in P2: the
// grid only re-requests currently-painted cells, so stale entries just stop being
// re-issued and drain naturally; revisit if scrolling perf ever demands it.
class ThumbLoader : public QObject {
    Q_OBJECT

public:
    // Stored thumbnails are up to 320px (P1's target); the grid displays them much
    // smaller, so pixmaps are scaled down to fit this box (aspect-preserved) before
    // ever reaching the model - otherwise an oversized decoration bleeds into
    // neighboring grid cells. MainWindow's grid iconSize is derived from this too, so
    // there's one source of truth for how big a grid cell actually is.
    static constexpr int kThumbIconSize = 150;

    explicit ThumbLoader(QObject *parent = nullptr);
    ~ThumbLoader() override;

public slots:
    void request(qint64 fileId, qint64 thumbId);

signals:
    void thumbReady(qint64 fileId, QPixmap pixmap);

private slots:
    void processOne();

private:
    struct Req {
        qint64 fileId;
        qint64 thumbId;
    };

    QThread thread_;
    std::unique_ptr<pixet::Database> db_; // created lazily, on the worker thread
    QVector<Req> stack_;
    QSet<qint64> pending_;
    bool processing_ = false;
};
