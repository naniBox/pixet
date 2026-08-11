#include "StatusBarRow.h"

#include <QFrame>
#include <QResizeEvent>

#include "StatusLabel.h"

namespace {
constexpr int kDividerWidth = 2;
}

StatusBarRow::StatusBarRow(QWidget *parent) : QWidget(parent) {}

StatusLabel *StatusBarRow::addLabel(int nominalWidth) {
    QWidget *divider = nullptr;
    if (!entries_.isEmpty()) {
        divider = new QFrame(this);
        static_cast<QFrame *>(divider)->setFrameShape(QFrame::VLine);
        static_cast<QFrame *>(divider)->setFrameShadow(QFrame::Sunken);
        divider->setFixedWidth(kDividerWidth);
        divider->show();
    }
    auto *label = new StatusLabel(this);
    label->show();
    entries_.append({label, divider, nominalWidth});
    relayout();
    return label;
}

QSize StatusBarRow::sizeHint() const {
    int totalNominal = 0;
    int h = 16;
    for (const Entry &e : entries_) {
        totalNominal += e.nominalWidth;
        if (e.divider) totalNominal += kDividerWidth;
        h = qMax(h, e.label->sizeHint().height());
    }
    return QSize(totalNominal, h);
}

void StatusBarRow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    relayout();
}

void StatusBarRow::relayout() {
    int totalNominal = 0;
    for (const Entry &e : entries_) totalNominal += e.nominalWidth;
    int totalDividers = 0;
    for (const Entry &e : entries_) {
        if (e.divider) totalDividers += kDividerWidth;
    }

    int available = qMax(0, width() - totalDividers);
    // Every cell shrinks by the same proportion when there isn't enough room, rather
    // than any one of them collapsing disproportionately - see the class comment.
    double scale = totalNominal > 0 ? qMin(1.0, (double)available / totalNominal) : 1.0;

    int x = 0;
    for (const Entry &e : entries_) {
        if (e.divider) {
            e.divider->setGeometry(x, 0, kDividerWidth, height());
            x += kDividerWidth;
        }
        int labelWidth = qMax(0, (int)qRound(e.nominalWidth * scale));
        e.label->setGeometry(x, 0, labelWidth, height());
        x += labelWidth;
    }
}
