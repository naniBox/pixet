#include "ShellOps.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace shellops {

QString revealActionLabel() {
#ifdef Q_OS_MACOS
    return QStringLiteral("Open in Finder");
#else
    return QStringLiteral("Open in Explorer");
#endif
}

void revealInFileManager(const QString &path) {
    if (path.isEmpty()) return;

    const QFileInfo info(path);
    if (!info.exists()) return;

    // A directory just gets opened. QDesktopServices is enough here and is what the About
    // box already uses for the settings folder, so both routes behave the same way.
    if (info.isDir()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
        return;
    }

    const QString native = QDir::toNativeSeparators(info.absoluteFilePath());

#if defined(Q_OS_WIN)
    // /select, is one token, comma included and no space after it - Explorer parses the
    // path as part of the same argument, and splitting them makes it silently open the
    // user's Documents folder instead. Passed through the argument list rather than a
    // command string so Qt handles quoting for paths with spaces.
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + native});
#elif defined(Q_OS_MACOS)
    // -R reveals the file in its folder with the file selected, rather than trying to open
    // it in whatever application claims the type - which is what a bare `open` would do,
    // and the opposite of what this menu entry promises.
    QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), native});
#else
    // No portable "select this file" exists, so fall back to opening the folder that
    // contains it. Nothing else in this app builds here today, but silently doing nothing
    // would be a worse default than being one step less precise.
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
#endif
}

} // namespace shellops
