#pragma once

#include <cstdint>

namespace pixet {

// Ceilings on how much memory a single decode is allowed to ask for.
//
// Every image codec here decodes at the source's *native* resolution before anything is
// downscaled - libpng, libtiff, libwebp, libavif and libheif all have to, none of them
// offering libjpeg's scaled-DCT trick - so the cost of a decode is set entirely by the
// file, not by the 320px thumbnail that comes out the far end. Nothing bounded that, and
// one file was enough to prove why it needed to be: a 14.1 GiB uncompressed BigTIFF scan
// (97943 x 51536, 5.0 gigapixels) that happened to be sitting in a Downloads folder took
// ~50GB of RAM to thumbnail, in four stacked allocations that nothing in between ever
// looked at:
//
//   readWholeFile()             15.1 GB   the file, in one std::vector
//   decodeTiff()'s RGBA raster  20.2 GB   4 bytes/px at 5.0 gigapixels
//   libtiff's own strip buffer  15.1 GB   the image is a single strip, so it reads whole
//   RgbImage::pixels            15.1 GB   3 bytes/px, the same image again
//
// times whatever else the indexer's thread pool was decoding alongside it. So there are
// two separate limits here, because there are two separate ways to get there:
//
//  - maxFileBytes bounds the whole-file read that every non-video decode starts with
//    (see ThumbGenerator's generateThumb and decode/DisplayCodec.cpp). Checked from a
//    stat, before a single byte is read.
//  - maxPixels bounds the decode itself, from the dimensions in the file's header. This
//    is the one that actually matters: a *compressed* 5-gigapixel TIFF or a PNG bomb can
//    sit well under any sane file-size cap and still ask for tens of GB once expanded.
//
// A file over either limit is reported as ThumbTier::Unsupported rather than Failed - it
// is a deliberate policy decision that this file will not be thumbnailed, recorded once
// so no later scan retries it, not an error to go looking into.
//
// Configured from the GUI's preferences (prefs::applyDecodeLimits) and from pixet-index's
// command line, the same arrangement rawcache uses - pixet_core has no settings of its
// own. Left at the defaults below if nothing ever calls configure(), which is what keeps
// the tests and any future non-GUI caller working without having to know this exists.
namespace decodelimits {

// 512 MiB. Comfortably past any real photograph - a 100MB drum-scan TIFF and a 150MP
// medium-format RAW both clear it by a wide margin - while stopping the multi-gigabyte
// files that only ever end up in a photo folder by accident.
constexpr int64_t kDefaultMaxFileBytes = 512LL * 1024 * 1024;

// 500 megapixels, i.e. ~1.5GB as RgbImage and ~2GB as libtiff's intermediate RGBA raster.
// The largest cameras in real use are ~150MP, and a big stitched panorama lands in the
// low hundreds, so this leaves a factor of three over anything genuinely photographic.
constexpr int64_t kDefaultMaxPixels = 500LL * 1000 * 1000;

// Both limits at once. 0 or negative disables that limit entirely (no ceiling), which is
// how a caller opts out rather than by passing some enormous number. Safe to call from
// any thread at any time - the values are read on decode threads without locking, and a
// change mid-scan simply applies to whatever starts after it.
void configure(int64_t maxFileBytes, int64_t maxPixels);

int64_t maxFileBytes();
int64_t maxPixels();

// True if a file this big may be read whole. Also true for any size when the limit is
// disabled, and for a negative `sizeBytes` (a failed stat), which is deliberately not
// this function's business to reject - the read itself will fail soon enough and report
// it properly.
bool fileSizeAllowed(int64_t sizeBytes);

// True if an image this size may be decoded. Dimensions of 0 or less pass, so a codec's
// own "this header is nonsense" check stays the thing that reports a broken file rather
// than this quietly turning it into "too big".
bool pixelsAllowed(int64_t width, int64_t height);

} // namespace decodelimits
} // namespace pixet
