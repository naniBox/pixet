#pragma once

#include <QDialog>

class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QLabel;

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

private slots:
    void onBrowseCustomPlayer();
    void onReindexClicked();
    void onNukeClicked();
    void accept() override;

private:
    QRadioButton *systemPlayerRadio_;
    QRadioButton *customPlayerRadio_;
    QLineEdit *customPlayerPathEdit_;
    QPushButton *browseButton_;
    QSpinBox *thumbnailSizeSpin_;
    QLabel *reindexStatusLabel_;
    QLabel *nukeStatusLabel_;

    int originalThumbnailSize_ = 0;

    void updateCustomPlayerEnabled();
};
