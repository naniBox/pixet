#pragma once

#include <QList>
#include <QObject>
#include <QString>

class MainWindow;

// Tracks every open MainWindow so the app can have more than one.
//
// pixet was single-window because main() owned the one MainWindow by value. On Windows a
// second window is just a second process - double-click the exe again - but macOS routes a
// second launch back to the running instance (`open -a` reuses it, and so does the Dock and
// Finder), so there is no user-reachable way to get a second window without the app growing
// one itself. Hence File > New Window and the View > Windows list.
//
// Also the place windows learn about each other: every window's View > Windows submenu is
// rebuilt from changed(), so opening, closing or navigating any window updates the list in
// all of them.
class WindowRegistry : public QObject {
    Q_OBJECT

public:
    static WindowRegistry &instance();

    // Creates, shows and returns a new window. `initialPath` is the folder it should open on;
    // empty means "whatever the persisted lastDirectory says", which is what the first window
    // at launch wants. Windows are heap-allocated and delete themselves on close (see
    // MainWindow's Qt::WA_DeleteOnClose), so the caller does not own the result and must not
    // hold it past a close.
    MainWindow *createWindow(const QString &initialPath = QString());

    // Called by MainWindow's constructor/destructor - not something callers need to do.
    void add(MainWindow *w);
    void remove(MainWindow *w);

    // Called when a window is activated, so lastActive() can answer "the window the user was
    // most recently looking at" rather than "whichever happens to be first in the list".
    void noteActivated(MainWindow *w);

    const QList<MainWindow *> &windows() const { return windows_; }
    int count() const { return (int)windows_.size(); }

    // The most recently activated window that is still open, or nullptr if none are. Used for
    // two things: routing a macOS FileOpen event to the window the user is actually using, and
    // deciding which window's geometry gets persisted - see MainWindow::saveWindowState().
    MainWindow *lastActive() const;

    // A window's folder (and so its title) changed. Separate entry point from add/remove
    // purely for readability at the call site; both end up emitting changed().
    void titlesChanged();

signals:
    // A window opened, closed, or was renamed. Every window listens and rebuilds its
    // View > Windows submenu.
    void changed();

private:
    WindowRegistry() = default;

    // Creation order, which is the order the View > Windows list shows - deliberately stable
    // rather than sorted by folder or recency, so an entry doesn't move out from under the
    // cursor while the menu is open.
    QList<MainWindow *> windows_;
    // Same set, ordered least- to most-recently-activated. Kept separate from windows_ so the
    // menu order can stay stable while lastActive() still reflects real recency.
    QList<MainWindow *> activationOrder_;
};
