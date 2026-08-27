#include "LicenseDialog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "Preferences.h"
#include "version.h"

namespace license {
namespace {

QString licenseText() {
    // Compiled into the binary via icons.qrc rather than read from a file sitting beside the
    // app. A macOS bundle is dragged to /Applications as a single unit, so a loose LICENSE
    // file next to it would not travel with it, and the one thing this dialog must never do
    // is come up empty.
    QFile file(QStringLiteral(":/LICENSE"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(file.readAll());
}

} // namespace

bool ensureAccepted() {
    if (prefs::licenseAccepted()) return true;

    const QString text = licenseText();
    if (text.isEmpty()) {
        // The resource is missing or unreadable, which is a packaging fault rather than
        // anything the user did. Refusing to start would turn a build mistake into an
        // unusable app, and presenting an empty agreement to click through would be worse
        // than either - so start, and leave acceptance unrecorded so a fixed build asks
        // properly.
        return true;
    }

    QDialog dlg;
    dlg.setWindowTitle(QStringLiteral("pixet %1 - License").arg(QString::fromLatin1(pixet::version())));
    dlg.setModal(true);

    auto *layout = new QVBoxLayout(&dlg);

    auto *intro = new QLabel(QStringLiteral("pixet is released under the Apache License 2.0. You need to "
                                             "accept it to use the application."),
                              &dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *view = new QPlainTextEdit(text, &dlg);
    view->setReadOnly(true);
    // The license is hard-wrapped plain text carrying its own indentation and column
    // alignment, so it needs a fixed-width font and no re-wrapping on top of its own to stay
    // readable - proportional text with word wrap turns the section headings into mush.
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(view, /*stretch=*/1);

    auto *buttons = new QDialogButtonBox(&dlg);
    QPushButton *acceptButton = buttons->addButton(QStringLiteral("Accept"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QStringLiteral("Cancel"), QDialogButtonBox::RejectRole);
    // Explicitly not the default button. Agreeing to a license should take a deliberate click
    // on the word "Accept", not be what happens when someone hits Return to dismiss a dialog
    // they weren't expecting.
    acceptButton->setAutoDefault(false);
    acceptButton->setDefault(false);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    // Big enough that the license reads as a document to scroll rather than a keyhole, while
    // still fitting a small laptop screen.
    dlg.resize(760, 620);

    // Escape and the window's close button both route to QDialog::reject(), which is exactly
    // the outcome they should have here: anything other than an explicit Accept is a decline,
    // and nothing gets recorded.
    if (dlg.exec() != QDialog::Accepted) return false;

    prefs::setLicenseAccepted(true);
    return true;
}

} // namespace license
