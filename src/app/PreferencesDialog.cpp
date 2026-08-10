#include "PreferencesDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Preferences.h"

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Preferences"));

    auto *layout = new QVBoxLayout(this);

    // --- Video player ---
    auto *playerGroup = new QGroupBox(QStringLiteral("Video Player"), this);
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
    layout->addWidget(playerGroup);

    bool useSystem = prefs::useSystemVideoPlayer();
    systemPlayerRadio_->setChecked(useSystem);
    customPlayerRadio_->setChecked(!useSystem);
    customPlayerPathEdit_->setText(prefs::customVideoPlayerPath());
    updateCustomPlayerEnabled();

    connect(systemPlayerRadio_, &QRadioButton::toggled, this, &PreferencesDialog::updateCustomPlayerEnabled);
    connect(browseButton_, &QPushButton::clicked, this, &PreferencesDialog::onBrowseCustomPlayer);

    // --- Thumbnails ---
    auto *thumbGroup = new QGroupBox(QStringLiteral("Thumbnails"), this);
    auto *thumbLayout = new QFormLayout(thumbGroup);
    thumbnailSizeSpin_ = new QSpinBox(thumbGroup);
    thumbnailSizeSpin_->setRange(prefs::kMinThumbnailIconSize, prefs::kMaxThumbnailIconSize);
    thumbnailSizeSpin_->setSuffix(QStringLiteral(" px"));
    thumbnailSizeSpin_->setSingleStep(10);
    originalThumbnailSize_ = prefs::thumbnailIconSize();
    thumbnailSizeSpin_->setValue(originalThumbnailSize_);
    thumbLayout->addRow(QStringLiteral("Grid thumbnail size:"), thumbnailSizeSpin_);
    layout->addWidget(thumbGroup);

    // --- Index ---
    auto *indexGroup = new QGroupBox(QStringLiteral("Index"), this);
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
    layout->addWidget(indexGroup);
    connect(reindexButton, &QPushButton::clicked, this, &PreferencesDialog::onReindexClicked);

    // --- Danger zone ---
    auto *dangerGroup = new QGroupBox(QStringLiteral("Danger Zone"), this);
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
    layout->addWidget(dangerGroup);
    connect(nukeButton, &QPushButton::clicked, this, &PreferencesDialog::onNukeClicked);

    // --- OK/Cancel ---
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &PreferencesDialog::reject);
    layout->addWidget(buttons);

    setMinimumWidth(420);
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

void PreferencesDialog::accept() {
    prefs::setUseSystemVideoPlayer(systemPlayerRadio_->isChecked());
    prefs::setCustomVideoPlayerPath(customPlayerPathEdit_->text());

    int newSize = thumbnailSizeSpin_->value();
    prefs::setThumbnailIconSize(newSize);
    if (newSize != originalThumbnailSize_) emit thumbnailSizeChanged();

    QDialog::accept();
}
