#pragma once

#include <QColor>
#include <QIcon>

// Small flat/outline icons for the grid's sort controls (View > Sort By, and the
// button bar next to the path bar - see MainWindow) - drawn with QPainter rather than
// shipped as SVG/PNG assets, so there's no new resource file, no Qt6Svg dependency to
// deploy (icons.qrc currently holds only the app's own .ico), and no separate
// light/dark variants to maintain: make() takes the color to draw with, so a caller
// passing the current palette's text color gets an icon that's already correct for
// whichever theme is active.
namespace sorticons {

enum class Kind { Name, FileDate, TakenDate, Size, Reverse };

// Renders at a fixed, deliberately-larger-than-toolbar-size canvas and lets QIcon
// scale down for whatever size a QToolButton actually requests - downscaling a clean
// vector-drawn source looks sharp at any DPI; the reverse (upscaling) is what would
// look soft, and toolbar icons are never requested anywhere near this large.
QIcon make(Kind kind, const QColor &color);

} // namespace sorticons
