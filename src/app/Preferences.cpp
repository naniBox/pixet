#include "Preferences.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "cache/RawCache.h"
#include "util/AppPaths.h"

namespace {
// -1 = not yet loaded from QSettings. See thumbnailIconSize()'s header comment for
// why this is cached rather than read fresh every call.
std::atomic<int> g_thumbnailIconSize{-1};
} // namespace

namespace prefs {

void applyRawCacheSettings() {
    pixet::rawcache::configure(rawCacheDir().toStdString(), rawCacheMaxBytes(), rawCacheLongEdge(),
                                rawCacheMemoryBytes());
}

QString settingsFilePath() {
    return QString::fromStdString(pixet::appDataDir()) + QStringLiteral("/pixet.ini");
}

QSettings settingsStore() { return QSettings(settingsFilePath(), QSettings::IniFormat); }

QString rawCacheDir() { return QString::fromStdString(pixet::appDataDir()) + QStringLiteral("/rawcache"); }

int rawCacheLongEdge() {
    int v = settingsStore().value(QStringLiteral("rawCacheLongEdge"), kDefaultRawCacheLongEdge).toInt();
    // Clamped rather than snapped to a preset: any sane size works, and refusing to honour
    // a hand-edited ini would be surprising for no gain.
    return std::clamp(v, 640, 8192);
}

void setRawCacheLongEdge(int px) { settingsStore().setValue(QStringLiteral("rawCacheLongEdge"), px); }

qint64 rawCacheMaxBytes() {
    qint64 v = settingsStore().value(QStringLiteral("rawCacheMaxBytes"), (qint64)kDefaultRawCacheMaxBytes).toLongLong();
    return std::max<qint64>(0, v); // 0 is meaningful: cache disabled
}

void setRawCacheMaxBytes(qint64 bytes) {
    settingsStore().setValue(QStringLiteral("rawCacheMaxBytes"), std::max<qint64>(0, bytes));
}

qint64 rawCacheMemoryBytes() {
    qint64 v = settingsStore()
                    .value(QStringLiteral("rawCacheMemoryBytes"), (qint64)kDefaultRawCacheMemoryBytes)
                    .toLongLong();
    return std::max<qint64>(0, v);
}

void setRawCacheMemoryBytes(qint64 bytes) {
    settingsStore().setValue(QStringLiteral("rawCacheMemoryBytes"), std::max<qint64>(0, bytes));
}

bool licenseAccepted() { return settingsStore().value(QStringLiteral("licenseAccepted"), false).toBool(); }

void setLicenseAccepted(bool accepted) { settingsStore().setValue(QStringLiteral("licenseAccepted"), accepted); }

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

bool autoRethumbnail() {
    return settingsStore().value(QStringLiteral("autoRethumbnail"), false).toBool();
}

void setAutoRethumbnail(bool enabled) {
    settingsStore().setValue(QStringLiteral("autoRethumbnail"), enabled);
}

QList<int> thumbnailSizeChoices(int current) {
    QList<int> sizes;
    for (int px : kThumbnailSizePresets) sizes.append(px);
    if (!sizes.contains(current)) {
        sizes.append(current);
        std::sort(sizes.begin(), sizes.end());
    }
    return sizes;
}

int thumbnailImageAreaHeightFor(int iconWidth) {
    return std::max(1, (int)std::lround(iconWidth * kThumbnailTileAspect));
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

int indexerThreadCount() {
    return std::max(0, settingsStore().value(QStringLiteral("indexerThreadCount"), 0).toInt());
}

void setIndexerThreadCount(int count) {
    settingsStore().setValue(QStringLiteral("indexerThreadCount"), std::max(0, count));
}

SortKey gridSortKey() {
    int v = settingsStore().value(QStringLiteral("gridSortKey"), (int)SortKey::Name).toInt();
    if (v < (int)SortKey::Name || v > (int)SortKey::Size) v = (int)SortKey::Name;
    return (SortKey)v;
}

void setGridSortKey(SortKey key) { settingsStore().setValue(QStringLiteral("gridSortKey"), (int)key); }

bool gridSortDescending() { return settingsStore().value(QStringLiteral("gridSortDescending"), false).toBool(); }

void setGridSortDescending(bool descending) {
    settingsStore().setValue(QStringLiteral("gridSortDescending"), descending);
}

} // namespace prefs
