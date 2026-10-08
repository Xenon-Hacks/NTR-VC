/*
 * audio_share -- publishes the 3DS game audio, as received from NTR-HR, to
 * named shared memory, so another program (such as a bot) can listen to
 * the game without going through the PC's speakers. The volume mixer,
 * mute and output device don't affect it.
 *
 * Windows: named file mapping "Local\NTRViewerHR_Audio"
 * Other:   POSIX shared memory "/ntrviewer_hr_audio"
 *
 * Layout (little-endian): a 64-byte header, then a ring of sample frames.
 *   0  char     magic[8]        "NTRAUDIO"
 *   8  uint32   version         1
 *  12  uint32   header_size     64
 *  16  uint32   sample_rate     32728
 *  20  uint32   channels        2
 *  24  uint32   ring_frames     sample frames the ring holds (262144, ~8 s)
 *  28  uint32   stream_id       +1 every time the stream (re)connects
 *  32  uint64   write_count     sample frames written so far, ever
 *  40  uint64   write_time_ms   system monotonic ms of the last write
 *  48  uint32   reserved[4]
 *  64  int16    ring[ring_frames][channels]
 *
 * Sample frame n (counting from 0) is at ring[n % ring_frames]. Small
 * stream gaps are filled with silence, as for playback, so the samples
 * are continuous in time; a big gap (or a reconnect) just resumes.
 *
 * Readers: read write_count (w1), copy the frames they haven't read yet
 * (at most ring_frames, the newest ones), read write_count again (w2) and
 * drop any copied frame older than w2 - ring_frames: the viewer may have
 * overwritten it while it was copied.
 *
 * The audio is written before the viewer's playback jitter buffer, so it
 * arrives at the reader a little earlier than it is heard.
 */
#ifndef AUDIO_SHARE_H
#define AUDIO_SHARE_H

#include <stdint.h>

void audio_share_init(void);
void audio_share_close(void);
/* frames: interleaved s16 stereo sample frames. One writer thread only. */
void audio_share_write(const uint8_t *pcm, int nframes);
void audio_share_new_stream(void);

#endif
