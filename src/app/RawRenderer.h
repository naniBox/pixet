#pragma once

#include <QObject>
#include <QThread>

#include <memory>

class QTimer;

namespace pixet {
class Database;
}

// Continuously, at idle/low priority, finds directories that still have at least one
// RAW file sitting on its fast embedded-preview thumbnail
// (pixet::FileState::DoneNeedsRender) and upgrades it to a full demosaic render of the
// actual sensor data - the GUI-automatic equivalent of running `pixet-index
// --render-raws` by hand, so a RAW-heavy library gradually catches up to full quality
// just by having the app open rather than only when the CLI pass is run. Thin wrapper
// around Indexer (IndexOptions::renderRaws) run one directory at a time - same shape
// as BackgroundReconciler, reusing its claim coordination and DB-update logic rather
// than reimplementing any of it, but queries for "any directory with pending render
// work" directly instead of rotating through every known directory - a RAW file
// becoming DoneNeedsRender doesn't have to wait for an unrelated drift-detection sweep
// to reach that directory in its own (much slower, whole-library) rotation.
//
// MainWindow calls prioritize() on every folder navigation, so whatever the user is
// actually looking at right now jumps ahead of any unrelated backlog elsewhere in the
// library instead of waiting its turn.
class RawRenderer : public QObject {
    Q_OBJECT

public:
    // One instance for the whole application - same reasoning as
    // BackgroundReconciler::shared(): this hunts the entire library for RAW files still on a
    // fast embedded preview, so it is not per-window work, and a full demosaic render is far
    // too expensive to be running N of concurrently.
    static RawRenderer &shared();

    explicit RawRenderer(QObject *parent = nullptr);
    ~RawRenderer() override;

    // Kicks off the loop on the worker thread. Call once, after the main window is up.
    void start();

public slots:
    // Prioritizes `path` for the next (and every subsequent, until it runs dry) render
    // check - called whenever the user navigates to a folder, so a RAW file needing a
    // render there doesn't sit behind whatever else happens to be earlier in the
    // whole-library backlog. Also wakes the loop immediately rather than waiting out
    // however much of the current idle/pacing delay is left, so navigating to a
    // RAW-heavy folder starts rendering right away instead of up to a minute later.
    // Falls back to the normal "any directory with pending work" query once the
    // priority directory itself has nothing left to do.
    void prioritize(QString path);

signals:
    // A directory just had at least one RAW file upgraded to a full render - lets
    // MainWindow refresh the grid (and the rendered/preview status counts) if that's
    // the directory currently on screen.
    void directoryChanged(QString path);

private slots:
    void renderNext();

private:
    QThread thread_;
    std::unique_ptr<pixet::Database> db_;
    QTimer *timer_ = nullptr;
    QString priorityDir_;
};
