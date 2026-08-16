#include "ThumbLoader.h"

#include <QMetaObject>

#include "Preferences.h"
#include "QtInterop.h"
#include "db/Database.h"
#include "decode/JpegCodec.h"
#include "util/AppPaths.h"
#include "util/Profile.h"

namespace {

// One decode: reads the thumbnail blob and decodes+scales it to a display-ready
// QImage. Runs on one of ThumbLoader::pool_'s worker threads - never on ThumbLoader's
// own thread_, and never touches stack_/pending_/inFlight_ (those stay confined to
// thread_ - see ThumbLoader::onDecodeFinished()).
//
// Returns a QImage, not a QPixmap, and deliberately so: an earlier version of this
// function returned QPixmap directly, built via QPixmap::fromImage()/scaled() right
// here on the pool worker thread - and crashed for real (reproduced live: debug CRT
// heap-corruption abort, ucrtbased.dll, STATUS_BREAKPOINT) within seconds of
// navigating to a real folder once more than one worker could be decoding at once.
// QPixmap's Windows backend isn't safe to construct/scale concurrently from multiple
// threads, even though a single non-GUI thread doing it serially (thread_, in
// onDecodeFinished() - or the old one-QThread-only design this replaced) is fine and
// was proven so for this app's entire history before this rewrite. QImage has no such
// restriction - each thread's instance here is independent, never shared while being
// built - so the actual QPixmap construction now happens only in onDecodeFinished(),
// back on thread_, where it's safe.
//
// db/stmt are thread_local rather than passed in or held as ThumbLoader members: each
// of pool_'s worker threads is reused across many requests over its lifetime (unlike
// the old one-QThread design, where a single member sufficed), and Database is
// explicitly documented as one-per-thread, not shareable. Lazily created on first use
// per thread, then kept alive - a pool worker thread makes its own connection exactly
// once and reuses it (and its one prepared statement) for every decode it ever
// handles, rather than re-preparing the same SQL on every single call.
QImage decodeThumb(qint64 thumbId, int deviceW, int deviceH) {
    thread_local std::unique_ptr<pixet::Database> db;
    if (!db) db = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), true);
    thread_local pixet::Statement stmt = db->prepare("SELECT bytes FROM thumbs.thumbs WHERE id=?");

    // Copy the blob out and reset immediately - before decoding, and before returning.
    //
    // This statement is thread_local, so it outlives the call, and a stepped statement that
    // hasn't been reset is an OPEN READ TRANSACTION holding a SHARED lock on thumbs.db. The
    // old code reset at the *start* of the next call instead of the end of this one, which
    // meant every idle ThumbLoader pool thread sat on a SHARED lock indefinitely once the user
    // had browsed a folder with thumbnails.
    //
    // thumbs.db is in WAL mode now, which makes that harmless (readers never block writers),
    // but it was not, and the consequence was severe: a writer's COMMIT has to promote
    // RESERVED -> EXCLUSIVE on a rollback-journal database, EXCLUSIVE cannot be taken while
    // any SHARED lock exists, so every commit touching thumbs.db waited out the 10s
    // busy_timeout and then failed with "database is locked" at COMMIT. That killed the whole
    // application via an unguarded QThread slot - see BackgroundReconciler::sweepNext(). Both
    // that lock and the journal mode are fixed; this ordering is the actual bug.
    //
    // Resetting before the decode rather than after also matters on its own terms: decodeJpeg
    // plus the rescale below is the expensive part, and there is no reason to hold a read
    // transaction open across it. columnBlob() returns an owning copy, so nothing here still
    // points into SQLite's memory after the reset.
    std::vector<uint8_t> bytes;
    {
        PIXET_PROF_SCOPE("thumb.blobRead");
        stmt.reset();
        stmt.bind(1, thumbId);
        if (stmt.step()) bytes = stmt.columnBlob(0);
        stmt.reset();
    }
    PIXET_PROF_COUNT("thumb.blobBytes", bytes.size());

    QImage image;
    if (!bytes.empty()) {
        pixet::RgbImage img;
        bool ok;
        {
            PIXET_PROF_SCOPE("thumb.jpegDecode");
            ok = pixet::decodeJpeg(bytes.data(), bytes.size(), qMax(deviceW, deviceH), img);
        }
        if (ok) {
            PIXET_PROF_SCOPE("thumb.toQImage+scale");
            image = rgbImageToQImage(img);
            // decodeJpeg only lands *close* to the target via coarse DCT scale steps -
            // the grid needs an exact fit or oversized decorations bleed into
            // neighboring cells.
            if (image.width() > deviceW || image.height() > deviceH) {
                PIXET_PROF_COUNT("thumb.rescaled", 1);
                image = image.scaled(deviceW, deviceH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
    }
    return image;
}

} // namespace

ThumbLoader::ThumbLoader(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start();
}

ThumbLoader::~ThumbLoader() {
    thread_.quit();
    thread_.wait();
}

void ThumbLoader::setDevicePixelRatio(qreal ratio) {
    devicePixelRatio_.store(ratio > 0.0 ? ratio : 1.0, std::memory_order_relaxed);
}

void ThumbLoader::request(qint64 fileId, qint64 thumbId) {
    if (thumbId == 0 || pending_.contains(fileId)) return;
    pending_.insert(fileId);
    stack_.push_back({fileId, thumbId});
    dispatchNext();
}

void ThumbLoader::dispatchNext() {
    while (inFlight_ < (int)kMaxConcurrentDecodes && !stack_.isEmpty()) {
        Req req = stack_.takeLast(); // LIFO - most recently requested goes first
        ++inFlight_;

        const qreal dpr = devicePixelRatio_.load(std::memory_order_relaxed);
        // Device pixels, not logical points. prefs::thumbnailIconSize() is the on-screen
        // (logical) cell size, so on a Retina display the grid can show twice that many real
        // pixels - and prefs::thumbnailTargetLongEdge() already stores blobs at 2x the icon
        // size precisely so this headroom exists to be used.
        const int iconSize = prefs::thumbnailIconSize();
        // The cell's image area is landscape-shaped, not square (see
        // prefs::kThumbnailTileAspect), so the thumbnail has to be fitted to a W x H box
        // rather than to a single edge - otherwise a portrait shot decodes to the full
        // width-sized square and overflows the shorter area it's drawn into.
        const int deviceW = qMax(1, qRound(iconSize * dpr));
        const int deviceH = qMax(1, qRound(prefs::thumbnailImageAreaHeightFor(iconSize) * dpr));

        qint64 fileId = req.fileId;
        qint64 thumbId = req.thumbId;
        PIXET_PROF_COUNT("thumb.dispatched", 1);
        pool_.submit([this, fileId, thumbId, dpr, deviceW, deviceH]() {
            PIXET_PROF_SCOPE("thumb.decodeTotal");
            QImage image = decodeThumb(thumbId, deviceW, deviceH);
            // Hops back onto thread_ - the pool worker thread must never touch
            // stack_/pending_/inFlight_ directly (see the class comment), and must
            // never build a QPixmap itself (see decodeThumb()'s comment on the crash
            // that taught us that).
            QMetaObject::invokeMethod(
                this, [this, fileId, image, dpr]() { onDecodeFinished(fileId, image, dpr); }, Qt::QueuedConnection);
        });
    }
}

void ThumbLoader::onDecodeFinished(qint64 fileId, QImage image, qreal dpr) {
    PIXET_PROF_SCOPE("thumb.onDecodeFinished");
    --inFlight_;
    pending_.remove(fileId);
    QPixmap pixmap;
    if (!image.isNull()) {
        PIXET_PROF_SCOPE("thumb.QPixmap::fromImage");
        pixmap = QPixmap::fromImage(image);
        // Stamping the ratio is what keeps the grid's geometry in logical units: the
        // pixmap is now physically 2x on Retina, and ThumbGridView centres it using
        // deviceIndependentSize() so the cell layout is unchanged.
        pixmap.setDevicePixelRatio(dpr);
    }
    emit thumbReady(fileId, pixmap);
    dispatchNext(); // the slot this decode just freed may have more work waiting
}
