#include "PreferencesDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
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
#include "cache/RawCache.h"

namespace {
// One shape for both RAW-cache budget drop-downs: a label and the exact byte count it
// means, so a preset that is still selected never has to be re-parsed from its own text.
struct BudgetPreset {
    const char *label;
    qint64 bytes;
};
} // namespace

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Preferences"));

    auto *layout = new QVBoxLayout(this);
    // Tabbed rather than one long stacked column - it was getting tall enough
    // (especially with Keybindings' 7 rows) to run off a normal-height screen.
    auto *tabs = new QTabWidget(this);
    layout->addWidget(tabs, /*stretch=*/1);

    // Every tab scrolls. Keybindings has needed it since it grew past a screenful, and
    // Maintenance grew the same way - four groups of buttons, each with a wrapped
    // paragraph explaining it. Without a scroll area, a QVBoxLayout given less height
    // than its contents want doesn't clip, it squeezes every child toward its minimum,
    // so the symptom isn't a cut-off tab but a bunched-up unreadable one. Applied to all
    // three rather than only where it currently hurts, since any of them can grow.
    auto addScrollingTab = [tabs](QWidget *content, const QString &title) {
        auto *scroll = new QScrollArea(tabs);
        scroll->setWidget(content);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        // No horizontal scrolling: the wrapped hint labels should reflow to the dialog's
        // width, not stay wide and push a scrollbar underneath them.
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tabs->addTab(scroll, title);
    };

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

    // --- RAW decode cache -------------------------------------------------------------
    // A full demosaic is the one decode slow enough to be worth keeping on disk - seconds
    // per file, repeated every time the same RAW is looked at. See core/cache/RawCache.h.
    auto *rawGroup = new QGroupBox(QStringLiteral("RAW decode cache"), generalTab);
    auto *rawLayout = new QFormLayout(rawGroup);

    rawCacheSizeCombo_ = new QComboBox(rawGroup);
    const int currentEdge = prefs::rawCacheLongEdge();
    bool edgeListed = false;
    for (int px : prefs::kRawCacheSizePresets) {
        rawCacheSizeCombo_->addItem(QStringLiteral("%1 px").arg(px), px);
        if (px == currentEdge) edgeListed = true;
    }
    // A size from a hand-edited ini (or a future build's longer list) is kept as its own
    // entry rather than being silently rounded to a neighbour - same courtesy the grid
    // thumbnail list extends to its own out-of-list values.
    if (!edgeListed) rawCacheSizeCombo_->insertItem(0, QStringLiteral("%1 px").arg(currentEdge), currentEdge);
    rawCacheSizeCombo_->setCurrentIndex(rawCacheSizeCombo_->findData(currentEdge));
    rawLayout->addRow(QStringLiteral("Cached decode size:"), rawCacheSizeCombo_);

    rawCacheBudgetCombo_ = new QComboBox(rawGroup);
    rawCacheBudgetCombo_->setEditable(true);
    rawCacheBudgetCombo_->setInsertPolicy(QComboBox::NoInsert);
    static const BudgetPreset kBudgets[] = {
        {"Off (don't cache)", 0},
        {"512 MB", 512LL * 1024 * 1024},
        {"1 GB", 1LL * 1024 * 1024 * 1024},
        {"2 GB", 2LL * 1024 * 1024 * 1024},
        {"5 GB", 5LL * 1024 * 1024 * 1024},
        {"10 GB", 10LL * 1024 * 1024 * 1024},
        {"20 GB", 20LL * 1024 * 1024 * 1024},
    };
    for (const BudgetPreset &b : kBudgets) {
        rawCacheBudgetCombo_->addItem(QString::fromLatin1(b.label), (qint64)b.bytes);
    }
    const qint64 currentBudget = prefs::rawCacheMaxBytes();
    int budgetIndex = rawCacheBudgetCombo_->findData(currentBudget);
    if (budgetIndex >= 0) {
        rawCacheBudgetCombo_->setCurrentIndex(budgetIndex);
    } else {
        // Editable, so a value that isn't a preset shows as text the user can edit rather
        // than snapping to the nearest one - the whole point of it being free choice.
        rawCacheBudgetCombo_->setCurrentText(
            QStringLiteral("%1 GB").arg(currentBudget / double(1024 * 1024 * 1024), 0, 'g', 3));
    }
    rawCacheBudgetCombo_->setToolTip(
        QStringLiteral("Pick a preset or type your own, e.g. \"3 GB\", \"750 MB\". "
                        "When the cache goes over this, the least recently used entries are deleted."));
    rawLayout->addRow(QStringLiteral("Maximum cache size:"), rawCacheBudgetCombo_);

    // The memory tier. Same editable-combo treatment as the disk budget, and for the same
    // reason: how much RAM to spend is a personal number. Entries here are decoded images
    // rather than JPEGs, so the useful range is much smaller than the disk one.
    rawCacheMemoryCombo_ = new QComboBox(rawGroup);
    rawCacheMemoryCombo_->setEditable(true);
    rawCacheMemoryCombo_->setInsertPolicy(QComboBox::NoInsert);
    static const BudgetPreset kMemBudgets[] = {
        {"Off (always read from disk)", 0},
        {"128 MB", 128LL * 1024 * 1024},
        {"256 MB", 256LL * 1024 * 1024},
        {"512 MB", 512LL * 1024 * 1024},
        {"1 GB", 1LL * 1024 * 1024 * 1024},
        {"2 GB", 2LL * 1024 * 1024 * 1024},
    };
    for (const BudgetPreset &b : kMemBudgets) {
        rawCacheMemoryCombo_->addItem(QString::fromLatin1(b.label), (qint64)b.bytes);
    }
    const qint64 currentMem = prefs::rawCacheMemoryBytes();
    int memIndex = rawCacheMemoryCombo_->findData(currentMem);
    if (memIndex >= 0) {
        rawCacheMemoryCombo_->setCurrentIndex(memIndex);
    } else {
        rawCacheMemoryCombo_->setCurrentText(
            QStringLiteral("%1 MB").arg(currentMem / (1024 * 1024)));
    }
    rawCacheMemoryCombo_->setToolTip(
        QStringLiteral("Decoded images held in RAM, so what's on screen opens with no disk read "
                        "and no decode. A 2560 px decode is about 13 MB."));
    rawLayout->addRow(QStringLiteral("Keep in memory:"), rawCacheMemoryCombo_);

    auto *usageRow = new QWidget(rawGroup);
    auto *usageLayout = new QHBoxLayout(usageRow);
    usageLayout->setContentsMargins(0, 0, 0, 0);
    rawCacheUsageLabel_ = new QLabel(usageRow);
    usageLayout->addWidget(rawCacheUsageLabel_, /*stretch=*/1);
    auto *clearRawButton = new QPushButton(QStringLiteral("Clear Cache"), usageRow);
    connect(clearRawButton, &QPushButton::clicked, this, &PreferencesDialog::onClearRawCacheClicked);
    usageLayout->addWidget(clearRawButton);
    rawLayout->addRow(QStringLiteral("Currently using:"), usageRow);
    refreshRawCacheUsage();

    generalLayout->addWidget(rawGroup);

    // --- Decode limits ----------------------------------------------------------------
    // Ceilings on what one image is allowed to cost to open. See core/decode/DecodeLimits.h
    // for what these bound and the file that made them necessary.
    auto *limitsGroup = new QGroupBox(QStringLiteral("Decode limits"), generalTab);
    auto *limitsLayout = new QFormLayout(limitsGroup);

    maxDecodeFileSizeCombo_ = new QComboBox(limitsGroup);
    maxDecodeFileSizeCombo_->setEditable(true);
    maxDecodeFileSizeCombo_->setInsertPolicy(QComboBox::NoInsert);
    static const BudgetPreset kFileSizeLimits[] = {
        {"128 MB", 128LL * 1024 * 1024},
        {"256 MB", 256LL * 1024 * 1024},
        {"512 MB", 512LL * 1024 * 1024},
        {"1 GB", 1LL * 1024 * 1024 * 1024},
        {"4 GB", 4LL * 1024 * 1024 * 1024},
        {"No limit", 0},
    };
    for (const BudgetPreset &b : kFileSizeLimits) {
        maxDecodeFileSizeCombo_->addItem(QString::fromLatin1(b.label), (qint64)b.bytes);
    }
    const qint64 currentFileLimit = prefs::maxDecodeFileBytes();
    const int fileLimitIndex = maxDecodeFileSizeCombo_->findData(currentFileLimit);
    if (fileLimitIndex >= 0) {
        maxDecodeFileSizeCombo_->setCurrentIndex(fileLimitIndex);
    } else {
        maxDecodeFileSizeCombo_->setCurrentText(
            QStringLiteral("%1 MB").arg(currentFileLimit / (1024 * 1024)));
    }
    maxDecodeFileSizeCombo_->setToolTip(
        QStringLiteral("Opening an image reads the whole file into memory first, so this is the largest "
                        "one pixet will try. Anything bigger is listed but not thumbnailed. Videos are "
                        "streamed rather than read whole and are never affected by this."));
    limitsLayout->addRow(QStringLiteral("Largest file to open:"), maxDecodeFileSizeCombo_);

    maxDecodeMegapixelsCombo_ = new QComboBox(limitsGroup);
    maxDecodeMegapixelsCombo_->setEditable(true);
    maxDecodeMegapixelsCombo_->setInsertPolicy(QComboBox::NoInsert);
    for (int mp : prefs::kMaxDecodeMegapixelPresets) {
        maxDecodeMegapixelsCombo_->addItem(
            mp == 0 ? QStringLiteral("No limit") : QStringLiteral("%1 MP").arg(mp), mp);
    }
    const int currentMp = prefs::maxDecodeMegapixels();
    const int mpIndex = maxDecodeMegapixelsCombo_->findData(currentMp);
    if (mpIndex >= 0) {
        maxDecodeMegapixelsCombo_->setCurrentIndex(mpIndex);
    } else {
        maxDecodeMegapixelsCombo_->setCurrentText(QStringLiteral("%1 MP").arg(currentMp));
    }
    // The second limit is the one that actually does the work, so its tooltip is where the
    // reasoning goes: a file-size cap alone can't stop a small compressed file from
    // expanding into tens of gigabytes once decoded.
    maxDecodeMegapixelsCombo_->setToolTip(
        QStringLiteral("Every format except JPEG has to be decoded at its full size before it can be "
                        "shrunk to a thumbnail, at roughly 7 MB of memory per megapixel. This caps that, "
                        "and it is the limit that catches a small file which expands enormously - a "
                        "compressed gigapixel scan, or a deliberately crafted one. Today's biggest "
                        "cameras are around 150 MP."));
    limitsLayout->addRow(QStringLiteral("Largest image to decode:"), maxDecodeMegapixelsCombo_);

    auto *limitsHint = new QLabel(
        QStringLiteral("Files over either limit still appear in the grid, with a placeholder instead of a "
                        "thumbnail - they are skipped, not hidden. Raise these if you work with very large "
                        "scans or panoramas and have the memory for it."),
        limitsGroup);
    limitsHint->setWordWrap(true);
    limitsLayout->addRow(limitsHint);

    generalLayout->addWidget(limitsGroup);
    generalLayout->addStretch(1);
    addScrollingTab(generalTab, QStringLiteral("General"));

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

    addScrollingTab(keyContent, QStringLiteral("Keybindings"));

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
    addScrollingTab(maintenanceTab, QStringLiteral("Maintenance"));

    // --- OK/Cancel (shared across all tabs) ---
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &PreferencesDialog::reject);
    layout->addWidget(buttons);

    setMinimumWidth(440);
    // Taller than it was: the Keybindings tab gained four folder-navigation rows and
    // Maintenance four groups, and while both scroll now, opening straight into a
    // scrollbar for content that would have fitted is a worse first impression than a
    // slightly bigger dialog. Still well inside a laptop screen.
    resize(520, 620);
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

void PreferencesDialog::refreshRawCacheUsage() {
    const pixet::rawcache::Stats s = pixet::rawcache::stats();
    rawCacheUsageLabel_->setText(QStringLiteral("%1 MB in %2 file%3  ·  %4 MB resident")
                                      .arg(s.bytes / double(1024 * 1024), 0, 'f', 1)
                                      .arg(s.entries)
                                      .arg(s.entries == 1 ? QString() : QStringLiteral("s"))
                                      .arg(s.memoryBytes / double(1024 * 1024), 0, 'f', 1));
}

void PreferencesDialog::onClearRawCacheClicked() {
    // No confirmation: this is a cache. The worst case is that the next RAW takes as long
    // to open as it did the first time, which is not a loss worth a modal dialog.
    pixet::rawcache::clear();
    refreshRawCacheUsage();
}

qint64 PreferencesDialog::parseByteSize(const QComboBox *combo, bool bareNumberIsGb) const {
    // A preset that is still selected as-is carries its exact byte count, so prefer that
    // over re-parsing our own label and risking a rounding difference.
    const int idx = combo->currentIndex();
    if (idx >= 0 && combo->itemText(idx) == combo->currentText()) {
        return combo->itemData(idx).toLongLong();
    }

    QString text = combo->currentText().trimmed().toLower();
    if (text.isEmpty()) return -1;
    if (text.startsWith(QStringLiteral("off"))) return 0;

    // Accepts "3 gb", "3g", "750mb", "1.5 GB" - a deliberately forgiving parse, because the
    // field is free text and the units are the part people leave off or abbreviate.
    qint64 multiplier = bareNumberIsGb ? 1024LL * 1024 * 1024 : 1024LL * 1024;
    if (text.contains(QStringLiteral("t"))) multiplier = 1024LL * 1024 * 1024 * 1024;
    else if (text.contains(QStringLiteral("m"))) multiplier = 1024LL * 1024;
    else if (text.contains(QStringLiteral("k"))) multiplier = 1024LL;

    QString number;
    for (QChar c : text) {
        if (c.isDigit() || c == QLatin1Char('.')) number += c;
        else if (!number.isEmpty()) break;
    }
    bool ok = false;
    const double value = number.toDouble(&ok);
    if (!ok || value < 0) return -1;
    return (qint64)(value * multiplier);
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

    prefs::setRawCacheLongEdge(rawCacheSizeCombo_->currentData().toInt());
    // -1 means the text couldn't be parsed as a size. Leaving the old value alone beats
    // both alternatives: writing 0 would silently disable the cache over a typo, and
    // refusing to close the dialog would be a lot of ceremony for one malformed field.
    const qint64 budget = parseByteSize(rawCacheBudgetCombo_, /*bareNumberIsGb=*/true);
    if (budget >= 0) prefs::setRawCacheMaxBytes(budget);
    // Bare numbers here mean MB, not GB: the presets are in MB and "512" typed into a field
    // labelled in megabytes should not silently become half a terabyte of RAM.
    const qint64 memBudget = parseByteSize(rawCacheMemoryCombo_, /*bareNumberIsGb=*/false);
    if (memBudget >= 0) prefs::setRawCacheMemoryBytes(memBudget);

    // Bare numbers mean MB here too - the presets are mostly in MB, and someone typing
    // "750" into a field whose neighbours read "512 MB" does not mean 750 gigabytes.
    const qint64 fileLimit = parseByteSize(maxDecodeFileSizeCombo_, /*bareNumberIsGb=*/false);
    if (fileLimit >= 0) prefs::setMaxDecodeFileBytes(fileLimit);

    // Same -1 convention as the byte fields: unparseable text leaves the stored value
    // alone. "No limit" is the preset, whose data is 0, so it comes back through the
    // currentData() branch rather than the text one.
    const int mpIdx = maxDecodeMegapixelsCombo_->currentIndex();
    if (mpIdx >= 0 && maxDecodeMegapixelsCombo_->itemText(mpIdx) == maxDecodeMegapixelsCombo_->currentText()) {
        prefs::setMaxDecodeMegapixels(maxDecodeMegapixelsCombo_->itemData(mpIdx).toInt());
    } else {
        const QString mpText = maxDecodeMegapixelsCombo_->currentText().trimmed().toLower();
        if (mpText.startsWith(QStringLiteral("no"))) {
            prefs::setMaxDecodeMegapixels(0);
        } else {
            bool mpOk = false;
            const int typed = mpText.split(QLatin1Char(' ')).first().toInt(&mpOk);
            if (mpOk && typed >= 0) prefs::setMaxDecodeMegapixels(typed);
        }
    }
    // Applied immediately rather than at next launch, and this is also what trims the
    // cache when the budget was lowered - see rawcache::configure().
    emit rawCacheSettingsChanged();

    for (auto it = keyBindingEdits_.constBegin(); it != keyBindingEdits_.constEnd(); ++it) {
        keybindings::setBinding(it.key(), it.value()->keySequence());
    }

    QDialog::accept();
}
