#include "JpegExif.h"

#include <cstring>

namespace pixet {

namespace {

struct TiffReader {
    const uint8_t *data;
    size_t size; // total buffer size (bounds are always checked against this)
    bool little;

    uint16_t u16(size_t off) const {
        return little ? (uint16_t)(data[off] | (data[off + 1] << 8)) : (uint16_t)((data[off] << 8) | data[off + 1]);
    }
    uint32_t u32(size_t off) const {
        return little ? (uint32_t)(data[off] | (data[off + 1] << 8) | (data[off + 2] << 16) | (data[off + 3] << 24))
                       : (uint32_t)((data[off] << 24) | (data[off + 1] << 16) | (data[off + 2] << 8) | data[off + 3]);
    }
    bool inBounds(size_t off, size_t len) const { return off + len <= size; }

    // ASCII (type 2): `count` includes the trailing NUL. Inline in the 4-byte value
    // field if it fits (count<=4); otherwise the field holds an offset (relative to
    // `tiffStart`) to the string elsewhere in the buffer. Trailing NULs/whitespace
    // trimmed - EXIF ASCII fields are routinely padded.
    std::string ascii(size_t tiffStart, size_t valueFieldOff, uint32_t count) const {
        if (count == 0) return {};
        size_t strOff = (count <= 4) ? valueFieldOff : tiffStart + u32(valueFieldOff);
        if (!inBounds(strOff, count)) return {};
        size_t len = count;
        while (len > 0 && (data[strOff + len - 1] == '\0' || data[strOff + len - 1] == ' ')) --len;
        return std::string((const char *)data + strOff, len);
    }

    // RATIONAL (type 5, always a pointer - 8 bytes never fits inline): two LONGs,
    // numerator then denominator. A zero denominator (seen in the wild from buggy
    // writers) reads as 0 rather than dividing by zero.
    double rational(size_t tiffStart, size_t valueFieldOff) const {
        size_t off = tiffStart + u32(valueFieldOff);
        if (!inBounds(off, 8)) return 0;
        uint32_t num = u32(off), den = u32(off + 4);
        return den == 0 ? 0.0 : (double)num / (double)den;
    }

    // SRATIONAL (type 10): same layout, but both halves are signed.
    double srational(size_t tiffStart, size_t valueFieldOff) const {
        size_t off = tiffStart + u32(valueFieldOff);
        if (!inBounds(off, 8)) return 0;
        int32_t num = (int32_t)u32(off), den = (int32_t)u32(off + 4);
        return den == 0 ? 0.0 : (double)num / (double)den;
    }

    // One element of a RATIONAL array (type 5, count>1 - e.g. GPSLatitude's
    // degrees/minutes/seconds triplet). Always a pointer, same as a lone rational();
    // `index` selects which consecutive 8-byte pair to read.
    double rationalAt(size_t tiffStart, size_t valueFieldOff, uint32_t index) const {
        size_t arrayOff = tiffStart + u32(valueFieldOff);
        size_t off = arrayOff + (size_t)index * 8;
        if (!inBounds(off, 8)) return 0;
        uint32_t num = u32(off), den = u32(off + 4);
        return den == 0 ? 0.0 : (double)num / (double)den;
    }
};

// Reads one IFD starting at `ifdPos` (absolute offset into the buffer). Calls
// `onEntry(tag, type, count, valueFieldOffset)` for each entry. Returns the
// offset to the next IFD (0 if none/invalid).
template <typename F>
uint32_t readIfd(const TiffReader &r, size_t ifdPos, F onEntry) {
    if (!r.inBounds(ifdPos, 2)) return 0;
    uint16_t count = r.u16(ifdPos);
    size_t entriesStart = ifdPos + 2;
    if (!r.inBounds(entriesStart, (size_t)count * 12)) return 0;

    for (uint16_t i = 0; i < count; ++i) {
        size_t entryOff = entriesStart + (size_t)i * 12;
        uint16_t tag = r.u16(entryOff);
        uint16_t type = r.u16(entryOff + 2);
        uint32_t valCount = r.u32(entryOff + 4);
        onEntry(tag, type, valCount, entryOff + 8);
    }

    size_t nextIfdPos = entriesStart + (size_t)count * 12;
    if (!r.inBounds(nextIfdPos, 4)) return 0;
    return r.u32(nextIfdPos);
}

// tiffStart/tiffLen delimit the TIFF blob within `data` (right after the "Exif\0\0"
// marker in the APP1 segment). All offsets inside the TIFF (IFD pointers, value
// offsets) are relative to tiffStart.
void parseTiff(const uint8_t *data, size_t size, size_t tiffStart, size_t tiffLen, ExifInfo &out) {
    if (tiffLen < 8 || tiffStart + tiffLen > size) return;

    bool little;
    if (data[tiffStart] == 'I' && data[tiffStart + 1] == 'I') {
        little = true;
    } else if (data[tiffStart] == 'M' && data[tiffStart + 1] == 'M') {
        little = false;
    } else {
        return;
    }

    TiffReader r{data, tiffStart + tiffLen, little};
    if (r.u16(tiffStart + 2) != 0x002A) return;
    uint32_t ifd0Rel = r.u32(tiffStart + 4);

    uint32_t orientation = 1;
    uint32_t ifd1Rel =
        readIfd(r, tiffStart + ifd0Rel, [&](uint16_t tag, uint16_t type, uint32_t, size_t valueOff) {
            if (tag == 0x0112 && type == 3 /* SHORT */) orientation = r.u16(valueOff);
        });
    if (orientation >= 1 && orientation <= 8) out.orientation = (int)orientation;

    if (ifd1Rel == 0) return;

    uint32_t jpegOffsetRel = 0, jpegLength = 0;
    readIfd(r, tiffStart + ifd1Rel, [&](uint16_t tag, uint16_t type, uint32_t, size_t valueOff) {
        if (tag == 0x0201 && type == 4 /* LONG */) jpegOffsetRel = r.u32(valueOff);
        if (tag == 0x0202 && type == 4 /* LONG */) jpegLength = r.u32(valueOff);
    });

    if (jpegLength == 0) return;
    size_t absOffset = tiffStart + jpegOffsetRel;
    if (absOffset + jpegLength > size) return; // corrupt/truncated - ignore rather than overrun

    out.thumbOffset = absOffset;
    out.thumbLength = jpegLength;
}

// Scans JPEG markers for the first APP1 "Exif\0\0" segment and hands its TIFF blob's
// bounds to `onTiff(tiffStart, tiffLen)`. Shared by parseJpegExif() and
// parseJpegExifDetails() so there's exactly one implementation of the marker walk.
template <typename F>
void forEachExifTiffBlob(const uint8_t *data, size_t size, F onTiff) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return; // not a JPEG (no SOI)

    size_t pos = 2;
    while (pos + 2 <= size) {
        if (data[pos] != 0xFF) break;
        uint8_t marker = data[pos + 1];
        pos += 2;

        if (marker == 0xD9) break;                                          // EOI
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; // TEM / RSTn - no payload
        if (marker == 0xDA) break;                                          // SOS - compressed data follows, stop

        if (pos + 2 > size) break;
        uint16_t segLen = (uint16_t)((data[pos] << 8) | data[pos + 1]);
        if (segLen < 2 || pos + segLen > size) break;

        if (marker == 0xE1 && segLen >= 8) {
            size_t payloadOff = pos + 2;
            size_t payloadLen = segLen - 2;
            if (payloadLen >= 6 && std::memcmp(data + payloadOff, "Exif\0\0", 6) == 0) {
                onTiff(payloadOff + 6, payloadLen - 6);
            }
        }

        pos += segLen;
    }
}

// Reads the whole-image tags directly out of IFD0, then follows the Exif SubIFD
// pointer (tag 0x8769, absent on non-camera JPEGs) for the camera/exposure settings
// that live there instead.
void parseTiffDetails(const uint8_t *data, size_t size, size_t tiffStart, size_t tiffLen, ExifDetails &out) {
    if (tiffLen < 8 || tiffStart + tiffLen > size) return;

    bool little;
    if (data[tiffStart] == 'I' && data[tiffStart + 1] == 'I') {
        little = true;
    } else if (data[tiffStart] == 'M' && data[tiffStart + 1] == 'M') {
        little = false;
    } else {
        return;
    }

    TiffReader r{data, tiffStart + tiffLen, little};
    if (r.u16(tiffStart + 2) != 0x002A) return;
    uint32_t ifd0Rel = r.u32(tiffStart + 4);

    uint32_t exifSubIfdRel = 0;
    uint32_t gpsIfdRel = 0;
    readIfd(r, tiffStart + ifd0Rel, [&](uint16_t tag, uint16_t type, uint32_t count, size_t valueOff) {
        switch (tag) {
            case 0x010F: out.make = r.ascii(tiffStart, valueOff, count); break;
            case 0x0110: out.model = r.ascii(tiffStart, valueOff, count); break;
            case 0x0131: out.software = r.ascii(tiffStart, valueOff, count); break;
            case 0x0132: out.dateTime = r.ascii(tiffStart, valueOff, count); break;
            case 0x8769:
                if (type == 4 /* LONG */) exifSubIfdRel = r.u32(valueOff);
                break;
            case 0x8825:
                if (type == 4 /* LONG */) gpsIfdRel = r.u32(valueOff);
                break;
            default: break;
        }
    });

    if (gpsIfdRel != 0) {
        std::string latRef, lonRef;
        double latDms[3] = {0, 0, 0}, lonDms[3] = {0, 0, 0};
        bool hasLat = false, hasLon = false;
        readIfd(r, tiffStart + gpsIfdRel, [&](uint16_t tag, uint16_t type, uint32_t count, size_t valueOff) {
            switch (tag) {
                case 0x0001: latRef = r.ascii(tiffStart, valueOff, count); break;
                case 0x0003: lonRef = r.ascii(tiffStart, valueOff, count); break;
                case 0x0002: // GPSLatitude, RATIONAL x3 (degrees, minutes, seconds)
                    if (type == 5 && count == 3) {
                        for (int i = 0; i < 3; ++i) latDms[i] = r.rationalAt(tiffStart, valueOff, i);
                        hasLat = true;
                    }
                    break;
                case 0x0004: // GPSLongitude, RATIONAL x3
                    if (type == 5 && count == 3) {
                        for (int i = 0; i < 3; ++i) lonDms[i] = r.rationalAt(tiffStart, valueOff, i);
                        hasLon = true;
                    }
                    break;
                default: break;
            }
        });
        if (hasLat && hasLon) {
            double lat = latDms[0] + latDms[1] / 60.0 + latDms[2] / 3600.0;
            double lon = lonDms[0] + lonDms[1] / 60.0 + lonDms[2] / 3600.0;
            if (!latRef.empty() && (latRef[0] == 'S' || latRef[0] == 's')) lat = -lat;
            if (!lonRef.empty() && (lonRef[0] == 'W' || lonRef[0] == 'w')) lon = -lon;
            out.gpsLatitude = lat;
            out.gpsLongitude = lon;
            out.hasGps = true;
        }
    }

    if (exifSubIfdRel == 0) return;

    readIfd(r, tiffStart + exifSubIfdRel, [&](uint16_t tag, uint16_t type, uint32_t count, size_t valueOff) {
        switch (tag) {
            case 0x9003: out.dateTimeOriginal = r.ascii(tiffStart, valueOff, count); break;
            case 0x829A: // ExposureTime, RATIONAL
                if (type == 5) { out.exposureTimeSeconds = r.rational(tiffStart, valueOff); out.hasExposureTime = true; }
                break;
            case 0x829D: // FNumber, RATIONAL
                if (type == 5) { out.fNumber = r.rational(tiffStart, valueOff); out.hasFNumber = true; }
                break;
            case 0x8827: // ISOSpeedRatings, SHORT
                if (type == 3) { out.isoSpeed = r.u16(valueOff); out.hasIso = true; }
                break;
            case 0x920A: // FocalLength, RATIONAL
                if (type == 5) { out.focalLengthMm = r.rational(tiffStart, valueOff); out.hasFocalLength = true; }
                break;
            case 0x9204: // ExposureBiasValue, SRATIONAL
                if (type == 10) { out.exposureBiasEv = r.srational(tiffStart, valueOff); out.hasExposureBias = true; }
                break;
            case 0xA405: // FocalLengthIn35mmFilm, SHORT
                if (type == 3) out.focalLengthIn35mm = r.u16(valueOff);
                break;
            case 0xA434: out.lensModel = r.ascii(tiffStart, valueOff, count); break;
            default: break;
        }
    });
}

} // namespace

ExifInfo parseJpegExif(const uint8_t *data, size_t size) {
    ExifInfo out;
    forEachExifTiffBlob(data, size,
                         [&](size_t tiffStart, size_t tiffLen) { parseTiff(data, size, tiffStart, tiffLen, out); });
    return out;
}

ExifDetails parseJpegExifDetails(const uint8_t *data, size_t size) {
    ExifDetails out;
    forEachExifTiffBlob(data, size, [&](size_t tiffStart, size_t tiffLen) {
        parseTiffDetails(data, size, tiffStart, tiffLen, out);
    });
    return out;
}

} // namespace pixet
