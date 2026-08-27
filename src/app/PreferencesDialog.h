#pragma once

#include <QDialog>
#include <QMap>

#include "KeyBindings.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QLabel;
class QKeySequenceEdit;
class QSpinBox;

// Preferences dialog: default video player (system default, or a custom override -
// see MainWindow::onGridItemActivated() for where this is actually consumed), grid
// thumbnail size, a "Re-index Known Folders" action button, and a destructive
// "Reset Index" button (confirmed here, performed by MainWindow - see
// nukeDatabaseRequested()). Settings are loaded from prefs:: on construction and only
// written back on OK - Cancel discards edits in progress. Both action buttons are
// fire-and-forget rather than pending settings, so they take effect immediately
// regardless of OK/Cancel.
class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

signals:
    // "Re-index Known Folders" was clicked - MainWindow forwards this to
    // BackgroundReconciler::triggerFullSweepNow().
    void reindexRequested();
    // OK was clicked and the thumbnail size actually changed - MainWindow needs to
    // re-apply it to the live grid (ThumbGridView::applyIconSizeChange() plus a
    // reload so already-decoded pixmaps get re-requested at the new size).
    void thumbnailSizeChanged();
    // "Reset Index" was clicked *and* the user confirmed the warning dialog here -
    // MainWindow does the actual deletion (it owns the live Database connection this
    // dialog deliberately doesn't touch directly) and refreshes the grid afterward.
    void nukeDatabaseRequested();
    // "Database Statistics..." was clicked - MainWindow opens the dialog, for the same
    // reason nukeDatabaseRequested() exists: it owns the live Database connection, which
    // this dialog deliberately never touches directly.
    void databaseStatsRequested();
    // OK was clicked - MainWindow re-applies the RAW cache settings to pixet_core (see
    // rawcache::configure()). Emitted unconditionally rather than only on a change: the
    // call is cheap, and configure() is also what trims the cache when the budget was
    // lowered, so skipping it when "nothing changed" would be the one case that matters.
    void rawCacheSettingsChanged();

private slots:
    void onBrowseCustomPlayer();
    void onReindexClicked();
    void onNukeClicked();
    void onClearRawCacheClicked();
    void accept() override;

private:
    QRadioButton *systemPlayerRadio_;
    QRadioButton *customPlayerRadio_;
    QLineEdit *customPlayerPathEdit_;
    QPushButton *browseButton_;
    QComboBox *thumbnailSizeCombo_;
    QCheckBox *autoRethumbCheck_;
    // 0 = auto-detect (prefs::indexerThreadCount()) - see IndexOptions::threadCount.
    // Only affects on-demand navigation (FolderIndexer) and pixet-index's default;
    // BackgroundReconciler/RawRenderer stay pinned to 1 regardless of this setting.
    QSpinBox *indexerThreadsSpin_;
    // RAW decode cache - see core/cache/RawCache.h. The size combo is a fixed list; the
    // budget combo is editable, because "how much disk am I willing to spend" is a
    // genuinely personal number that no list of presets can cover.
    QComboBox *rawCacheSizeCombo_;
    QComboBox *rawCacheBudgetCombo_;
    QComboBox *rawCacheMemoryCombo_;
    QLabel *rawCacheUsageLabel_;
    QLabel *reindexStatusLabel_;
    QLabel *nukeStatusLabel_;
    QMap<keybindings::Action, QKeySequenceEdit *> keyBindingEdits_;

    int originalThumbnailSize_ = 0;

    void updateCustomPlayerEnabled();
    // Re-reads the cache directory and puts the total in rawCacheUsageLabel_. Walks the
    // directory, so it is called on open and after a clear, not on every keystroke.
    void refreshRawCacheUsage();
    // Parses the budget combo's current text - a preset's own data, or something the user
    // typed like "3 GB" / "500mb" / "1.5g". Returns -1 if it can't be read as a size, which
    // accept() treats as "leave the setting alone" rather than as zero.
    qint64 parseRawCacheBudget(const QComboBox *combo, bool bareNumberIsGb) const;
    void onResetKeyBindings();
    // Checks every keyBindingEdits_ entry against reservedSequences() and against
    // each other; on conflict, shows a QMessageBox naming both actions involved and
    // returns false. Called from accept() - a conflict blocks the whole dialog from
    // closing (not just the keybindings section), same as any other invalid input
    // would.
    bool validateKeyBindings();
};
