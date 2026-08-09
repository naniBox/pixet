#pragma once

#include <string>

#include "RgbImage.h"

namespace pixet {

// Extracts a single poster frame from a video file: seeks to min(3s, 10% of
// duration), decodes the nearest keyframe at or before that point, and converts it to
// RgbImage via swscale. No playback engine, no audio - just enough to get a
// representative thumbnail source frame.
//
// Takes a file path rather than an in-memory buffer, unlike every other codec here:
// video files can be enormous (hundreds of MB to GB, unlike the few-MB images every
// other decodeXxx() call handles), and FFmpeg's own demuxer already does efficient
// seeking straight from disk without needing the whole file read into memory first -
// the buffer-based pattern the image codecs use would mean reading gigabytes just to
// reach a few seconds in.
//
// Corrects for any rotation the video's own display-matrix metadata indicates (common
// for phone-recorded portrait video - the frame itself is stored landscape with a
// "rotate 90" flag rather than actually re-encoded portrait) so the poster frame comes
// out upright, the video equivalent of applyOrientation() for EXIF-rotated photos.
//
// Returns false on any error (missing/corrupt file, no video stream, decode failure)
// rather than throwing - this runs unattended over a real, messy file library.
bool decodeVideoPosterFrame(const std::wstring &filePath, RgbImage &out);

} // namespace pixet
