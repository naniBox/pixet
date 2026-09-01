#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>

#include <algorithm>
#include <QDateTime>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFileDialog>
#include <QDebug>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QMessageBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "BackgroundReconciler.h"
#include "ClipboardOps.h"
#include "CollisionDialog.h"
#include "DatabaseStatsDialog.h"
#include "FileOpsWorker.h"
#include "FolderIndexer.h"
#include "WindowRegistry.h"
#include "FolderTreeView.h"
#include "FullscreenViewer.h"
#include "RawCacheWarmer.h"
#include "ShellOps.h"
#include "HoverInfoWorker.h"
#include "KeyBindings.h"
#include "PathQ.h"
#include "Preferences.h"
#include "PreferencesDialog.h"
#include "PreviewDecoder.h"
#include "PreviewPane.h"
#include "RawRenderer.h"
#include "SortIcons.h"
#include "StatusBarRow.h"
#include "StatusLabel.h"
#include "ThumbGridModel.h"
#include "ThumbGridView.h"
#include "ThumbLoader.h"
#include "db/Database.h"
#include "db/Schema.h"
#include "util/AppPaths.h"
#include "util/Profile.h"
#include "util/FileMove.h"
#include "util/PathUtil.h"
#include "version.h"

namespace {
// dirs.path / files.name are UTF-8 in the DB; a path from QFileSystemModel has to be put
// into the exact same form pixet_core writes before it's used as a lookup key, or the
// query silently misses rows that are really there. That means the platform's separator
// style (QFileSystemModel uses forward slashes even on Windows) and, on macOS, the Unicode
// normalization form too - normalizePath() handles both.
QString normalizeForDb(const QString &path) {
    return QString::fromStdString(pixet::normalizePath(path.toStdString()));
}

// Next sibling of `idx` - or, if it's the last one, walks up until an ancestor
// actually has a next sibling ("aunt": your parent's next sibling; a grandparent's
// if the parent is *also* last, and so on) - rather than just stopping at the last
// child in a folder. Returns an invalid index only once this walks all the way up
// without finding one (nothing left in the tree). Shared by Ctrl+Down (always) and
// Ctrl+Right (only once it's out of children to descend into - see
// MainWindow::onNavigateFolderRequested()).
QModelIndex nextSiblingOrAunt(QFileSystemModel *model, const QModelIndex &idx) {
    QModelIndex current = idx;
    while (current.isValid()) {
        QModelIndex candidate = model->index(current.row() + 1, 0, current.parent());
        if (candidate.isValid()) return candidate;
        current = current.parent();
    }
    return QModelIndex();
}
} // namespace

MainWindow::MainWindow(bool resetLayout, QWidget *parent) : QMainWindow(parent), resetLayout_(resetLayout) {
    setWindowTitle(QStringLiteral("pixet %1").arg(pixet::version()));
    WindowRegistry::instance().add(this);
    // Every window rebuilds its own Windows submenu when the set of windows changes, so
    // opening or closing a window updates the list everywhere rather than only where it
    // happened.
    connect(&WindowRegistry::instance(), &WindowRegistry::changed, this, &MainWindow::rebuildWindowsMenu);
    resize(1280, 800);

    db_ = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), false);

    // Pushes the RAW cache settings into pixet_core, which has no access to prefs of its
    // own. Done per window rather than once in main() because it is idempotent and cheap,
    // and this way a window opened later can never be running against a stale budget.
    prefs::applyRawCacheSettings();

    // --- left panel: folder tree + bookmarks (top), preview (bottom, user-resizable) ---
    // placeholder-text is Qt's semantic role for "muted but still legible" text, which is
    // exactly what a section title wants. palette(mid) is the obvious-looking alternative
    // and is wrong here: on this app's dark theme it resolves to near-black, too close to
    // the background to actually see.
    auto makeSectionTitle = [](const QString &text, QWidget *parent) {
        auto *title = new QLabel(text, parent);
        title->setStyleSheet(QStringLiteral("color: palette(placeholder-text); font-weight: bold;"));
        return title;
    };

    bookmarks_ = new QListWidget(this);
    bookmarks_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(bookmarks_, &QListWidget::itemClicked, this, &MainWindow::onBookmarkClicked);
    connect(bookmarks_, &QListWidget::customContextMenuRequested, this, &MainWindow::onBookmarksContextMenu);

    auto *bookmarksPanel = new QWidget(this);
    auto *bookmarksLayout = new QVBoxLayout(bookmarksPanel);
    bookmarksLayout->setContentsMargins(0, 0, 0, 0);
    bookmarksLayout->setSpacing(2);
    bookmarksLayout->addWidget(makeSectionTitle(QStringLiteral("Bookmarks"), bookmarksPanel));
    bookmarksLayout->addWidget(bookmarks_, /*stretch=*/1);

    fsModel_ = new QFileSystemModel(this);
    fsModel_->setRootPath(QString());
    // QDir::Drives is what surfaces the drive-letter list on Windows and is simply inert
    // elsewhere. Kept unconditionally: it costs nothing on macOS and removing it would only
    // make the Windows behaviour depend on a platform check.
    fsModel_->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);

    tree_ = new FolderTreeView(this);
    tree_->setModel(fsModel_);
    // On Windows an empty root index means "show the drives", which is a short, useful list.
    // On macOS it means a single "/" node, so every launch starts with the user four levels
    // of clicking away from their own photos, past /System and /Library. Rooting the tree at
    // $HOME is the sane default there; external volumes are still reachable because
    // navigateTo() can go anywhere (Choose Folder..., the path bar, or a bookmark), and
    // /Volumes gets seeded as a bookmark on first run - see loadBookmarks().
#ifdef Q_OS_MACOS
    tree_->setRootIndex(fsModel_->index(QDir::homePath()));
#else
    tree_->setRootIndex(fsModel_->index(QString()));
#endif
    tree_->setHeaderHidden(true);
    for (int col = 1; col < fsModel_->columnCount(); ++col) tree_->hideColumn(col);
    // QTreeView defaults to stretchLastSection(true), which forces the (only visible)
    // column to always exactly fill the viewport - a long/deeply-indented name just
    // gets elided with no way to see the rest. Let the column grow to fit its content
    // instead, with a horizontal scrollbar picking up the overflow.
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    tree_->setTextElideMode(Qt::ElideNone);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QWidget::customContextMenuRequested, this, &MainWindow::onTreeContextMenu);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::onTreeSelectionChanged);
    connect(fsModel_, &QFileSystemModel::directoryLoaded, this, &MainWindow::onTreeDirectoryLoaded);

    // Titled to match bookmarksPanel. A folder tree is self-explanatory on its own, but
    // sitting side by side with a labeled list the asymmetry reads as a missing label
    // rather than an intentional omission.
    auto *treePanel = new QWidget(this);
    auto *treePanelLayout = new QVBoxLayout(treePanel);
    treePanelLayout->setContentsMargins(0, 0, 0, 0);
    treePanelLayout->setSpacing(2);
    treePanelLayout->addWidget(makeSectionTitle(QStringLiteral("Folders"), treePanel));
    treePanelLayout->addWidget(tree_, /*stretch=*/1);

    // --- right: preview pane (constructed here so topSplitter_/leftPanel_ below can
    // reference it - actually placed at the bottom of the left column, see layout) ---
    preview_ = new PreviewPane(this);
    preview_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(preview_, &QWidget::customContextMenuRequested, this, &MainWindow::onPreviewContextMenu);

    topSplitter_ = new QSplitter(Qt::Horizontal, this);
    topSplitter_->addWidget(treePanel);
    topSplitter_->addWidget(bookmarksPanel);
    topSplitter_->setStretchFactor(0, 7);
    topSplitter_->setStretchFactor(1, 3);
    topSplitter_->setCollapsible(0, false);
    topSplitter_->setCollapsible(1, false);

    // A real QSplitter handle rather than a preview locked to a fixed aspect: the user
    // picks whatever height actually suits the image - wide-but-short for a panorama, tall
    // for a portrait shot - and leftSplitterState below persists that choice the same way
    // mainSplitterState/topSplitterState do.
    leftSplitter_ = new QSplitter(Qt::Vertical, this);
    leftSplitter_->addWidget(topSplitter_);
    leftSplitter_->addWidget(preview_);
    leftSplitter_->setStretchFactor(0, 1);
    leftSplitter_->setStretchFactor(1, 0);
    leftSplitter_->setCollapsible(0, false);
    leftSplitter_->setCollapsible(1, false);

    leftPanel_ = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(leftPanel_);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(leftSplitter_);

    // --- center: thumbnail grid ---
    gridModel_ = new ThumbGridModel(*db_, this);
    grid_ = new ThumbGridView(this);
    grid_->setModel(gridModel_);
    grid_->setIconSize(QSize(prefs::thumbnailIconSize(), prefs::thumbnailIconSize()));
    connect(grid_, &ThumbGridView::currentRowChanged, this, &MainWindow::onGridSelectionChanged);
    // currentRowChanged (above) covers the lead-row-moved case (preview/path bar/
    // status-bar detail); selectionChanged covers count/membership changes that don't
    // necessarily move the lead (e.g. a Ctrl+click toggling some other row) - both
    // feed updateSelectionStatus(), which reads whichever of currentRow()/
    // selectedRows() it needs regardless of which signal triggered it.
    connect(grid_, &ThumbGridView::selectionChanged, this, &MainWindow::updateSelectionStatus);
    connect(grid_, &ThumbGridView::selectionChanged, this, &MainWindow::updateEditActionsEnabled);
    connect(grid_, &ThumbGridView::ctrlHoverRowChanged, this, &MainWindow::onGridCtrlHoverChanged);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_, &QWidget::customContextMenuRequested, this, &MainWindow::onGridContextMenu);
    connect(grid_, &ThumbGridView::navigateFolderRequested, this, &MainWindow::onNavigateFolderRequested);
    // Fires on both double-click and Enter/Return - exactly the two ways to "open"
    // an item.
    connect(grid_, &ThumbGridView::activated, this, &MainWindow::onGridItemActivated);
    connect(grid_, &ThumbGridView::filesDropped, this, &MainWindow::onFilesDroppedOnGrid);
    connect(grid_, &ThumbGridView::dragOutRequested, this, &MainWindow::onDragOutRequested);

    fullscreenViewer_ = new FullscreenViewer(this);
    // Keep the grid's selection following along while browsing fullscreen, so
    // closing it (Escape/double-click) leaves the grid on whatever image was last
    // shown there instead of wherever it was when fullscreen opened.
    connect(fullscreenViewer_, &FullscreenViewer::contextMenuRequested, this,
            &MainWindow::onFullscreenContextMenu);
    connect(fullscreenViewer_, &FullscreenViewer::rowChanged, this, [this](int row) {
        grid_->setCurrentRow(row);
        grid_->scrollToRow(row);
    });

    // --- path bar: shows/edits currentPath_; Enter navigates (see navigateToInput).
    // Editable QComboBox rather than a plain QLineEdit so its dropdown can double as
    // recently-visited-folder history (see refreshPathBarHistory()/prefs::pathHistory()) ---
    pathBar_ = new QComboBox(this);
    pathBar_->setEditable(true);
    pathBar_->setInsertPolicy(QComboBox::NoInsert); // history is only ever written via navigateTo(), never by typing
    pathBar_->setPlaceholderText(QStringLiteral("Path..."));
    refreshPathBarHistory();
    // returnPressed lives on the combo's internal line edit, not the combo itself. This is
    // the path for text typed or pasted directly, so an arbitrary path submits without
    // having to already be a history entry.
    connect(pathBar_->lineEdit(), &QLineEdit::returnPressed, this, &MainWindow::onPathBarReturnPressed);
    // Fires when a dropdown entry is actually picked (mouse click, or arrow keys +
    // Enter while the popup is open) - textActivated already updates currentText()
    // before this runs, so the same submit path applies.
    connect(pathBar_, &QComboBox::textActivated, this, &MainWindow::onPathBarHistoryActivated);
    // select-all-on-focus is implemented in eventFilter() (QEvent::FocusIn), which only
    // fires because of this call. Installed on the combo's actual line edit, since that's
    // what receives keyboard focus (and therefore FocusIn) for an editable QComboBox, not
    // the QComboBox widget itself.
    pathBar_->lineEdit()->installEventFilter(this);
    // Application-wide, for the mouse's back/forward buttons (see eventFilter()). It has to
    // be this broad: the grid, the tree and the preview pane all accept mouse presses
    // themselves, so an override on this window would only ever see events none of them
    // wanted - which is never, over the areas the user is actually clicking.
    QCoreApplication::instance()->installEventFilter(this);

    // --- back/forward: plain in-memory navigateTo() stack, see navHistory_'s doc
    // comment. Fixed (non-configurable) shortcuts on the QAction itself, same
    // reasoning as the Edit menu's standard shortcuts - the toolbar buttons below
    // just display whichever QAction they're set as the default action for, so the
    // shortcut/enabled-state/icon only ever need to be set in one place. ---
    backAction_ = new QAction(style()->standardIcon(QStyle::SP_ArrowBack), QStringLiteral("Back"), this);
    backAction_->setShortcut(QKeySequence(QStringLiteral("Alt+Left")));
    backAction_->setEnabled(false);
    connect(backAction_, &QAction::triggered, this, &MainWindow::onNavigateBack);

    forwardAction_ = new QAction(style()->standardIcon(QStyle::SP_ArrowForward), QStringLiteral("Forward"), this);
    forwardAction_->setShortcut(QKeySequence(QStringLiteral("Alt+Right")));
    forwardAction_->setEnabled(false);
    connect(forwardAction_, &QAction::triggered, this, &MainWindow::onNavigateForward);

    // Up one directory. Alt+Up completes the Alt+Left/Alt+Right set and is the same binding
    // Explorer and most file managers use; deliberately fixed rather than configurable, same
    // reasoning as back/forward above.
    upAction_ = new QAction(style()->standardIcon(QStyle::SP_ArrowUp), QStringLiteral("Up"), this);
    upAction_->setShortcut(QKeySequence(QStringLiteral("Alt+Up")));
    upAction_->setEnabled(false);
    connect(upAction_, &QAction::triggered, this, &MainWindow::onNavigateUp);

    auto *backButton = new QToolButton(this);
    backButton->setDefaultAction(backAction_);
    backButton->setAutoRaise(true); // flat, address-bar-adjacent look rather than a raised push button
    auto *forwardButton = new QToolButton(this);
    forwardButton->setDefaultAction(forwardAction_);
    forwardButton->setAutoRaise(true);
    auto *upButton = new QToolButton(this);
    upButton->setDefaultAction(upAction_);
    upButton->setAutoRaise(true);

    // --- Sort By: created here, before navRowLayout below (which needs valid QAction
    // pointers for the toolbar buttons) rather than down in the View menu setup, which
    // runs much later in this constructor. The View > Sort By submenu (further down)
    // adds these same QAction objects rather than creating its own, so the menu and
    // the toolbar can't drift out of sync with each other - same idea as
    // backAction_/forwardAction_ above, just shared with a menu too. ---
    // Icon color follows the current palette rather than a hardcoded black/white, so
    // the flat outline icons drawn by sorticons::make() read correctly against both a
    // light and a dark OS theme without needing separate asset variants - see that
    // header's own comment. Snapshotted once here (matching this app's existing level
    // of theming support - nothing else re-derives colors on a live theme change
    // either), not re-read per icon.
    const QColor iconColor = palette().color(QPalette::ButtonText);
    auto *sortKeyGroup = new QActionGroup(this); // exclusive by default
    auto addSortKeyAction = [&](const QString &text, prefs::SortKey key, sorticons::Kind icon) {
        QAction *a = new QAction(sorticons::make(icon, iconColor), text, this);
        a->setCheckable(true);
        // Clicking the key that is already active flips the direction, the way every file
        // manager treats a second click on the active sort column. Without this the click
        // would do nothing at all: an exclusive QActionGroup won't let a click uncheck its
        // checked member, so re-triggering the active key just recomputes an identical sort.
        //
        // This is why the key actions go through a lambda rather than connecting straight to
        // onSortOrderChanged() the way the reverse toggle does: the flip has to know *which*
        // key was clicked, and by the time the slot runs the QActionGroup has already moved
        // the check mark, so nothing left to read distinguishes "switched to Name" from
        // "clicked Name again". prefs::gridSortKey() is still the pre-click key here
        // (onSortOrderChanged() below is what writes the new one), which is the comparison
        // needed.
        //
        // setChecked() rather than toggle() to be explicit that no triggered() is re-emitted
        // from here - only toggled(), which nothing is connected to. Applies to the View >
        // Sort By submenu too, since these are the same QAction objects; picking the already
        // ticked entry there reverses as well.
        connect(a, &QAction::triggered, this, [this, key] {
            if (key == prefs::gridSortKey())
                sortReverseAction_->setChecked(!sortReverseAction_->isChecked());
            onSortOrderChanged();
        });
        sortKeyGroup->addAction(a);
        return a;
    };
    // Short labels rather than e.g. "Date Modified" - text still shows in the View >
    // Sort By submenu, but the toolbar buttons below go icon-only the moment an icon
    // is set (QToolButton's default style), so the tooltip is what actually explains
    // each one there. Tooltips are set by updateSortTooltips() rather than passed in
    // here, because they now have to say which direction a second click will give you
    // and so change with the sort state - see that method.
    sortByNameAction_ = addSortKeyAction(QStringLiteral("Name"), prefs::SortKey::Name, sorticons::Kind::Name);
    sortByFileDateAction_ =
        addSortKeyAction(QStringLiteral("Modified"), prefs::SortKey::FileDate, sorticons::Kind::FileDate);
    sortByTakenDateAction_ =
        addSortKeyAction(QStringLiteral("Taken"), prefs::SortKey::TakenDate, sorticons::Kind::TakenDate);
    sortBySizeAction_ = addSortKeyAction(QStringLiteral("Size"), prefs::SortKey::Size, sorticons::Kind::Size);
    switch (prefs::gridSortKey()) {
        case prefs::SortKey::FileDate: sortByFileDateAction_->setChecked(true); break;
        case prefs::SortKey::TakenDate: sortByTakenDateAction_->setChecked(true); break;
        case prefs::SortKey::Size: sortBySizeAction_->setChecked(true); break;
        case prefs::SortKey::Name:
        default: sortByNameAction_->setChecked(true); break;
    }
    sortReverseAction_ = new QAction(sorticons::make(sorticons::Kind::Reverse, iconColor), QStringLiteral("Reverse"), this);
    sortReverseAction_->setCheckable(true);
    sortReverseAction_->setChecked(prefs::gridSortDescending());
    // Still shares the slot directly: this one only ever means "flip", so unlike the key
    // actions above it has nothing extra to decide before onSortOrderChanged() re-reads state.
    connect(sortReverseAction_, &QAction::triggered, this, &MainWindow::onSortOrderChanged);
    // First run, once every sort action exists and is in its restored-from-prefs state.
    updateSortTooltips();
    // Pushes the persisted order in before the very first setDirectory() call
    // (restoreLastDirectory(), further down this constructor) - safe to call with
    // rows_ still empty, see ThumbGridModel::setSortOrder()'s doc comment.
    gridModel_->setSortOrder(prefs::gridSortKey(), prefs::gridSortDescending());

    // --- top-level: back/forward + path bar above, left column (40%) vs. thumbnail grid (60%) below ---
    splitter_ = new QSplitter(this);
    splitter_->addWidget(leftPanel_);
    splitter_->addWidget(grid_);
    splitter_->setStretchFactor(0, 2);
    splitter_->setStretchFactor(1, 3);
    splitter_->setCollapsible(0, false);
    splitter_->setCollapsible(1, false);

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(4, 4, 4, 4);
    auto *navRowLayout = new QHBoxLayout();
    navRowLayout->setContentsMargins(0, 0, 0, 0);
    navRowLayout->addWidget(backButton);
    navRowLayout->addWidget(forwardButton);
    navRowLayout->addWidget(upButton);
    navRowLayout->addWidget(pathBar_, /*stretch=*/1);
    // Sort button bar, right of the path bar (nothing after it has stretch, so these
    // stay pinned to the right edge). Same QAction objects as the View > Sort By
    // submenu - see that block's comment. Icon-only (see addSortKeyAction() above),
    // with a plain "Sort:" label ahead of them since there's no text left on the
    // buttons themselves to say what the row is.
    auto *sortLabel = new QLabel(QStringLiteral("Sort:"), this);
    sortLabel->setContentsMargins(4, 0, 2, 0);
    navRowLayout->addWidget(sortLabel);
    for (QAction *a : {sortByNameAction_, sortByFileDateAction_, sortByTakenDateAction_, sortBySizeAction_,
                        sortReverseAction_}) {
        auto *button = new QToolButton(this);
        button->setDefaultAction(a);
        button->setAutoRaise(true);
        navRowLayout->addWidget(button);
    }
    centralLayout->addLayout(navRowLayout);
    centralLayout->addWidget(splitter_, /*stretch=*/1);
    setCentralWidget(central);

    restoreWindowState(); // window position/size only - see the method for why

    // Splitter sizing needs real geometry to mean anything (percentages of width, or
    // restoreState() blobs that encode absolute pixel positions) and none exists yet
    // pre-show() - central widget width is still 0 here. Deferring to after the first
    // real layout pass, rather than computing now and reasserting the same (already
    // wrong) numbers later, is the fix; capturing-then-replaying a pre-layout value was
    // tried first and just reproduced the same "everything squashed into 40px" bug this
    // paragraph exists to warn about.
    QTimer::singleShot(0, this, [this]() {
        QSettings settings = prefs::settingsStore();

        bool splitterRestored = false;
        if (!resetLayout_) {
            QByteArray state = settings.value(QStringLiteral("mainSplitterState")).toByteArray();
            if (!state.isEmpty()) splitterRestored = splitter_->restoreState(state);
        }
        if (!splitterRestored) {
            int w = width();
            splitter_->setSizes({static_cast<int>(w * 0.4), static_cast<int>(w * 0.6)});
        }

        bool topSplitterRestored = false;
        if (!resetLayout_) {
            QByteArray state = settings.value(QStringLiteral("topSplitterState")).toByteArray();
            if (!state.isEmpty()) topSplitterRestored = topSplitter_->restoreState(state);
        }
        if (!topSplitterRestored) {
            int leftW = leftPanel_->width() > 0 ? leftPanel_->width() : static_cast<int>(width() * 0.4);
            topSplitter_->setSizes({static_cast<int>(leftW * 0.7), static_cast<int>(leftW * 0.3)});
        }

        bool leftSplitterRestored = false;
        if (!resetLayout_) {
            QByteArray state = settings.value(QStringLiteral("leftSplitterState")).toByteArray();
            if (!state.isEmpty()) leftSplitterRestored = leftSplitter_->restoreState(state);
        }
        if (!leftSplitterRestored) {
            // First-run default: preview height set to the left panel's width, so it
            // starts out roughly square at typical widths. Nothing is forced - the handle
            // is draggable from the first moment.
            int leftW = leftPanel_->width() > 0 ? leftPanel_->width() : static_cast<int>(width() * 0.4);
            int h = leftSplitter_->height() > 0 ? leftSplitter_->height() : height();
            leftSplitter_->setSizes({qMax(0, h - leftW), leftW});
        }
    });

    // --- background workers (no parent - see the member declarations in the header) ---
    thumbLoader_ = std::make_unique<ThumbLoader>();
    // The worker can't safely ask a widget or QScreen for this itself, so it's pushed in from
    // here - see ThumbLoader::setDevicePixelRatio(). Without it grid thumbnails decode to the
    // logical icon size and get upscaled on a Retina display.
    thumbLoader_->setDevicePixelRatio(devicePixelRatioF());
    connect(gridModel_, &ThumbGridModel::thumbNeeded, thumbLoader_.get(), &ThumbLoader::request);
    connect(thumbLoader_.get(), &ThumbLoader::thumbReady, gridModel_, &ThumbGridModel::setThumbnail);
    // Additional taps on the same two signals, purely for onCopyGridDebugInfo()'s
    // navigation-timing section - see navThumbTimer_'s doc comment. Doesn't replace
    // or reorder the connections above; Qt fires both for a given signal regardless
    // of connection order.
    connect(gridModel_, &ThumbGridModel::thumbNeeded, this, &MainWindow::onNavThumbRequested);
    connect(thumbLoader_.get(), &ThumbLoader::thumbReady, this, &MainWindow::onNavThumbReady);
    // ThumbGridModel::setThumbnail() already emits dataChanged(), and
    // ThumbGridView::setModel() already connects that straight to a viewport
    // repaint - no separate "make sure it actually repainted" connection needed
    // here anymore. (A previous QListView-based grid needed exactly that as a
    // belt-and-suspenders fix for IconMode's own unreliable partial repaints; the
    // custom-painted replacement doesn't have that failure mode, since there's no
    // second layout/paint engine left that could disagree with what dataChanged
    // says needs redrawing.)

    previewDecoder_ = std::make_unique<PreviewDecoder>();
    connect(previewDecoder_.get(), &PreviewDecoder::previewReady, this, &MainWindow::onPreviewReady);

    rawCacheWarmer_ = std::make_unique<RawCacheWarmer>();
    folderIndexer_ = std::make_unique<FolderIndexer>();
    connect(this, &MainWindow::requestIndex, folderIndexer_.get(), &FolderIndexer::indexFolder);
    connect(folderIndexer_.get(), &FolderIndexer::started, this, &MainWindow::onIndexerStarted);
    connect(folderIndexer_.get(), &FolderIndexer::filesListed, this, &MainWindow::onFilesListed);
    connect(folderIndexer_.get(), &FolderIndexer::thumbsProgress, this, &MainWindow::onThumbsProgress);
    connect(folderIndexer_.get(), &FolderIndexer::indexFailed, this, &MainWindow::onIndexFailed);
    connect(folderIndexer_.get(), &FolderIndexer::finished, this, &MainWindow::onIndexerFinished);
    // Whatever's on screen gets thumbnailed first - see pushGridPriorityToIndexer().
    // Connected after folderIndexer_ exists, since that's what the hint is pushed into.
    connect(grid_, &ThumbGridView::visibleRowsChanged, this, &MainWindow::pushGridPriorityToIndexer);

    // Shared across every window rather than owned per-window - see
    // BackgroundReconciler::shared(). Both sweep the whole library regardless of what any
    // window is showing, so one instance per window would multiply that work by the window
    // count. start() is idempotent enough to call again here (it just queues another
    // beginLoop), but guarding on first use keeps the intent obvious.
    static bool sharedServicesStarted = false;
    connect(&BackgroundReconciler::shared(), &BackgroundReconciler::directoryChanged, this,
            &MainWindow::onBackgroundDirectoryChanged);
    connect(this, &MainWindow::requestFullReindex, &BackgroundReconciler::shared(),
            &BackgroundReconciler::triggerFullSweepNow);
    connect(&RawRenderer::shared(), &RawRenderer::directoryChanged, this,
            &MainWindow::onBackgroundDirectoryChanged);
    connect(this, &MainWindow::requestRawRenderPriority, &RawRenderer::shared(), &RawRenderer::prioritize);
    if (!sharedServicesStarted) {
        sharedServicesStarted = true;
        BackgroundReconciler::shared().start();
        RawRenderer::shared().start();
    }

    fileOps_ = std::make_unique<FileOpsWorker>();
    connect(this, &MainWindow::requestFileOpPreflight, fileOps_.get(), &FileOpsWorker::preflight);
    connect(this, &MainWindow::requestFileOpExecute, fileOps_.get(), &FileOpsWorker::execute);
    connect(fileOps_.get(), &FileOpsWorker::preflightReady, this, &MainWindow::onFileOpPreflightReady);
    connect(fileOps_.get(), &FileOpsWorker::progress, this, &MainWindow::onFileOpProgress);
    connect(fileOps_.get(), &FileOpsWorker::finished, this, &MainWindow::onFileOpFinished);
    connect(this, &MainWindow::requestFileDelete, fileOps_.get(), &FileOpsWorker::deleteFiles);
    connect(fileOps_.get(), &FileOpsWorker::deleteFinished, this, &MainWindow::onFileDeleteFinished);

    previewDebounce_ = new QTimer(this);
    previewDebounce_->setSingleShot(true);
    previewDebounce_->setInterval(80);
    connect(previewDebounce_, &QTimer::timeout, this, &MainWindow::triggerPreviewRequest);

    // See folderWatcher_'s member comment: a real-time watch on whichever folder is
    // currently on screen, so an external change (most concretely, Explorer
    // performing the actual move for a Cut that originated in pixet) shows up
    // immediately instead of waiting on BackgroundReconciler's slow rotation.
    folderWatcher_ = new QFileSystemWatcher(this);
    connect(folderWatcher_, &QFileSystemWatcher::directoryChanged, this, &MainWindow::onWatchedDirectoryChanged);
    folderWatchDebounce_ = new QTimer(this);
    folderWatchDebounce_->setSingleShot(true);
    folderWatchDebounce_->setInterval(400); // coalesces the several native notifications one multi-file paste fires
    connect(folderWatchDebounce_, &QTimer::timeout, this, [this]() {
        if (!currentPath_.isEmpty()) emit requestIndex(currentPath_, /*force=*/true, false);
    });

    // --- menu ---
    // Shortcuts are user-configurable (see KeyBindings.h/PreferencesDialog) - the
    // QAction pointers are kept as members so onPreferences() can re-apply their
    // shortcuts after the editor closes, since QAction doesn't watch settings
    // itself.
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    // Cmd/Ctrl+N. On Windows a second window is just a second process, but macOS routes a
    // second launch back into the running instance, so without this there is no way to get
    // one at all - see WindowRegistry's class comment.
    QAction *newWindowAction = fileMenu->addAction(QStringLiteral("New Window"), this, &MainWindow::onNewWindow);
    newWindowAction->setShortcut(QKeySequence::New);
    // MenuRole matters on macOS: without an explicit non-default role Qt's text heuristics can
    // relocate an item into the application menu, and "New Window" belongs in File.
    newWindowAction->setMenuRole(QAction::NoRole);
    QAction *closeWindowAction = fileMenu->addAction(QStringLiteral("Close Window"), this, &QWidget::close);
    closeWindowAction->setShortcut(QKeySequence::Close);
    closeWindowAction->setMenuRole(QAction::NoRole);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Choose Folder..."), this, &MainWindow::onChooseFolder);

    // Select All uses a fixed QKeySequence::StandardKey rather than the configurable
    // KeyBindings system - see KeyBindings.h's class comment. setShortcuts() (plural)
    // installs every platform binding for that standard key, not just the primary
    // one (e.g. Windows also gets the legacy Ins/Del-based Copy/Cut/Paste aliases
    // once those are added in a later phase).
    auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    cutAction_ = editMenu->addAction(QStringLiteral("Cut"), this, &MainWindow::onEditCut);
    cutAction_->setShortcuts(QKeySequence::Cut);
    copyAction_ = editMenu->addAction(QStringLiteral("Copy"), this, &MainWindow::onEditCopy);
    copyAction_->setShortcuts(QKeySequence::Copy);
    pasteAction_ = editMenu->addAction(QStringLiteral("Paste"), this, &MainWindow::onEditPaste);
    pasteAction_->setShortcuts(QKeySequence::Paste);
    editMenu->addSeparator();
    renameAction_ = editMenu->addAction(QStringLiteral("Rename..."), this, &MainWindow::onEditRename);
#ifdef Q_OS_MACOS
    deleteAction_ = editMenu->addAction(QStringLiteral("Move to Trash"), this, &MainWindow::onEditDelete);
#else
    deleteAction_ = editMenu->addAction(QStringLiteral("Move to Recycle Bin"), this, &MainWindow::onEditDelete);
#endif
    deleteAction_->setShortcuts(QKeySequence::Delete);
    editMenu->addSeparator();
    selectAllAction_ = editMenu->addAction(QStringLiteral("Select All"), this, &MainWindow::onEditSelectAll);
    selectAllAction_->setShortcuts(QKeySequence::SelectAll);
    // Cut/Copy need a selection; Paste needs somewhere to land. Kept in sync from
    // two places: selectionChanged (so a disabled action's shortcut correctly
    // doesn't fire even without the menu ever being opened) and aboutToShow (belt
    // and suspenders for anything else that could change either condition).
    connect(editMenu, &QMenu::aboutToShow, this, &MainWindow::updateEditActionsEnabled);

    auto *bookmarksMenu = menuBar()->addMenu(QStringLiteral("&Bookmarks"));
    addBookmarkAction_ = bookmarksMenu->addAction(QStringLiteral("Add Current Folder"), this, &MainWindow::onAddBookmark);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    refreshAction_ = viewMenu->addAction(QStringLiteral("Refresh"), this, &MainWindow::onRefresh);
    toggleSidePanelAction_ = viewMenu->addAction(QStringLiteral("Toggle Side Panel"), this, &MainWindow::onToggleSidePanel);
    viewMenu->addSeparator();
    hoverInfoAction_ = viewMenu->addAction(QStringLiteral("Show Hover Info"), this, &MainWindow::onToggleHoverInfo);
    hoverInfoAction_->setCheckable(true);
    hoverInfoAction_->setChecked(prefs::hoverInfoEnabled());

    // --- Windows: inline in this menu rather than a submenu, so the open windows are visible
    // the moment View is opened instead of needing a second hover. A disabled action acts as
    // the section heading; the entries themselves are inserted between it and
    // windowSectionEnd_ by rebuildWindowsMenu(), which is the only reason that separator is
    // held as a member - insertAction() needs something to insert *before*. ---
    viewMenu->addSeparator();
    viewMenu_ = viewMenu;
    QAction *windowsHeader = viewMenu->addAction(QStringLiteral("Windows"));
    windowsHeader->setEnabled(false);
    // NoRole matters on macOS even for a disabled item: Qt's menu-role heuristics look at the
    // text, and an item left at TextHeuristicRole can be relocated into the application menu.
    windowsHeader->setMenuRole(QAction::NoRole);
    windowSectionEnd_ = viewMenu->addSeparator();
    // aboutToShow rather than only on WindowRegistry::changed(), so the checkmark and the
    // folder paths are correct at the moment the menu opens - activation deliberately doesn't
    // trigger a rebuild (see WindowRegistry::noteActivated()).
    connect(viewMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildWindowsMenu);
    rebuildWindowsMenu();

    // --- Sort By: the QActions themselves were created earlier (see the comment by
    // sortKeyGroup's construction, above) - this just places the same objects into a
    // menu too, via addAction(QAction*) rather than the addAction(text, receiver,
    // slot) overload that would create new ones. ---
    auto *sortMenu = viewMenu->addMenu(QStringLiteral("Sort By"));
    sortMenu->addAction(sortByNameAction_);
    sortMenu->addAction(sortByFileDateAction_);
    sortMenu->addAction(sortByTakenDateAction_);
    sortMenu->addAction(sortBySizeAction_);
    sortMenu->addSeparator();
    sortMenu->addAction(sortReverseAction_);

    // Not in any menu - see the member's own doc comment - just registered on the
    // window so its shortcut is live.
    focusAddressBarAction_ = new QAction(this);
    connect(focusAddressBarAction_, &QAction::triggered, this, &MainWindow::onFocusAddressBar);
    addAction(focusAddressBarAction_);

    applyKeyBindingShortcuts();

    // Tools always exists now (unconditionally, on both platforms) - Force
    // Re-thumbnail lives here rather than buried in the grid's right-click menu,
    // same reasoning as Refresh already living in View: a folder-wide, occasional
    // action belongs in a real menu, not a context menu meant for per-item actions
    // (Cut/Copy/Paste/View Fullscreen - see onGridContextMenu()).
    auto *toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
    toolsMenu->addAction(QStringLiteral("Force Re-thumbnail This Folder"), this, &MainWindow::onForceRethumbnail);
    toolsMenu->addAction(QStringLiteral("Purge Path History"), this, &MainWindow::onPurgePathHistory);

    // Qt's Cocoa menu bar relocates Preferences and About into the macOS *application* menu
    // (as Cmd-, and "About pixet"). So on Apple they're added to File, which still has
    // Choose Folder left in it after the move. Tools is built on both platforms because it
    // always holds Force Re-thumbnail - a Tools menu holding *only* Preferences would come
    // out empty on macOS, once Cocoa relocated that one entry away.
    QAction *prefsAction = nullptr;
    QAction *aboutAction = nullptr;
#ifdef Q_OS_MACOS
    prefsAction = fileMenu->addAction(QStringLiteral("Preferences..."), this, &MainWindow::onPreferences);
    aboutAction = fileMenu->addAction(QStringLiteral("About pixet"), this, &MainWindow::onAbout);
#else
    prefsAction = toolsMenu->addAction(QStringLiteral("Preferences..."), this, &MainWindow::onPreferences);
    auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    aboutAction = helpMenu->addAction(QStringLiteral("About pixet"), this, &MainWindow::onAbout);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("E&xit"), this, &MainWindow::close);
#endif
    // Set explicitly rather than leaning on Qt's text heuristic. The heuristic does work,
    // but it keys off the strings still containing "preferences"/"about", so renaming a menu
    // item would quietly move it back out of the application menu. There is deliberately no
    // QuitRole action: macOS supplies its own Quit in the application menu, and adding one
    // here would duplicate it.
    prefsAction->setMenuRole(QAction::PreferencesRole);
    aboutAction->setMenuRole(QAction::AboutRole);

    // Behind a build option rather than plain NDEBUG, because MainWindow.h's comment on
    // onCopyGridDebugInfo() is explicit that this should stay available on the daily-driver
    // *release* build - so it defaults ON everywhere and nothing changes for local use.
    // scripts/deploy-mac.sh configures with -DPIXET_DEBUG_MENU=OFF, so it doesn't ship in a
    // DMG handed to someone else. That's the "reconsider before any wider distribution" case
    // the same comment names, without removing the fast path it asks to keep permanently.
#ifdef PIXET_DEBUG_MENU
    auto *debugMenu = menuBar()->addMenu(QStringLiteral("&Debug"));
    debugMenu->addAction(QStringLiteral("Copy Grid Debug Info"), this, &MainWindow::onCopyGridDebugInfo);
    // Only offered when there's something to show. util/Profile.h compiles to nothing unless
    // the build was configured -DPIXET_PROFILE=ON, so on a normal build this action would
    // report an empty table and just raise questions.
    if (pixet::profile::enabled()) {
        debugMenu->addAction(QStringLiteral("Copy Profile Report"), this, &MainWindow::onCopyProfileReport);
    }
#endif

    // Insurance for window/layout persistence: closeEvent() is not guaranteed to run. On
    // macOS, Cmd+Q and "Quit pixet" go through the application menu and terminate without
    // necessarily delivering a close event to the window, so saving only from there leaves
    // geometry, splitter sizes and the last directory silently un-remembered for the most
    // common way of quitting. Saving twice is harmless (same values, same keys).
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &MainWindow::saveWindowState);

    // Nominal pixel widths, sized generously for typical content - shrink together
    // proportionally (see StatusBarRow.h) if the window is too narrow for all of
    // them at once, rather than either overlapping (a QHBoxLayout of Qt::Fixed-
    // policy cells can't shrink them, so it has nothing left to do but position the
    // next cell at an X offset that assumes a smaller size than the previous one is
    // actually rendered at) or every cell collapsing to the same tiny floor
    // regardless of its own nominal width (what a QHBoxLayout of shrinkable cells
    // actually did here) - both confirmed live via real bugs, not hypothetical ones.
    auto *statusRow = new StatusBarRow(this);
    folderStatsLabel_ = statusRow->addLabel(300);
    rawStatusLabel_ = statusRow->addLabel(190);
    fileNameLabel_ = statusRow->addLabel(240);
    formatLabel_ = statusRow->addLabel(50);
    dimsLabel_ = statusRow->addLabel(150);
    sizeLabel_ = statusRow->addLabel(85);
    dateLabel_ = statusRow->addLabel(115);
    durationLabel_ = statusRow->addLabel(45);
    statusBar()->addPermanentWidget(statusRow);

    // Thumbnail size, at the far right where it's always reachable - the same setting also
    // lives in Preferences (as a free-form spinbox), and both funnel through
    // applyThumbnailSizeToGrid() so they can't drift apart.
    thumbSizeCombo_ = new QComboBox(this);
    thumbSizeCombo_->setToolTip(QStringLiteral("Thumbnail size"));
    thumbSizeCombo_->setFocusPolicy(Qt::NoFocus); // must not steal the grid's arrow keys
    syncThumbSizeCombo();
    connect(thumbSizeCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onThumbSizeChanged);
    statusBar()->addPermanentWidget(thumbSizeCombo_);

    thumbStatusButton_ = new QToolButton(this);
    thumbStatusButton_->setAutoRaise(true);
    thumbStatusButton_->setFocusPolicy(Qt::NoFocus);
    thumbStatusButton_->setText(QStringLiteral("●"));
    connect(thumbStatusButton_, &QToolButton::clicked, this, &MainWindow::onThumbStatusClicked);
    statusBar()->addPermanentWidget(thumbStatusButton_);
    updateThumbStatusIndicator();

    loadBookmarks();
    restoreLastDirectory();

    // So arrow keys work immediately on launch without clicking the grid first.
    // Requesting focus on a not-yet-shown widget is fine - Qt applies it once the
    // window actually becomes visible (show() runs after the constructor returns).
    grid_->setFocus();
}

MainWindow::~MainWindow() {
    // Unregister before any member is torn down: remove() emits changed(), every other window
    // rebuilds its Windows submenu from it, and none of them must find a half-destroyed entry.
    //
    // Was `= default;` until multi-window - which is exactly how the first version of this got
    // it wrong. A patch aimed at "the first brace after the destructor" landed in navigateTo()
    // instead, so every folder change silently unregistered the window and the Windows list
    // was permanently empty. The body has to be real for the call to have anywhere to live.
    WindowRegistry::instance().remove(this);
}

void MainWindow::navigateTo(const QString &path, bool forceReindex, bool forceRethumbnail) {

    QString normalized = normalizeForDb(path);
    if (normalized.isEmpty()) return;

    // Scoped to one navigation deliberately. A session-long accumulation buries the thing
    // being investigated ("opening this folder is slow") under every folder visited before it,
    // and the timeline marks are only readable relative to a known t=0.
    invalidateStaleCache(); // different folder - the cached count says nothing about this one
    PIXET_PROF_RESET();
    PIXET_PROF_MARK("nav.begin");
#ifdef PIXET_PROFILE
    // PIXET_PROFILE_DUMP_MS=<n> dumps the report to stderr n milliseconds after each
    // navigation starts, so a measurement run needs no human to click a menu item:
    //   PIXET_PROFILE_DUMP_MS=15000 pixet /some/big/folder 2>prof.txt
    // Without this the only way out is a graceful quit, and a killed process runs no atexit
    // handler - which is how the first attempt at measuring this produced an empty file.
    // Held in a named QByteArray on purpose: qgetenv() returns a temporary, and binding a
    // const char* to its .constData() in an if-init-statement leaves the pointer dangling by
    // the time the condition runs. Which is exactly what happened - the first version of this
    // silently never fired and produced an empty measurement file.
    const QByteArray dumpMs = qgetenv("PIXET_PROFILE_DUMP_MS");
    if (!dumpMs.isEmpty()) {
        QTimer::singleShot(dumpMs.toInt(), this, [] { PIXET_PROF_DUMP(); });
    }
#endif

    // A pending "select this file" (from navigateToInput()) only applies to the
    // navigation that requested it - a plain directory change (tree click, bookmark)
    // must not later resurrect a stale one.
    pendingSelectFileName_.clear();
    currentPath_ = normalized;
    updateWindowTitle(); // the Windows submenu and the Dock/app-switcher label track the folder
    navSettleTimer_.restart(); // bounds how long onTreeDirectoryLoaded() keeps chasing this row - see its member comment
    navThumbTimer_.restart(); // see navThumbTimer_'s member comment
    navThumbsRequested_.clear();
    navThumbsReceived_.clear();
    navFirstThumbMs_ = -1;
    navAllThumbsMs_ = -1;
    // Directory only, never a file path - `normalized` is always a resolved folder
    // here (navigateToInput() resolves a file path to its parent before ever
    // calling this), which is what keeps a pasted/typed file path out of history.
    prefs::addToPathHistory(normalized);
    refreshPathBarHistory();
    pathBar_->setCurrentText(normalized);
    // Skipped when a back/forward button is what's driving this call - the stack
    // already has this entry (that's the whole point of Back/Forward), so
    // re-recording it here would both be redundant and, worse, truncate the
    // "forward" entries a Back just moved away from.
    if (!navigatingViaHistory_) recordNavHistory(normalized);

    // Re-point the live watch (see folderWatcher_'s member comment) at the folder
    // now on screen. addPath() on a path already being watched, or removePaths()
    // on an empty list, are both harmless no-ops, so this doesn't need to special-
    // case "watching nothing yet" (first navigation) or "re-navigating to the same
    // folder".
    if (!folderWatcher_->directories().isEmpty()) folderWatcher_->removePaths(folderWatcher_->directories());
    folderWatcher_->addPath(normalized);
    preview_->clear();
    grid_->setCurrentFolderPath(normalized); // for the hover tooltip's on-demand EXIF read - see its doc comment
    // Show whatever's already cached instantly. For a folder that's new or stale,
    // this shows 0 rows - onFilesListed (fired once Pass A commits, see FolderIndexer)
    // reloads again with the real file list, well before thumbnails are ready.
    gridModel_->setDirectory(normalized);
    updateSelectionStatus(); // folder aggregates changed even before anything's selected

    QModelIndex idx = fsModel_->index(normalized);
    // Reveal this folder's own immediate children on every navigation, regardless of
    // how it was triggered (tree click, bookmark, path bar) - browsing into a folder
    // should show what's inside it in the tree without an extra manual expand click.
    if (idx.isValid()) tree_->expand(idx);

    if (idx.isValid() && tree_->currentIndex() != idx) {
        // This path only runs for navigation that didn't originate from a click
        // already inside the tree (bookmark click, restoreLastDirectory on startup) -
        // a direct tree click already has currentIndex() == idx by the time we get
        // here. Reveal every ancestor first so idx is actually part of the tree's
        // visible row structure (a collapsed parent means visualRect() below would
        // come back invalid).
        for (QModelIndex parent = idx.parent(); parent.isValid(); parent = parent.parent()) {
            if (!tree_->isExpanded(parent)) tree_->expand(parent);
        }

        tree_->setCurrentIndex(idx);

        // Deliberately not tree_->scrollTo(idx): its default EnsureVisible hint only
        // scrolls the minimum needed, and in the process resets horizontal scroll
        // back to the row's start - hiding whatever of a long name was scrolled into
        // view. Position vertically at the top instead; horizontal position untouched.
        // This first attempt is best-effort - expand() above kicks off asynchronous
        // directory listing, so most ancestors' full row counts (and therefore idx's
        // real position) usually aren't known yet. onTreeDirectoryLoaded() reapplies
        // this as each ancestor's listing finishes, but that alone wasn't enough for a
        // deeply nested path under a directory with many siblings (e.g. a home folder
        // full of app-config dirs) - the fixed-delay retries below are what actually
        // catch up in that case, empirically, up to a few seconds out.
        repositionTreeToTop(idx);
        for (int delayMs : {300, 800, 1500, 3000}) {
            QTimer::singleShot(delayMs, this, [this, normalized]() {
                if (normalized == currentPath_) repositionTreeToTop(fsModel_->index(normalized));
            });
        }
    }

    saveLastDirectory(normalized);
    // Drop whatever folder the indexer is still working through before asking for this one.
    // The worker handles one folder at a time, so without this the new folder's file list -
    // Pass A, a fraction of a second's work - waits behind the *whole* of the previous
    // folder's thumbnailing, and the grid the user is looking at stays empty meanwhile.
    // See FolderIndexer::cancelCurrent() for why the abandoned folder loses nothing.
    folderIndexer_->cancelCurrent();
    emit requestIndex(normalized, forceReindex, forceRethumbnail);
    // Whatever RAW files here still need a full render jump ahead of any unrelated
    // backlog elsewhere in the library - see RawRenderer::prioritize().
    emit requestRawRenderPriority(normalized);
}

void MainWindow::repositionTreeToTop(const QModelIndex &idx) {
    if (!idx.isValid()) return;

    // If the row is already visible - or within a couple of rows of the viewport -
    // leave the scroll position alone. Jumping a folder that's already in view (say,
    // the middle of the tree) up to the top on every navigation is disorienting; only
    // reposition when the target genuinely isn't reachable without scrolling. An
    // invalid rect (row not yet part of the laid-out tree structure - still expanding
    // asynchronously) falls through to the repositioning logic below, same as before.
    QRect rowRect = tree_->visualRect(idx);
    if (rowRect.isValid()) {
        int rowHeight = rowRect.height() > 0 ? rowRect.height() : 20;
        QRect tolerance = tree_->viewport()->rect().adjusted(0, -3 * rowHeight, 0, 3 * rowHeight);
        if (tolerance.intersects(rowRect)) return;
    }

    // Trust Qt's own (tested, handles all the nested-expansion/row-height bookkeeping
    // correctly) positioning logic for the vertical part, rather than computing pixel
    // offsets by hand. Only manually intervene for the one specific side effect that's
    // actually a problem: it also resets horizontal scroll to the row's start, hiding
    // whatever of a long name was scrolled into view - so save/restore around it.
    int hScroll = tree_->horizontalScrollBar()->value();
    tree_->scrollTo(idx, QAbstractItemView::PositionAtTop);
    tree_->horizontalScrollBar()->setValue(hScroll);
    // Same class of bug as the thumbnail grid's partial-repaint issue: Qt's internal
    // scroll state ends up correct (verified via visualRect()/scrollbar value) but the
    // viewport doesn't reliably repaint to actually show it without this nudge.
    tree_->viewport()->update();
}

void MainWindow::onTreeDirectoryLoaded(const QString &) {
    // Fires for every directory the tree has ever listed, not just ones relevant to
    // the current navigation - cheap to just recheck unconditionally each time. Only
    // reposition while the tree's own selection still agrees with where we navigated
    // to (so this doesn't fight a selection the user has since changed manually) *and*
    // we're still within the post-navigation settle window - see navSettleTimer_'s
    // doc comment. Without that second check, expanding some unrelated folder (whose
    // currentIndex() never became the browsed one - clicking a branch's expand arrow
    // doesn't select the row) would still snap the view back to currentPath_ every
    // time, indefinitely.
    if (currentPath_.isEmpty()) return;
    if (!navSettleTimer_.isValid() || navSettleTimer_.elapsed() > kTreeSettleWindowMs) return;
    QModelIndex idx = fsModel_->index(currentPath_);
    if (idx.isValid() && tree_->currentIndex() == idx) repositionTreeToTop(idx);
}

void MainWindow::onNavigateFolderRequested(Qt::Key direction) {
    QModelIndex idx = fsModel_->index(currentPath_);
    if (!idx.isValid()) return;

    QModelIndex target;
    switch (direction) {
        case Qt::Key_Up: {
            QModelIndex parent = idx.parent();
            if (idx.row() > 0) target = fsModel_->index(idx.row() - 1, 0, parent);
            break;
        }
        case Qt::Key_Down:
            target = nextSiblingOrAunt(fsModel_, idx);
            break;
        case Qt::Key_Left:
            target = idx.parent();
            break;
        case Qt::Key_Right: {
            // fsModel_'s filter (see constructor) is dirs-only, so any row here is
            // genuinely a subfolder. The current folder is already expanded by the
            // time its thumbnails are on screen (navigateTo() does that), so its
            // children are normally already populated - a folder Ctrl+Right lands on
            // that was never expanded first just no-ops rather than fetching async.
            if (fsModel_->rowCount(idx) > 0) {
                target = fsModel_->index(0, 0, idx);
            } else {
                // Nothing to descend into - continue in tree order instead of just
                // stopping at a leaf, same as Ctrl+Down would from here.
                target = nextSiblingOrAunt(fsModel_, idx);
            }
            break;
        }
        default:
            return;
    }

    if (target.isValid()) navigateTo(fsModel_->filePath(target));
}

void MainWindow::recordNavHistory(const QString &path) {
    if (navHistoryIndex_ >= 0 && navHistoryIndex_ < navHistory_.size() && navHistory_[navHistoryIndex_] == path) {
        return; // re-navigating to the same spot (Refresh, re-clicking the current tree node, ...)
    }

    while (navHistory_.size() > navHistoryIndex_ + 1) navHistory_.removeLast();
    navHistory_.append(path);
    navHistoryIndex_ = navHistory_.size() - 1;
    updateNavButtonsEnabled();
}

void MainWindow::updateNavButtonsEnabled() {
    const bool canBack = navHistoryIndex_ > 0;
    const bool canForward = navHistoryIndex_ >= 0 && navHistoryIndex_ < navHistory_.size() - 1;
    backAction_->setEnabled(canBack);
    forwardAction_->setEnabled(canForward);

    // Tooltips name the actual destination rather than repeating the button's own label,
    // which would say nothing the button doesn't. Where these go is in-memory history, so it
    // isn't guessable from the path bar the way Up is - "Back" is the one button whose target
    // genuinely needs stating.
    //
    // The plain label is kept as the fallback for a disabled button: some styles still show a
    // tooltip on one, and a stale path pointing somewhere you can no longer go would be worse
    // than no path at all.
    backAction_->setToolTip(canBack ? navHistory_[navHistoryIndex_ - 1] : QStringLiteral("Back"));
    forwardAction_->setToolTip(canForward ? navHistory_[navHistoryIndex_ + 1] : QStringLiteral("Forward"));

    const QString parent = parentDirectoryOf(currentPath_);
    upAction_->setEnabled(!parent.isEmpty());
    upAction_->setToolTip(parent.isEmpty() ? QStringLiteral("Up") : parent);
}

QString MainWindow::parentDirectoryOf(const QString &path) const {
    if (path.isEmpty()) return {};
    QDir dir(path);
    // False at a filesystem root, which is exactly when Up should be disabled.
    if (!dir.cdUp()) return {};
    const QString parent = normalizeForDb(dir.absolutePath());
    // Belt and braces: if cdUp() ever succeeds without actually moving (a path that normalises
    // onto itself), treat it as "no parent" rather than offering a no-op navigation.
    return parent == normalizeForDb(path) ? QString() : parent;
}

void MainWindow::onNavigateUp() {
    const QString parent = parentDirectoryOf(currentPath_);
    if (!parent.isEmpty()) navigateTo(parent);
}

void MainWindow::onNavigateBack() {
    if (navHistoryIndex_ <= 0) return;
    --navHistoryIndex_;
    navigatingViaHistory_ = true;
    navigateTo(navHistory_[navHistoryIndex_]);
    navigatingViaHistory_ = false;
    updateNavButtonsEnabled();
}

void MainWindow::onNavigateForward() {
    if (navHistoryIndex_ < 0 || navHistoryIndex_ >= navHistory_.size() - 1) return;
    ++navHistoryIndex_;
    navigatingViaHistory_ = true;
    navigateTo(navHistory_[navHistoryIndex_]);
    navigatingViaHistory_ = false;
    updateNavButtonsEnabled();
}

void MainWindow::onTreeSelectionChanged(const QModelIndex &current) {
    QString path = fsModel_->filePath(current);
    if (!path.isEmpty() && normalizeForDb(path) != currentPath_) navigateTo(path);
}

void MainWindow::onBookmarkClicked(QListWidgetItem *item) {
    QString path = item->data(Qt::UserRole).toString();
    navigateTo(path);
}

void MainWindow::onBookmarksContextMenu(const QPoint &pos) {
    QListWidgetItem *item = bookmarks_->itemAt(pos);
    if (!item) return;

    QMenu menu(this);
    QAction *removeAction = menu.addAction(QStringLiteral("Remove Bookmark"));
    if (menu.exec(bookmarks_->mapToGlobal(pos)) == removeAction) {
        removeBookmark(item->data(Qt::UserRole + 1).toLongLong());
    }
}

void MainWindow::buildItemContextMenu(QMenu &menu, int row, bool fromFullscreen) {
    // Refresh and Force Re-thumbnail moved out to real menus (View and Tools
    // respectively) - this menu is for per-item actions on whatever's under the
    // cursor/selected, not folder-wide ones. Reuses the Edit menu's own
    // cutAction_/copyAction_/pasteAction_ QAction objects directly (Qt actions are
    // meant to be shared across multiple menus/toolbars - same shortcut text,
    // enabled state, and slot everywhere they appear, no duplicate wiring needed)
    // rather than building parallel entries here.
    addFileInfoToContextMenu(menu, row);

    if (!fromFullscreen) {
        QAction *fullscreenAction = menu.addAction(QStringLiteral("View Fullscreen"), this, [this]() {
            if (grid_->currentRow() >= 0) onGridItemActivated(grid_->currentRow());
        });
        fullscreenAction->setEnabled(grid_->currentRow() >= 0);
        menu.addSeparator();
    }

    // Safe to offer from the fullscreen viewer too: it keeps the grid's current row in step
    // with whatever it's showing (see the rowChanged connection in the constructor), so
    // these act on the image on screen.
    menu.addAction(cutAction_);
    menu.addAction(copyAction_);
    menu.addAction(pasteAction_);
    menu.addAction(renameAction_);
    menu.addAction(deleteAction_);

    if (row >= 0) {
        menu.addSeparator();
        QString name = gridModel_->index(row).data(Qt::DisplayRole).toString();
        QString fullPath = joinPathQ(currentPath_, name);
        menu.addAction(QStringLiteral("Copy Name"), this,
                        [name]() { QGuiApplication::clipboard()->setText(name); });
        menu.addAction(QStringLiteral("Copy Path"), this,
                        [fullPath]() { QGuiApplication::clipboard()->setText(fullPath); });
        // Reveals the file itself, selected in its folder, rather than just opening the
        // folder - see shellops::revealInFileManager(). Sits with Copy Path because it
        // answers the same question ("where is this?") by a different route, and reaches
        // the fullscreen viewer for free, since that shares this menu.
        menu.addAction(shellops::revealActionLabel(), this,
                        [fullPath]() { shellops::revealInFileManager(fullPath); });
    }
}

void MainWindow::onGridContextMenu(const QPoint &pos) {
    updateEditActionsEnabled();
    QMenu menu(this);
    buildItemContextMenu(menu, grid_->currentRow(), /*fromFullscreen=*/false);
    menu.exec(grid_->mapToGlobal(pos));
}

void MainWindow::onFullscreenContextMenu(int row, QPoint globalPos) {
    updateEditActionsEnabled();
    // Parented to the viewer, not to `this`: the viewer is a separate top-level window
    // sitting above the main one, and a menu parented to the window underneath can end up
    // rendered behind it.
    QMenu menu(fullscreenViewer_);
    buildItemContextMenu(menu, row, /*fromFullscreen=*/true);
    menu.exec(globalPos);
}

void MainWindow::addFileInfoToContextMenu(QMenu &menu, int row) {
    if (row < 0) return;

    for (const QString &line : grid_->cachedInfoText(row).split(QLatin1Char('\n'))) {
        QAction *a = menu.addAction(line);
        a->setEnabled(false);
    }

    QModelIndex idx = gridModel_->index(row);
    int fmt = idx.data(ThumbGridModel::FormatRole).toInt();
    QString path = joinPathQ(currentPath_, idx.data(Qt::DisplayRole).toString());

    // Synchronous, unlike the hover tooltip's worker-thread fetch - see
    // hoverinfo::readExifDetailsSync's own doc comment on why that tradeoff is fine
    // for a right-click's single, explicitly-requested file.
    pixet::ExifDetails details = hoverinfo::readExifDetailsSync(path, fmt);
    QString exifText = hoverinfo::formatExifDetails(details);
    if (!exifText.isEmpty()) {
        for (const QString &line : exifText.split(QLatin1Char('\n'))) {
            QAction *a = menu.addAction(line);
            a->setEnabled(false);
        }
    }

    if (details.hasGps) {
        // Plain "lat,lon" decimal degrees - the format Google Maps' own search box
        // accepts pasted directly, no need for a maps.google.com URL.
        QString coords = QStringLiteral("%1,%2").arg(details.gpsLatitude, 0, 'f', 6).arg(details.gpsLongitude, 0, 'f', 6);
        menu.addAction(QStringLiteral("Copy GPS Coordinates (%1)").arg(coords), this,
                        [coords]() { QGuiApplication::clipboard()->setText(coords); });
    }

    menu.addSeparator();
}

void MainWindow::onFilesDroppedOnGrid(QStringList localPaths, bool move) {
    if (currentPath_.isEmpty() || localPaths.isEmpty()) return;

    FileOpsWorker::Request req;
    req.id = ++fileOpCounter_;
    req.move = move;
    req.dstDirPath = currentPath_;
    for (const QString &path : localPaths) {
        FileOpsWorker::Item item;
        item.srcPath = path;
        // srcFileId/srcDirId stay 0 - these are external files, not rows pixet's
        // index already knows about.
        req.items << item;
    }
    emit requestFileOpPreflight(req);
}

void MainWindow::onFileOpPreflightReady(FileOpsWorker::Request req, QStringList rejected) {
    if (!rejected.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("%1 item(s) skipped (folders aren't supported yet, or the file is gone)")
                .arg(rejected.size()),
            6000);
    }
    if (req.items.isEmpty()) {
        pendingCutClipboardClear_ = false; // nothing will reach onFileOpFinished() to consume this
        return;
    }

    int totalConflicts = 0;
    for (const auto &item : req.items) {
        if (item.hasConflict) totalConflicts++;
    }

    // One dialog per conflict, resolved entirely here before any I/O starts (see
    // FileOpsWorker's two-stage preflight/execute protocol) - "apply to all
    // remaining" is honored by not asking again for the rest of this loop.
    bool applyToAll = false;
    CollisionDialog::Choice appliedChoice = CollisionDialog::Skip;
    int conflictsSeen = 0;

    for (int i = 0; i < req.items.size(); ++i) {
        FileOpsWorker::Item &item = req.items[i];
        if (!item.hasConflict) continue;

        CollisionDialog::Choice choice;
        if (applyToAll) {
            choice = appliedChoice;
        } else {
            int64_t srcSize = 0, srcMtime = 0;
            // item.conflictSize/conflictMtime describe the *existing destination*
            // file (set by FileOpsWorker::preflight) - the dialog's "Incoming" line
            // needs the source's own stat.
            pixet::statFile(item.srcPath.toStdString(), &srcSize, &srcMtime);
            int remaining = totalConflicts - conflictsSeen - 1;
            bool checkedApplyToAll = false;
            choice = CollisionDialog::ask(this, item.dstName, req.dstDirPath, srcSize, srcMtime, item.conflictSize,
                                          item.conflictMtime, remaining, &checkedApplyToAll);
            if (choice == CollisionDialog::CancelAll) {
                // Cancelling must not clear a Cut clipboard - nothing was actually
                // moved, so the user's cut selection should still be pasteable.
                pendingCutClipboardClear_ = false;
                statusBar()->showMessage(QStringLiteral("File operation cancelled"), 4000);
                return;
            }
            if (checkedApplyToAll) {
                applyToAll = true;
                appliedChoice = choice;
            }
        }
        conflictsSeen++;

        switch (choice) {
            case CollisionDialog::Replace:
                item.resolution = FileOpsWorker::Collision::Replace;
                break;
            case CollisionDialog::Skip:
                item.resolution = FileOpsWorker::Collision::Skip;
                break;
            case CollisionDialog::KeepBoth:
                item.resolution = FileOpsWorker::Collision::KeepBoth;
                break;
            case CollisionDialog::CancelAll:
                break; // unreachable - handled above
        }
    }

    emit requestFileOpExecute(req);
}

void MainWindow::onFileOpProgress(quint64, int done, int total, QString currentName) {
    statusBar()->showMessage(QStringLiteral("%1 / %2 - %3").arg(done).arg(total).arg(currentName));
}

void MainWindow::onFileOpFinished(quint64, QString dstDirPath, QList<qint64> srcFileIds, QStringList addedNames,
                                   int succeeded, int failed, QStringList errors) {
    if (pendingCutClipboardClear_) {
        pendingCutClipboardClear_ = false;
        // Matches Explorer's own behavior of clearing the clipboard once a
        // cut-paste actually completes, so a second Ctrl+V can't try to move
        // already-moved files.
        clipops::clearAfterCutPaste();
    }

    if (dstDirPath == currentPath_) {
        // Safe even for ids that aren't currently loaded (a no-op) - see the
        // signal's doc comment on FileOpsWorker::finished.
        for (qint64 id : srcFileIds) gridModel_->removeFileById(id);

        // Two passes, deliberately not one: insertOrUpdateFileByName()'s return
        // value is only that row's index *at the moment of that one insertion* -
        // inserting "a.jpg" after "b.jpg" shifts "b.jpg" up by one, silently
        // invalidating an index already collected for it. Re-deriving every index
        // fresh via rowForName() only after every insert has landed (rowByName_ is
        // fully rebuilt after each one - see reindexLookups()) is what keeps a
        // multi-file drop from ending up with some of newRows pointing at the wrong
        // row, or a duplicate row, once shifting is accounted for.
        for (const QString &name : addedNames) gridModel_->insertOrUpdateFileByName(name);
        QList<int> newRows;
        for (const QString &name : addedNames) {
            int r = gridModel_->rowForName(name);
            if (r >= 0) newRows << r;
        }
        if (!newRows.isEmpty()) grid_->setSelection(newRows, newRows.last());
        updateSelectionStatus();

        // The new rows are already committed to the DB (state=New) - a plain
        // non-forced index is enough to pick up thumbnails. Pass B runs
        // unconditionally after Indexer's freshness check rather than only inside
        // it, so this never triggers a wasted full re-diff of the rest of the folder.
        emit requestIndex(currentPath_, /*force=*/false, /*forceRethumbnail=*/false);
    } else if (!gridModel_->hasDirectory()) {
        reloadGridPreservingSelection();
    }

    if (failed > 0) {
        statusBar()->showMessage(QStringLiteral("%1 succeeded, %2 failed").arg(succeeded).arg(failed), 8000);
        QMessageBox::warning(this, QStringLiteral("Some items failed"),
                              errors.size() <= 5 ? errors.join(QStringLiteral("\n"))
                                                  : errors.mid(0, 5).join(QStringLiteral("\n")) +
                                                        QStringLiteral("\n...and %1 more").arg(errors.size() - 5));
    } else if (succeeded > 0) {
        statusBar()->showMessage(QStringLiteral("%1 item(s) added").arg(succeeded), 4000);
    }
}

void MainWindow::onFileDeleteFinished(quint64, QList<qint64> removedFileIds, int succeeded, int failed,
                                       QStringList errors) {
    // Safe even for ids that aren't currently loaded (a no-op) - same contract as
    // onFileOpFinished()'s srcFileIds.
    for (qint64 id : removedFileIds) gridModel_->removeFileById(id);
    if (!removedFileIds.isEmpty()) updateSelectionStatus();

    if (failed > 0) {
        statusBar()->showMessage(QStringLiteral("%1 succeeded, %2 failed").arg(succeeded).arg(failed), 8000);
        QMessageBox::warning(this, QStringLiteral("Some items failed"),
                              errors.size() <= 5 ? errors.join(QStringLiteral("\n"))
                                                  : errors.mid(0, 5).join(QStringLiteral("\n")) +
                                                        QStringLiteral("\n...and %1 more").arg(errors.size() - 5));
    } else if (succeeded > 0) {
        statusBar()->showMessage(QStringLiteral("%1 item(s) deleted").arg(succeeded), 4000);
    }
}

void MainWindow::onDragOutRequested() {
    if (currentPath_.isEmpty()) return;

    QStringList paths;
    for (int r : grid_->selectedRows()) {
        QString name = gridModel_->index(r).data(Qt::DisplayRole).toString();
        paths << joinPathQ(currentPath_, name);
    }
    if (paths.isEmpty()) return;

    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString &p : paths) urls << QUrl::fromLocalFile(p);

    auto *mime = new QMimeData;
    mime->setUrls(urls);
    mime->setText(paths.join(QLatin1Char('\n')));
    clipops::markPreferMove(mime);

    auto *drag = new QDrag(grid_); // Qt deletes it once the drag finishes
    drag->setMimeData(mime);

    QModelIndex leadIdx = gridModel_->index(grid_->currentRow());
    QVariant deco = leadIdx.data(Qt::DecorationRole);
    if (deco.canConvert<QPixmap>()) {
        QPixmap pix = deco.value<QPixmap>();
        if (!pix.isNull()) {
            QPixmap scaled = pix.scaled(QSize(96, 96), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            drag->setPixmap(scaled);
            drag->setHotSpot(QPoint(scaled.width() / 2, scaled.height() / 2));
        }
    }

    // Disclosed limitation, not a bug to chase: pixet does not perform the physical
    // move itself here - Explorer/Finder does, as the drop target, and the actual
    // copy-vs-move outcome remains governed by the OS's own conventions at the drop
    // side (same-volume vs. cross-volume, Ctrl/Shift overrides). markPreferMove()
    // and Qt::MoveAction below are hints a target is free to ignore.
    Qt::DropAction result = drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
    if (result == Qt::MoveAction) {
        // Explorer/Finder already performed the move; pixet only has to notice.
        // Reusing the already-tested by-name diff (a forced non-recursive rescan)
        // is both correct and simpler than independently guessing which of these
        // specific files are now gone - and it can't accidentally delete rows for
        // files that are still there. Feeds into onFilesListed() ->
        // reloadGridPreservingSelection(), so this doesn't reintroduce
        // scroll-position loss. Never gets the thumb_id-preservation win drag-out
        // (unlike Cut/Paste and drag-in) - pixet never learns the destination path.
        emit requestIndex(currentPath_, /*force=*/true, false);
    }
}

void MainWindow::onGridItemActivated(int row) {
    if (row < 0) return;
    QModelIndex index = gridModel_->index(row);

    // Video has no playback engine in the fullscreen viewer (poster frame only, see
    // FullscreenViewer's class comment) - activating one launches an actual player
    // instead, per the user's own preference, rather than opening the viewer at all.
    if ((pixet::Format)index.data(ThumbGridModel::FormatRole).toInt() == pixet::Format::Video) {
        QString filePath = joinPathQ(currentPath_, index.data(Qt::DisplayRole).toString());
        if (prefs::useSystemVideoPlayer() || prefs::customVideoPlayerPath().isEmpty()) {
            // Also the fallback when "Custom" is selected but no path was ever set -
            // silently doing nothing on activation would look like the double-click
            // just didn't register.
            QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
        } else {
            const QString player = prefs::customVideoPlayerPath();
            bool started = false;
            if (player.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive) || QFileInfo(player).isDir()) {
                // A macOS "application" the user picks in a file dialog is an .app bundle,
                // i.e. a *directory* - startDetached() can't exec it, and the native open
                // panel hands one back happily since it treats bundles as selectable files.
                // `open -a` is the supported way to hand a document to a bundled app.
                started = QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                                   {QStringLiteral("-a"), player, filePath});
            } else {
                started = QProcess::startDetached(player, {filePath});
            }
            // Checked rather than discarded: a misconfigured player path would otherwise
            // make a double-click do nothing at all with no indication why - the same
            // failure mode the empty-path fallback above avoids.
            if (!started) {
                statusBar()->showMessage(
                    QStringLiteral("Could not launch video player: %1  (check Preferences)").arg(player), 8000);
            }
        }
        return;
    }

    fullscreenViewer_->openAt(gridModel_, currentPath_, row);
}

void MainWindow::onPathBarReturnPressed() { navigateToInput(pathBar_->currentText()); }

void MainWindow::onPathBarHistoryActivated() { navigateToInput(pathBar_->currentText()); }

void MainWindow::onGridSelectionChanged() {
    PIXET_PROF_SCOPE("ui.onGridSelectionChanged");
    PIXET_PROF_MARK("ui.selectionChanged");
    currentPreviewBpp_ = 0; // stale from whatever was selected before - cleared until this item's preview lands
    updateSelectionStatus();
    // Same class of unreliable-partial-repaint issue as thumbnail loading (see the
    // thumbReady connection below) - Qt's internal old/new-current-item rect updates
    // aren't always enough to actually repaint the selection border, most noticeably
    // when moving the selection with arrow keys. Cheap and coalesced, same reasoning
    // as elsewhere in this file.
    grid_->viewport()->update();

    // Keep a visible fullscreen viewer on whatever the grid is showing. The viewer can be
    // moved aside or switched to a window (F), which leaves the grid clickable behind it,
    // and the two must not end up displaying different images. Driven from here, the one
    // place a lead-row change already lands, and handed currentPath_ so a folder change is
    // carried across as well; the viewer itself decides whether anything needs doing (see
    // followGridSelection).
    fullscreenViewer_->followGridSelection(currentPath_, grid_->currentRow());

    QModelIndex idx = gridModel_->index(grid_->currentRow());
    if (!idx.isValid()) {
        pendingPreviewPath_.clear();
        pendingPreviewIsCtrlHover_ = false;
        preview_->clear();
        pathBar_->setCurrentText(currentPath_);
        return;
    }

    QString name = idx.data(Qt::DisplayRole).toString();
    pendingPreviewFmt_ = idx.data(ThumbGridModel::FormatRole).toInt();
    pendingPreviewPath_ = joinPathQ(currentPath_, name);
    pendingPreviewIsCtrlHover_ = false;
    // Display only - the full file path never gets recorded into history (see
    // prefs::pathHistory()'s doc comment). setCurrentText() on an editable combo
    // just sets the edit line's text; it doesn't need to match (or become) a
    // dropdown item.
    pathBar_->setCurrentText(pendingPreviewPath_);

    previewDebounce_->start();
}

void MainWindow::onGridCtrlHoverChanged(int row) {
    if (row < 0) {
        // Ctrl released / mouse left the grid / hovered empty space - go back to
        // showing whatever's actually selected. Reuses onGridSelectionChanged()
        // wholesale rather than a separate revert path: the real selection never
        // changed, so recomputing its status-bar/path-bar/viewport state is
        // redundant but harmless, and this way there's exactly one place that knows
        // how to derive "what should the preview show" from the actual selection.
        onGridSelectionChanged();
        return;
    }

    QModelIndex idx = gridModel_->index(row);
    if (!idx.isValid()) return;

    // Deliberately skips onGridSelectionChanged()'s other side effects (status bar,
    // path bar text, viewport repaint) - the actual selection hasn't changed, only
    // the preview pane should react to a Ctrl-hover peek.
    pendingPreviewFmt_ = idx.data(ThumbGridModel::FormatRole).toInt();
    pendingPreviewPath_ = joinPathQ(currentPath_, idx.data(Qt::DisplayRole).toString());
    pendingPreviewIsCtrlHover_ = true;
    previewDebounce_->start();
}

void MainWindow::triggerPreviewRequest() {
    if (pendingPreviewPath_.isEmpty()) return;
    currentPreviewRequestId_ = ++previewRequestCounter_;
    currentPreviewIsCtrlHover_ = pendingPreviewIsCtrlHover_;
    previewDecoder_->requestPreview(currentPreviewRequestId_, pendingPreviewPath_, pendingPreviewFmt_,
                                     preview_->preferredTargetLongEdge());
}

void MainWindow::onPreviewReady(qint64 requestId, QImage image) {
    if (requestId != currentPreviewRequestId_) return; // superseded by a newer selection
    preview_->setImage(image);
    if (currentPreviewIsCtrlHover_) return; // a hover peek, not the real selection - see the member comment
    // QImage::depth() (bits per pixel, e.g. 24 for 8-bit/channel RGB) is the only place
    // this app has bit-depth info without adding a DB column - see the member comment.
    currentPreviewBpp_ = image.isNull() ? 0 : image.depth();
    updateSelectionStatus();
}

void MainWindow::onAddBookmark() {
    if (!currentPath_.isEmpty()) addBookmark(currentPath_);
}

bool MainWindow::isBookmarked(const QString &path) const {
    if (!db_ || path.isEmpty()) return false;
    auto sel = db_->prepare("SELECT 1 FROM bookmarks WHERE path=? LIMIT 1");
    sel.bind(1, path.toStdString());
    return sel.step();
}

void MainWindow::onTreeContextMenu(const QPoint &pos) {
    // customContextMenuRequested reports `pos` in the *view's* coordinates (the policy is set
    // on tree_, so tree_ is what receives the event), but indexAt() measures from the
    // viewport - which is inset by the frame border. Converting rather than passing `pos`
    // straight through is what keeps a click near a row boundary from resolving to the row
    // above it.
    const QPoint viewportPos = tree_->viewport()->mapFrom(tree_, pos);
    QModelIndex idx = tree_->indexAt(viewportPos);
    if (!idx.isValid()) return; // right-click on empty space below the tree

    // Normalized the same way every other path that reaches the DB is, so the
    // already-bookmarked check compares like with like - QFileSystemModel hands back
    // forward slashes on every platform, and on macOS a different Unicode normalization
    // form than the DB stores.
    const QString path = normalizeForDb(fsModel_->filePath(idx));
    if (path.isEmpty()) return;

    QMenu menu(this);
    // Leading disabled entry naming the target, matching the grid's own context menu - the
    // right-clicked folder isn't necessarily the selected one, so saying which folder this
    // is about is worth a line.
    QAction *header = menu.addAction(QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName());
    header->setEnabled(false);
    menu.addSeparator();

    if (isBookmarked(path)) {
        QAction *already = menu.addAction(QStringLiteral("Already in Bookmarks"));
        already->setEnabled(false);
    } else {
        menu.addAction(QStringLiteral("Add to Bookmarks"), this, [this, path]() { addBookmark(path); });
    }

    menu.addSeparator();
    menu.addAction(QStringLiteral("Create Folder..."), this, [this, path]() { createFolderIn(path); });

    menu.addSeparator();
    // The right-clicked folder, not the one being browsed - those differ whenever someone
    // right-clicks a folder they haven't navigated into yet, which is most of the time in
    // a tree.
    menu.addAction(shellops::revealActionLabel(), this,
                    [path]() { shellops::revealInFileManager(path); });

    menu.exec(tree_->mapToGlobal(pos)); // `pos` is view-relative, so map from the view
}

void MainWindow::onPreviewContextMenu(const QPoint &pos) {
    // The pane shows the grid's current selection, so that is what the menu acts on. No
    // selection means nothing is on display and there is nothing to reveal - showing an
    // empty or all-disabled menu would be worse than not opening one.
    QModelIndex idx = gridModel_->index(grid_->currentRow());
    if (!idx.isValid()) return;

    const QString name = idx.data(Qt::DisplayRole).toString();
    if (name.isEmpty()) return;
    const QString filePath = joinPathQ(currentPath_, name);

    QMenu menu(this);
    // Named for the same reason the tree and grid menus name their target: the pane has no
    // visible selection of its own, so without this the menu is a verb with no subject.
    QAction *header = menu.addAction(name);
    header->setEnabled(false);
    menu.addSeparator();
    menu.addAction(shellops::revealActionLabel(), this,
                    [filePath]() { shellops::revealInFileManager(filePath); });

    menu.exec(preview_->mapToGlobal(pos));
}

void MainWindow::onFocusAddressBar() {
    pathBar_->setFocus(Qt::ShortcutFocusReason);
    if (QLineEdit *le = pathBar_->lineEdit()) le->selectAll();
}

void MainWindow::onRefresh() {
    if (!currentPath_.isEmpty()) navigateTo(currentPath_, /*forceReindex=*/true);
}

void MainWindow::onForceRethumbnail() {
    if (!currentPath_.isEmpty()) navigateTo(currentPath_, /*forceReindex=*/true, /*forceRethumbnail=*/true);
}

void MainWindow::syncThumbSizeCombo() {
    if (!thumbSizeCombo_) return;
    const int current = prefs::thumbnailIconSize();

    // Shared with the Preferences dialog's own size list - see prefs::thumbnailSizeChoices().
    const QList<int> sizes = prefs::thumbnailSizeChoices(current);

    // Rebuilding fires currentIndexChanged, which would look exactly like the user picking
    // an entry and trigger a re-thumbnail.
    QSignalBlocker blocker(thumbSizeCombo_);
    thumbSizeCombo_->clear();
    for (int px : sizes) thumbSizeCombo_->addItem(QStringLiteral("%1 px").arg(px), px);
    thumbSizeCombo_->setCurrentIndex((int)sizes.indexOf(current));
}

int MainWindow::displayThumbLongEdge() const {
    return qMax(1, qRound(prefs::thumbnailIconSize() * devicePixelRatioF()));
}

int MainWindow::countStaleThumbnails(const QString &dirPath) const {
    PIXET_PROF_SCOPE("ui.countStaleThumbnails");
    if (!db_ || dirPath.isEmpty()) return 0;

    // Measured at 88ms on a 1280-file folder, and updateThumbStatusIndicator() runs it twice
    // per navigation (once when Pass A lists the files, once when indexing finishes) - so it
    // costs ~176ms of every folder change, the largest single cost in a warm navigation.
    //
    // It's expensive for a structural reason: the join reaches into thumbs.db for t.w/t.h on
    // every file in the folder, which is one random page read per file in a 185MB blob
    // database. Caching is the cheap fix; the real one is denormalising the thumbnail's long
    // edge into files, so the question can be answered from index.db alone.
    //
    // Invalidated by invalidateStaleCache() wherever thumbnails can actually have changed, so
    // the second call after a *warm* navigation (where Pass B did nothing) is free while a
    // folder that really did get re-thumbnailed still recomputes.
    const int needed = displayThumbLongEdge();
    if (staleCacheValue_ >= 0 && staleCachePath_ == dirPath && staleCacheNeeded_ == needed) {
        return staleCacheValue_;
    }

    auto dirSel = db_->prepare("SELECT id FROM dirs WHERE path=?");
    dirSel.bind(1, dirPath.toStdString());
    if (!dirSel.step()) return 0; // folder not indexed yet - nothing to be stale
    int64_t dirId = dirSel.columnInt64(0);

    // Judged against what the *display* needs, not against prefs::thumbnailTargetLongEdge()
    // (which is deliberately ~2x for headroom). Using the generation target would light this
    // red whenever a folder happened to be indexed under a smaller setting, even when its
    // blobs are perfectly adequate for what's on screen - blobs stored at 320 shown at 240
    // look fine and shouldn't nag.
    //
    // MIN(needed, original long edge) is what stops a legitimately small photo from counting
    // as stale forever: a 200px original can never produce a 480px thumbnail, so it isn't
    // behind - it's already as good as it will ever get. files.width/height are the
    // ORIGINAL dimensions (thumbs.thumbs.w/h are the thumbnail's), which is exactly the
    // distinction this relies on.
    auto sel = db_->prepare(
        "SELECT COUNT(*) FROM files f JOIN thumbs.thumbs t ON t.id = f.thumb_id "
        "WHERE f.dir_id = ? AND MAX(t.w, t.h) < MIN(?, MAX(f.width, f.height))");
    sel.bind(1, dirId);
    sel.bind(2, (int64_t)needed);
    if (!sel.step()) return 0;

    staleCachePath_ = dirPath;
    staleCacheNeeded_ = needed;
    staleCacheValue_ = (int)sel.columnInt64(0);
    return staleCacheValue_;
}

void MainWindow::invalidateStaleCache() { staleCacheValue_ = -1; }

void MainWindow::updateThumbStatusIndicator() {
    PIXET_PROF_SCOPE("ui.updateThumbStatusIndicator");
    if (!thumbStatusButton_) return;

    const int stale = countStaleThumbnails(currentPath_);
    const bool upToDate = (stale == 0);

    // "Auto rethumb" (Preferences > Maintenance): regenerate on arrival instead of waiting
    // to be clicked. Guarded on the path so it fires once per folder - the re-thumbnail
    // navigates back to this same folder, whose completion calls straight back into here,
    // and without the guard a folder that somehow stayed stale would loop forever.
    if (!upToDate && prefs::autoRethumbnail() && autoRethumbPath_ != currentPath_) {
        autoRethumbPath_ = currentPath_;
        onForceRethumbnail();
        return; // the indicator gets refreshed again when that pass reports back
    }
    // Colour via stylesheet on a text bullet rather than an icon, matching the "×" reset
    // buttons in PreferencesDialog - it scales with the UI font instead of needing a pixmap
    // per device pixel ratio.
    thumbStatusButton_->setStyleSheet(upToDate ? QStringLiteral("QToolButton { color: #3fb950; }")
                                                : QStringLiteral("QToolButton { color: #f85149; }"));
    thumbStatusButton_->setToolTip(
        upToDate ? QStringLiteral("Thumbnails here are up to date for this size.\nClick to regenerate anyway.")
                  : QStringLiteral("%1 thumbnail(s) here were made for a smaller size and will look soft.\n"
                                    "Click to regenerate this folder.")
                        .arg(stale));
}

void MainWindow::applyThumbnailSizeToGrid() {
    const int size = prefs::thumbnailIconSize();
    grid_->setIconSize(QSize(size, size)); // recomputes the grid layout itself

    if (currentPath_.isEmpty()) {
        updateThumbStatusIndicator();
        return;
    }

    // Only pay for a re-thumbnail when the blobs on disk genuinely can't satisfy the new
    // size. Shrinking never needs one (the stored thumbnails are already larger), and nor
    // does an increase still covered by what's stored - both stay instant, and the reload
    // below is enough because already-decoded pixmaps in the model were sized for the *old*
    // icon size and have to be re-requested from ThumbLoader regardless.
    if (countStaleThumbnails(currentPath_) > 0) {
        navigateTo(currentPath_, /*forceReindex=*/true, /*forceRethumbnail=*/true);
    } else {
        reloadGridPreservingSelection();
    }
    updateThumbStatusIndicator();
}

void MainWindow::onThumbSizeChanged() {
    if (!thumbSizeCombo_) return;
    const int size = thumbSizeCombo_->currentData().toInt();
    if (size <= 0 || size == prefs::thumbnailIconSize()) return;
    prefs::setThumbnailIconSize(size);
    applyThumbnailSizeToGrid();
}

void MainWindow::onThumbStatusClicked() {
    // Deliberately works when green too: that's a force-regenerate, the same action as the
    // grid context menu's "Force Re-thumbnail This Folder", in the place the dot has just
    // drawn attention to.
    onForceRethumbnail();
}

void MainWindow::onPreferences() {
    PreferencesDialog dlg(this);
    connect(&dlg, &PreferencesDialog::reindexRequested, this, [this]() { emit requestFullReindex(); });
    connect(&dlg, &PreferencesDialog::thumbnailSizeChanged, this, [this]() {
        // Same path the status-bar drop-down takes, so the two controls can't behave
        // differently for the same setting - including the "only re-thumbnail when the
        // stored blobs are actually too small" rule.
        syncThumbSizeCombo();
        applyThumbnailSizeToGrid();
    });
    connect(&dlg, &PreferencesDialog::databaseStatsRequested, this, [this, &dlg]() {
        // Parented to the Preferences dialog it was opened from, so it centres on that
        // rather than on the main window behind it. Safe to capture &dlg: this connection
        // only ever fires while dlg.exec() below is running.
        DatabaseStatsDialog stats(*db_, &dlg);
        stats.exec();
    });
    connect(&dlg, &PreferencesDialog::nukeDatabaseRequested, this, &MainWindow::nukeDatabase);
    connect(&dlg, &PreferencesDialog::rawCacheSettingsChanged, this, []() { prefs::applyRawCacheSettings(); });
    dlg.exec();
    // Cheap enough to always re-apply regardless of whether the keybindings editor
    // actually changed anything (Cancel discards its own edits before this point) -
    // no need for a dedicated changed-detection signal just for this.
    applyKeyBindingShortcuts();
}

void MainWindow::onChooseFolder() {
    QString start = currentPath_.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                                            : currentPath_;
    QString chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose Folder"), start);
    if (!chosen.isEmpty()) navigateTo(normalizeForDb(chosen));
}

void MainWindow::onAbout() {
    // A hand-built QDialog rather than QMessageBox::about(), which can show text and nothing
    // else. This needs an icon, a clickable link and a button that opens a folder, and fighting
    // a QMessageBox into all three (its label's openExternalLinks is off and only reachable by
    // findChild on an internal object name) is more fragile than just building the dialog.
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("About pixet"));

    // The PNG from the compiled-in Qt resource, not the bundle's pixet.icns and not the .ico.
    // PNG decoding lives in QtGui itself, while both ICNS and ICO are runtime plugins that
    // scripts/deploy-mac.sh prunes - so either of those would render as an empty space in a
    // deployed build while looking fine in a dev one. Sized against the device pixel ratio so
    // it isn't a soft upscale on a Retina display.
    auto *iconLabel = new QLabel(&dlg);
    const QIcon appIcon(QStringLiteral(":/pixet.png"));
    iconLabel->setPixmap(appIcon.pixmap(QSize(72, 72), devicePixelRatioF()));
    iconLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

    // The build's exact provenance, so a bug report from a given binary identifies the source
    // it came from. "modified" matters as much as the hash: a dirty build is *not* that commit,
    // and showing the hash alone would claim otherwise.
    QString build = QStringLiteral("%1 &middot; %2")
                        .arg(QString::fromLatin1(pixet::gitCommit()),
                             QString::fromLatin1(pixet::buildTime()));
    if (pixet::gitDirty()) {
        build += QStringLiteral(" <span style='color:#f0883e'>(modified)</span>");
    }

    // Qt's runtime version is worth showing: the AGL link workaround in
    // src/app/CMakeLists.txt is specific to 6.8, so knowing which Qt a given build was made
    // against is the first thing anyone would want when it eventually stops being needed.
    const QString prefsPath = prefs::settingsFilePath();
    auto *text = new QLabel(&dlg);
    text->setTextFormat(Qt::RichText);
    // Both flags are needed and do different jobs: TextBrowserInteraction makes the link
    // clickable and the paths selectable for copying, openExternalLinks is what actually hands
    // the URL to the browser instead of emitting linkActivated into the void.
    text->setTextInteractionFlags(Qt::TextBrowserInteraction);
    text->setOpenExternalLinks(true);
    text->setText(QStringLiteral("<b style='font-size:15pt'>pixet %1</b><br>"
                                  "Photo and video viewer<br><br>"
                                  "Build: %2<br>"
                                  "Qt %3<br><br>"
                                  "Cache: <code>%4</code><br>"
                                  "Settings: <code>%5</code><br><br>"
                                  "<a href='%6'>%6</a>")
                       .arg(QString::fromLatin1(pixet::version()), build,
                            QString::fromLatin1(qVersion()),
                            QString::fromStdString(pixet::appDataDir()), prefsPath,
                            QStringLiteral("https://github.com/naniBox/pixet")));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    // Opens the containing folder rather than revealing the .ini itself: "open that path" is
    // what was asked for, and handing a plain file URL to the shell would have the OS try to
    // launch pixet.ini in whatever is registered for .ini files.
    QPushButton *revealButton =
        buttons->addButton(QStringLiteral("Open Settings Folder"), QDialogButtonBox::ActionRole);
    connect(revealButton, &QPushButton::clicked, this, [prefsPath]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(prefsPath).absolutePath()));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto *row = new QHBoxLayout();
    row->addWidget(iconLabel);
    row->addSpacing(12);
    row->addWidget(text, /*stretch=*/1);

    auto *layout = new QVBoxLayout(&dlg);
    layout->addLayout(row);
    layout->addWidget(buttons);
    dlg.exec();
}

void MainWindow::openSystemPath(const QString &path) {
    if (path.isEmpty()) return;
    // The request came from outside the app (Finder, the Dock, `open -a`), so the window is
    // very likely behind something or freshly launched - bring it forward, or the file
    // silently opens into a window the user can't see.
    show();
    raise();
    activateWindow();
    navigateToInput(path);
}

void MainWindow::applyKeyBindingShortcuts() {
    refreshAction_->setShortcut(keybindings::binding(keybindings::Action::Refresh));
    toggleSidePanelAction_->setShortcut(keybindings::binding(keybindings::Action::ToggleSidePanel));
    addBookmarkAction_->setShortcut(keybindings::binding(keybindings::Action::AddBookmark));
    focusAddressBarAction_->setShortcut(keybindings::binding(keybindings::Action::FocusAddressBar));
    renameAction_->setShortcut(keybindings::binding(keybindings::Action::Rename));
}

QLineEdit *MainWindow::focusedLineEdit() const { return qobject_cast<QLineEdit *>(QApplication::focusWidget()); }

void MainWindow::onEditSelectAll() {
    if (QLineEdit *le = focusedLineEdit()) {
        le->selectAll();
        return;
    }
    grid_->selectAll();
}

void MainWindow::onEditCopy() {
    if (QLineEdit *le = focusedLineEdit()) {
        le->copy();
        return;
    }
    QStringList paths;
    for (int r : grid_->selectedRows()) {
        QString name = gridModel_->index(r).data(Qt::DisplayRole).toString();
        paths << joinPathQ(currentPath_, name);
    }
    if (paths.isEmpty()) return;
    clipops::writeFiles(paths, /*cut=*/false);
    statusBar()->showMessage(QStringLiteral("%1 item(s) copied").arg(paths.size()), 3000);
}

void MainWindow::onEditCut() {
    if (QLineEdit *le = focusedLineEdit()) {
        le->cut();
        return;
    }
    QStringList paths;
    for (int r : grid_->selectedRows()) {
        QString name = gridModel_->index(r).data(Qt::DisplayRole).toString();
        paths << joinPathQ(currentPath_, name);
    }
    if (paths.isEmpty()) return;
    // No I/O here - Cut only marks the clipboard. The actual move happens on
    // Paste, exactly like Explorer's own Ctrl+X.
    clipops::writeFiles(paths, /*cut=*/true);
    statusBar()->showMessage(QStringLiteral("%1 item(s) cut").arg(paths.size()), 3000);
}

void MainWindow::onEditPaste() {
    if (QLineEdit *le = focusedLineEdit()) {
        le->paste();
        return;
    }
    if (currentPath_.isEmpty()) return;

    clipops::Files files = clipops::read();
    if (files.paths.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Nothing to paste"), 3000);
        return;
    }

    FileOpsWorker::Request req;
    req.id = ++fileOpCounter_;
    req.move = files.isCut;
    req.dstDirPath = currentPath_;
    for (const QString &path : files.paths) {
        FileOpsWorker::Item item;
        item.srcPath = path;

        // If this happens to be a file pixet's own index already knows about (e.g.
        // Cut in one pixet-browsed folder, Paste in another), populate srcFileId/
        // srcDirId so the smart move (see fileops::execute()) can preserve its
        // thumbnail rather than re-thumbnailing from scratch - the same lookup
        // navigateTo() already does to turn a filesystem path into a DB row.
        QString normalized = normalizeForDb(path);
        QFileInfo info(normalized);
        auto dirSel = db_->prepare("SELECT id FROM dirs WHERE path=?");
        dirSel.bind(1, normalizeForDb(info.absolutePath()).toStdString());
        if (dirSel.step()) {
            int64_t dirId = dirSel.columnInt64(0);
            auto fileSel = db_->prepare("SELECT id FROM files WHERE dir_id=? AND name=?");
            fileSel.bind(1, dirId);
            fileSel.bind(2, info.fileName().toStdString());
            if (fileSel.step()) {
                item.srcFileId = fileSel.columnInt64(0);
                item.srcDirId = dirId;
            }
        }

        req.items << item;
    }

    pendingCutClipboardClear_ = files.isCut;
    emit requestFileOpPreflight(req);
}

void MainWindow::onEditDelete() {
    if (focusedLineEdit()) return; // let the line edit delete a character instead of files

    QList<int> rows = grid_->selectedRows();
    if (rows.isEmpty() || currentPath_.isEmpty()) return;

    int64_t dirId = 0;
    auto dirSel = db_->prepare("SELECT id FROM dirs WHERE path=?");
    dirSel.bind(1, currentPath_.toStdString());
    if (dirSel.step()) dirId = dirSel.columnInt64(0);

    FileOpsWorker::DeleteRequest req;
    req.id = ++fileOpCounter_;
    QStringList names;
    for (int r : rows) {
        QModelIndex idx = gridModel_->index(r);
        QString name = idx.data(Qt::DisplayRole).toString();

        FileOpsWorker::DeleteItem item;
        item.path = joinPathQ(currentPath_, name);
        item.fileId = idx.data(ThumbGridModel::FileIdRole).toLongLong();
        item.dirId = dirId;
        req.items << item;
        names << name;
    }

#ifdef Q_OS_MACOS
    const QString trashWord = QStringLiteral("Trash");
#else
    const QString trashWord = QStringLiteral("Recycle Bin");
#endif
    QString question = names.size() == 1
                            ? QStringLiteral("Move \"%1\" to the %2?").arg(names.first(), trashWord)
                            : QStringLiteral("Move %1 items to the %2?").arg(names.size()).arg(trashWord);
    auto choice = QMessageBox::question(this, QStringLiteral("Delete"), question, QMessageBox::Yes | QMessageBox::No,
                                         QMessageBox::No);
    if (choice != QMessageBox::Yes) return;

    emit requestFileDelete(req);
}

void MainWindow::onEditRename() {
    if (focusedLineEdit()) return;
    // Bulk rename is out of scope - see updateEditActionsEnabled()'s comment.
    if (grid_->selectionCount() != 1) return;

    QModelIndex idx = gridModel_->index(grid_->currentRow());
    if (!idx.isValid()) return;

    QString oldName = idx.data(Qt::DisplayRole).toString();
    QString newName = promptRename(oldName);
    if (newName.isEmpty() || newName == oldName) return;

    int64_t dirId = 0;
    auto dirSel = db_->prepare("SELECT id FROM dirs WHERE path=?");
    dirSel.bind(1, currentPath_.toStdString());
    if (dirSel.step()) dirId = dirSel.columnInt64(0);

    FileOpsWorker::Request req;
    req.id = ++fileOpCounter_;
    req.move = true;
    req.dstDirPath = currentPath_;

    FileOpsWorker::Item item;
    item.srcPath = joinPathQ(currentPath_, oldName);
    item.dstName = newName;
    item.srcFileId = idx.data(ThumbGridModel::FileIdRole).toLongLong();
    item.srcDirId = dirId;
    req.items << item;

    // Same preflight -> CollisionDialog -> execute pipeline as Paste/drag-in - a
    // rename onto a name that already exists in this folder gets exactly the same
    // Replace/Skip/Keep Both treatment, rather than a separate rename-specific
    // collision path.
    emit requestFileOpPreflight(req);
}

QString MainWindow::promptRename(const QString &currentName) {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Rename"));

    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(QStringLiteral("New name:"), &dlg));

    auto *lineEdit = new QLineEdit(currentName, &dlg);
    // Pre-select just the stem (before the last '.'), matching Explorer's own rename
    // UX - typing immediately replaces the name but leaves the extension alone unless
    // the user selects further. A leading-dot dotfile with no other '.' has no
    // extension to preserve, so the whole name is selected instead.
    int dot = currentName.lastIndexOf(QLatin1Char('.'));
    int stemLen = dot > 0 ? dot : currentName.size();
    lineEdit->setSelection(0, stemLen);
    layout->addWidget(lineEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QPushButton *okButton = buttons->button(QDialogButtonBox::Ok);
    okButton->setEnabled(!currentName.trimmed().isEmpty());
    connect(lineEdit, &QLineEdit::textChanged, okButton,
            [okButton](const QString &text) { okButton->setEnabled(!text.trimmed().isEmpty()); });
    connect(lineEdit, &QLineEdit::returnPressed, &dlg, [&dlg, okButton]() {
        if (okButton->isEnabled()) dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    lineEdit->setFocus();
    if (dlg.exec() != QDialog::Accepted) return QString();
    return lineEdit->text().trimmed();
}

void MainWindow::createFolderIn(const QString &parentPath) {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Create Folder"));

    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(QStringLiteral("New folder in %1:").arg(QDir(parentPath).dirName()), &dlg));

    auto *lineEdit = new QLineEdit(QStringLiteral("New Folder"), &dlg);
    lineEdit->selectAll();
    layout->addWidget(lineEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QPushButton *okButton = buttons->button(QDialogButtonBox::Ok);
    connect(lineEdit, &QLineEdit::textChanged, okButton,
            [okButton](const QString &text) { okButton->setEnabled(!text.trimmed().isEmpty()); });
    connect(lineEdit, &QLineEdit::returnPressed, &dlg, [&dlg, okButton]() {
        if (okButton->isEnabled()) dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    lineEdit->setFocus();
    if (dlg.exec() != QDialog::Accepted) return;

    const QString name = lineEdit->text().trimmed();
    if (name.isEmpty()) return;

    // Separators are rejected rather than quietly creating a chain of directories: "a/b"
    // typed into a field labelled "New folder" reads as one folder with an odd name, and
    // silently making two (or worse, escaping the parent entirely with "..") is not what
    // was asked for.
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
        name == QStringLiteral(".") || name == QStringLiteral("..")) {
        QMessageBox::warning(this, QStringLiteral("Create Folder"),
                              QStringLiteral("A folder name can't contain \\ or / characters."));
        return;
    }

    QDir parent(parentPath);
    const QString fullPath = parent.filePath(name);
    if (QFileInfo::exists(fullPath)) {
        QMessageBox::warning(this, QStringLiteral("Create Folder"),
                              QStringLiteral("\"%1\" already exists here.").arg(name));
        return;
    }

    // mkdir(), not mkpath(): mkpath succeeds silently when the directory is already there
    // and would create intermediate levels, both of which hide exactly the mistakes this
    // dialog should be reporting.
    if (!parent.mkdir(name)) {
        // No errno to report from QDir, so the message says what was attempted rather than
        // guessing at a cause. Read-only location and a name the filesystem rejects (a
        // reserved device name, a trailing dot on Windows) both land here.
        QMessageBox::warning(this, QStringLiteral("Create Folder"),
                              QStringLiteral("Could not create \"%1\" in %2.").arg(name, parentPath));
        return;
    }

    // The tree is a QFileSystemModel and picks the new directory up on its own watch, but
    // not necessarily before this returns - so expand the parent and select the child once
    // the model has caught up, rather than leaving the user to find what they just made.
    QModelIndex parentIdx = fsModel_->index(parentPath);
    if (parentIdx.isValid()) tree_->expand(parentIdx);
    QTimer::singleShot(0, this, [this, fullPath]() {
        QModelIndex idx = fsModel_->index(fullPath);
        if (!idx.isValid()) return;
        tree_->setCurrentIndex(idx);
        tree_->scrollTo(idx);
    });
}

void MainWindow::onToggleSidePanel() {
    // QSplitter automatically gives a hidden child's space to the remaining visible
    // one(s) - grid_ expands to fill the whole width - and ThumbGridView already
    // recomputes its column count on any resize (see relayout()), so hiding/showing
    // leftPanel_ handles the layout itself. But the scroll position is a raw pixel
    // offset, not row-aware - the same offset lands on a completely different set of
    // rows once the column count changes (e.g. 3-wide -> 6-wide), so the current
    // selection can end up scrolled off screen. Re-center on it explicitly, deferred
    // to the next event loop turn since QSplitter's redistribution is a posted
    // layout request rather than synchronous - scrolling immediately here would
    // still measure against the pre-toggle viewport width/column count.
    leftPanel_->setVisible(!leftPanel_->isVisible());
    int row = grid_->currentRow();
    if (row >= 0) QTimer::singleShot(0, grid_, [this, row]() { grid_->scrollToRow(row, /*center=*/true); });
}

void MainWindow::onToggleHoverInfo(bool enabled) {
    prefs::setHoverInfoEnabled(enabled);
    if (!enabled) grid_->hideHoverTooltip();
}

void MainWindow::updateSortTooltips() {
    // The toolbar buttons are icon-only, so the tooltip is the only thing that can explain
    // them - and now also the only place the second-click-reverses behaviour is discoverable,
    // hence naming the direction a click would give you rather than just the key. Descriptions
    // live here rather than at construction so the two halves of each tooltip can't drift.
    const bool descending = sortReverseAction_->isChecked();
    const struct {
        QAction *action;
        QString what;
    } keys[] = {
        {sortByNameAction_, QStringLiteral("name")},
        {sortByFileDateAction_, QStringLiteral("file modified date")},
        {sortByTakenDateAction_, QStringLiteral("photo/video taken date (EXIF)")},
        {sortBySizeAction_, QStringLiteral("file size")},
    };
    for (const auto &k : keys) {
        QString tip = QStringLiteral("Sort by %1").arg(k.what);
        // Only the active key advertises the flip; on the other three a click switches key
        // and keeps the current direction, so promising a reversal there would be a lie.
        if (k.action->isChecked()) {
            tip += descending ? QStringLiteral(" (descending)")
                              : QStringLiteral(" (ascending");
        }
        k.action->setToolTip(tip);
    }
    sortReverseAction_->setToolTip(descending ? QStringLiteral("Descending")
                                              : QStringLiteral("Ascending"));
}

void MainWindow::onSortOrderChanged() {
    // Re-reads which of the exclusive group is checked rather than being told, since
    // this one slot is shared by all 5 actions (see the constructor and this method's
    // own doc comment in the header) - by the time triggered() fires, the
    // QActionGroup has already updated which one is checked. The key actions reach it
    // through a small lambda that may have flipped sortReverseAction_ first; that flip
    // is already applied by the time the descending read below happens.
    prefs::SortKey key = prefs::SortKey::Name;
    if (sortByFileDateAction_->isChecked()) key = prefs::SortKey::FileDate;
    else if (sortByTakenDateAction_->isChecked()) key = prefs::SortKey::TakenDate;
    else if (sortBySizeAction_->isChecked()) key = prefs::SortKey::Size;
    const bool descending = sortReverseAction_->isChecked();

    prefs::setGridSortKey(key);
    prefs::setGridSortDescending(descending);
    updateSortTooltips();

    // Same selection-by-file-id preservation as reloadGridPreservingSelection(), but
    // reordering in place (ThumbGridModel::setSortOrder()) instead of a full
    // setDirectory() reload - a sort change doesn't need a DB round trip and
    // shouldn't throw away every already-decoded thumbnail pixmap just to show them
    // in a different order.
    QList<qint64> selectedIds;
    for (int r : grid_->selectedRows()) {
        qint64 id = gridModel_->index(r).data(ThumbGridModel::FileIdRole).toLongLong();
        if (id != 0) selectedIds << id;
    }
    qint64 currentId = gridModel_->index(grid_->currentRow()).data(ThumbGridModel::FileIdRole).toLongLong();

    gridModel_->setSortOrder(key, descending);

    QList<int> rows;
    for (qint64 id : selectedIds) {
        int r = gridModel_->rowForFileId(id);
        if (r >= 0) rows << r;
    }
    int newCurrent = gridModel_->rowForFileId(currentId);
    grid_->setSelection(rows, newCurrent);
    // The old scroll position doesn't correspond to anything meaningful once the row
    // order has changed - land on whatever's still selected if there is one, or the
    // top of the new order otherwise (matching Explorer/Finder re-sorting a folder).
    if (newCurrent >= 0) grid_->scrollToRow(newCurrent, /*center=*/true);
    else grid_->verticalScrollBar()->setValue(0);
}

void MainWindow::onNavThumbRequested(qint64 fileId, qint64 /*thumbId*/) { navThumbsRequested_.insert(fileId); }

void MainWindow::onNavThumbReady(qint64 fileId, QPixmap /*pixmap*/) {
    navThumbsReceived_.insert(fileId);
    if (navFirstThumbMs_ < 0) navFirstThumbMs_ = navThumbTimer_.elapsed();
    // Re-evaluated on every arrival, not just once: a burst can still be growing
    // (the model keeps discovering more visible rows as onFilesListed's real file
    // list lands, or the user scrolls) after the first few thumbnails already
    // arrived, so "caught up" can go true, then false again as more get requested,
    // then true again later - the last time matters for "how long until the folder
    // actually looked done", which is what a stale earlier catch-up wouldn't show.
    if (navThumbsReceived_.size() >= navThumbsRequested_.size()) navAllThumbsMs_ = navThumbTimer_.elapsed();
}

void MainWindow::onNewWindow() {
    // Opens on the folder this window is showing rather than the persisted lastDirectory: the
    // reason to want a second window is almost always to compare something against what is
    // already on screen, and starting from here makes that one navigation instead of two.
    WindowRegistry::instance().createWindow(currentPath_);
}

void MainWindow::rebuildWindowsMenu() {
    if (!viewMenu_ || !windowSectionEnd_) return;

    // Only the entries this function put there last time - the rest of the View menu (Refresh,
    // Hover Info, Sort By...) is built once in the constructor and must survive untouched.
    for (QAction *stale : windowSectionActions_) {
        viewMenu_->removeAction(stale);
        delete stale;
    }
    windowSectionActions_.clear();

    // Registry order, which is creation order - deliberately not sorted by name or recency, so
    // an entry doesn't move out from under the cursor between one opening of the menu and the
    // next.
    for (MainWindow *w : WindowRegistry::instance().windows()) {
        QAction *act = new QAction(w->windowMenuLabel(), viewMenu_);
        act->setCheckable(true);
        act->setChecked(w == this);
        act->setMenuRole(QAction::NoRole);
        // Receiver is `w`, so Qt drops the connection if that window is destroyed - and the
        // registry removes a window before its destructor runs anyway, which rebuilds this
        // list. Both together mean an entry for a dead window can't be left behind and clicked.
        connect(act, &QAction::triggered, w, [w]() {
            // Both calls are needed: a minimised window has to be un-minimised before it can
            // be raised, and on macOS raise() alone does not move keyboard focus.
            if (w->isMinimized()) w->showNormal();
            w->raise();
            w->activateWindow();
        });
        viewMenu_->insertAction(windowSectionEnd_, act);
        windowSectionActions_.push_back(act);
    }
}

QString MainWindow::windowMenuLabel() const {
    if (currentPath_.isEmpty()) return QStringLiteral("pixet");

    // Split on '/' rather than the native separator: currentPath_ has been through
    // normalizeForDb(), so it is forward-slashed on Windows too, and this is the same form the
    // path bar displays.
    const QStringList parts = currentPath_.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return currentPath_; // a filesystem root ("/") has no components

    // The folder and its parent are always shown in full, however long that makes the label.
    // Two windows deep in the same tree are frequently distinguishable *only* by the parent -
    // "media" and "media" tells you nothing, "20260309_jakarta/media" does - so eliding either
    // of them would defeat the point of putting a path here at all.
    int first = qMax(0, parts.size() - 2);
    QString label = QStringList(parts.mid(first)).join(QLatin1Char('/'));

    // Then keep taking further ancestors for as long as they fit the budget. Grown one whole
    // component at a time rather than by trimming characters, so the label is always a run of
    // real directory names and never a half-word.
    while (first > 0) {
        const QString wider = QStringList(parts.mid(first - 1)).join(QLatin1Char('/'));
        if (wider.size() > kWindowMenuLabelChars) break;
        label = wider;
        --first;
    }
    return label;
}

QString MainWindow::windowLeafName() const {
    if (currentPath_.isEmpty()) return QString();
    QString name = QFileInfo(currentPath_).fileName();
    // Empty at a filesystem root ("/" on macOS, "C:/" on Windows), where fileName() has
    // nothing to return - same drive-root fallback addBookmark() uses.
    return name.isEmpty() ? currentPath_ : name;
}

void MainWindow::updateWindowTitle() {
    // Folder first so the window is identifiable from the Dock, the app switcher and the
    // Windows submenu, all of which truncate from the right. Then the version, so "which
    // build is this?" is answerable at a glance without opening About.
    //
    // **The title must end with "pixet"**, which is why the version sits in the middle rather
    // than in the natural-reading "shots - pixet 1.4.0" position. main() calls
    // setApplicationDisplayName("pixet"), and Qt's QPlatformWindow::formatWindowTitle()
    // responds by appending " - <display name>" to every window title that does not already
    // end with it. So "shots - pixet 1.4.0" reaches the screen as "shots - pixet 1.4.0 -
    // pixet". Ending with the display name is the supported way to opt out of that; the
    // alternative - folding the version into the display name itself - also renames the macOS
    // application menu's About/Hide/Quit items, which is a worse trade for a title bar.
    //
    // The empty-path branch takes the same shape for the same reason. It previously read
    // "pixet 1.4.0", which does *not* end in the display name and so was being served as
    // "pixet 1.4.0 - pixet".
    //
    // The View > Windows list is unaffected: it labels entries from windowMenuLabel(), not
    // from this, so repeating the version down that menu was never a risk.
    //
    // fromLatin1 rather than letting arg() take the const char* directly: Qt 6's
    // multi-argument arg() is a template constrained to string-like types, and a raw
    // const char* doesn't satisfy it even though the single-argument form accepts one.
    const QString version = QString::fromLatin1(pixet::version());
    if (currentPath_.isEmpty()) {
        setWindowTitle(QStringLiteral("%1 - pixet").arg(version));
    } else {
        setWindowTitle(QStringLiteral("%1 - %2 - pixet").arg(windowLeafName(), version));
    }
    WindowRegistry::instance().titlesChanged();
}

void MainWindow::onCopyProfileReport() {
    // Also written to stderr, because the interesting runs are usually launched from a
    // terminal and having it only on the clipboard means alt-tabbing to paste it somewhere.
    PIXET_PROF_DUMP();
    QString text = QString::fromStdString(PIXET_PROF_REPORT());
    QGuiApplication::clipboard()->setText(text);
    statusBar()->showMessage(QStringLiteral("Profile report copied (%1 lines)").arg(text.count(QLatin1Char('\n'))),
                             4000);
}

void MainWindow::onCopyGridDebugInfo() {
    QScreen *scr = screen();
    auto rectStr = [](const QRect &r) { return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()); };
    auto sizeStr = [](const QSize &s) { return QStringLiteral("%1x%2").arg(s.width()).arg(s.height()); };
    auto sizesStr = [](const QList<int> &sizes) {
        QStringList parts;
        for (int s : sizes) parts << QString::number(s);
        return parts.join(QStringLiteral(", "));
    };

    QStringList lines;
    lines << QStringLiteral("=== pixet grid debug info ===");
    lines << QStringLiteral("");
    lines << QStringLiteral("Window:");
    lines << QStringLiteral("  isMaximized: %1").arg(isMaximized() ? "true" : "false");
    lines << QStringLiteral("  isFullScreen: %1").arg(isFullScreen() ? "true" : "false");
    lines << QStringLiteral("  windowState: 0x%1").arg((int)windowState(), 0, 16);
    lines << QStringLiteral("  geometry: %1").arg(rectStr(geometry()));
    lines << QStringLiteral("  frameGeometry: %1").arg(rectStr(frameGeometry()));
    lines << QStringLiteral("  devicePixelRatio: %1").arg(devicePixelRatioF());
    if (scr) {
        lines << QStringLiteral("  screen: %1").arg(scr->name());
        lines << QStringLiteral("  screen geometry: %1").arg(rectStr(scr->geometry()));
        lines << QStringLiteral("  screen devicePixelRatio: %1").arg(scr->devicePixelRatio());
        lines << QStringLiteral("  screen logicalDotsPerInchX: %1").arg(scr->logicalDotsPerInchX());
    }
    lines << QStringLiteral("");
    lines << QStringLiteral("Splitters:");
    lines << QStringLiteral("  mainSplitter sizes: [%1]").arg(sizesStr(splitter_->sizes()));
    lines << QStringLiteral("  topSplitter sizes: [%1]").arg(sizesStr(topSplitter_->sizes()));
    lines << QStringLiteral("  leftSplitter sizes: [%1]").arg(sizesStr(leftSplitter_->sizes()));
    lines << QStringLiteral("  leftPanel visible: %1").arg(leftPanel_->isVisible() ? "true" : "false");
    lines << QStringLiteral("  leftPanel size: %1").arg(sizeStr(leftPanel_->size()));
    lines << QStringLiteral("");
    lines << QStringLiteral("Grid:");
    lines << QStringLiteral("  grid_ size: %1").arg(sizeStr(grid_->size()));
    lines << QStringLiteral("  grid_ viewport size: %1").arg(sizeStr(grid_->viewport()->size()));
    lines << QStringLiteral("  grid_ frameWidth: %1").arg(grid_->frameWidth());
    lines << QStringLiteral("  verticalScrollBar visible: %1, width: %2")
                 .arg(grid_->verticalScrollBar()->isVisible() ? "true" : "false")
                 .arg(grid_->verticalScrollBar()->width());
    lines << QStringLiteral("  cell size: %1").arg(sizeStr(grid_->debugCellSize()));
    lines << QStringLiteral("  iconSize(): %1").arg(sizeStr(grid_->iconSize()));
    lines << QStringLiteral("  computed columns: %1").arg(grid_->debugComputedColumns());
    lines << QStringLiteral("  actually rendered columns: %1 (always == computed now - custom-painted, not QListView IconMode)")
                 .arg(grid_->debugRenderedColumnCount());
    lines << QStringLiteral("  model row count: %1").arg(gridModel_->rowCount());
    lines << QStringLiteral("");
    lines << QStringLiteral("Preferences:");
    lines << QStringLiteral("  thumbnailIconSize: %1").arg(prefs::thumbnailIconSize());
    lines << QStringLiteral("  thumbnailTargetLongEdge: %1").arg(prefs::thumbnailTargetLongEdge());
    lines << QStringLiteral("");
    lines << QStringLiteral("Folder:");
    lines << QStringLiteral("  currentPath: %1").arg(currentPath_);
    lines << QStringLiteral("");
    lines << QStringLiteral("Navigation timing (since navigating to currentPath_):");
    lines << QStringLiteral("  thumbnails requested so far: %1").arg(navThumbsRequested_.size());
    lines << QStringLiteral("  thumbnails received so far: %1").arg(navThumbsReceived_.size());
    lines << QStringLiteral("  time to first thumbnail: %1")
                 .arg(navFirstThumbMs_ >= 0 ? QStringLiteral("%1ms").arg(navFirstThumbMs_) : QStringLiteral("none yet"));
    lines << QStringLiteral("  time until requested == received (last time this was true): %1")
                 .arg(navAllThumbsMs_ >= 0 ? QStringLiteral("%1ms").arg(navAllThumbsMs_) : QStringLiteral("not yet caught up"));
    lines << QStringLiteral("  (requested count includes any further scrolling since navigation started, not just the initial on-screen burst)");

    QString text = lines.join(QStringLiteral("\n"));
    QGuiApplication::clipboard()->setText(text);
    statusBar()->showMessage(QStringLiteral("Grid debug info copied to clipboard (%1 lines)").arg(lines.size()), 5000);
}

void MainWindow::onIndexerStarted(QString path) {
    if (path != currentPath_) return;
    PIXET_PROF_MARK("nav.indexerStarted");
    // Folder name only, not the full path (see addBookmark() for the same
    // drive-root fallback) - a long full path was the same class of collision-with-
    // the-permanent-widget-row bug as onIndexerFinished's old message (see that
    // handler's comment), and is more than anyone needs for "which folder is this
    // about" when it's already the one on screen. No explicit timeout - cleared by
    // onIndexerFinished(), not by a fixed delay, since indexing can take anywhere
    // from milliseconds to well over a minute on a large folder.
    QString label = QFileInfo(path).fileName();
    if (label.isEmpty()) label = path;
    statusBar()->showMessage(QStringLiteral("Indexing %1...").arg(label));
}

void MainWindow::onFilesListed(QString path) {
    // Pass A just finished - the file list is final, even though thumbnails are
    // mostly still pending. This is usually the very first real file list for a
    // freshly-navigated-to folder (nothing selected yet, nothing to preserve), but
    // it also fires on an explicit Refresh of the folder already on screen -
    // reloadGridPreservingSelection() carries the user's selection/scroll position
    // across that case instead of dropping them on every Refresh.
    if (path != currentPath_) return;
    PIXET_PROF_MARK("nav.filesListed");
    PIXET_PROF_SCOPE("nav.onFilesListed");
    reloadGridPreservingSelection();
    updateSelectionStatus(); // folder aggregates changed (this is often the first real file list)
    updateThumbStatusIndicator(); // the file list is final, so the freshness count is meaningful now
    trySelectPendingFile(); // this folder may not have had rows loaded until just now
}

void MainWindow::pushGridPriorityToIndexer() {
    if (currentPath_.isEmpty()) return;

    const QPair<int, int> visible = grid_->visibleRowRange();
    if (visible.first < 0) {
        // Nothing on screen (an empty folder, or one whose file list hasn't landed yet):
        // clear rather than leave the previous folder's ids in place, so a stale hint
        // can't outlive the folder it was about.
        folderIndexer_->setPriorityFiles(currentPath_, {});
        return;
    }

    // On screen first, then a screenful below, then a screenful above. The margins are
    // what keeps a slow scroll from spending all its time waiting: by the time a row
    // reaches the viewport its thumbnail was usually queued a page ago. Below before
    // above because scrolling down is the overwhelmingly common direction through a
    // folder, and this list is a priority order, not a set.
    const int page = visible.second - visible.first + 1;
    QVector<qint64> ids = gridModel_->fileIdsForRows(visible.first, visible.second);
    ids += gridModel_->fileIdsForRows(visible.second + 1, visible.second + page);
    ids += gridModel_->fileIdsForRows(visible.first - page, visible.first - 1);
    folderIndexer_->setPriorityFiles(currentPath_, std::move(ids));

    // Same viewport, different consumer: pull the RAW cache entries for what is on screen
    // into memory, so opening one of them is a copy rather than a read plus a decode. Only
    // the visible rows, not the margins the indexer also gets - a resident decode costs
    // ~13MB, which is a different order of expense from a place in a queue.
    QStringList rawPaths;
    for (int row = visible.first; row <= visible.second; ++row) {
        QModelIndex idx = gridModel_->index(row);
        if (!idx.isValid()) continue;
        if (idx.data(ThumbGridModel::FormatRole).toInt() != (int)pixet::Format::Raw) continue;
        rawPaths << joinPathQ(currentPath_, idx.data(Qt::DisplayRole).toString());
    }
    rawCacheWarmer_->warm(rawPaths);
}

void MainWindow::onThumbsProgress(QString path) {
    // A Pass B batch just landed - pull in the newly-ready thumbnails without
    // resetting the model, so already-displayed ones don't flicker.
    if (path != currentPath_) return;
    PIXET_PROF_SCOPE("nav.onThumbsProgress");
    invalidateStaleCache(); // a Pass B batch just wrote new blobs
    gridModel_->refreshThumbStates();
    grid_->viewport()->update(); // see the comment on the thumbReady connection above
    updateSelectionStatus();     // dimensions/taken-at/duration only land at decode time
}

void MainWindow::onIndexerFinished(QString path) {
    if (path != currentPath_) return;
    PIXET_PROF_MARK("nav.indexerFinished");
    gridModel_->refreshThumbStates(); // catch any trailing batch
    grid_->viewport()->update();
    // Deliberately no transient statusBar()->showMessage("<path> - N items") here. It would
    // duplicate folderStatsLabel_, which shows the same thing persistently, and QStatusBar's
    // built-in temporary-message label doesn't elide/shrink the way StatusLabel does (see
    // its class comment) - so a long full path runs straight into the permanent widget row,
    // rendering as "...\Models - 4 i" immediately followed by folderStatsLabel_'s own
    // "4 items (4 img, 0 vid) · 9.52 MiB" with no gap. updateSelectionStatus() covers the
    // same ground and keeps rawStatusLabel_ (RAW rendered/preview counts) fresh as well.
    updateSelectionStatus();
    // Thumbnails have settled, so a folder that was red because it was mid-generation can
    // now legitimately go green.
    updateThumbStatusIndicator();
    // Clears onIndexerStarted()'s "Indexing <folder>..." message, which has no timeout of
    // its own (indexing can take anywhere from milliseconds to well over a minute). Nothing
    // else in this handler posts a status message that would replace it, so without this
    // call it lingers on screen forever.
    statusBar()->clearMessage();
    // Posted here rather than in onIndexFailed(), because that handler necessarily runs
    // *before* this one (both signals are queued from the same worker thread, in emission
    // order) and the clearMessage() just above would wipe the message a moment after it
    // appeared. A long timeout, not a permanent message: it's information, not a state.
    if (!pendingIndexError_.isEmpty()) {
        statusBar()->showMessage(pendingIndexError_, 15000);
        pendingIndexError_.clear();
    }
}

void MainWindow::onIndexFailed(QString path, QString message) {
    // Logged unconditionally, before the current-folder check below can discard it. The
    // status bar is best-effort - the user may well have navigated elsewhere by now - but a
    // database error should never vanish just because nobody was looking at the right folder
    // when it happened. This message is the only thing identifying which error occurred.
    qWarning() << "pixet: indexing failed on" << path << "-" << message;
    if (path != currentPath_) return;

    QString label = QFileInfo(path).fileName();
    if (label.isEmpty()) label = path;
    pendingIndexError_ = QStringLiteral("Indexing %1 failed: %2").arg(label, message);
}

void MainWindow::onBackgroundDirectoryChanged(QString path) {
    if (path != currentPath_) return;
    // Same light-touch refresh as onThumbsProgress - a background sweep re-thumbnailing
    // a file the user happens to be looking at shouldn't reset the model and flicker
    // everything else on screen.
    gridModel_->refreshThumbStates();
    grid_->viewport()->update();
    updateSelectionStatus();

    // The preview pane holds a decode of whatever is selected, and for a RAW that decode
    // may have just been superseded: a render finishing here is exactly when the camera's
    // embedded preview (black and white, if that is how the shot was taken) gets replaced
    // by the real colour demosaic. The grid picks that up through refreshThumbStates()
    // above; the preview has to be asked again, or it keeps showing the placeholder until
    // the selection moves.
    QModelIndex current = gridModel_->index(grid_->currentRow());
    if (current.isValid() && current.data(ThumbGridModel::FormatRole).toInt() == (int)pixet::Format::Raw) {
        triggerPreviewRequest();
    }
}

void MainWindow::onWatchedDirectoryChanged(const QString &path) {
    if (path != currentPath_) return; // stale signal for a folder navigated away from since
    folderWatchDebounce_->start(); // (re)starts - several notifications in a row collapse to one rescan
}

void MainWindow::loadBookmarks() {
    bookmarks_->clear();
    auto sel = db_->prepare("SELECT id, path, label FROM bookmarks ORDER BY sort");
    while (sel.step()) {
        qint64 id = sel.columnInt64(0);
        QString path = QString::fromStdString(sel.columnText(1));
        QString label = QString::fromStdString(sel.columnText(2));
        if (label.isEmpty()) label = path;

        auto *item = new QListWidgetItem(label, bookmarks_);
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, id);
    }
}

void MainWindow::addBookmark(const QString &path) {
    QString label = QFileInfo(path).fileName();
    if (label.isEmpty()) label = path; // e.g. a drive root like "C:\"

    auto countSel = db_->prepare("SELECT count(*) FROM bookmarks");
    countSel.step();
    int64_t nextSort = countSel.columnInt64(0);

    auto ins = db_->prepare("INSERT INTO bookmarks(path, label, sort) VALUES(?,?,?)");
    ins.bind(1, path.toStdString());
    ins.bind(2, label.toStdString());
    ins.bind(3, nextSort);
    ins.step();

    loadBookmarks();
}

void MainWindow::removeBookmark(qint64 id) {
    auto del = db_->prepare("DELETE FROM bookmarks WHERE id=?");
    del.bind(1, (int64_t)id);
    del.step();
    loadBookmarks();
}

void MainWindow::restoreLastDirectory() {
    QSettings settings = prefs::settingsStore();
    QString last = settings.value(QStringLiteral("lastDirectory")).toString();

    if (!last.isEmpty() && QDir(last).exists()) {
        navigateTo(last);
        return;
    }

    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (!pictures.isEmpty() && QDir(pictures).exists()) navigateTo(pictures);
}

void MainWindow::saveLastDirectory(const QString &path) {
    QSettings settings = prefs::settingsStore();
    settings.setValue(QStringLiteral("lastDirectory"), path);
}

void MainWindow::nukeDatabase() {
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);

    // Deliberately not `bookmarks` - those are user-curated navigation shortcuts,
    // not scan-derived cache, and re-scanning can't regenerate them. Everything else
    // here is exactly what a fresh rescan rebuilds from scratch.
    db_->exec("DELETE FROM files;");
    db_->exec("DELETE FROM dirs;");
    db_->exec("DELETE FROM claims;");
    db_->exec("DELETE FROM journal;");
    db_->exec("DELETE FROM thumbs.thumbs;");
    // Reclaims the actual disk space the deleted rows/blobs held - without this the
    // files would just have a lot of internally-tracked free space, which doesn't
    // match "nuke" for a cache that can genuinely run to several GB of thumbnails.
    db_->exec("VACUUM;");
    db_->exec("VACUUM thumbs;");

    QGuiApplication::restoreOverrideCursor();

    // Every row the grid/tree/status bar currently reference is gone - force a fully
    // fresh Pass A/B of whatever's on screen right now rather than leaving an empty
    // grid the user has to manually Refresh to escape.
    if (!currentPath_.isEmpty()) navigateTo(currentPath_, /*forceReindex=*/true);
}

void MainWindow::saveWindowState() {
    // With several windows open these preferences are single-valued and every window would
    // write them - on its own close, and again for all of them on aboutToQuit - so the
    // surviving values would be whichever window happened to be signalled last. Restricting
    // the write to the most recently activated window makes it deterministic and matches what
    // "restore what I was last looking at" should mean.
    //
    // Note the window being closed is still registered at this point (closeEvent runs before
    // the destructor's remove()), so closing the active window correctly saves its own state.
    if (WindowRegistry::instance().lastActive() != this) return;

    QSettings settings = prefs::settingsStore();
    settings.setValue(QStringLiteral("windowGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("mainSplitterState"), splitter_->saveState());
    settings.setValue(QStringLiteral("topSplitterState"), topSplitter_->saveState());
    settings.setValue(QStringLiteral("leftSplitterState"), leftSplitter_->saveState());
}

void MainWindow::changeEvent(QEvent *event) {
    // Feeds WindowRegistry::lastActive(), which decides which window's geometry is persisted
    // and where a macOS FileOpen event is delivered.
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        WindowRegistry::instance().noteActivated(this);
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveWindowState();
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    // Mouse back/forward buttons -> folder history, matching Alt+Left / Alt+Right. The type
    // check is first and cheap because this filter is installed application-wide.
    if (event->type() == QEvent::MouseButtonPress) {
        Qt::MouseButton button = static_cast<QMouseEvent *>(event)->button();
        if (button == Qt::BackButton || button == Qt::ForwardButton) {
            // Every window installs this filter on the *application*, so with two windows
            // open a single back-button click arrives here once per window. Without this
            // guard one click would navigate every open window at once.
            if (!isActiveWindow()) return false;
            // Not while the fullscreen viewer is up: there, "back" reads as the previous
            // image, and silently changing folders underneath it would be worse than doing
            // nothing. Left unhandled rather than repurposed - image navigation already has
            // the arrow keys and the wheel.
            if (fullscreenViewer_ && fullscreenViewer_->isVisible()) return false;
            if (button == Qt::BackButton) onNavigateBack();
            else onNavigateForward();
            return true;
        }
    }

    if (watched == pathBar_->lineEdit() && event->type() == QEvent::FocusIn) {
        // A plain selectAll() here gets immediately undone by the mouse-press event
        // that triggered this focus-in (it repositions the cursor to the click point,
        // collapsing the selection) - deferring to the next event loop turn lets that
        // click finish being processed first.
        QTimer::singleShot(0, pathBar_->lineEdit(), &QLineEdit::selectAll);
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::isWindowOnScreen() const {
    QRect frame = frameGeometry();
    for (QScreen *screen : QGuiApplication::screens()) {
        // Require a reasonably-sized chunk of the frame to be reachable (not just a
        // stray pixel), so a window barely clipped at a screen edge still counts as "on
        // screen" rather than getting needlessly recentered.
        QRect visible = screen->availableGeometry().intersected(frame);
        if (visible.width() > 100 && visible.height() > 60) return true;
    }
    return false;
}

void MainWindow::navigateToInput(const QString &input) {
    QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) return;

    QFileInfo info(trimmed);
    if (info.isDir()) {
        navigateTo(trimmed);
        return;
    }
    if (info.isFile()) {
        navigateTo(info.absolutePath());
        // navigateTo() just cleared pendingSelectFileName_ (it's meant to be armed
        // per-navigation, not leftover from some earlier attempt) - set it after,
        // then try immediately for the common case where this folder is already
        // cached/loaded. onFilesListed() catches the case where it wasn't.
        pendingSelectFileName_ = info.fileName();
        trySelectPendingFile();
        return;
    }

    statusBar()->showMessage(QStringLiteral("Not found: %1").arg(trimmed), 4000);
    pathBar_->setCurrentText(currentPath_);
}

void MainWindow::trySelectPendingFile() {
    if (pendingSelectFileName_.isEmpty()) return;
    int row = gridModel_->rowForName(pendingSelectFileName_);
    if (row < 0) return; // folder's rows may not be loaded yet - retried on the next reload
    grid_->setCurrentRow(row);
    grid_->scrollToRow(row, /*center=*/true);
    pendingSelectFileName_.clear();
}

void MainWindow::refreshPathBarHistory() {
    // Preserved explicitly rather than relying on clear()/addItems() to leave it
    // alone - QComboBox::clear() drops the current index (and, with it, an editable
    // combo's displayed text) as a side effect, which would otherwise blow away
    // whatever's currently shown (e.g. a selected file's full path) every time a new
    // folder is visited.
    QString current = pathBar_->currentText();
    QSignalBlocker blocker(pathBar_); // clear()/addItems() below must not look like a user pick
    pathBar_->clear();
    pathBar_->addItems(prefs::pathHistory());
    pathBar_->setCurrentText(current);
}

void MainWindow::onPurgePathHistory() {
    prefs::clearPathHistory();
    refreshPathBarHistory();
    statusBar()->showMessage(QStringLiteral("Path history cleared"), 3000);
}

void MainWindow::reloadGridPreservingSelection() {
    QList<qint64> selectedIds;
    for (int r : grid_->selectedRows()) {
        qint64 id = gridModel_->index(r).data(ThumbGridModel::FileIdRole).toLongLong();
        if (id != 0) selectedIds << id;
    }
    // FileIdRole is 0 for an invalid index (grid_->currentRow() == -1) - rowForFileId(0)
    // below correctly finds nothing, so no separate "was there a selection at all" check.
    qint64 currentId = gridModel_->index(grid_->currentRow()).data(ThumbGridModel::FileIdRole).toLongLong();
    int scrollValue = grid_->verticalScrollBar()->value();

    gridModel_->setDirectory(currentPath_);

    QList<int> rows;
    for (qint64 id : selectedIds) {
        int r = gridModel_->rowForFileId(id);
        if (r >= 0) rows << r;
    }
    grid_->setSelection(rows, gridModel_->rowForFileId(currentId));

    // Set after setSelection() (which itself doesn't scroll) so nothing downstream
    // moves the scrollbar again after this - restores the exact pre-reload position
    // rather than re-deriving it from whichever row ended up selected.
    grid_->verticalScrollBar()->setValue(scrollValue);
}

void MainWindow::updateSelectionStatus() {
    PIXET_PROF_SCOPE("ui.updateSelectionStatus");
    // Folder-level aggregates - always shown when a folder is loaded, regardless of
    // whether anything is selected.
    int imageCount = gridModel_->imageCount();
    int videoCount = gridModel_->videoCount();
    int totalCount = imageCount + videoCount;
    folderStatsLabel_->setStatusText(totalCount > 0 ? QStringLiteral("%1 items (%2 img, %3 vid)  ·  %4")
                                                           .arg(totalCount)
                                                           .arg(imageCount)
                                                           .arg(videoCount)
                                                           .arg(QLocale().formattedDataSize(gridModel_->totalBytes()))
                                                     : QString());

    // How many of this folder's RAW files have a real full-render thumbnail vs. still
    // just the fast embedded-preview one - see RawRenderer/`pixet-index --render-raws`.
    // Blank whenever there's nothing to report (no RAW files here, or none have
    // reached either state yet) rather than showing "0 rendered, 0 preview".
    int rawRendered = gridModel_->rawRenderedCount();
    int rawPreview = gridModel_->rawPreviewCount();
    int rawKnown = rawRendered + rawPreview;
    rawStatusLabel_->setStatusText(
        rawKnown > 0 ? QStringLiteral("%1 RAW: %2 rendered, %3 preview").arg(rawKnown).arg(rawRendered).arg(rawPreview)
                     : QString());

    QModelIndex idx = gridModel_->index(grid_->currentRow());
    if (!idx.isValid()) {
        fileNameLabel_->setStatusText(QString());
        formatLabel_->setStatusText(QString());
        dimsLabel_->setStatusText(QString());
        sizeLabel_->setStatusText(QString());
        dateLabel_->setStatusText(QString());
        durationLabel_->setStatusText(QString());
        return;
    }

    // Elided rather than left to wrap/clip raggedly - the label's fixed width means a
    // long filename would otherwise just get cut off mid-character. Full name still
    // available on hover (StatusLabel::setStatusText() sets it as a tooltip).
    //
    // With more than one item selected, this and the size label switch to an
    // Explorer-style aggregate (count + the previewed item's name; combined size);
    // format/dimensions/date/duration keep describing the previewed (lead) item
    // specifically, same as the single-selection case - blanking them on every
    // Ctrl+click would be exactly the label-jitter StatusBarRow/StatusLabel were
    // built to avoid (see the member comment on these labels in MainWindow.h).
    int selCount = grid_->selectionCount();
    QString name = idx.data(Qt::DisplayRole).toString();
    if (selCount > 1) {
        fileNameLabel_->setStatusText(QStringLiteral("%1 selected · %2").arg(selCount).arg(name), Qt::ElideMiddle);
    } else {
        fileNameLabel_->setStatusText(name, Qt::ElideMiddle);
    }

    int fmt = idx.data(ThumbGridModel::FormatRole).toInt();
    formatLabel_->setStatusText(QString::fromUtf8(pixet::formatName((pixet::Format)fmt)));

    int w = idx.data(ThumbGridModel::WidthRole).toInt();
    int h = idx.data(ThumbGridModel::HeightRole).toInt();
    if (w > 0 && h > 0) {
        QString dims = QStringLiteral("%1×%2").arg(w).arg(h);
        // Only known once the preview decode lands (see currentPreviewBpp_) - may be
        // briefly absent right after selecting, same as the async width/height fields.
        if (currentPreviewBpp_ > 0) dims += QStringLiteral(" (%1bpp)").arg(currentPreviewBpp_);
        dimsLabel_->setStatusText(dims);
    } else {
        dimsLabel_->setStatusText(QString());
    }

    qint64 size = selCount > 1 ? gridModel_->sizeForRows(grid_->selectedRows())
                                : idx.data(ThumbGridModel::SizeRole).toLongLong();
    sizeLabel_->setStatusText(size > 0 ? QLocale().formattedDataSize(size) : QString());

    qint64 takenAt = idx.data(ThumbGridModel::TakenAtRole).toLongLong();
    dateLabel_->setStatusText(takenAt > 0
                                   ? QDateTime::fromSecsSinceEpoch(takenAt).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                                   : QString());

    qint64 durationMs = idx.data(ThumbGridModel::DurationMsRole).toLongLong();
    if (durationMs > 0) {
        qint64 totalSec = durationMs / 1000;
        durationLabel_->setStatusText(
            QStringLiteral("%1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0')));
    } else {
        durationLabel_->setStatusText(QString());
    }

    // Bundled here (rather than left to selectionChanged/aboutToShow alone) so a
    // keyboard-only Ctrl+V works immediately after a navigateTo()/onFilesListed()
    // call, which resets the grid's selection without emitting selectionChanged
    // (see ThumbGridView::setModel's modelReset handling) and doesn't otherwise
    // touch this - without this, currentPath_ becoming non-empty on first launch
    // wouldn't enable Paste until the user opened the Edit menu once.
    updateEditActionsEnabled();
}

void MainWindow::updateEditActionsEnabled() {
    bool hasSelection = grid_->selectionCount() > 0;
    cutAction_->setEnabled(hasSelection);
    copyAction_->setEnabled(hasSelection);
    pasteAction_->setEnabled(!currentPath_.isEmpty());
    deleteAction_->setEnabled(hasSelection);
    // Bulk rename (Windows 11's rename-all-selected-with-a-numbered-suffix) is a
    // distinct, much bigger feature nobody's asked for - Rename only ever targets
    // exactly one file.
    renameAction_->setEnabled(grid_->selectionCount() == 1);
}

void MainWindow::restoreWindowState() {
    // Restores window geometry saved by closeEvent() (splitter layout is handled
    // separately, deferred until after the first real layout pass - see the
    // QTimer::singleShot in the constructor). resetLayout_ (set from the
    // --reset-layout CLI flag, see main.cpp) skips restoring here - the next normal
    // close then saves fresh default values right back, so a window parked in a
    // strange or off-screen state self-heals with a single relaunch flag rather than
    // needing to hand-edit or delete QSettings.
    QSettings settings = prefs::settingsStore();

    bool geometryRestored = false;
    if (!resetLayout_) {
        QByteArray geometry = settings.value(QStringLiteral("windowGeometry")).toByteArray();
        if (!geometry.isEmpty() && restoreGeometry(geometry) && isWindowOnScreen()) geometryRestored = true;
    }
    if (!geometryRestored) {
        resize(1280, 800);
        if (QScreen *screen = QGuiApplication::primaryScreen()) {
            QRect avail = screen->availableGeometry();
            move(avail.center() - QPoint(width() / 2, height() / 2));
        }
    }
}
