#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QIcon>
#include <QObject>
#include <QPointer>

#include "FolderListModel.h"
#include "FullscreenViewer.h"
#include "LicenseDialog.h"
#include "MainWindow.h"
#include "PathQ.h"
#include "Preferences.h"
#include "WindowRegistry.h"
#include "db/Schema.h"
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

    // Standalone viewer mode (see openStandaloneViewer()): a second "open this file"
    // request while the viewer is up should show it in that same viewer, not escalate to a
    // full window - which is what would otherwise happen, since there is no MainWindow for
    // lastActive() to return and the fallback below builds one.
    void setStandaloneViewer(FullscreenViewer *viewer, FolderListModel *model) {
        viewer_ = viewer;
        model_ = model;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::FileOpen) {
            auto *fileEvent = static_cast<QFileOpenEvent *>(event);
            QString path = fileEvent->file();
            if (path.isEmpty()) path = fileEvent->url().toLocalFile();
            if (!path.isEmpty()) {
                if (viewer_ && model_ && viewer_->isVisible()) {
                    // Re-lists whichever folder the new file is in, so this handles a file
                    // from a different folder as readily as a sibling of the current one.
                    int row = model_->setFile(path);
                    if (row >= 0) {
                        viewer_->openAt(model_, model_->directoryPath(), row);
                        viewer_->raise();
                        viewer_->activateWindow();
                        return true;
                    }
                    // Unreadable folder, or a type this viewer can't show. Fall through to
                    // the full app, which has somewhere to put it either way.
                }
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

private:
    // QPointer, not a raw pointer: the viewer is WA_DeleteOnClose and outlives this filter
    // only until Escape or an escalation to the folder view closes it. After that a raw
    // pointer would dangle, and the isVisible() guard above would be reading freed memory
    // rather than protecting against it - reachable in the escalation case, where the app
    // keeps running with a MainWindow after the viewer is gone. QPointer nulls itself.
    // The model is parented to the viewer and so dies with it, hence the same treatment.
    QPointer<FullscreenViewer> viewer_; // not owned; null outside standalone viewer mode
    QPointer<FolderListModel> model_;   // not owned
};

// Whether `path` is a single still image, i.e. whether the standalone viewer can handle it
// at all. A folder, a missing path, or a .txt all answer no and get the full app, which is
// the right home for each: a folder is what it browses, and a path that doesn't resolve
// deserves the main window's own error handling rather than a black fullscreen widget with
// nothing in it.
//
// Video is excluded even though every video format decodes here, because what it decodes to
// is one poster frame - FullscreenViewer has no playback engine (see its class comment), and
// MainWindow::onGridItemActivated() consequently never opens the viewer for a video at all,
// launching the user's player instead. Answering yes here would make double-clicking a video
// show a single frozen frame, which is worse than either of the two things the app already
// does with one. Videos fall through to the browser, unchanged from before this mode existed
// - and scripts/pixet.iss deliberately doesn't register video types anyway.
bool isViewableFile(const QString &path) {
    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) return false;
    const pixet::Format fmt = pixet::classifyFormat(info.fileName().toStdString());
    return fmt != pixet::Format::Unknown && pixet::kindForFormat(fmt) == pixet::Kind::Image;
}

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
    QCommandLineOption browseOption(
        QStringLiteral("browse"),
        QStringLiteral("Open the full browser window even for a single image file, instead of "
                        "the standalone fullscreen viewer."));
    parser.addOption(browseOption);
    // Optional, not required for positionalArguments() to work below - only affects
    // --help's generated usage line (adds a "[file]" hint instead of just "[options]").
    parser.addPositionalArgument(QStringLiteral("file"),
                                  QStringLiteral("One image file opens straight into the fullscreen viewer "
                                                 "(Enter for the folder it is in, Escape to quit); use --browse "
                                                 "for the full window instead. A folder - or several paths - "
                                                 "opens the browser, one window per path."),
                                  QStringLiteral("[file...]"));
    // process() exits on an unrecognised option. Safe for a Finder launch, which passes no
    // arguments at all (Qt strips the legacy -psn_* argument itself), and file opens come
    // through FileOpenForwarder rather than argv.
    parser.process(app);

#ifdef Q_OS_MACOS
    // macOS ships as a DMG the user drags to /Applications, so there is no installer to put
    // the license in front of anyone - first run is the only opportunity. Windows users have
    // already accepted it during setup (scripts/pixet.iss's LicenseFile), which is why this
    // is platform-gated rather than universal: asking again there would be a second prompt
    // for an agreement already made.
    //
    // Placed after parser.process() so --help and --version still answer without a licence
    // prompt, and before the first window so that declining leaves nothing behind - no
    // window, no database connection, no indexing thread.
    if (!license::ensureAccepted()) return 0;
#endif

    // Installed before any window exists but, being on the application object, still catches
    // a FileOpen event queued during launch - the usual case when the app is started *by*
    // opening a file - once the event loop runs.
    auto *forwarder = new FileOpenForwarder(&app);
    app.installEventFilter(forwarder);

    // Windows (and Linux) pass an "open this file" request as a plain positional argument
    // rather than the Cocoa FileOpen event macOS uses above - e.g. once file association is
    // registered (see scripts/pixet.iss), double-clicking a .jpg launches
    // `pixet.exe "C:\...\photo.jpg"`. Explorer hands over a single path per launch, so in
    // practice there is only ever one.
    const QStringList positional = parser.positionalArguments();

    // Exactly one path, and it names an image: skip the browser entirely and put the picture
    // on screen. This is the standalone viewer mode - no MainWindow, and so none of what a
    // MainWindow brings with it (two SQLite files opened and migrated, a QFileSystemModel
    // over the whole filesystem, the folder tree and bookmarks, the indexer/thumbnail/
    // reconciler threads, then a folder scan and a screenful of thumbnail decodes) - just a
    // directory listing and one decode. Which is the point of the mode: looking at one photo
    // should not cost what browsing a library costs.
    //
    // Gated on there being a single path because the multi-path form below is explicitly an
    // "open these side by side" request, and on the path being a file rather than a folder,
    // which is a request to browse by definition. --browse forces the old behaviour for a
    // single image; anything not viewable falls through to the browser too, so a mistyped
    // path still lands somewhere that can report it.
    const bool viewerMode = !parser.isSet(browseOption) && positional.size() == 1 &&
                            isViewableFile(positional.at(0));
    if (viewerMode) {
        // pixet_core has no access to prefs, so nothing else would configure the decoded-RAW
        // cache in this mode - and a RAW opened here would re-run a multi-second demosaic
        // whose result the browser had already cached. MainWindow's constructor is what
        // normally does this.
        prefs::applyRawCacheSettings();

        // The model is parented to the viewer so the two die together: it must outlive every
        // in-flight decode request, all of which read their paths back out of it.
        auto *viewer = new FullscreenViewer();
        auto *model = new FolderListModel(viewer);
        int row = model->setFile(positional.at(0));
        if (row >= 0) {
            viewer->setAttribute(Qt::WA_DeleteOnClose);
            viewer->setStandalone(true);
            // No indexer here to have measured native pixel sizes, so the viewer teaches the
            // model instead, out of the full-resolution decode it was going to run anyway.
            // Without this WidthRole/HeightRole stay 0 and zoom never becomes available.
            QObject::connect(viewer, &FullscreenViewer::nativeSizeDiscovered, model,
                              &FolderListModel::setNativeSize);
            // Enter: hand the current file to a real MainWindow, which navigates to its
            // folder and selects it - so the folder view opens on exactly what was on screen.
            //
            // The window is shown *before* the viewer closes, deliberately: with
            // quitOnLastWindowClosed (the default) still in force, closing the only window
            // first would post a quit before the replacement ever appeared.
            QObject::connect(viewer, &FullscreenViewer::browseRequested, &app, [viewer, model](int r) {
                const QString name = r >= 0 ? model->data(model->index(r), Qt::DisplayRole).toString()
                                            : QString();
                auto *window = new MainWindow(/*resetLayout=*/false);
                window->setAttribute(Qt::WA_DeleteOnClose);
                window->show();
                window->openSystemPath(name.isEmpty() ? model->directoryPath()
                                                      : joinPathQ(model->directoryPath(), name));
                viewer->close();
            });
            forwarder->setStandaloneViewer(viewer, model);
            viewer->openAt(model, model->directoryPath(), row);
            return app.exec();
        }
        // The file exists and has a known extension, but its folder would not list (removed
        // media, a permission not granted yet). Falls through to the browser, which has the
        // UI to say so.
        delete viewer;
    }

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

    // The first path lands in the window just created; any others each get their own window,
    // so `pixet folderA folderB` comes up as a side-by-side comparison in one step.
    for (int i = 0; i < positional.size(); ++i) {
        if (i == 0) window->openSystemPath(positional.at(i));
        else WindowRegistry::instance().createWindow(positional.at(i));
    }

    return app.exec();
}
