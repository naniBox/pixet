#pragma once

#include <QDialog>

class QCheckBox;

// One collision at a time, shown before any file operation touches disk (see
// FileOpsWorker's two-stage preflight/execute protocol) - Replace/Skip/Keep Both,
// with an "apply to all remaining" checkbox the caller honors by not asking again.
// Escape/window-close is always CancelAll: this is the app's first destructive
// dialog, and ambiguity here is the expensive kind, so there's no silent-Skip
// fallback for dismissing it.
class CollisionDialog : public QDialog {
    Q_OBJECT

public:
    enum Choice { Replace, Skip, KeepBoth, CancelAll };

    // `name` is the colliding filename, `dstDir` the destination folder it's
    // conflicting in. `srcSize`/`srcMtime` describe the incoming file, `dstSize`/
    // `dstMtime` the one already there (mtime as unix seconds - matches every other
    // date field in this app; see MainWindow::updateSelectionStatus()). `remaining`
    // is how many more conflicts are queued after this one, shown next to the
    // apply-to-all checkbox so its scope is never ambiguous. `*applyToAll` is set to
    // true if the user checked it - the caller then applies the returned Choice to
    // every remaining conflict without asking again.
    static Choice ask(QWidget *parent, const QString &name, const QString &dstDir, qint64 srcSize, qint64 srcMtime,
                       qint64 dstSize, qint64 dstMtime, int remaining, bool *applyToAll);

private:
    explicit CollisionDialog(QWidget *parent);

    QCheckBox *applyToAllCheck_;
    Choice choice_ = CancelAll;
};
