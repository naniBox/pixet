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

} // namespace

ExifInfo parseJpegExif(const uint8_t *data, size_t size) {
    ExifInfo out;
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return out; // not a JPEG (no SOI)

    size_t pos = 2;
    while (pos + 2 <= size) {
        if (data[pos] != 0xFF) break;
        uint8_t marker = data[pos + 1];
        pos += 2;

        if (marker == 0xD9) break;                                  // EOI
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; // TEM / RSTn - no payload
        if (marker == 0xDA) break;                                  // SOS - compressed data follows, stop

        if (pos + 2 > size) break;
        uint16_t segLen = (uint16_t)((data[pos] << 8) | data[pos + 1]);
        if (segLen < 2 || pos + segLen > size) break;

        if (marker == 0xE1 && segLen >= 8) {
            size_t payloadOff = pos + 2;
            size_t payloadLen = segLen - 2;
            if (payloadLen >= 6 && std::memcmp(data + payloadOff, "Exif\0\0", 6) == 0) {
                parseTiff(data, size, payloadOff + 6, payloadLen - 6, out);
            }
        }

        pos += segLen;
    }

    return out;
}

} // namespace pixet
