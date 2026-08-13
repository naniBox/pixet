#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileOpenEvent>
#include <QIcon>
#include <QObject>

#include "MainWindow.h"
#include "version.h"

namespace {

// macOS never passes an "open this file with pixet" request through argv - not from Finder's
// Open With, not from a file dropped on the Dock icon, not from `open -a pixet shot.jpg`. It
// arrives as a QEvent::FileOpen on the application object, which is why QCommandLineParser
// can't see it and why this forwarder has to exist.
//
// It's a hard requirement of the CFBundleDocumentTypes entries in Info.plist rather than a
// nice-to-have: declaring those types puts pixet in the Open With menu, and without this the
// app would launch and then appear to ignore whatever the user picked.
class FileOpenForwarder : public QObject {
public:
    explicit FileOpenForwarder(MainWindow *window, QObject *parent = nullptr)
        : QObject(parent), window_(window) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::FileOpen) {
            auto *fileEvent = static_cast<QFileOpenEvent *>(event);
            QString path = fileEvent->file();
            if (path.isEmpty()) path = fileEvent->url().toLocalFile();
            if (!path.isEmpty()) {
                window_->openSystemPath(path);
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    MainWindow *window_;
};

} // namespace

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // Drives the macOS application-menu title, the Dock label, the standard About panel and
    // QCommandLineParser's --help header. All of this was unset, which left the app menu
    // showing whatever the executable happened to be named.
    //
    // Deliberately does NOT affect where settings live: prefs::settingsStore() is an
    // explicit-path QSettings::IniFormat store rather than a NativeFormat one keyed off
    // these names (see Preferences.h), so adding them can't silently relocate anyone's
    // existing pixet.ini.
    QApplication::setApplicationName(QStringLiteral("pixet"));
    QApplication::setApplicationDisplayName(QStringLiteral("pixet"));
    QApplication::setApplicationVersion(QString::fromLatin1(pixet::version()));
    QApplication::setOrganizationName(QStringLiteral("naniBox"));
    QApplication::setOrganizationDomain(QStringLiteral("nanibox.com"));

#ifdef Q_OS_WIN
    // pixet.rc embeds pixet.ico as the .exe's own file icon (what Explorer shows),
    // but Windows does NOT also use that as the *running* window's icon (title bar,
    // taskbar, Alt+Tab) - Qt needs an explicit setWindowIcon() call for that. Loaded
    // from a Qt resource (icons.qrc) rather than a loose path so it doesn't depend
    // on anything being deployed alongside the exe. Qt's ICO/BMP support is built
    // directly into QtGui, not a runtime-discovered plugin, so this doesn't
    // reintroduce the image-plugin dependency the macOS branch below avoids.
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/pixet.ico")));
#endif
    // No further setWindowIcon() call on macOS. The Dock/Finder icon comes from the
    // bundle's CFBundleIconFile (src/app/pixet.icns), and loading the same icon again
    // through Qt would mean either a .qrc holding a duplicate copy or relying on Qt's ICNS
    // image plugin - and this app deliberately avoids depending on Qt's image plugins at
    // runtime (see QtInterop.h).

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("pixet photo/video viewer"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption resetLayoutOption(
        QStringLiteral("reset-layout"),
        QStringLiteral("Ignore saved window position/size and pane layout for this launch, "
                        "and save fresh defaults back on exit."));
    parser.addOption(resetLayoutOption);
    // Optional, not required for positionalArguments() to work below - only affects
    // --help's generated usage line (adds a "[file]" hint instead of just "[options]").
    parser.addPositionalArgument(QStringLiteral("file"),
                                  QStringLiteral("A file to open, navigating to its folder and selecting it "
                                                 "(what a double-click via a registered file association sends)."),
                                  QStringLiteral("[file]"));
    // process() exits on an unrecognised option. Safe for a Finder launch, which passes no
    // arguments at all (Qt strips the legacy -psn_* argument itself), and file opens come
    // through FileOpenForwarder rather than argv.
    parser.process(app);

    MainWindow window(parser.isSet(resetLayoutOption));
    window.show();

    // Installed after the window exists but before exec(), so a FileOpen event queued during
    // launch - the usual case when the app is started *by* opening a file - is still
    // delivered once the event loop runs.
    auto *forwarder = new FileOpenForwarder(&window, &app);
    app.installEventFilter(forwarder);

    // Windows (and Linux) pass an "open this file" request as a plain positional
    // argument rather than the Cocoa FileOpen event macOS uses above - e.g. once file
    // association is registered (see scripts/pixet.iss), double-clicking a .jpg
    // launches `pixet.exe "C:\...\photo.jpg"`. Only ever one positional argument in
    // practice - Explorer hands over a single path per launch - so the rest are
    // ignored rather than treated as an error.
    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty()) window.openSystemPath(positional.first());

    return app.exec();
}
