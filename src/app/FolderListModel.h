#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>

// A database-free stand-in for ThumbGridModel, backing the standalone viewer (see
// main.cpp's viewer mode) - the mode `pixet <file>` launches, which shows one image
// full screen without building a MainWindow at all.
//
// It exists because FullscreenViewer's whole reason to hold a model is to answer four
// questions per row - what is it called, what format is it, how big is it natively,
// and is there a thumbnail lying around - and every one of those is answerable from a
// plain directory listing. ThumbGridModel answers them from index.db, which means
// opening two SQLite files, running migrations and standing up the indexer threads
// before the first pixel can be decoded. That is the right trade when the user is
// about to browse a folder and entirely the wrong one when they double-clicked a
// single JPEG.
//
// Deliberately not a base class shared with ThumbGridModel, and deliberately not a
// mode flag on it either. The two have almost no implementation in common (one is a
// SQL query with incremental refresh, the other is one listDir() call) and the only
// thing that actually has to agree between them is the role numbering - which is why
// this uses ThumbGridModel::Role's values directly rather than declaring its own.
// See FullscreenViewer::openAt(), which takes the QAbstractListModel base both share.
//
// Rows are the supported image/video files of one directory (classifyFormat() decides,
// the same filter the indexer applies), sorted to match the grid's current sort order
// so that stepping through with the arrow keys visits files in the order the folder
// view would show them - see reload() for the one case that can't be honoured here.
class FolderListModel : public QAbstractListModel {
    Q_OBJECT

public:
    explicit FolderListModel(QObject *parent = nullptr);

    // Lists `filePath`'s containing directory and returns the row `filePath` itself
    // landed on, or -1 if the directory couldn't be read or holds no row for that name
    // (an unsupported extension, or the file vanished between the shell launching us
    // and this call). The caller is expected to fall back to the full app in that case
    // rather than showing an empty viewer.
    int setFile(const QString &filePath);

    // The directory setFile() last listed - what FullscreenViewer::openAt() wants
    // alongside the row, since rows carry only a name.
    QString directoryPath() const { return directoryPath_; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

public slots:
    // Records a row's true native pixel size, learnt from a full-resolution decode
    // that has already happened - see FullscreenViewer::nativeSizeDiscovered().
    //
    // Nothing here reads image headers: the alternative to being told is opening every
    // file to parse dimensions out of it, which for a folder of RAWs is exactly the
    // startup cost this whole class exists to avoid. So WidthRole/HeightRole answer 0
    // until the viewer's zoom prefetch lands, and zoom stays unavailable until then -
    // the same "no known native size" path FullscreenViewer already takes for a file
    // the indexer hasn't measured yet, and it self-corrects within a second of opening.
    void setNativeSize(int row, QSize size);

private:
    struct Row {
        QString name;
        int fmt = 0;    // pixet::Format
        qint64 size = 0;
        qint64 mtime = 0;
        int width = 0;  // 0 until setNativeSize() is told otherwise
        int height = 0;
    };

    QString directoryPath_;
    QVector<Row> rows_;
};
