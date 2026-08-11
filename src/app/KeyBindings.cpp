#include "KeyBindings.h"

#include <QKeyEvent>
#include <QSettings>

#include "Preferences.h"

namespace keybindings {

namespace {

const QString kGroup = QStringLiteral("keybindings");

QList<ActionInfo> buildActionList() {
    return {
        {Action::ToggleSidePanel, QStringLiteral("toggleSidePanel"), QStringLiteral("Toggle side panel"),
         QKeySequence(Qt::Key_T)},
        // F5 is the right default on Windows and a poor one on a Mac keyboard, where the
        // function row defaults to hardware controls and F5 needs Fn (or is dictation).
        // Cmd+R is the platform convention for "refresh" - and note "Ctrl+R" is how that is
        // spelled portably: Qt maps Qt::ControlModifier to Command on macOS, so this
        // *renders and behaves* as Cmd+R there. Only the default differs; stored overrides
        // are portable text either way, so no migration is involved.
        {Action::Refresh, QStringLiteral("refresh"), QStringLiteral("Refresh"),
#ifdef Q_OS_MACOS
         QKeySequence(QStringLiteral("Ctrl+R"))},
#else
         QKeySequence(QStringLiteral("F5"))},
#endif
        {Action::AddBookmark, QStringLiteral("addBookmark"), QStringLiteral("Add current folder to bookmarks"),
         QKeySequence(QStringLiteral("Ctrl+D"))},
        {Action::ActivateFullscreen, QStringLiteral("activateFullscreen"),
         QStringLiteral("Open / close fullscreen viewer"), QKeySequence(Qt::Key_Return)},
        {Action::FullscreenToggleTrueFullscreen, QStringLiteral("fullscreenTrueFullscreen"),
         QStringLiteral("Fullscreen: toggle true fullscreen"), QKeySequence(Qt::Key_F)},
        {Action::FullscreenToggleZoom, QStringLiteral("fullscreenZoom"), QStringLiteral("Fullscreen: toggle zoom"),
         QKeySequence(Qt::Key_Z)},
        {Action::FullscreenToggleInfoOverlay, QStringLiteral("fullscreenInfoOverlay"),
         QStringLiteral("Fullscreen: toggle info overlay"), QKeySequence(Qt::Key_I)},
    };
}

QList<QKeySequence> buildReservedList() {
    return {
        QKeySequence(Qt::Key_Left),
        QKeySequence(Qt::Key_Right),
        QKeySequence(Qt::Key_Up),
        QKeySequence(Qt::Key_Down),
        QKeySequence(Qt::Key_Home),
        QKeySequence(Qt::Key_End),
        QKeySequence(Qt::Key_PageUp),
        QKeySequence(Qt::Key_PageDown),
        QKeySequence(Qt::Key_Space),
        QKeySequence(Qt::Key_Escape),
        QKeySequence(QStringLiteral("Ctrl+Left")),
        QKeySequence(QStringLiteral("Ctrl+Right")),
        QKeySequence(QStringLiteral("Ctrl+Up")),
        QKeySequence(QStringLiteral("Ctrl+Down")),
    };
}

} // namespace

const QList<ActionInfo> &allActions() {
    static const QList<ActionInfo> actions = buildActionList();
    return actions;
}

const ActionInfo &info(Action action) {
    for (const ActionInfo &a : allActions()) {
        if (a.action == action) return a;
    }
    Q_UNREACHABLE();
}

QKeySequence binding(Action action) {
    const ActionInfo &a = info(action);
    QSettings s = prefs::settingsStore();
    s.beginGroup(kGroup);
    if (!s.contains(a.settingsKey)) return a.defaultSequence;
    return QKeySequence::fromString(s.value(a.settingsKey).toString());
}

void setBinding(Action action, const QKeySequence &seq) {
    const ActionInfo &a = info(action);
    QSettings s = prefs::settingsStore();
    s.beginGroup(kGroup);
    s.setValue(a.settingsKey, seq.toString());
}

void resetBinding(Action action) {
    const ActionInfo &a = info(action);
    QSettings s = prefs::settingsStore();
    s.beginGroup(kGroup);
    s.remove(a.settingsKey);
}

const QList<QKeySequence> &reservedSequences() {
    static const QList<QKeySequence> reserved = buildReservedList();
    return reserved;
}

bool matches(const QKeyEvent *event, const QKeySequence &seq) {
    if (seq.isEmpty()) return false;
    int key = event->key();
    if (key == Qt::Key_Enter) key = Qt::Key_Return; // numpad Enter == Return, historical behavior
    Qt::KeyboardModifiers mods =
        event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    QKeySequence pressed(key | mods);
    return pressed == seq;
}

} // namespace keybindings
