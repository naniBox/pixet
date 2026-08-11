#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

class QKeyEvent;

// User-configurable keyboard shortcuts, backed by prefs::settingsStore() (see
// Preferences.h). Deliberately covers only the "single action trigger" keys
// (menu commands, fullscreen toggles) - not grid/fullscreen directional navigation
// (arrows, Home/End, PageUp/PageDown, Space, Ctrl+arrow folder nav) or the
// always-on Escape-to-close, which stay fixed as standard conventions nobody
// really wants to remap, and which the "reserved" keys below protect from being
// silently shadowed by a rebind. See PreferencesDialog for the editor UI.
namespace keybindings {

enum class Action {
    ToggleSidePanel,
    Refresh,
    AddBookmark,
    // Grid: opens the current selection in FullscreenViewer (ThumbGridView's
    // activated() signal). FullscreenViewer: closes back to the grid - the same
    // binding does both, a deliberate toggle (see FullscreenViewer::keyPressEvent).
    // Escape always closes too, regardless of this binding, so there's never a way
    // to lock yourself out of the viewer by reassigning this to something odd.
    ActivateFullscreen,
    FullscreenToggleTrueFullscreen,
    FullscreenToggleZoom,
    FullscreenToggleInfoOverlay,
};

struct ActionInfo {
    Action action;
    QString settingsKey; // stored under the "keybindings" group in prefs::settingsStore()
    QString displayName; // shown in PreferencesDialog
    QKeySequence defaultSequence;
};

// Fixed order, used both to build the PreferencesDialog editor and to iterate all
// bindings when validating/applying.
const QList<ActionInfo> &allActions();
const ActionInfo &info(Action action);

// Reads the stored override if present, else the action's defaultSequence. An
// explicitly-empty stored override (user cleared the binding) is preserved as
// empty, not silently replaced by the default.
QKeySequence binding(Action action);
void setBinding(Action action, const QKeySequence &seq);
void resetBinding(Action action); // removes the override; binding() falls back to the default again

// Keys ThumbGridView/FullscreenViewer's own keyPressEvent() always handles itself
// (navigation, Escape) - picking one of these for a configurable action would make
// that action unreachable, so the editor UI blocks it. One shared list rather than
// splitting by which surface each action actually lives in: none of the
// configurable actions have any legitimate reason to want a navigation key anyway,
// so a single superset keeps validation trivial without weakening it.
const QList<QKeySequence> &reservedSequences();

// True if `event` matches `seq` - a plain equality check, except Qt::Key_Enter (the
// numpad Enter key) is treated as equivalent to Qt::Key_Return, matching this
// app's historical behavior of accepting either for "activate." Always false for
// an empty (unbound) sequence.
bool matches(const QKeyEvent *event, const QKeySequence &seq);

} // namespace keybindings
