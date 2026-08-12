#pragma once

#include <QObject>
#include <QString>
#include <QThread>

#include "meta/JpegExif.h"

// Shared with MainWindow's right-click context menu info display, which needs the
// same "cached + EXIF" text but can't go through the async worker below - a context
// menu has to be fully built before QMenu::exec() shows it, so there's no "fill it in
// once the worker replies" option without closing and reopening the menu.
namespace hoverinfo {

// One human-readable line per fact, only for facts the file actually recorded - see
// ExifDetails' own doc comment on its hasXxx flags for why absent tags are omitted
// rather than shown as "unknown" or a placeholder.
QString formatExifDetails(const pixet::ExifDetails &details);

// Bounded-prefix read + parse, synchronous. This is the exact same work
// HoverInfoWorker::request does off-thread for a passive mouse hover - but a
// right-click is a single, deliberate, explicitly-requested action (like Explorer's
// own "Properties"), so paying a few milliseconds of blocking I/O for it inline is an
// acceptable tradeoff that doesn't justify routing through the worker thread too.
pixet::ExifDetails readExifDetailsSync(const QString &path, int format);

} // namespace hoverinfo

// Off-UI-thread fetch of a file's EXIF details for the grid's hover tooltip (see
// ThumbGridView) - same parentless-QObject-on-its-own-QThread pattern as every other
// background worker in this app (FolderIndexer, ThumbLoader, ...), since even a
// bounded-prefix file read (see util/FileIO::readFilePrefix) is still I/O that
// shouldn't block the UI thread on every mouse hover, especially over a network
// share or a drive that's gone to sleep.
class HoverInfoWorker : public QObject {
    Q_OBJECT

public:
    explicit HoverInfoWorker(QObject *parent = nullptr);
    ~HoverInfoWorker() override;

public slots:
    // `format` is a pixet::Format int value (see db/Schema.h) - only Jpeg actually
    // has EXIF parsing today (see meta/JpegExif.h); every other format replies with
    // an empty details string, which the caller simply doesn't append anything for.
    void request(quint64 id, QString path, int format);

signals:
    void ready(quint64 id, QString detailsText);

private:
    QThread thread_;
};
