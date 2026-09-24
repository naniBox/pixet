#include "NameFilterBar.h"

#include <QAction>
#include <QButtonGroup>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QToolButton>

NameFilterBar::NameFilterBar(NameFilter::Mode mode, QWidget *parent) : QWidget(parent) {
    edit_ = new QLineEdit(this);
    edit_->setClearButtonEnabled(true);
    // Escape/Return/Down - see eventFilter().
    edit_->installEventFilter(this);
    connect(edit_, &QLineEdit::textChanged, this, &NameFilterBar::filterChanged);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(edit_, /*stretch=*/1);

    modeGroup_ = new QButtonGroup(this); // exclusive by default
    const struct {
        NameFilter::Mode mode;
        QString label;
        QString description;
    } modes[] = {
        {NameFilter::Mode::Wildcard, QStringLiteral("Wildcard"),
         QStringLiteral("The text must appear exactly. * matches any run of characters, ? any one.\n"
                        "*john_is* finds today_john_is_tall, not today_john_tall_is.\n"
                        "Without * or ?, finds names containing the text.")},
        {NameFilter::Mode::Fuzzy, QStringLiteral("Fuzzy"),
         QStringLiteral("The letters must appear in order, with anything in between.\n"
                        "jhntl finds john_is_tall. Separate words can match in any order.")},
        {NameFilter::Mode::Regex, QStringLiteral("Regex"),
         QStringLiteral("A regular expression, matched anywhere in the name.\n"
                        "Anchor with ^ and $. Case-insensitive unless it starts with (?-i).")},
    };
    for (int i = 0; i < 3; ++i) {
        const QKeySequence shortcut(Qt::CTRL | Qt::Key(Qt::Key_1 + i));
        auto *radio = new QRadioButton(modes[i].label, this);
        // Clicking one must leave the cursor in the text field, where it was.
        radio->setFocusPolicy(Qt::NoFocus);
        // NativeText so macOS shows ⌘1 rather than "Ctrl+1" - Qt's Ctrl *is* Command there.
        radio->setToolTip(QStringLiteral("%1 (%2)\n%3")
                              .arg(modes[i].label, shortcut.toString(QKeySequence::NativeText), modes[i].description));
        modeGroup_->addButton(radio, int(modes[i].mode));
        layout->addWidget(radio);

        // An action on the bar rather than QAbstractButton::setShortcut(), which animates a
        // press and delays the click ~100ms. The class comment has why it only works while
        // the bar is open.
        auto *action = new QAction(this);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, radio, [radio] { radio->setChecked(true); });
        addAction(action);
    }
    modeGroup_->button(int(mode))->setChecked(true);
    updatePlaceholder();
    // Connected after the initial check above, so building the bar doesn't announce a
    // mode change nobody made.
    connect(modeGroup_, &QButtonGroup::idToggled, this, [this](int id, bool checked) {
        if (!checked) return; // the button being unchecked; its replacement reports the change
        updatePlaceholder();
        emit modeChanged(NameFilter::Mode(id));
        emit filterChanged();
    });

    statusLabel_ = new QLabel(this);
    statusLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Room for a typical count up front, so the field doesn't twitch narrower and wider as
    // the number of matches changes digit count while typing.
    statusLabel_->setMinimumWidth(statusLabel_->fontMetrics().horizontalAdvance(QStringLiteral("00000 of 00000")));
    layout->addWidget(statusLabel_);

    auto *closeButton = new QToolButton(this);
    closeButton->setText(QStringLiteral("×"));
    closeButton->setAutoRaise(true);
    closeButton->setFocusPolicy(Qt::NoFocus);
    closeButton->setToolTip(QStringLiteral("Close the filter and show every file (Esc)"));
    connect(closeButton, &QToolButton::clicked, this, &NameFilterBar::closeRequested);
    layout->addWidget(closeButton);
}

NameFilter::Mode NameFilterBar::mode() const { return NameFilter::Mode(modeGroup_->checkedId()); }

NameFilter NameFilterBar::filter() const { return NameFilter(mode(), edit_->text()); }

void NameFilterBar::activate() {
    show();
    edit_->setFocus(Qt::ShortcutFocusReason);
    edit_->selectAll();
}

void NameFilterBar::clearText() {
    QSignalBlocker blocker(edit_);
    edit_->clear();
}

void NameFilterBar::setMatchCount(int shown, int total) {
    shownCount_ = shown;
    totalCount_ = total;
    updateStatusLabel();
}

void NameFilterBar::setError(const QString &message) {
    error_ = message;
    updateStatusLabel();
}

bool NameFilterBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == edit_ && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::KeypadModifier) {
            switch (key->key()) {
                case Qt::Key_Escape:
                    emit closeRequested();
                    return true;
                case Qt::Key_Return:
                case Qt::Key_Enter:
                case Qt::Key_Down:
                    emit focusGridRequested();
                    return true;
                default:
                    break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void NameFilterBar::updatePlaceholder() {
    switch (mode()) {
        case NameFilter::Mode::Wildcard:
            edit_->setPlaceholderText(QStringLiteral("Filter by name"));
            break;
        case NameFilter::Mode::Fuzzy:
            edit_->setPlaceholderText(QStringLiteral("Filter by name, letters in order"));
            break;
        case NameFilter::Mode::Regex:
            edit_->setPlaceholderText(QStringLiteral("Filter by name, regular expression, e.g. ^IMG_\\d+"));
            break;
    }
}

void NameFilterBar::updateStatusLabel() {
    if (!error_.isEmpty()) {
        statusLabel_->setText(QStringLiteral("Invalid: %1").arg(error_));
        // The same red as the thumbnail freshness dot, which reads on both themes.
        statusLabel_->setStyleSheet(QStringLiteral("color: #f85149;"));
        return;
    }
    statusLabel_->setStyleSheet(QString());
    statusLabel_->setText(shownCount_ >= 0 && totalCount_ >= 0
                              ? QStringLiteral("%1 of %2").arg(shownCount_).arg(totalCount_)
                              : QString());
}
