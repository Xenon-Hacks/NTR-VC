/*
 * diag -- long-run diagnostics for NTRViewer-HR (frame-share build).
 *
 *  - redirects stderr (everything err_log() prints) into a log file
 *    "ntrviewer_log_<date>_<time>.txt" next to the exe (or current dir)
 *  - every 10 s writes a one-line heartbeat: packet / frame / render counters,
 *    how long ago each stage last did something, and process memory/handle
 *    counts (to spot leaks)
 *  - once a second checks for stalls (network silent, decoder stuck, render
 *    stuck) and logs which stage each thread was in; the first stall of each
 *    kind also writes a minidump "ntrviewer_stall_<kind>_<time>.dmp"
 *  - on a crash, logs the exception and writes "ntrviewer_crash_<time>.dmp"
 *
 * Disable with the environment variable NTRVIEWER_NO_LOG=1.
 */
#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* thread stage ids (see diag.c for names) */
enum {
    DIAG_UDP_STARTUP = 0, DIAG_UDP_RECVFROM, DIAG_UDP_POLL, DIAG_UDP_ACTION, DIAG_UDP_REPLY,
    DIAG_DEC_IDLE = 10, DIAG_DEC_DECODE, DIAG_DEC_PUBLISH_LOCK, DIAG_DEC_PUBLISH,
    DIAG_REN_IDLE = 20, DIAG_REN_WAIT_FRAME, DIAG_REN_SYNC_WAIT, DIAG_REN_DRAW, DIAG_REN_UI, DIAG_REN_PRESENT,
};

enum { DIAG_T_PKT, DIAG_T_DEC_TOP, DIAG_T_DEC_BOT, DIAG_T_RENDER, DIAG_T_MAIN, DIAG_T_COUNT };

extern uint32_t diag_last_ms[DIAG_T_COUNT];   /* tick (ms) of the last event */
extern uint64_t diag_count[DIAG_T_COUNT];     /* total events */
extern int diag_stage_udp, diag_stage_dec, diag_stage_ren;

uint32_t diag_now_ms(void);
void diag_init(void);
void diag_close(void);
const char *diag_ts(void);   /* "HH:MM:SS.mmm" since start, thread-local buffer */

#define DIAG_TICK(k) do { __atomic_store_n(&diag_last_ms[k], diag_now_ms(), __ATOMIC_RELAXED); \
                          __atomic_add_fetch(&diag_count[k], 1, __ATOMIC_RELAXED); } while (0)
#define DIAG_STAGE(var, v) __atomic_store_n(&(var), (v), __ATOMIC_RELAXED)

#ifdef __cplusplus
}
#endif
#endif
