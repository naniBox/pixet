#pragma once

namespace pixet {

// A process-wide "we are quitting, stop what you're doing" flag.
//
// Exists because Qt's own shutdown has no way to interrupt a worker that is inside a
// blocking call. Every worker in src/app is a QObject moved onto its own QThread and torn
// down with quit() + wait(); quit() only asks the thread's *event loop* to return, which
// does nothing at all while one of its slots is still executing, and wait() has no
// timeout. So a single slow decode holds the whole process open after its window is gone -
// observed with a 5-gigapixel TIFF, where the UI thread sat in ~FolderIndexer's wait()
// while the worker it was waiting for carried on allocating toward 50GB. See
// decode/DecodeLimits.h for the file that caused it.
//
// This is the cooperative half of the fix (the other half is app/ThreadShutdown.h, which
// stops waiting forever). Long-running loops poll it and give up early:
//
//  - generateThumb() returns ThumbTier::Cancelled immediately, so a Pass B wave that has
//    already been dispatched to the thread pool collapses instead of decoding another
//    20 files nobody will ever see. ThreadPool deliberately drains its queue on
//    destruction (see util/ThreadPool.cpp), so without this those tasks all still run.
//  - Indexer::run() breaks out of its directory and wave loops.
//
// Deliberately one-way: nothing clears it. A process that has decided to quit does not
// change its mind, and a resettable flag would only invite a race over whether a worker
// saw the set or the clear.
//
// Not for FileOpsWorker. A half-finished copy or move is not something to abandon on the
// way out - see its destructor, which keeps waiting indefinitely on purpose.
void requestShutdown();

// Safe to call from any thread, and cheap enough for an inner loop (a relaxed atomic
// load). Always false until requestShutdown() is called, so anything linking pixet_core
// without ever calling it - the tests, pixet-index - behaves exactly as it did before.
bool shutdownRequested();

} // namespace pixet
