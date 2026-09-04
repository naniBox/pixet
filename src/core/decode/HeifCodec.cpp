#include "HeifCodec.h"

#include <cstring>

#include <libheif/heif.h>

#include "DecodeLimits.h"

namespace pixet {

namespace {

bool decodeHandle(const heif_image_handle *handle, RgbImage &out) {
    // The handle knows the size without decoding, so refuse here rather than after
    // heif_decode_image has already materialized the full-resolution plane. Applies to the
    // embedded-preview path too (decodeHeifThumb), whose handle is the thumbnail's own and
    // so will pass this on any file where the main image wouldn't.
    if (!decodelimits::pixelsAllowed(heif_image_handle_get_width(handle),
                                      heif_image_handle_get_height(handle))) {
        return false;
    }

    heif_image *img = nullptr;
    heif_error err = heif_decode_image(handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGB, nullptr);
    if (err.code != heif_error_Ok || !img) return false;

    int w = heif_image_get_width(img, heif_channel_interleaved);
    int h = heif_image_get_height(img, heif_channel_interleaved);
    int stride = 0;
    const uint8_t *plane = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);

    bool ok = false;
    if (plane && w > 0 && h > 0) {
        out.w = w;
        out.h = h;
        out.pixels.resize((size_t)w * h * 3);
        // stride may include row padding beyond width*3 - copy row by row rather than
        // assuming it's tightly packed like RgbImage itself always is.
        for (int y = 0; y < h; ++y) {
            std::memcpy(out.pixels.data() + (size_t)y * w * 3, plane + (size_t)y * stride, (size_t)w * 3);
        }
        ok = true;
    }

    heif_image_release(img);
    return ok;
}

} // namespace

bool decodeHeifThumb(const uint8_t *data, size_t size, RgbImage &out) {
    heif_context *ctx = heif_context_alloc();
    if (!ctx) return false;

    bool ok = false;
    if (heif_context_read_from_memory_without_copy(ctx, data, size, nullptr).code == heif_error_Ok) {
        heif_image_handle *primary = nullptr;
        if (heif_context_get_primary_image_handle(ctx, &primary).code == heif_error_Ok) {
            heif_item_id thumbId = 0;
            if (heif_image_handle_get_number_of_thumbnails(primary) > 0 &&
                heif_image_handle_get_list_of_thumbnail_IDs(primary, &thumbId, 1) > 0) {
                heif_image_handle *thumbHandle = nullptr;
                if (heif_image_handle_get_thumbnail(primary, thumbId, &thumbHandle).code == heif_error_Ok) {
                    ok = decodeHandle(thumbHandle, out);
                    heif_image_handle_release(thumbHandle);
                }
            }
            heif_image_handle_release(primary);
        }
    }

    heif_context_free(ctx);
    return ok;
}

bool decodeHeif(const uint8_t *data, size_t size, RgbImage &out) {
    heif_context *ctx = heif_context_alloc();
    if (!ctx) return false;

    bool ok = false;
    if (heif_context_read_from_memory_without_copy(ctx, data, size, nullptr).code == heif_error_Ok) {
        heif_image_handle *handle = nullptr;
        if (heif_context_get_primary_image_handle(ctx, &handle).code == heif_error_Ok) {
            ok = decodeHandle(handle, out);
            heif_image_handle_release(handle);
        }
    }

    heif_context_free(ctx);
    return ok;
}

bool readHeifDimensions(const uint8_t *data, size_t size, int &width, int &height) {
    heif_context *ctx = heif_context_alloc();
    if (!ctx) return false;

    bool ok = false;
    if (heif_context_read_from_memory_without_copy(ctx, data, size, nullptr).code == heif_error_Ok) {
        heif_image_handle *handle = nullptr;
        if (heif_context_get_primary_image_handle(ctx, &handle).code == heif_error_Ok) {
            width = heif_image_handle_get_width(handle);
            height = heif_image_handle_get_height(handle);
            ok = width > 0 && height > 0;
            heif_image_handle_release(handle);
        }
    }

    heif_context_free(ctx);
    return ok;
}

} // namespace pixet
