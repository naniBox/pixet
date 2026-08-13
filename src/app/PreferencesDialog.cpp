#include "PreferencesDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <iterator>
#include <thread>

#include "KeyBindings.h"
#include "Preferences.h"

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Preferences"));

    auto *layout = new QVBoxLayout(this);
    // Tabbed rather than one long stacked column - it was getting tall enough
    // (especially with Keybindings' 7 rows) to run off a normal-height screen.
    auto *tabs = new QTabWidget(this);
    layout->addWidget(tabs, /*stretch=*/1);

    // --- General tab: video player + thumbnails ---
    auto *generalTab = new QWidget(tabs);
    auto *generalLayout = new QVBoxLayout(generalTab);

    auto *playerGroup = new QGroupBox(QStringLiteral("Video Player"), generalTab);
    auto *playerLayout = new QVBoxLayout(playerGroup);
    systemPlayerRadio_ = new QRadioButton(QStringLiteral("System default"), playerGroup);
    customPlayerRadio_ = new QRadioButton(QStringLiteral("Custom player:"), playerGroup);
    auto *customPathRow = new QHBoxLayout();
    customPlayerPathEdit_ = new QLineEdit(playerGroup);
    browseButton_ = new QPushButton(QStringLiteral("Browse..."), playerGroup);
    customPathRow->addWidget(customPlayerPathEdit_, /*stretch=*/1);
    customPathRow->addWidget(browseButton_);
    playerLayout->addWidget(systemPlayerRadio_);
    playerLayout->addWidget(customPlayerRadio_);
    playerLayout->addLayout(customPathRow);
    generalLayout->addWidget(playerGroup);

    bool useSystem = prefs::useSystemVideoPlayer();
    systemPlayerRadio_->setChecked(useSystem);
    customPlayerRadio_->setChecked(!useSystem);
    customPlayerPathEdit_->setText(prefs::customVideoPlayerPath());
    updateCustomPlayerEnabled();

    connect(systemPlayerRadio_, &QRadioButton::toggled, this, &PreferencesDialog::updateCustomPlayerEnabled);
    connect(browseButton_, &QPushButton::clicked, this, &PreferencesDialog::onBrowseCustomPlayer);

    auto *thumbGroup = new QGroupBox(QStringLiteral("Thumbnails"), generalTab);
    auto *thumbLayout = new QFormLayout(thumbGroup);
    // A drop-down over prefs::thumbnailSizeChoices(), not a free-form spinbox: the status
    // bar offers the same setting as a fixed list, and a spinbox here could produce a value
    // that list had no entry for. One shared source, so the two controls always agree.
    originalThumbnailSize_ = prefs::thumbnailIconSize();
    thumbnailSizeCombo_ = new QComboBox(thumbGroup);
    for (int px : prefs::thumbnailSizeChoices(originalThumbnailSize_)) {
        thumbnailSizeCombo_->addItem(QStringLiteral("%1 px").arg(px), px);
    }
    thumbnailSizeCombo_->setCurrentIndex(
        thumbnailSizeCombo_->findData(originalThumbnailSize_));
    thumbLayout->addRow(QStringLiteral("Grid thumbnail size:"), thumbnailSizeCombo_);
    generalLayout->addWidget(thumbGroup);
    generalLayout->addStretch(1);
    tabs->addTab(generalTab, QStringLiteral("General"));

    // --- Keybindings tab, scrollable - see KeyBindings.h's class comment for why
    // only these action-trigger keys are here (not grid/fullscreen directional
    // navigation or Escape-to-close). No enclosing QGroupBox needed here the way the
    // other tabs' sections have one - the tab label already says what this is.
    auto *keyContent = new QWidget;
    auto *keyLayout = new QFormLayout(keyContent);
    for (const keybindings::ActionInfo &a : keybindings::allActions()) {
        auto *edit = new QKeySequenceEdit(keybindings::binding(a.action), keyContent);
        edit->setMaximumSequenceLength(1); // a single key/chord, not a multi-step sequence
        keyBindingEdits_[a.action] = edit;

        // Per-row reset, next to "Reset All to Defaults" below for resetting
        // everything at once - a small "x" rather than a labeled button since
        // there's one of these per row and the row already says what it's for.
        auto *resetOneButton = new QToolButton(keyContent);
        resetOneButton->setText(QStringLiteral("×"));
        resetOneButton->setAutoRaise(true);
        // NativeText, not the default PortableText: on macOS the latter spells this "Ctrl+D"
        // while the QKeySequenceEdit two pixels away renders the same binding as "⌘D".
        resetOneButton->setToolTip(
            QStringLiteral("Reset to default (%1)").arg(a.defaultSequence.toString(QKeySequence::NativeText)));
        QKeySequence defaultSeq = a.defaultSequence;
        connect(resetOneButton, &QToolButton::clicked, this, [edit, defaultSeq]() { edit->setKeySequence(defaultSeq); });

        auto *row = new QHBoxLayout();
        row->addWidget(edit, /*stretch=*/1);
        row->addWidget(resetOneButton);
        keyLayout->addRow(a.displayName + QStringLiteral(":"), row);
    }
    auto *resetKeysButton = new QPushButton(QStringLiteral("Reset All to Defaults"), keyContent);
    keyLayout->addRow(resetKeysButton);
    connect(resetKeysButton, &QPushButton::clicked, this, &PreferencesDialog::onResetKeyBindings);

    auto *keyScroll = new QScrollArea(tabs);
    keyScroll->setWidget(keyContent);
    keyScroll->setWidgetResizable(true);
    keyScroll->setFrameShape(QFrame::NoFrame);
    tabs->addTab(keyScroll, QStringLiteral("Keybindings"));

    // --- Maintenance tab: index + danger zone ---
    auto *maintenanceTab = new QWidget(tabs);
    auto *maintenanceLayout = new QVBoxLayout(maintenanceTab);

    auto *indexGroup = new QGroupBox(QStringLiteral("Index"), maintenanceTab);
    auto *indexLayout = new QVBoxLayout(indexGroup);
    auto *reindexButton = new QPushButton(QStringLiteral("Re-index Known Folders"), indexGroup);
    auto *reindexHint = new QLabel(
        QStringLiteral("Re-checks every folder pixet has already scanned for files changed outside the app. "
                        "Doesn't touch new folders or re-render thumbnails - this is the same drift check the "
                        "background sweep already does continuously, just run right now instead of eventually."),
        indexGroup);
    reindexHint->setWordWrap(true);
    reindexStatusLabel_ = new QLabel(indexGroup);
    indexLayout->addWidget(reindexButton);
    indexLayout->addWidget(reindexHint);
    indexLayout->addWidget(reindexStatusLabel_);

    unsigned detectedCores = std::thread::hardware_concurrency();
    auto *threadsRow = new QHBoxLayout();
    auto *threadsLabel = new QLabel(QStringLiteral("Indexing threads:"), indexGroup);
    indexerThreadsSpin_ = new QSpinBox(indexGroup);
    // 0 = auto; upper bound is generous rather than exactly detectedCores - a user who
    // deliberately wants to oversubscribe (e.g. testing, or cores that aren't equally
    // fast) can, it's their machine.
    indexerThreadsSpin_->setRange(0, (int)std::max(1u, detectedCores) * 4);
    indexerThreadsSpin_->setSpecialValueText(
        QStringLiteral("Auto (%1 detected)").arg(detectedCores > 0 ? (int)detectedCores : 1));
    indexerThreadsSpin_->setValue(prefs::indexerThreadCount());
    threadsRow->addWidget(threadsLabel);
    threadsRow->addWidget(indexerThreadsSpin_, /*stretch=*/1);
    indexLayout->addLayout(threadsRow);
    auto *threadsHint = new QLabel(
        QStringLiteral("How many files are thumbnailed at once when you navigate to a folder that needs it, or "
                        "when running pixet-index. Only applies there - the continuous background sweep and RAW "
                        "render catch-up stay single-threaded regardless, so they don't compete with whatever "
                        "you're actively waiting on for CPU."),
        indexGroup);
    threadsHint->setWordWrap(true);
    indexLayout->addWidget(threadsHint);

    maintenanceLayout->addWidget(indexGroup);
    connect(reindexButton, &QPushButton::clicked, this, &PreferencesDialog::onReindexClicked);

    auto *thumbMaintGroup = new QGroupBox(QStringLiteral("Thumbnails"), maintenanceTab);
    auto *thumbMaintLayout = new QVBoxLayout(thumbMaintGroup);
    autoRethumbCheck_ = new QCheckBox(QStringLiteral("Auto rethumb"), thumbMaintGroup);
    autoRethumbCheck_->setChecked(prefs::autoRethumbnail());
    auto *autoRethumbHint = new QLabel(
        QStringLiteral("Thumbnails are stored at whatever size was configured when a folder was scanned, so "
                        "raising the grid size leaves older folders looking soft. The status bar's dot turns red "
                        "when the folder on screen is affected, and clicking it regenerates that folder.\n\n"
                        "With this on, those folders are regenerated automatically as you browse into them "
                        "instead of waiting to be clicked. Off by default: regenerating re-reads and re-decodes "
                        "every original in the folder, which is worth opting into rather than having browsing "
                        "quietly trigger it."),
        thumbMaintGroup);
    autoRethumbHint->setWordWrap(true);
    thumbMaintLayout->addWidget(autoRethumbCheck_);
    thumbMaintLayout->addWidget(autoRethumbHint);
    maintenanceLayout->addWidget(thumbMaintGroup);

    auto *dbGroup = new QGroupBox(QStringLiteral("Database"), maintenanceTab);
    auto *dbLayout = new QVBoxLayout(dbGroup);
    auto *statsButton = new QPushButton(QStringLiteral("Database Statistics..."), dbGroup);
    auto *statsHint = new QLabel(
        QStringLiteral("Row counts, on-disk size, and how much space could be reclaimed by compacting."),
        dbGroup);
    statsHint->setWordWrap(true);
    dbLayout->addWidget(statsButton);
    dbLayout->addWidget(statsHint);
    maintenanceLayout->addWidget(dbGroup);
    connect(statsButton, &QPushButton::clicked, this, &PreferencesDialog::databaseStatsRequested);

    auto *dangerGroup = new QGroupBox(QStringLiteral("Danger Zone"), maintenanceTab);
    auto *dangerLayout = new QVBoxLayout(dangerGroup);
    auto *nukeButton = new QPushButton(QStringLiteral("Reset Index..."), dangerGroup);
    nukeButton->setStyleSheet(QStringLiteral("QPushButton { color: #c0392b; font-weight: bold; }"));
    auto *nukeHint = new QLabel(
        QStringLiteral("Permanently deletes pixet's entire index - every scanned folder, file record, and cached "
                        "thumbnail. Your actual photos and videos on disk are never touched, only pixet's cache of "
                        "them; bookmarks are kept. pixet rescans everything from scratch afterward. Cannot be undone."),
        dangerGroup);
    nukeHint->setWordWrap(true);
    nukeStatusLabel_ = new QLabel(dangerGroup);
    dangerLayout->addWidget(nukeButton);
    dangerLayout->addWidget(nukeHint);
    dangerLayout->addWidget(nukeStatusLabel_);
    maintenanceLayout->addWidget(dangerGroup);
    connect(nukeButton, &QPushButton::clicked, this, &PreferencesDialog::onNukeClicked);

    maintenanceLayout->addStretch(1);
    tabs->addTab(maintenanceTab, QStringLiteral("Maintenance"));

    // --- OK/Cancel (shared across all tabs) ---
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &PreferencesDialog::reject);
    layout->addWidget(buttons);

    setMinimumWidth(440);
    resize(480, 480);
}

void PreferencesDialog::updateCustomPlayerEnabled() {
    bool custom = !systemPlayerRadio_->isChecked();
    customPlayerPathEdit_->setEnabled(custom);
    browseButton_->setEnabled(custom);
}

void PreferencesDialog::onBrowseCustomPlayer() {
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Select Video Player"),
                                                  customPlayerPathEdit_->text());
    if (!path.isEmpty()) customPlayerPathEdit_->setText(path);
}

void PreferencesDialog::onReindexClicked() {
    emit reindexRequested();
    reindexStatusLabel_->setText(
        QStringLiteral("Re-index started - already-known folders will catch up in the background."));
}

void PreferencesDialog::onNukeClicked() {
    auto choice = QMessageBox::warning(
        this, QStringLiteral("Reset Index?"),
        QStringLiteral("This will permanently delete pixet's entire index - every folder, file record, and cached "
                        "thumbnail it has ever scanned. Your actual photos and videos on disk are never touched, "
                        "only pixet's cache of them; bookmarks are kept.\n\n"
                        "pixet will need to rescan everything from a completely empty index. This cannot be undone."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) return;

    // Connected direct (same thread) in MainWindow, so this has already fully run -
    // deletion, VACUUM, and the current folder's fresh rescan - by the time emit()
    // returns, unlike the reindex button's genuinely-backgrounded action below.
    emit nukeDatabaseRequested();
    nukeStatusLabel_->setText(QStringLiteral("Index reset."));
}

void PreferencesDialog::onResetKeyBindings() {
    for (auto it = keyBindingEdits_.constBegin(); it != keyBindingEdits_.constEnd(); ++it) {
        it.value()->setKeySequence(keybindings::info(it.key()).defaultSequence);
    }
}

bool PreferencesDialog::validateKeyBindings() {
    // QMap iteration order is by key (Action, an enum) - stable and good enough for
    // a deterministic "which pair conflicts" message, no need to sort by anything
    // more meaningful for an error dialog that should rarely actually appear.
    for (auto it = keyBindingEdits_.constBegin(); it != keyBindingEdits_.constEnd(); ++it) {
        QKeySequence seq = it.value()->keySequence();
        if (seq.isEmpty()) continue; // clearing a binding is valid - just disables it

        if (keybindings::reservedSequences().contains(seq)) {
            QMessageBox::warning(this, QStringLiteral("Keybinding conflict"),
                                  QStringLiteral("\"%1\" is already used for navigation and can't be reassigned to "
                                                 "\"%2\".")
                                      .arg(seq.toString(QKeySequence::NativeText),
                                            keybindings::info(it.key()).displayName));
            return false;
        }

        for (auto other = std::next(it); other != keyBindingEdits_.constEnd(); ++other) {
            if (other.value()->keySequence() == seq) {
                QMessageBox::warning(
                    this, QStringLiteral("Keybinding conflict"),
                    QStringLiteral("\"%1\" is assigned to both \"%2\" and \"%3\" - pick a different key for one of "
                                    "them.")
                        .arg(seq.toString(QKeySequence::NativeText), keybindings::info(it.key()).displayName,
                             keybindings::info(other.key()).displayName));
                return false;
            }
        }
    }
    return true;
}

void PreferencesDialog::accept() {
    if (!validateKeyBindings()) return; // leaves the dialog open so the conflict can be fixed

    prefs::setUseSystemVideoPlayer(systemPlayerRadio_->isChecked());
    prefs::setCustomVideoPlayerPath(customPlayerPathEdit_->text());

    prefs::setAutoRethumbnail(autoRethumbCheck_->isChecked());
    prefs::setIndexerThreadCount(indexerThreadsSpin_->value());

    int newSize = thumbnailSizeCombo_->currentData().toInt();
    prefs::setThumbnailIconSize(newSize);
    if (newSize != originalThumbnailSize_) emit thumbnailSizeChanged();

    for (auto it = keyBindingEdits_.constBegin(); it != keyBindingEdits_.constEnd(); ++it) {
        keybindings::setBinding(it.key(), it.value()->keySequence());
    }

    QDialog::accept();
}
