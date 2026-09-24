#pragma once

#include <QWidget>

#include "NameFilter.h"

class QButtonGroup;
class QLabel;
class QLineEdit;

// The strip above the thumbnail grid for filtering it by file name: a text field, the three
// matching modes as radio buttons (see NameFilter for what each one means), the match
// count, and a close button. Hidden until the filter shortcut (Ctrl+Shift+F by default,
// see keybindings::Action::FilterByName) opens it.
//
// Being open *is* filter mode. Ctrl+1/2/3 switch mode for as long as the bar is visible,
// wherever focus is in the window - not only while the text field has it - so after Return
// hands focus to the grid to browse the matches, the mode can still be changed without
// clicking back into the field. Those three are QActions with the default WindowShortcut
// context, and Qt only honours an action's shortcut while a widget it belongs to is
// visible, so hiding the bar is exactly what turns them off again. Fixed rather than
// configurable, and reserved against the configurable ones (see
// keybindings::reservedSequences()), since a clash would silently disable both.
//
// Holds no filtering logic and no reference to the grid: it only reports what the user
// asked for (filterChanged(), closeRequested(), focusGridRequested()) and MainWindow does
// the rest - see MainWindow::applyNameFilter().
class NameFilterBar : public QWidget {
    Q_OBJECT

public:
    explicit NameFilterBar(NameFilter::Mode mode, QWidget *parent = nullptr);

    NameFilter::Mode mode() const;
    // What the field and mode currently describe - possibly invalid (an unfinished regex),
    // which is the caller's to handle.
    NameFilter filter() const;

    // Shows the bar if it's hidden, and puts the cursor in the field with its text
    // selected, so typing replaces whatever was there.
    void activate();
    // Empties the field without emitting filterChanged(). For closing, where MainWindow
    // hides the bar and clears the grid's filter itself.
    void clearText();

    // The "12 of 340" to the right of the field. -1 for either means there's nothing to
    // report (no active filter) and the label goes blank.
    void setMatchCount(int shown, int total);
    // Shown in place of the match count until cleared with an empty string - used for a
    // regex that doesn't compile, while the grid keeps the last valid filter's results.
    void setError(const QString &message);

signals:
    // The text or the mode changed - re-read filter().
    void filterChanged();
    // Separately from filterChanged(), so the choice can be remembered without writing
    // settings on every keystroke.
    void modeChanged(NameFilter::Mode mode);
    // Escape in the field, or the close button.
    void closeRequested();
    // Return, Enter or Down in the field: done typing, go browse the matches.
    void focusGridRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updatePlaceholder();
    void updateStatusLabel();

    QLineEdit *edit_;
    QButtonGroup *modeGroup_;
    QLabel *statusLabel_;
    int shownCount_ = -1;
    int totalCount_ = -1;
    QString error_;
};
