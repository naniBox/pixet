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
    // Was implicitly floored elsewhere when this pane was forced square (see git
    // history) - now that it's a plain QSplitter child the user can drag to any
    // height, this is what stops a drag-to-the-edge from squeezing it to nothing.
    setMinimumHeight(60);
}

int PreviewPane::preferredTargetLongEdge() const {
    int longEdge = std::max(width(), height());
    return std::clamp(longEdge, 256, 2000);
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
    label_->setPixmap(original_.scaled(label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
