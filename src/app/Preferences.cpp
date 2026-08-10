#include "Preferences.h"

#include <QSettings>

#include <algorithm>
#include <atomic>

namespace {
QSettings settings() { return QSettings(QStringLiteral("pixet"), QStringLiteral("pixet")); }

// -1 = not yet loaded from QSettings. See thumbnailIconSize()'s header comment for
// why this is cached rather than read fresh every call.
std::atomic<int> g_thumbnailIconSize{-1};
} // namespace

namespace prefs {

int thumbnailIconSize() {
    int cached = g_thumbnailIconSize.load(std::memory_order_relaxed);
    if (cached >= 0) return cached;

    int loaded = settings()
                     .value(QStringLiteral("thumbnailIconSize"), kDefaultThumbnailIconSize)
                     .toInt();
    loaded = std::clamp(loaded, kMinThumbnailIconSize, kMaxThumbnailIconSize);
    g_thumbnailIconSize.store(loaded, std::memory_order_relaxed);
    return loaded;
}

void setThumbnailIconSize(int px) {
    px = std::clamp(px, kMinThumbnailIconSize, kMaxThumbnailIconSize);
    g_thumbnailIconSize.store(px, std::memory_order_relaxed);
    settings().setValue(QStringLiteral("thumbnailIconSize"), px);
}

int thumbnailTargetLongEdge() {
    // 2x the display size is enough headroom for a HiDPI screen without generating
    // needlessly huge stored blobs; qMax keeps today's 320px baseline even if the
    // user picks a small on-screen size.
    return std::max(320, thumbnailIconSize() * 2);
}

bool useSystemVideoPlayer() {
    return settings().value(QStringLiteral("useSystemVideoPlayer"), true).toBool();
}

void setUseSystemVideoPlayer(bool useSystem) {
    settings().setValue(QStringLiteral("useSystemVideoPlayer"), useSystem);
}

QString customVideoPlayerPath() {
    return settings().value(QStringLiteral("customVideoPlayerPath")).toString();
}

void setCustomVideoPlayerPath(const QString &path) {
    settings().setValue(QStringLiteral("customVideoPlayerPath"), path);
}

} // namespace prefs
