#include "Preferences.h"

#include <algorithm>
#include <atomic>

#include "util/AppPaths.h"

namespace {
// -1 = not yet loaded from QSettings. See thumbnailIconSize()'s header comment for
// why this is cached rather than read fresh every call.
std::atomic<int> g_thumbnailIconSize{-1};
} // namespace

namespace prefs {

QSettings settingsStore() {
    QString path = QString::fromStdString(pixet::appDataDir()) + QStringLiteral("/pixet.ini");
    return QSettings(path, QSettings::IniFormat);
}

int thumbnailIconSize() {
    int cached = g_thumbnailIconSize.load(std::memory_order_relaxed);
    if (cached >= 0) return cached;

    int loaded = settingsStore()
                     .value(QStringLiteral("thumbnailIconSize"), kDefaultThumbnailIconSize)
                     .toInt();
    loaded = std::clamp(loaded, kMinThumbnailIconSize, kMaxThumbnailIconSize);
    g_thumbnailIconSize.store(loaded, std::memory_order_relaxed);
    return loaded;
}

void setThumbnailIconSize(int px) {
    px = std::clamp(px, kMinThumbnailIconSize, kMaxThumbnailIconSize);
    g_thumbnailIconSize.store(px, std::memory_order_relaxed);
    settingsStore().setValue(QStringLiteral("thumbnailIconSize"), px);
}

int thumbnailTargetLongEdge() {
    // 2x the display size is enough headroom for a HiDPI screen without generating
    // needlessly huge stored blobs; qMax keeps today's 320px baseline even if the
    // user picks a small on-screen size.
    return std::max(320, thumbnailIconSize() * 2);
}

bool useSystemVideoPlayer() {
    return settingsStore().value(QStringLiteral("useSystemVideoPlayer"), true).toBool();
}

void setUseSystemVideoPlayer(bool useSystem) {
    settingsStore().setValue(QStringLiteral("useSystemVideoPlayer"), useSystem);
}

QString customVideoPlayerPath() {
    return settingsStore().value(QStringLiteral("customVideoPlayerPath")).toString();
}

void setCustomVideoPlayerPath(const QString &path) {
    settingsStore().setValue(QStringLiteral("customVideoPlayerPath"), path);
}

QStringList pathHistory() { return settingsStore().value(QStringLiteral("pathHistory")).toStringList(); }

void addToPathHistory(const QString &dirPath) {
    if (dirPath.isEmpty()) return;
    QStringList history = pathHistory();
    history.removeAll(dirPath); // dedupe - revisiting moves it to the front instead of duplicating
    history.prepend(dirPath);
    while (history.size() > kMaxPathHistory) history.removeLast();
    settingsStore().setValue(QStringLiteral("pathHistory"), history);
}

void clearPathHistory() { settingsStore().remove(QStringLiteral("pathHistory")); }

bool hoverInfoEnabled() { return settingsStore().value(QStringLiteral("hoverInfoEnabled"), true).toBool(); }

void setHoverInfoEnabled(bool enabled) { settingsStore().setValue(QStringLiteral("hoverInfoEnabled"), enabled); }

} // namespace prefs
