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
