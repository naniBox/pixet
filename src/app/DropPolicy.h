#pragma once

#include <QMimeData>
#include <QUrl>

// The two questions every drop target in pixet has to answer, answered in one place
// because both targets - ThumbGridView (drop into the folder currently open) and
// FolderTreeView (drop onto any folder in the tree) - must answer them identically.
//
// The copy-vs-move one in particular is a *policy*, not a detail: Windows' own
// OS-level default for a plain cross-application drag is Copy, which pixet
// deliberately inverts - dragging photos into a library should move them in, leaving
// a duplicate behind being the surprising outcome here rather than the safe one - with
// Ctrl switching to Copy, matching Explorer's own override key for the same drag. Two
// drop targets in the same window disagreeing about that would be worse than either
// choice on its own, and a duplicated three-line predicate is exactly the kind of
// thing that drifts once someone revisits one of them.
namespace droppolicy {

// Is this payload something a pixet drop target can act on at all? Directories are
// deliberately *not* filtered out here - rejecting those is FileOpsWorker::preflight()'s
// job, so there's exactly one place that decision is made (the same reasoning as
// clipops::read()'s).
inline bool hasLocalFileUrl(const QMimeData *mime) {
    if (!mime || !mime->hasUrls()) return false;
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile()) return true;
    }
    return false;
}

// True if this drop should copy rather than move - see the namespace comment for why
// move is the default. Takes the modifiers rather than the event so that a caller
// holding any of QDragEnterEvent/QDragMoveEvent/QDropEvent can ask without this header
// having to know which.
inline bool wantsCopy(Qt::KeyboardModifiers modifiers) { return modifiers & Qt::ControlModifier; }

} // namespace droppolicy
