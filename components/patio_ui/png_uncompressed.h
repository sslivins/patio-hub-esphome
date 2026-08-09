/*
 * Minimal uncompressed PNG encoder for screenshots.
 *
 * Produces valid PNG files using DEFLATE stored (uncompressed) blocks.
 * Extremely lightweight on CPU — no zlib compression, just memcpy +
 * CRC32/Adler32 checksums.  Output is ~2.77 MB for a 720×1280 RGB888
 * image (vs ~800 KB compressed), but encoding takes <50 ms instead of
 * several seconds.
 *
 * Designed for streaming over HTTP chunked transfer — the write callback
 * is invoked incrementally so no second buffer allocation is needed.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write callback — called repeatedly with chunks of PNG data.
 *
 * @param ctx   Opaque context (e.g. httpd_req_t*)
 * @param buf   Data to write
 * @param len   Number of bytes
 * @return ESP_OK to continue, any other value to abort encoding
 */
typedef esp_err_t (*png_write_fn_t)(void *ctx, const void *buf, size_t len);

/**
 * Stream an uncompressed PNG of tightly-packed RGB888 pixel data.
 *
 * Pixels must be row-major, no stride padding, 3 bytes per pixel (R,G,B).
 * The write callback is called multiple times with chunks of the PNG file.
 *
 * Memory usage: only a small stack buffer (~16 bytes) beyond the input
 * pixel buffer — no heap allocation.
 *
 * @param pixels    w×h×3 bytes of RGB888 data
 * @param w         Image width in pixels
 * @param h         Image height in pixels
 * @param write_fn  Callback to emit PNG bytes
 * @param ctx       Opaque context passed to write_fn
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for bad params,
 *         or the first error returned by write_fn
 */
esp_err_t png_encode_uncompressed_rgb888(const uint8_t *pixels, uint32_t w, uint32_t h,
                                          png_write_fn_t write_fn, void *ctx);

#ifdef __cplusplus
}
#endif
