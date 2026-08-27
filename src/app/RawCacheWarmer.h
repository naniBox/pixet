#pragma once

#include <QObject>
#include <QStringList>
#include <QThread>

#include <atomic>

// Pulls the RAW cache entries for whatever the grid currently shows into memory, off the
// UI thread, so opening one of those photos costs nothing.
//
// The disk tier alone already removes the demosaic, but a 2560px entry is still a file
// read plus a JPEG decode - tens of milliseconds, which is the difference between quick
// and instant when someone is arrowing through a folder. Warming turns the first open of
// anything already on screen into a memory copy.
//
// Only promotes entries that already exist on disk (see rawcache::prewarm) - it never
// decodes a RAW that hasn't been decoded before. Scrolling past a folder of un-viewed RAWs
// must not turn into a demosaic of every one of them, which is the obvious way for a
// feature like this to become far worse than the problem it solves.
class RawCacheWarmer : public QObject {
    Q_OBJECT

public:
    explicit RawCacheWarmer(QObject *parent = nullptr);
    ~RawCacheWarmer() override;

    // Call from the UI thread with the absolute paths of the RAW files now on screen, in
    // priority order. Supersedes any previous request rather than queueing behind it: the
    // viewport has moved, so the old list describes somewhere the user no longer is.
    void warm(const QStringList &filePaths);

private slots:
    void doWarm(QStringList filePaths, qint64 generation);

private:
    QThread thread_;
    // Bumped on the UI thread the moment a new list arrives, and checked between files on
    // the worker. A scroll should abandon the rest of the previous screenful immediately -
    // finishing it would mean the useful work waits behind work that stopped mattering.
    std::atomic<qint64> generation_{0};
};
