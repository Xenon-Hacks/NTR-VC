/*
 * ntr_ds -- "DS mode" receiver: shows the two 256x192 screens of a
 * Nintendo DS game streamed as raw lines over UDP (port 8300 by default,
 * override with the environment variable NTRVIEWER_DS_PORT).
 *
 * Packet layout (little-endian), one UDP datagram each:
 *   0  char   magic[4]     "NDS1"
 *   4  uint16 frame_id     +1 per frame, per screen (wraps)
 *   6  uint8  screen       0 = top, 1 = bottom
 *   7  uint8  format       0 = BGR555 (DS native), 1 = RGB565
 *   8  uint8  line_start   first line in this packet (0..191)
 *   9  uint8  line_count   lines in this packet (1..191)
 *  10  uint16 reserved     0
 *  12  pixels line_count * 256 uint16 pixels, left to right, top to bottom
 *
 * A frame is shown when all 192 lines of it have arrived, when a newer
 * frame_id starts, or 100 ms after its last packet. Missing lines keep the
 * previous frame's content. Older frame_ids than the current one are dropped.
 */
#ifndef NTR_DS_H
#define NTR_DS_H

#include <stdint.h>
#include <stdbool.h>

#define DS_SCREEN_W 256
#define DS_SCREEN_H 192
#define DS_SCREEN_COUNT 2
#define DS_PORT_DEFAULT 8300
#define DS_HDR_SIZE 12
#define DS_FORMAT_BGR555 0
#define DS_FORMAT_RGB565 1
#define DS_FLUSH_MS 100

/* One screen's frame being put together. Pixels are stored the way the
 * renderer expects: the 3DS's rotated layout, DS_SCREEN_W rows of
 * DS_SCREEN_H RGBA pixels, each row one screen column, bottom pixel first. */
struct ds_assembler {
    uint8_t rgba[DS_SCREEN_W * DS_SCREEN_H * 4];
    uint8_t lines_seen[DS_SCREEN_H];
    int lines_seen_count;
    uint16_t frame_id;
    bool started;   /* a frame_id has been seen */
    bool pending;   /* lines received that have not been shown yet */
    int bytes;      /* payload bytes for the pending frame */
};

typedef void (*ds_publish_fn)(int screen, const uint8_t *rgba, int width, int height, int bytes, void *user);

void ds_assembler_reset(struct ds_assembler *a);

/* Feeds one datagram. Returns the screen it was for, or -1 if it was
 * rejected (bad magic, size or line range, or a stale frame_id). */
int ds_feed(struct ds_assembler a[DS_SCREEN_COUNT], const uint8_t *buf, int len, ds_publish_fn publish, void *user);

/* Shows a screen's pending partial frame (used after DS_FLUSH_MS of silence). */
void ds_flush(struct ds_assembler *a, int screen, ds_publish_fn publish, void *user);

#ifndef NTR_DS_TEST
#include "const.h"
thread_ret_t ds_recv_thread_func(void *);
#endif

#endif
