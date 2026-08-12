#include "ClipboardOps.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QUrl>

#ifdef Q_OS_WIN
#include <QtEndian>
#endif

namespace clipops {

namespace {

#ifdef Q_OS_WIN
// Explorer decides cut-vs-copy from the registered clipboard format
// CFSTR_PREFERREDDROPEFFECT ("Preferred DropEffect"), a 4-byte DROPEFFECT DWORD. Qt
// exposes arbitrary native clipboard/OLE formats through its built-in "any-mime"
// converter under this mime-type spelling, in *both* directions - so no raw Win32
// clipboard code is needed here.
//
// A raw OpenClipboard/SetClipboardData shim alongside Qt's own setMimeData() is NOT a
// viable alternative: SetClipboardData requires the caller to already be the
// clipboard owner, which is only established by EmptyClipboard() - i.e. by
// destroying the data Qt just put there. QWindowsMimeConverter (public since Qt 6.5)
// is the documented fallback if this mime-type route ever stops reaching Explorer,
// but it would only be reimplementing what the any-mime converter already does.
const QString kPreferredDropEffect = QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\"");
constexpr quint32 kDropEffectCopy = 1; // DROPEFFECT_COPY
constexpr quint32 kDropEffectMove = 2; // DROPEFFECT_MOVE

QByteArray dropEffectBytes(bool cut) {
    QByteArray bytes(4, '\0');
    qToLittleEndian<quint32>(cut ? kDropEffectMove : kDropEffectCopy, reinterpret_cast<uchar *>(bytes.data()));
    return bytes;
}
#else
// macOS has no Finder "Cut" to interoperate with - Finder's move-on-paste is
// Cmd+Opt+V applied to an ordinary *copy* pasteboard, not a distinct cut state. A
// private marker type, read back only by pixet's own paste, is the honest answer
// here rather than a compromise. Best-effort from the Windows machine this was
// built on - VERIFY ON THE MAC that a custom mime type survives an NSPasteboard
// round trip within the app (see devlog's P5 precedent for this class of deferred
// platform verification).
const QString kPixetCutMarker = QStringLiteral("application/x-pixet-cut");
#endif

} // namespace

void writeFiles(const QStringList &absolutePaths, bool cut) {
    QList<QUrl> urls;
    urls.reserve(absolutePaths.size());
    for (const QString &path : absolutePaths) urls << QUrl::fromLocalFile(path);

    auto *mime = new QMimeData; // QClipboard takes ownership
    mime->setUrls(urls);
    // Plain-text fallback so pasting into a text editor/terminal gives the paths -
    // costs nothing and is what every file manager does.
    mime->setText(absolutePaths.join(QLatin1Char('\n')));

#ifdef Q_OS_WIN
    mime->setData(kPreferredDropEffect, dropEffectBytes(cut));
#else
    if (cut) mime->setData(kPixetCutMarker, QByteArrayLiteral("1"));
#endif

    QGuiApplication::clipboard()->setMimeData(mime);
}

Files read() {
    Files result;
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) return result;

    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile()) result.paths << url.toLocalFile();
    }
    if (result.paths.isEmpty()) return result;

#ifdef Q_OS_WIN
    // Explorer's own Ctrl+X sets this too, so a copy-then-cut sequence between
    // pixet and Explorer reads consistently either way.
    if (mime->hasFormat(kPreferredDropEffect)) {
        QByteArray bytes = mime->data(kPreferredDropEffect);
        if (bytes.size() >= 4) {
            quint32 effect = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()));
            result.isCut = (effect == kDropEffectMove);
        }
    }
#else
    result.isCut = mime->hasFormat(kPixetCutMarker);
#endif

    return result;
}

bool hasFiles() {
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) return false;
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile()) return true;
    }
    return false;
}

void clearAfterCutPaste() {
    // Real Explorer just clears the clipboard after a cut-paste rather than the
    // fully-formal CFSTR_PERFORMEDDROPEFFECT round trip - simpler, and achieves the
    // same practical outcome (no accidental double-move on a second Ctrl+V).
    QGuiApplication::clipboard()->clear();
}

void markPreferMove(QMimeData *mime) {
#ifdef Q_OS_WIN
    mime->setData(kPreferredDropEffect, dropEffectBytes(/*cut=*/true));
#else
    Q_UNUSED(mime);
#endif
}

} // namespace clipops
