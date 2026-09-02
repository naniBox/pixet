#pragma once

class QThread;

// Stopping a worker thread without the possibility of hanging the process on it.
//
// Every worker in this directory follows the same shape: a QObject moved onto its own
// QThread, with a destructor that does `thread_.quit(); thread_.wait();`. That is the
// documented Qt idiom and it is fine right up until a slot takes a long time, because
// neither half of it can interrupt one:
//
//   - quit() asks the thread's *event loop* to return. While a slot is executing there is
//     no event loop to ask, so it does nothing whatsoever.
//   - wait() has no timeout, so the calling thread - the UI thread, since these
//     destructors run from ~MainWindow - blocks until that slot finishes on its own.
//
// The observed failure was exactly this: navigating to a folder containing a 5-gigapixel
// TIFF put FolderIndexer's worker inside a single decode that wanted ~50GB, and closing
// the window left the UI thread parked in ~FolderIndexer's wait() while the worker it was
// waiting for carried on allocating. The window was gone, the process was not, and the
// only way out was Task Manager. See decode/DecodeLimits.h for the file, and
// util/Shutdown.h for the cooperative side of the fix.
//
// So: ask nicely, wait a bounded time, and if the worker still will not stop *and the
// application is on its way out anyway*, end the process rather than leave it pinned. That
// last condition matters - see the comment on the grace period below.
namespace threadshutdown {

// How long a worker gets to notice the quit and return. Generous on purpose: this is not a
// latency budget, it is the point at which we conclude the thread is never coming back.
// With the decode limits in place no single unit of work should come close, so reaching
// this means something genuinely unbounded is happening and taking the process down is the
// better of the two remaining options.
constexpr int kGraceMs = 8000;

// Quits `thread`'s event loop and waits up to `graceMs` for it to actually finish.
//
// If it finishes, this returns normally and the caller destroys its members as usual.
//
// If it doesn't, what happens next depends on whether the whole application is quitting
// (pixet::shutdownRequested(), set by MainWindow when the last window closes):
//
//   - Not quitting. This is one window of several closing while its worker is busy, and
//     the other windows are still perfectly usable - so this falls back to waiting
//     indefinitely, which is exactly what the code did before. Killing a live application
//     because one of its windows was slow to close would be a far worse bug than the one
//     this file exists to fix.
//   - Quitting. There is nothing left to protect and the alternative is the hang above, so
//     the process is terminated immediately with _exit(). Everything that needed persisting
//     already has been: settings are written in MainWindow::closeEvent, and the indexer
//     commits each Pass B batch as it goes, so at most one batch of thumbnails is lost and
//     those files simply stay state=New for the next scan. SQLite's WAL makes an
//     interrupted transaction a rollback, not damage.
//
// `name` appears in the warning logged before a forced exit, so the log says which worker
// refused to stop rather than just that one did.
//
// Deliberately not used by FileOpsWorker: a half-finished copy or move is worth waiting
// for however long it takes, and its destructor keeps the plain unbounded wait on purpose.
void stopWorker(QThread &thread, const char *name, int graceMs = kGraceMs);

} // namespace threadshutdown
