#pragma once

#include <QStringList>

class QMimeData;

// Real OS clipboard interop for Cut/Copy/Paste of files - not a pixet-internal-only
// clipboard, so a Ctrl+C in pixet actually pastes files in Explorer/Finder, and
// vice versa.
namespace clipops {

// Writes `absolutePaths` to the OS clipboard as real file URLs (QMimeData::setUrls(),
// which Qt maps to CF_HDROP on Windows and public.file-url on macOS), plus the
// platform's cut-vs-copy hint so a subsequent Explorer/Finder paste does the right
// thing. `cut` performs no I/O itself - see MainWindow::onEditCut(): the actual move
// happens only when something is later pasted, exactly like Explorer's own Ctrl+X.
void writeFiles(const QStringList &absolutePaths, bool cut);

struct Files {
    QStringList paths; // local files/folders only - non-local URLs are dropped here
    bool isCut = false; // best-effort: false wherever the platform gives no hint
};

// Reads whatever's currently on the clipboard, filtered to local paths (directories
// included - rejecting those is FileOpsWorker::preflight()'s job, same as for a
// drag-in drop, so there's exactly one place that decision is made). Works equally
// for pixet's own Cut/Copy and for files copied from Explorer/Finder itself.
Files read();
bool hasFiles();

// Clears the clipboard after a successful cut-paste - matches what Explorer itself
// does, so a second Ctrl+V doesn't try to move files that already moved once.
void clearAfterCutPaste();

// Stamps `mime` with the platform's "prefer a move over a copy" hint for
// Explorer/Finder - Windows only (the same CFSTR_PREFERREDDROPEFFECT mechanism
// writeFiles() uses for Cut); a no-op elsewhere, since a macOS drag target reads
// Qt::MoveAction/CopyAction directly rather than a separate hint. Shared by
// writeFiles() and MainWindow's drag-out QDrag construction (see
// MainWindow::onDragOutRequested()), both of which need Explorer to treat the
// transfer as a move.
void markPreferMove(QMimeData *mime);

} // namespace clipops
