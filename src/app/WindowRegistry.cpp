#include "WindowRegistry.h"

#include <QApplication>
#include <QScreen>

#include "MainWindow.h"

WindowRegistry &WindowRegistry::instance() {
    // Function-local static rather than a global: this is reached during MainWindow's
    // constructor, and a global would be at the mercy of static initialisation order.
    static WindowRegistry registry;
    return registry;
}

MainWindow *WindowRegistry::createWindow(const QString &initialPath) {
    // resetLayout is false for every window but the first (which main() creates directly with
    // whatever --reset-layout said): the flag means "ignore the saved layout for this launch",
    // and a window opened mid-session should inherit the layout the user is already looking at.
    auto *w = new MainWindow(/*resetLayout=*/false);
    w->setAttribute(Qt::WA_DeleteOnClose);

    // Offset from the window that spawned this one, so a new window isn't hidden exactly on
    // top of its parent looking like nothing happened. Cascades down-right and wraps back to
    // the origin before it can walk off the bottom of the screen.
    if (MainWindow *from = lastActive(); from && from != w) {
        const int step = 32;
        QRect avail = from->screen() ? from->screen()->availableGeometry() : QRect();
        QRect g = from->frameGeometry();
        QPoint p = g.topLeft() + QPoint(step, step);
        if (!avail.isNull() && (p.x() + g.width() > avail.right() || p.y() + g.height() > avail.bottom())) {
            p = avail.topLeft() + QPoint(step, step);
        }
        w->move(p);
    }

    w->show();
    // After show(): navigating populates the grid, and doing it while the window is still
    // hidden means the first thumbnail requests are sized against a zero-width viewport.
    if (!initialPath.isEmpty()) w->openSystemPath(initialPath);
    return w;
}

void WindowRegistry::add(MainWindow *w) {
    if (windows_.contains(w)) return;
    windows_.push_back(w);
    activationOrder_.push_back(w);
    emit changed();
}

void WindowRegistry::remove(MainWindow *w) {
    windows_.removeAll(w);
    activationOrder_.removeAll(w);
    emit changed();
}

void WindowRegistry::noteActivated(MainWindow *w) {
    if (!windows_.contains(w)) return;
    // Deliberately does not emit changed(): activation happens on every click into a window,
    // and rebuilding every menu each time would be pure churn. Only the checkmark in the
    // Windows submenu depends on this, and that is refreshed when the menu is shown.
    activationOrder_.removeAll(w);
    activationOrder_.push_back(w);
}

MainWindow *WindowRegistry::lastActive() const {
    return activationOrder_.isEmpty() ? nullptr : activationOrder_.back();
}

void WindowRegistry::titlesChanged() { emit changed(); }
