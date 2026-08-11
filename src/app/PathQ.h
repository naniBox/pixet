#pragma once

#include <QString>

#include "util/PathUtil.h"

// QString-side wrapper over pixet_core's path helpers. Deliberately its own header rather
// than living in QtInterop.h: that one exists to convert a decoded RgbImage into a QImage
// and drags in decode/JpegCodec.h to do it, which has no business being pulled into
// MainWindow or FullscreenViewer just to join two strings.

// Joins a directory path and a single entry name exactly the way pixet_core does, so the
// result string-compares equal to what's stored in dirs.path and is what the decoders
// expect to be handed.
//
// Shared rather than duplicated because MainWindow and FullscreenViewer both build file
// paths from the (directory, name) pairs coming out of ThumbGridModel, and the two must
// agree - they previously each did `dir + QStringLiteral("\\") + name` inline, which was
// invisible on Windows and silently broke every preview, fullscreen decode and video
// launch anywhere else.
inline QString joinPathQ(const QString &dir, const QString &name) {
    return QString::fromStdString(pixet::joinPath(dir.toStdString(), name.toStdString()));
}
