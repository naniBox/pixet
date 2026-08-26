#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../db/Schema.h"

namespace pixet {

// Which rung of the extraction ladder actually produced the thumbnail. The indexer's
// throughput benchmark reports its numbers broken down by this.
enum class ThumbTier {
    EmbeddedPreview, // decoded from an EXIF/container-embedded thumbnail - cheapest
    Decoded,         // decoded the main image (scaled-DCT where the format allows it)
    Unsupported,     // no decoder for this format yet
    Failed,          // decode was attempted and failed (corrupt/truncated file)
};

struct ThumbResult {
    ThumbTier tier = ThumbTier::Failed;
    std::vector<uint8_t> jpegBytes; // encoded thumbnail, ready to store in thumbs.db
    int width = 0;  // thumbnail's own pixel dimensions (what's actually in jpegBytes)
    int height = 0;
    // The original file's true native pixel dimensions - independent of `width`/
    // `height` above, which describe the (much smaller) generated thumbnail. Either
    // may be 0 if the source dimensions couldn't be determined (e.g. read failed).
    int origWidth = 0;
    int origHeight = 0;
    int orientation = 1;

    // EXIF GPS, extracted while the file's bytes are already in hand for the thumbnail.
    // gpsChecked is what separates "looked, this shot has no coordinates" from "nothing has
    // looked at this file yet" - the grid's geotag marker has to tell those apart, and the
    // backfill sweep needs to know which files it can skip. See files.gps_checked.
    bool gpsChecked = false;
    bool hasGps = false;
    double gpsLatitude = 0;  // decimal degrees, + = North
    double gpsLongitude = 0; // decimal degrees, + = East
};

// Runs the extraction ladder for one file: embedded preview -> scaled decode ->
// resize to targetLongEdge -> re-encode as JPEG q`quality`. Never throws - every
// failure mode (missing file, corrupt data, unsupported format) comes back as a
// ThumbResult with the appropriate tier so a bulk scan can continue past it.
//
// forceFullRender skips straight past the embedded-preview rung for formats that have
// one (currently just RAW) - only meaningful there; every other format already always
// does its best available decode; RAW's embedded preview is camera-baked rather than
// rendered from the actual sensor data, which is what makes it worth an explicit
// opt-in "no really, do the expensive real render" escape hatch (see
// FileState::DoneNeedsRender and IndexOptions::renderRaws).
// `filePath` is UTF-8.
ThumbResult generateThumb(const std::string &filePath, Format fmt, int targetLongEdge = 320, int quality = 85,
                           bool forceFullRender = false);

} // namespace pixet
