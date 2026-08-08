#pragma once

#include <QObject>
#include <QThread>

#include <memory>

namespace pixet {
class Database;
}

// GUI-facing wrapper around pixet_core's Indexer, run non-recursively (one folder,
// not a whole tree) off the UI thread every time the user navigates. This is what
// makes "no mandatory pre-index step" real: browsing a folder is what indexes it.
// Reuses the exact same Pass A/B code the pixet-index CLI benchmarked in P1.
class FolderIndexer : public QObject {
    Q_OBJECT

public:
    explicit FolderIndexer(QObject *parent = nullptr);
    ~FolderIndexer() override;

public slots:
    // force=true is the explicit "Refresh" action - bypasses the mtime freshness
    // shortcut and re-verifies every file in the folder.
    void indexFolder(QString path, bool force);

signals:
    void started(QString path);
    void finished(QString path);

private:
    QThread thread_;
    std::unique_ptr<pixet::Database> db_; // created lazily, on the worker thread
};
