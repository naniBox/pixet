#include "CollisionDialog.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

CollisionDialog::CollisionDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("File Already Exists"));
    applyToAllCheck_ = new QCheckBox(this);
}

CollisionDialog::Choice CollisionDialog::ask(QWidget *parent, const QString &name, const QString &dstDir,
                                              qint64 srcSize, qint64 srcMtime, qint64 dstSize, qint64 dstMtime,
                                              int remaining, bool *applyToAll) {
    CollisionDialog dlg(parent);

    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(QStringLiteral("\"%1\" already exists in %2.").arg(name, dstDir), &dlg));

    auto formatEntry = [](const QString &label, qint64 size, qint64 mtime) {
        QString dateStr = mtime > 0
                               ? QDateTime::fromSecsSinceEpoch(mtime).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                               : QStringLiteral("unknown date");
        return QStringLiteral("%1: %2, %3").arg(label, QLocale().formattedDataSize(size), dateStr);
    };
    layout->addWidget(new QLabel(formatEntry(QStringLiteral("Existing"), dstSize, dstMtime), &dlg));
    layout->addWidget(new QLabel(formatEntry(QStringLiteral("Incoming"), srcSize, srcMtime), &dlg));

    if (remaining > 0) {
        dlg.applyToAllCheck_->setText(QStringLiteral("Apply to all remaining conflicts (%1)").arg(remaining));
        layout->addWidget(dlg.applyToAllCheck_);
    } else {
        dlg.applyToAllCheck_->hide();
    }

    auto *buttons = new QDialogButtonBox(&dlg);
    QPushButton *keepBothBtn = buttons->addButton(QStringLiteral("Keep Both"), QDialogButtonBox::ActionRole);
    QPushButton *skipBtn = buttons->addButton(QStringLiteral("Skip"), QDialogButtonBox::ActionRole);
    QPushButton *replaceBtn = buttons->addButton(QStringLiteral("Replace"), QDialogButtonBox::DestructiveRole);
    QPushButton *cancelBtn = buttons->addButton(QDialogButtonBox::Cancel);
    keepBothBtn->setDefault(true); // the non-destructive, always-safe default action
    layout->addWidget(buttons);

    QObject::connect(keepBothBtn, &QPushButton::clicked, &dlg, [&dlg]() {
        dlg.choice_ = KeepBoth;
        dlg.accept();
    });
    QObject::connect(skipBtn, &QPushButton::clicked, &dlg, [&dlg]() {
        dlg.choice_ = Skip;
        dlg.accept();
    });
    QObject::connect(replaceBtn, &QPushButton::clicked, &dlg, [&dlg]() {
        dlg.choice_ = Replace;
        dlg.accept();
    });
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, [&dlg]() {
        dlg.choice_ = CancelAll;
        dlg.reject();
    });

    dlg.exec();
    // Closing via the window's X or Escape goes through reject() with choice_ still
    // at its CancelAll default - no separate handling needed for that path.
    if (applyToAll) *applyToAll = dlg.applyToAllCheck_->isChecked();
    return dlg.choice_;
}
