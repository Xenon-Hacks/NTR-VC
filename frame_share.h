/*
 * frame_share -- publishes every decoded TOP-screen frame (and the
 * connection quality) to named shared memory, so another program (such
 * as a bot) can read the picture even while this window is minimised.
 *
 * Windows: named file mapping "Local\NTRViewerHR_TopScreen"
 * Other:   POSIX shared memory "/ntrviewer_hr_top_screen"
 *
 * Layout (little-endian): a 64-byte header, then the pixels.
 *   0  char     magic[8]        "NTRFRAME"
 *   8  uint32   version         1
 *  12  uint32   header_size     64
 *  16  uint32   seq             odd while a frame is being written
 *  20  uint32   frame_index     +1 for every published frame
 *  24  uint32   width           short side as decoded (240)
 *  28  uint32   height          long side (400, or 800 in wide mode)
 *  32  uint32   channels        4 (B, G, R, A)
 *  36  uint32   is_wide
 *  40  uint64   frame_time_ms   system monotonic ms when published
 *  48  int32    quality_x10     connection quality x10, -1 if unknown
 *  52  uint32   reserved
 *  56  uint64   quality_time_ms monotonic ms of the last quality update
 *  64  pixels   height rows of width BGRA pixels (the 3DS's rotated
 *               framebuffer layout, exactly as the renderer receives it)
 *
 * Readers: read seq (must be even), copy, read seq again; if it changed,
 * retry.
 */
#ifndef FRAME_SHARE_H
#define FRAME_SHARE_H

#include <stdint.h>

void frame_share_init(void);
void frame_share_close(void);
void frame_share_publish(const uint8_t *bgra, int width, int height, int is_wide);
void frame_share_set_quality(double percent);
uint32_t frame_share_get_index(void);   /* frames published so far (for diagnostics) */

#endif
