#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileOpenEvent>
#include <QIcon>
#include <QObject>

#include "MainWindow.h"
#include "WindowRegistry.h"
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
    explicit FileOpenForwarder(QObject *parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::FileOpen) {
            auto *fileEvent = static_cast<QFileOpenEvent *>(event);
            QString path = fileEvent->file();
            if (path.isEmpty()) path = fileEvent->url().toLocalFile();
            if (!path.isEmpty()) {
                // Delivered to the window the user was most recently in rather than a
                // remembered one: with several windows open, "open this in pixet" should act
                // on the one in front. If every window has been closed but the process is
                // still alive, open a fresh one rather than dropping the request.
                if (MainWindow *target = WindowRegistry::instance().lastActive()) {
                    target->openSystemPath(path);
                    target->raise();
                    target->activateWindow();
                } else {
                    WindowRegistry::instance().createWindow(path);
                }
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

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
    // on anything being deployed alongside the exe.
    //
    // The PNG, not the .ico. ICO support is libqico, a runtime-discovered plugin, so
    // QIcon(":/pixet.ico") comes back null anywhere the imageformats plugins aren't deployed
    // - and both deploy scripts prune them deliberately, since this app decodes every image
    // itself. PNG is compiled into QtGui proper, so it is the form that holds up everywhere.
    // pixet.ico is still what pixet.rc stamps on the .exe: a native Win32 resource that
    // never goes through Qt.
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/pixet.png")));
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
                                  QStringLiteral("A file or folder to open, navigating to it (a file selects "
                                                 "itself within its folder). Give several to open one window "
                                                 "per path."),
                                  QStringLiteral("[file...]"));
    // process() exits on an unrecognised option. Safe for a Finder launch, which passes no
    // arguments at all (Qt strips the legacy -psn_* argument itself), and file opens come
    // through FileOpenForwarder rather than argv.
    parser.process(app);

    // Heap-allocated and owned by Qt (Qt::WA_DeleteOnClose) rather than a by-value local:
    // a local makes main() the owner of the one and only MainWindow, and nothing else can
    // then create another. WindowRegistry tracks them all instead.
    //
    // Created directly rather than through WindowRegistry::createWindow() for one reason: the
    // --reset-layout flag applies to this launch's first window only, and createWindow() is
    // for windows opened mid-session, which should inherit the layout already on screen.
    auto *window = new MainWindow(parser.isSet(resetLayoutOption));
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();

    // Installed after the window exists but before exec(), so a FileOpen event queued during
    // launch - the usual case when the app is started *by* opening a file - is still
    // delivered once the event loop runs.
    auto *forwarder = new FileOpenForwarder(&app);
    app.installEventFilter(forwarder);

    // Windows (and Linux) pass an "open this file" request as a plain positional argument
    // rather than the Cocoa FileOpen event macOS uses above - e.g. once file association is
    // registered (see scripts/pixet.iss), double-clicking a .jpg launches
    // `pixet.exe "C:\...\photo.jpg"`. Explorer hands over a single path per launch, so in
    // practice there is only ever one. The first lands in the window just created; any
    // others each get their own window, so `pixet folderA folderB` comes up as a
    // side-by-side comparison in one step.
    const QStringList positional = parser.positionalArguments();
    for (int i = 0; i < positional.size(); ++i) {
        if (i == 0) window->openSystemPath(positional.at(i));
        else WindowRegistry::instance().createWindow(positional.at(i));
    }

    return app.exec();
}
