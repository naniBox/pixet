#include "TestHarness.h"

#include "decode/JpegCodec.h"
#include "meta/JpegExif.h"

#include <cstdint>
#include <vector>

using namespace pixet;

namespace {

// Minimal byte-buffer builder so the synthetic JPEG/EXIF fixture below is
// self-consistent (offsets recorded from actual writes) instead of hand-computed
// magic numbers that could silently test a bug-compatible reimplementation.
struct ByteBuilder {
    std::vector<uint8_t> bytes;
    size_t pos() const { return bytes.size(); }
    void u8(uint8_t v) { bytes.push_back(v); }
    void u16le(uint16_t v) {
        bytes.push_back((uint8_t)(v & 0xFF));
        bytes.push_back((uint8_t)(v >> 8));
    }
    void u32le(uint32_t v) {
        bytes.push_back((uint8_t)(v & 0xFF));
        bytes.push_back((uint8_t)((v >> 8) & 0xFF));
        bytes.push_back((uint8_t)((v >> 16) & 0xFF));
        bytes.push_back((uint8_t)((v >> 24) & 0xFF));
    }
    void raw(const std::vector<uint8_t> &d) { bytes.insert(bytes.end(), d.begin(), d.end()); }
    void patchU32le(size_t at, uint32_t v) {
        bytes[at + 0] = (uint8_t)(v & 0xFF);
        bytes[at + 1] = (uint8_t)((v >> 8) & 0xFF);
        bytes[at + 2] = (uint8_t)((v >> 16) & 0xFF);
        bytes[at + 3] = (uint8_t)((v >> 24) & 0xFF);
    }
};

} // namespace

PIXET_TEST(ExifParsesOrientationAndEmbeddedThumb) {
    // Real embedded-thumbnail bytes: a tiny genuine JPEG produced by our own encoder.
    RgbImage tiny;
    tiny.w = 4;
    tiny.h = 4;
    tiny.pixels.assign(4 * 4 * 3, 128);
    std::vector<uint8_t> thumbJpeg;
    PIXET_CHECK(encodeJpeg(tiny, 80, thumbJpeg));
    PIXET_CHECK(!thumbJpeg.empty());

    ByteBuilder b;
    b.u8(0xFF);
    b.u8(0xD8); // SOI

    b.u8(0xFF);
    b.u8(0xE1); // APP1
    size_t lenFieldPos = b.pos();
    b.u16le(0); // length placeholder (patched below; JPEG segment lengths are big-endian)

    size_t app1PayloadStart = b.pos();
    b.raw({'E', 'x', 'i', 'f', 0, 0});

    size_t tiffStart = b.pos();
    b.u8('I');
    b.u8('I'); // little-endian TIFF
    b.u16le(0x002A);
    b.u32le(8); // IFD0 offset, relative to tiffStart

    // IFD0: one entry (Orientation = 6, "rotate 90 CW")
    b.u16le(1);
    b.u16le(0x0112);
    b.u16le(3); // type SHORT
    b.u32le(1); // count
    b.u16le(6);
    b.u16le(0); // value (left-justified in the 4-byte field) + padding
    size_t ifd0NextIfdFieldPos = b.pos();
    b.u32le(0); // next-IFD offset placeholder

    size_t ifd1Rel = (uint32_t)(b.pos() - tiffStart);

    // IFD1: JPEGInterchangeFormat (offset) + JPEGInterchangeFormatLength
    b.u16le(2);
    b.u16le(0x0201);
    b.u16le(4); // type LONG
    b.u32le(1);
    size_t jpegOffsetFieldPos = b.pos();
    b.u32le(0); // patched below once we know where the thumb bytes land

    b.u16le(0x0202);
    b.u16le(4);
    b.u32le(1);
    b.u32le((uint32_t)thumbJpeg.size());

    b.u32le(0); // no IFD2

    size_t thumbAbsPos = b.pos();
    b.raw(thumbJpeg);

    b.patchU32le(ifd0NextIfdFieldPos, (uint32_t)ifd1Rel);
    b.patchU32le(jpegOffsetFieldPos, (uint32_t)(thumbAbsPos - tiffStart));

    size_t app1Len = b.pos() - app1PayloadStart + 2; // includes the length field itself
    b.bytes[lenFieldPos + 0] = (uint8_t)((app1Len >> 8) & 0xFF); // big-endian
    b.bytes[lenFieldPos + 1] = (uint8_t)(app1Len & 0xFF);

    ExifInfo info = parseJpegExif(b.bytes.data(), b.bytes.size());
    PIXET_CHECK(info.orientation == 6);
    PIXET_CHECK(info.hasThumb());
    PIXET_CHECK(info.thumbLength == thumbJpeg.size());
    PIXET_CHECK(info.thumbOffset == thumbAbsPos);

    RgbImage decoded;
    PIXET_CHECK(decodeJpeg(b.bytes.data() + info.thumbOffset, info.thumbLength, 0, decoded));
    PIXET_CHECK(decoded.w == 4 && decoded.h == 4);
}

PIXET_TEST(ExifDefaultsOrientationWhenAbsent) {
    std::vector<uint8_t> data = {0xFF, 0xD8, 0xFF, 0xD9}; // SOI + EOI, no APP1 at all
    ExifInfo info = parseJpegExif(data.data(), data.size());
    PIXET_CHECK(info.orientation == 1);
    PIXET_CHECK(!info.hasThumb());
}
