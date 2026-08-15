#include "SortIcons.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace {

// 64x64 logical canvas, downscaled by QIcon for whatever a QToolButton actually
// requests - see the header comment on why that direction (not upscaling) is safe.
constexpr int kCanvas = 64;
constexpr qreal kStroke = 4.5;

QPen outlinePen(const QColor &color) {
    QPen pen(color);
    pen.setWidthF(kStroke);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

void drawArrowhead(QPainter &p, QPointF tip, bool pointingDown) {
    // Two short strokes forming a "v" (or "^"), matching outlinePen's round joins -
    // drawn as lines rather than a filled triangle so it reads as the same weight of
    // stroke as the rest of the icon, not a heavier solid accent.
    qreal dy = pointingDown ? -8 : 8;
    p.drawLine(tip, tip + QPointF(-7, dy));
    p.drawLine(tip, tip + QPointF(7, dy));
}

void paintName(QPainter &p, const QColor &color) {
    // Material Design's long-standing "sort alphabetically" motif: A above Z (first
    // before last), with a downward arrow reinforcing the top-to-bottom reading
    // order - recognizable at a glance without needing a tooltip.
    QFont font = p.font();
    font.setBold(true);
    font.setPixelSize(22);
    p.setFont(font);
    p.setPen(color);
    p.drawText(QRectF(6, 6, 26, 26), Qt::AlignCenter, QStringLiteral("A"));
    p.drawText(QRectF(6, 32, 26, 26), Qt::AlignCenter, QStringLiteral("Z"));

    p.setPen(outlinePen(color));
    p.drawLine(QPointF(46, 12), QPointF(46, 46));
    drawArrowhead(p, QPointF(46, 52), /*pointingDown=*/true);
}

void paintFileDate(QPainter &p, const QColor &color) {
    // A plain clock face - file modified time is a timestamp, not tied to any
    // specific device, so a generic clock (rather than e.g. a calendar page) is the
    // clearer read at icon size.
    p.setPen(outlinePen(color));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(32, 32), 20, 20);
    p.drawLine(QPointF(32, 32), QPointF(32, 18)); // hour hand, straight up
    p.drawLine(QPointF(32, 32), QPointF(44, 32)); // minute hand, straight right
}

void paintTakenDate(QPainter &p, const QColor &color) {
    // A camera - distinguishes "when the photo/video was actually taken" (EXIF) from
    // "when the file on disk was last modified" (paintFileDate) at a glance, which
    // matters here since the two can disagree (a copied or re-exported file keeps its
    // EXIF date but gets a new mtime).
    p.setPen(outlinePen(color));
    p.setBrush(Qt::NoBrush);
    QPainterPath body;
    body.addRoundedRect(QRectF(9, 22, 46, 30), 6, 6);
    p.drawPath(body);
    p.drawRect(QRectF(24, 14, 16, 8)); // viewfinder bump on top
    p.drawEllipse(QPointF(32, 37), 9, 9); // lens
}

void paintSize(QPainter &p, const QColor &color) {
    // Ascending bars - the standard "sort by size" glyph, filled rather than outlined
    // (the other four are outline-only) since a small filled bar reads more like a
    // magnitude than a thin outlined rectangle would at this size.
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(12, 40, 10, 12), 2, 2);
    p.drawRoundedRect(QRectF(27, 30, 10, 22), 2, 2);
    p.drawRoundedRect(QRectF(42, 18, 10, 34), 2, 2);
}

void paintReverse(QPainter &p, const QColor &color) {
    // A vertical double-headed arrow - direction-agnostic on purpose (it toggles
    // whichever key is active, not a fixed "descending" arrow), matching how the
    // action itself works.
    p.setPen(outlinePen(color));
    p.drawLine(QPointF(32, 14), QPointF(32, 50));
    drawArrowhead(p, QPointF(32, 12), /*pointingDown=*/false);
    drawArrowhead(p, QPointF(32, 52), /*pointingDown=*/true);
}

} // namespace

namespace sorticons {

QIcon make(Kind kind, const QColor &color) {
    QPixmap pixmap(kCanvas, kCanvas);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    switch (kind) {
        case Kind::Name: paintName(p, color); break;
        case Kind::FileDate: paintFileDate(p, color); break;
        case Kind::TakenDate: paintTakenDate(p, color); break;
        case Kind::Size: paintSize(p, color); break;
        case Kind::Reverse: paintReverse(p, color); break;
    }

    return QIcon(pixmap);
}

} // namespace sorticons
