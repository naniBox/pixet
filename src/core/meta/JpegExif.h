#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pixet {

struct ExifInfo {
    int orientation = 1; // EXIF 1..8, 1 = no transform needed

    // Byte range of the embedded thumbnail JPEG within the *same* buffer that was
    // parsed, if any (0 length = none). Cameras/phones almost always embed a small
    // (~160x120) preview here - decoding it is far cheaper than the main image.
    size_t thumbOffset = 0;
    size_t thumbLength = 0;

    bool hasThumb() const { return thumbLength > 0; }
};

// Scans JPEG markers for an APP1 "Exif" segment and pulls orientation + the
// embedded thumbnail's location out of its TIFF IFD0/IFD1. Tolerant of malformed
// input - always bounds-checks against `size` and simply stops early rather than
// throwing, since this runs over real-world files that may be truncated/corrupt.
// Deliberately minimal (just what indexing needs) and cheap enough to run on every
// JPEG during Pass B - see parseJpegExifDetails() below for the much larger tag set
// meant for on-demand display only, not the hot indexing path.
ExifInfo parseJpegExif(const uint8_t *data, size_t size);

// The wider set of standard EXIF/TIFF tags worth showing a human, not anything
// pixet's own indexing logic needs - deliberately kept out of ExifInfo/parseJpegExif
// so the hot Pass-B-thumbnailing path (every JPEG in the library) never pays for
// parsing tags nothing but an on-demand hover/detail view reads. An empty string/
// zero value means "tag absent" - never guessed or defaulted, so a caller can tell
// "camera didn't record this" apart from "this camera shoots at ISO 0".
struct ExifDetails {
    // IFD0 (whole-image) tags.
    std::string make;
    std::string model;
    std::string software;
    std::string dateTime; // file's own last-modified, per EXIF - not when the shot was taken, see dateTimeOriginal

    // Exif SubIFD tags (camera/exposure settings) - absent entirely on a screenshot,
    // a scan, or an image that's been re-saved by software that strips them.
    std::string dateTimeOriginal; // "YYYY:MM:DD HH:MM:SS", EXIF's own format - not reformatted here
    double exposureTimeSeconds = 0; // e.g. 0.004 for a 1/250s shutter
    double fNumber = 0;             // f/-number, e.g. 2.8 for f/2.8
    int isoSpeed = 0;
    double focalLengthMm = 0;
    int focalLengthIn35mm = 0; // 35mm-equivalent, 0 if the camera didn't record it
    std::string lensModel;
    double exposureBiasEv = 0; // can be negative; 0 is also the "not set" default, see hasExposureBias

    bool hasExposureTime = false;
    bool hasFNumber = false;
    bool hasIso = false;
    bool hasFocalLength = false;
    bool hasExposureBias = false;

    // GPS IFD (tag 0x8825 in IFD0, a whole separate sub-tree with its own tag
    // numbering) - present only for cameras/phones with location services enabled
    // for the shot, and routinely stripped by anything downstream that cares about
    // privacy. Decimal degrees, already sign-adjusted for hemisphere (GPSLatitudeRef/
    // GPSLongitudeRef: south/west are negative) - not the raw degrees/minutes/seconds
    // triplet EXIF itself stores, since decimal degrees is what every mapping
    // service (including the "paste this into Google Maps" use case) actually wants.
    bool hasGps = false;
    double gpsLatitude = 0;  // + = North
    double gpsLongitude = 0; // + = East
};

// Same tolerant-of-malformed-input bounds-checking discipline as parseJpegExif(), but
// walks the Exif SubIFD (tag 0x8769 in IFD0) too, and reads ASCII/RATIONAL/SRATIONAL
// values (parseJpegExif only ever needs SHORT/LONG). Intended for on-demand use (a
// hover tooltip, a detail panel) against a small prefix of the file - see
// util/FileIO.h's readFilePrefix() - not the full-file, every-JPEG indexing path.
ExifDetails parseJpegExifDetails(const uint8_t *data, size_t size);

} // namespace pixet
