#include "HoverInfoWorker.h"

#include <QStringList>

#include "db/Schema.h"
#include "meta/JpegExif.h"
#include "util/FileIO.h"

namespace {

// EXIF APP1 segments are capped at 64KB by the JPEG spec itself (a 16-bit segment
// length field), so reading this much of even a 100MB JPEG is enough to see every
// EXIF tag that could possibly be there - see readFilePrefix()'s own doc comment.
constexpr size_t kExifPrefixBytes = 64 * 1024;

QString formatShutterSpeed(double seconds) {
    if (seconds <= 0) return QString();
    if (seconds >= 1.0) return QStringLiteral("%1s").arg(seconds, 0, 'g', 3);
    return QStringLiteral("1/%1s").arg(qRound(1.0 / seconds));
}

} // namespace

namespace hoverinfo {

QString formatExifDetails(const pixet::ExifDetails &d) {
    QStringList lines;

    QString camera = QString::fromStdString(d.make);
    if (!d.model.empty()) {
        QString model = QString::fromStdString(d.model);
        camera = camera.isEmpty() ? model : QStringLiteral("%1 %2").arg(camera, model);
    }
    if (!camera.isEmpty()) lines << QStringLiteral("Camera: %1").arg(camera);
    if (!d.lensModel.empty()) lines << QStringLiteral("Lens: %1").arg(QString::fromStdString(d.lensModel));

    QStringList exposure;
    if (d.hasExposureTime) {
        QString shutter = formatShutterSpeed(d.exposureTimeSeconds);
        if (!shutter.isEmpty()) exposure << shutter;
    }
    if (d.hasFNumber && d.fNumber > 0) exposure << QStringLiteral("f/%1").arg(d.fNumber, 0, 'g', 2);
    if (d.hasIso && d.isoSpeed > 0) exposure << QStringLiteral("ISO %1").arg(d.isoSpeed);
    if (!exposure.isEmpty()) lines << QStringLiteral("Exposure: %1").arg(exposure.join(QStringLiteral(" · ")));

    if (d.hasFocalLength && d.focalLengthMm > 0) {
        QString fl = QStringLiteral("Focal length: %1mm").arg(d.focalLengthMm, 0, 'g', 3);
        if (d.focalLengthIn35mm > 0) fl += QStringLiteral(" (%1mm eq.)").arg(d.focalLengthIn35mm);
        lines << fl;
    }
    if (d.hasExposureBias && d.exposureBiasEv != 0) {
        lines << QStringLiteral("Exposure bias: %1%2 EV")
                     .arg(d.exposureBiasEv > 0 ? QStringLiteral("+") : QString())
                     .arg(d.exposureBiasEv, 0, 'g', 2);
    }
    if (!d.dateTimeOriginal.empty())
        lines << QStringLiteral("Date taken (EXIF): %1").arg(QString::fromStdString(d.dateTimeOriginal));
    if (!d.software.empty()) lines << QStringLiteral("Software: %1").arg(QString::fromStdString(d.software));

    if (d.hasGps) {
        lines << QStringLiteral("GPS: %1, %2")
                     .arg(d.gpsLatitude, 0, 'f', 6)
                     .arg(d.gpsLongitude, 0, 'f', 6);
    }

    return lines.join(QStringLiteral("\n"));
}

pixet::ExifDetails readExifDetailsSync(const QString &path, int format) {
    if ((pixet::Format)format != pixet::Format::Jpeg) return {};

    std::vector<uint8_t> prefix;
    if (!pixet::readFilePrefix(path.toStdString(), kExifPrefixBytes, prefix) || prefix.empty()) return {};

    return pixet::parseJpegExifDetails(prefix.data(), prefix.size());
}

} // namespace hoverinfo

HoverInfoWorker::HoverInfoWorker(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start();
}

HoverInfoWorker::~HoverInfoWorker() {
    thread_.quit();
    thread_.wait();
}

void HoverInfoWorker::request(quint64 id, QString path, int format) {
    pixet::ExifDetails details = hoverinfo::readExifDetailsSync(path, format);
    emit ready(id, hoverinfo::formatExifDetails(details));
}
