#pragma once

#include <QLabel>

// A status bar cell that re-elides its own text to whatever width it's *actually*
// given, both when the text changes and when the widget itself is resized - see
// MainWindow's status bar construction for why this exists: a QHBoxLayout asked to
// fit Qt::Fixed-policy children into less total space than they need doesn't shrink
// them (that's what Fixed means), so it has no way to pack them without overlap -
// confirmed live via a debug dump showing each label correctly reporting its full
// nominal width while the *next* one's x-position started well inside it. Letting
// labels actually shrink (QWidget::setMaximumWidth(), not setFixedWidth()) fixes
// the overlap, but a plain QLabel doesn't clip/elide its text to a shrunk width on
// its own - it just paints over whatever's next to it - so each cell needs to
// re-elide itself to its current size instead of whatever size it was given text at.
class StatusLabel : public QLabel {
    Q_OBJECT

public:
    explicit StatusLabel(QWidget *parent = nullptr);

    // Stores `text` as the full (unelided) string - shown as a tooltip - and
    // displays it elided to this label's *current* width. mode matters mainly for
    // filenames (ElideMiddle keeps the extension visible); everything else defaults
    // to ElideRight.
    void setStatusText(const QString &text, Qt::TextElideMode mode = Qt::ElideRight);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    QString fullText_;
    Qt::TextElideMode elideMode_ = Qt::ElideRight;

    void reElide();
};
