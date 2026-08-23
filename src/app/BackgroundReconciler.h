#pragma once

#include <QObject>
#include <QThread>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QTimer;

namespace pixet {
class Database;
}

// Continuously walks every directory pixet already knows about (the `dirs` table) at
// idle/low priority, re-running the same mtime/size diff Pass A + Pass B a manual
// Refresh does on one folder - so a file changed outside pixet (edited in place, same
// name, so a directory's own mtime doesn't move and the on-demand indexer's cheap
// freshness shortcut would never notice) eventually gets its thumbnail/metadata
// corrected automatically, without the user needing to notice and hit Refresh
// themselves. Deliberately slow: runs at the lowest OS thread priority and pauses
// between directories so a sweep - however large the library - never meaningfully
// competes with interactive browsing or a concurrent pixet-index run (coordinated via
// the same directory claims table Indexer already uses, under a distinct owner id so
// it never contends with the on-demand FolderIndexer either).
class BackgroundReconciler : public QObject {
    Q_OBJECT

public:
    // One instance for the whole application, shared by every window.
    //
    // This is library-wide hygiene work, not per-window work: it rotates over every directory
    // pixet has ever indexed, independent of what any window is showing. Creating one per
    // window (which is what happened before multi-window, since MainWindow owned it) would
    // mean N full sweeps of the library, N worker threads and N database connections all
    // grinding over the same rows. The claims table would keep them from corrupting each
    // other's work, but they would still be doing it N times.
    //
    // Every window connects to the same instance's directoryChanged() and filters it against
    // its own current folder, which is exactly what each window already did with its private
    // one - so sharing needed no change on the receiving side.
    static BackgroundReconciler &shared();

    explicit BackgroundReconciler(QObject *parent = nullptr);
    ~BackgroundReconciler() override;

    // Kicks off the sweep loop on the worker thread. Call once, after the main window
    // is up, so the first sweep doesn't compete with startup.
    void start();

public slots:
    // "Re-index Known Folders" (Preferences dialog) - reloads the directory list and
    // jumps the queue so the next sweep starts right away instead of waiting out
    // however much of the current per-directory pacing or full-cycle rest period is
    // left. Same forceRescan-only behavior as every other sweep pass (see the class
    // comment) - deliberately not a forceRethumbnail, per the button's own scope.
    void triggerFullSweepNow();

signals:
    // A directory the sweep just revisited actually had something change (new/removed/
    // re-thumbnailed files) - lets MainWindow refresh the grid if that directory
    // happens to be the one currently on screen. Not fired for directories found
    // already up to date, which is the common case once the library's been swept once.
    void directoryChanged(QString path);

private slots:
    void beginLoop();
    void sweepNext();

private:
    QThread thread_;
    std::unique_ptr<pixet::Database> db_;
    QTimer *timer_ = nullptr;

    std::vector<std::pair<int64_t, std::string>> pending_; // dirs left in the current cycle
    size_t cursor_ = 0;

    void loadDirList();

};
