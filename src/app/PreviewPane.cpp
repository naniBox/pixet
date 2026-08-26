#include "PreviewPane.h"

#include <QLabel>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>

PreviewPane::PreviewPane(QWidget *parent) : QWidget(parent) {
    label_ = new QLabel(this);
    label_->setAlignment(Qt::AlignCenter);
    label_->setText(QStringLiteral("No selection"));
    label_->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(label_);

    setMinimumWidth(160);
    // This pane is a plain QSplitter child, so its height is whatever the user drags it to.
    // This floor is what stops a drag to the very edge from squeezing it away to nothing.
    setMinimumHeight(60);
}

int PreviewPane::preferredTargetLongEdge() const {
    // Device pixels, not logical points. This pane deliberately keeps the undecoded-resolution
    // pixmap around so a resize rescales in place instead of re-decoding - but decoding to the
    // logical size in the first place means a Retina screen only ever gets half the pixels it
    // can show, and no amount of rescaling recovers them. The clamp ceiling doubles with it,
    // since 2000 was a limit on decoded pixels rather than on-screen points.
    int longEdge = qRound(std::max(width(), height()) * devicePixelRatioF());
    return std::clamp(longEdge, 256, 4000);
}

void PreviewPane::setImage(const QImage &image) {
    if (image.isNull()) {
        clear();
        return;
    }
    original_ = QPixmap::fromImage(image);
    updateScaledPixmap();
}

void PreviewPane::clear() {
    original_ = QPixmap();
    label_->setPixmap(QPixmap());
    label_->setText(QStringLiteral("No preview"));
}

void PreviewPane::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    updateScaledPixmap();
}

void PreviewPane::updateScaledPixmap() {
    if (original_.isNull()) return;
    label_->setText(QString());
    // Scale to the label's size in *device* pixels and then stamp the ratio, rather than
    // scaling to its logical size: the latter throws away half the resolution on a Retina
    // display before the compositor ever sees it. Setting the ratio is what keeps QLabel
    // laying the pixmap out at the intended on-screen size.
    const qreal dpr = devicePixelRatioF();
    QSize target(qRound(label_->width() * dpr), qRound(label_->height() * dpr));
    QPixmap scaled = original_.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    label_->setPixmap(scaled);
}
