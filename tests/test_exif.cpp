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

namespace {
// Appends a null-padded-to-even-length ASCII string (TIFF external values are
// conventionally word-aligned, though the reader doesn't require it) and returns
// its absolute byte position - used for IFD entries whose 4-byte value field holds
// an *offset* to this, rather than the bytes themselves.
size_t appendAsciiExternal(ByteBuilder &b, const std::string &s) {
    size_t pos = b.pos();
    for (char c : s) b.u8((uint8_t)c);
    b.u8(0); // NUL terminator, included in the tag's `count`
    return pos;
}

size_t appendRationalExternal(ByteBuilder &b, uint32_t num, uint32_t den) {
    size_t pos = b.pos();
    b.u32le(num);
    b.u32le(den);
    return pos;
}
} // namespace

// Builds a JPEG with an IFD0 (Make/Model/Software/DateTime + an Exif SubIFD pointer)
// and an Exif SubIFD (DateTimeOriginal/ExposureTime/FNumber/ISO/FocalLength/
// ExposureBias/FocalLengthIn35mm/LensModel) - deliberately one test covering most
// fields at once rather than one per tag, since they all exercise the same few code
// paths (external ASCII, external RATIONAL/SRATIONAL, inline SHORT) repeated with
// different tag numbers.
PIXET_TEST(ExifDetailsParsesCameraAndExposureTags) {
    ByteBuilder b;
    b.u8(0xFF);
    b.u8(0xD8); // SOI

    b.u8(0xFF);
    b.u8(0xE1); // APP1
    size_t lenFieldPos = b.pos();
    b.u16le(0); // length placeholder, big-endian, patched below

    size_t app1PayloadStart = b.pos();
    b.raw({'E', 'x', 'i', 'f', 0, 0});

    size_t tiffStart = b.pos();
    b.u8('I');
    b.u8('I'); // little-endian TIFF
    b.u16le(0x002A);
    b.u32le(8); // IFD0 offset, relative to tiffStart

    // IFD0: Make, Model, Software, DateTime (all ASCII, external - long enough to
    // need it) + the Exif SubIFD pointer (LONG, tag 0x8769).
    b.u16le(5);

    // Writes an ASCII entry with a placeholder value-field (patched once the
    // string's external position is known, after everything below is laid out) and
    // returns that field's position.
    auto writeAsciiEntryPlaceholder = [&](uint16_t tag, uint32_t count) -> size_t {
        b.u16le(tag);
        b.u16le(2); // type ASCII
        b.u32le(count);
        size_t valueFieldPos = b.pos();
        b.u32le(0); // patched once the string's external position is known
        return valueFieldPos;
    };

    std::string make = "PixelCam";
    std::string model = "PixelCam Mark II";
    std::string software = "pixet-test/1.0";
    std::string dateTime = "2026:01:02 03:04:05";

    size_t makeValuePos = writeAsciiEntryPlaceholder(0x010F, (uint32_t)make.size() + 1);
    size_t modelValuePos = writeAsciiEntryPlaceholder(0x0110, (uint32_t)model.size() + 1);
    size_t softwareValuePos = writeAsciiEntryPlaceholder(0x0131, (uint32_t)software.size() + 1);
    size_t dateTimeValuePos = writeAsciiEntryPlaceholder(0x0132, (uint32_t)dateTime.size() + 1);

    b.u16le(0x8769);
    b.u16le(4); // type LONG
    b.u32le(1);
    size_t exifSubIfdPtrPos = b.pos();
    b.u32le(0); // patched once the SubIFD's position is known

    size_t ifd0NextIfdFieldPos = b.pos();
    b.u32le(0); // no IFD1

    // External data for IFD0's ASCII values.
    size_t makeAbs = appendAsciiExternal(b, make);
    size_t modelAbs = appendAsciiExternal(b, model);
    size_t softwareAbs = appendAsciiExternal(b, software);
    size_t dateTimeAbs = appendAsciiExternal(b, dateTime);

    // Exif SubIFD: DateTimeOriginal (ASCII), ExposureTime (RATIONAL), FNumber
    // (RATIONAL), ISOSpeedRatings (SHORT, inline), FocalLength (RATIONAL),
    // ExposureBiasValue (SRATIONAL, negative - -1/3 EV), FocalLengthIn35mmFilm
    // (SHORT, inline), LensModel (ASCII).
    size_t exifSubIfdPos = b.pos();
    b.u16le(8);

    std::string dateTimeOriginal = "2026:01:02 03:03:59";
    size_t dtoValuePos = writeAsciiEntryPlaceholder(0x9003, (uint32_t)dateTimeOriginal.size() + 1);

    b.u16le(0x829A);
    b.u16le(5); // RATIONAL
    b.u32le(1);
    size_t exposureTimeValuePos = b.pos();
    b.u32le(0);

    b.u16le(0x829D);
    b.u16le(5);
    b.u32le(1);
    size_t fNumberValuePos = b.pos();
    b.u32le(0);

    b.u16le(0x8827);
    b.u16le(3); // SHORT
    b.u32le(1);
    b.u16le(400);
    b.u16le(0); // padding to fill the 4-byte value field

    b.u16le(0x920A);
    b.u16le(5);
    b.u32le(1);
    size_t focalLengthValuePos = b.pos();
    b.u32le(0);

    b.u16le(0x9204);
    b.u16le(10); // SRATIONAL
    b.u32le(1);
    size_t exposureBiasValuePos = b.pos();
    b.u32le(0);

    b.u16le(0xA405);
    b.u16le(3);
    b.u32le(1);
    b.u16le(50);
    b.u16le(0);

    std::string lensModel = "PixelCam 24-70mm f/2.8";
    size_t lensValuePos = writeAsciiEntryPlaceholder(0xA434, (uint32_t)lensModel.size() + 1);

    size_t subIfdNextIfdFieldPos = b.pos();
    b.u32le(0);

    size_t dtoAbs = appendAsciiExternal(b, dateTimeOriginal);
    size_t exposureTimeAbs = appendRationalExternal(b, 1, 250);   // 1/250s
    size_t fNumberAbs = appendRationalExternal(b, 28, 10);        // f/2.8
    size_t focalLengthAbs = appendRationalExternal(b, 50, 1);     // 50mm
    size_t exposureBiasAbs = b.pos();
    b.u32le((uint32_t)(int32_t)-1);
    b.u32le(3); // -1/3 EV
    size_t lensAbs = appendAsciiExternal(b, lensModel);

    // Patch every offset now that everything's final position is known.
    b.patchU32le(makeValuePos, (uint32_t)(makeAbs - tiffStart));
    b.patchU32le(modelValuePos, (uint32_t)(modelAbs - tiffStart));
    b.patchU32le(softwareValuePos, (uint32_t)(softwareAbs - tiffStart));
    b.patchU32le(dateTimeValuePos, (uint32_t)(dateTimeAbs - tiffStart));
    b.patchU32le(exifSubIfdPtrPos, (uint32_t)(exifSubIfdPos - tiffStart));
    b.patchU32le(ifd0NextIfdFieldPos, 0);
    b.patchU32le(dtoValuePos, (uint32_t)(dtoAbs - tiffStart));
    b.patchU32le(exposureTimeValuePos, (uint32_t)(exposureTimeAbs - tiffStart));
    b.patchU32le(fNumberValuePos, (uint32_t)(fNumberAbs - tiffStart));
    b.patchU32le(focalLengthValuePos, (uint32_t)(focalLengthAbs - tiffStart));
    b.patchU32le(exposureBiasValuePos, (uint32_t)(exposureBiasAbs - tiffStart));
    b.patchU32le(lensValuePos, (uint32_t)(lensAbs - tiffStart));
    b.patchU32le(subIfdNextIfdFieldPos, 0);

    size_t app1Len = b.pos() - app1PayloadStart + 2;
    b.bytes[lenFieldPos + 0] = (uint8_t)((app1Len >> 8) & 0xFF); // big-endian
    b.bytes[lenFieldPos + 1] = (uint8_t)(app1Len & 0xFF);

    ExifDetails d = parseJpegExifDetails(b.bytes.data(), b.bytes.size());
    PIXET_CHECK(d.make == make);
    PIXET_CHECK(d.model == model);
    PIXET_CHECK(d.software == software);
    PIXET_CHECK(d.dateTime == dateTime);
    PIXET_CHECK(d.dateTimeOriginal == dateTimeOriginal);
    PIXET_CHECK(d.hasExposureTime);
    PIXET_CHECK(d.exposureTimeSeconds > 0.003 && d.exposureTimeSeconds < 0.005); // 1/250
    PIXET_CHECK(d.hasFNumber);
    PIXET_CHECK(d.fNumber > 2.7 && d.fNumber < 2.9); // 2.8
    PIXET_CHECK(d.hasIso);
    PIXET_CHECK(d.isoSpeed == 400);
    PIXET_CHECK(d.hasFocalLength);
    PIXET_CHECK(d.focalLengthMm > 49.0 && d.focalLengthMm < 51.0); // 50mm
    PIXET_CHECK(d.focalLengthIn35mm == 50);
    PIXET_CHECK(d.hasExposureBias);
    PIXET_CHECK(d.exposureBiasEv < 0); // -1/3 EV, negative
    PIXET_CHECK(d.lensModel == lensModel);
}

// Builds a JPEG with just IFD0 -> GPS IFD (tag 0x8825) - no Exif SubIFD at all, since
// GPS lives in its own sub-tree with independent tag numbering and this test exists
// to cover that walk specifically, not re-exercise the camera/exposure tags above.
PIXET_TEST(ExifDetailsParsesGpsCoordinates) {
    ByteBuilder b;
    b.u8(0xFF);
    b.u8(0xD8); // SOI

    b.u8(0xFF);
    b.u8(0xE1); // APP1
    size_t lenFieldPos = b.pos();
    b.u16le(0); // length placeholder, big-endian, patched below

    size_t app1PayloadStart = b.pos();
    b.raw({'E', 'x', 'i', 'f', 0, 0});

    size_t tiffStart = b.pos();
    b.u8('I');
    b.u8('I'); // little-endian TIFF
    b.u16le(0x002A);
    b.u32le(8); // IFD0 offset, relative to tiffStart

    // IFD0: just the GPS IFD pointer (LONG, tag 0x8825).
    b.u16le(1);
    b.u16le(0x8825);
    b.u16le(4); // type LONG
    b.u32le(1);
    size_t gpsIfdPtrPos = b.pos();
    b.u32le(0); // patched once the GPS IFD's position is known
    size_t ifd0NextIfdFieldPos = b.pos();
    b.u32le(0); // no IFD1

    // GPS IFD: GPSLatitudeRef "N", GPSLatitude (37, 46, 30 -> 37.775 deg), GPSLongitudeRef "W",
    // GPSLongitude (122, 25, 10 -> ~122.419444 deg, negated for West).
    size_t gpsIfdPos = b.pos();
    b.u16le(4);

    b.u16le(0x0001);
    b.u16le(2); // ASCII
    b.u32le(2); // "N" + NUL
    b.u8('N');
    b.u8(0);
    b.u16le(0); // padding

    b.u16le(0x0002);
    b.u16le(5); // RATIONAL
    b.u32le(3); // degrees, minutes, seconds
    size_t latValuePos = b.pos();
    b.u32le(0); // patched

    b.u16le(0x0003);
    b.u16le(2); // ASCII
    b.u32le(2); // "W" + NUL
    b.u8('W');
    b.u8(0);
    b.u16le(0); // padding

    b.u16le(0x0004);
    b.u16le(5); // RATIONAL
    b.u32le(3);
    size_t lonValuePos = b.pos();
    b.u32le(0); // patched

    size_t gpsIfdNextIfdFieldPos = b.pos();
    b.u32le(0);

    size_t latAbs = b.pos();
    appendRationalExternal(b, 37, 1);
    appendRationalExternal(b, 46, 1);
    appendRationalExternal(b, 30, 1);

    size_t lonAbs = b.pos();
    appendRationalExternal(b, 122, 1);
    appendRationalExternal(b, 25, 1);
    appendRationalExternal(b, 10, 1);

    b.patchU32le(gpsIfdPtrPos, (uint32_t)(gpsIfdPos - tiffStart));
    b.patchU32le(ifd0NextIfdFieldPos, 0);
    b.patchU32le(latValuePos, (uint32_t)(latAbs - tiffStart));
    b.patchU32le(lonValuePos, (uint32_t)(lonAbs - tiffStart));
    b.patchU32le(gpsIfdNextIfdFieldPos, 0);

    size_t app1Len = b.pos() - app1PayloadStart + 2;
    b.bytes[lenFieldPos + 0] = (uint8_t)((app1Len >> 8) & 0xFF); // big-endian
    b.bytes[lenFieldPos + 1] = (uint8_t)(app1Len & 0xFF);

    ExifDetails d = parseJpegExifDetails(b.bytes.data(), b.bytes.size());
    PIXET_CHECK(d.hasGps);
    PIXET_CHECK(d.gpsLatitude > 37.77 && d.gpsLatitude < 37.78);   // 37.775 N -> positive
    PIXET_CHECK(d.gpsLongitude < -122.41 && d.gpsLongitude > -122.43); // ~122.419444 W -> negative
}

PIXET_TEST(ExifDetailsLeavesEverythingAbsentWithoutExifSegment) {
    std::vector<uint8_t> data = {0xFF, 0xD8, 0xFF, 0xD9}; // SOI + EOI, no APP1 at all
    ExifDetails d = parseJpegExifDetails(data.data(), data.size());
    PIXET_CHECK(d.make.empty());
    PIXET_CHECK(d.model.empty());
    PIXET_CHECK(!d.hasExposureTime);
    PIXET_CHECK(!d.hasFNumber);
    PIXET_CHECK(!d.hasIso);
    PIXET_CHECK(!d.hasFocalLength);
    PIXET_CHECK(!d.hasExposureBias);
    PIXET_CHECK(!d.hasGps);
}
