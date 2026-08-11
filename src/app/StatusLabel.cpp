#include "StatusLabel.h"

#include <QResizeEvent>

StatusLabel::StatusLabel(QWidget *parent) : QLabel(parent) {}

void StatusLabel::setStatusText(const QString &text, Qt::TextElideMode mode) {
    fullText_ = text;
    elideMode_ = mode;
    setToolTip(text.isEmpty() ? QString() : text);
    reElide();
}

void StatusLabel::resizeEvent(QResizeEvent *event) {
    QLabel::resizeEvent(event);
    reElide();
}

void StatusLabel::reElide() { QLabel::setText(fontMetrics().elidedText(fullText_, elideMode_, width())); }
