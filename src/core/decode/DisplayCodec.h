#pragma once

#include <string>

#include "RgbImage.h"
#include "../db/Schema.h"

namespace pixet {

// Decodes any supported format (including Video, via its poster frame) to an RgbImage
// for on-screen display (the side preview pane, the fullscreen viewer) - as opposed to
// ThumbGenerator's job (thumbnail generation for storage, which also tracks
// tier/native dimensions and re-encodes to JPEG for thumbs.db). `filePath` is UTF-8.
//
// targetLongEdge > 0 ("fit" mode - the side preview pane, or the fullscreen viewer
// before a zoom) uses the same embedded-preview-first ladder ThumbGenerator's
// per-format functions do for JPEG/RAW/HEIC: fast, since it's what runs every time the
// selection changes while browsing. targetLongEdge <= 0 ("zoom to native resolution" -
// the fullscreen viewer's 1:1 view) skips straight to the full decode instead: for RAW
// especially, the embedded preview is *never* full sensor resolution, so honoring the
// "give me native size" request means not settling for it even though it would
// nominally satisfy a targetLongEdge-based size check.
//
// Applies orientation explicitly for JPEG (the only format whose decoder doesn't
// already auto-apply it - see JpegCodec.h vs. RawCodec.h/TiffCodec.h/HeifCodec.h).
// Returns false for Unknown or any decode failure.
bool decodeForDisplay(const std::string &filePath, Format fmt, int targetLongEdge, RgbImage &out);

} // namespace pixet
