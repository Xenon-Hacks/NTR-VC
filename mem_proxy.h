/*
 * mem_proxy -- lets other programs read and write the 3DS game's memory
 * through NTR-HR's control link (TCP port 8000), which only one program
 * can hold: this viewer.
 *
 * Local UDP server on 127.0.0.1:45988 (MEM_PROXY_PORT) speaking Azahar's RPC
 * format, so scripts written for Azahar (UDP 45987, scripting/citra.py or
 * AzaharRPC.py) work against a real 3DS by changing the port.
 *
 * Request:  u32 version (1 or 2), u32 request id, u32 type, u32 data size, data
 * Reply:    same header (same version, id and type), data size = bytes that follow
 *
 * Azahar types (Azahar's own reply conventions):
 *   1 read        data: u32 addr, u32 size (<= 0x8000)  reply: the bytes, or nothing on failure
 *   2 write       data: u32 addr, u32 size, bytes      reply: empty on success; NO reply on failure
 *   3 processes   data: u32 start, u32 max             reply: u32 count, count x {u32 pid, u64 title id, char name[8]}
 *   4 process     data: u32 mode (0 get, 1 set), u32 pid  reply (get): u32 pid; (set): empty
 *     The process used by types 1 and 2. 0xFFFFFFFF (the default) = the current game.
 * NTR-VC types (always reply, with a status word first):
 *   0x100         same as 3
 *   0x101 read    data: u32 pid, u32 addr, u32 size    reply: u32 status, u32 pid, u32 detail, bytes
 *   0x102 write   data: u32 pid, u32 addr, u32 flags, bytes  reply: u32 status, u32 pid, u32 detail
 *                 flags bit 0: allow writing read-only pages (code patches)
 *   0x103 status  no data  reply: u32 link up, u32 memread supported, u32 requests, u32 failures,
 *                 u32 last round trip us, u32 average round trip us, u32 3DS game pid (0 unknown)
 *   status: 0 ok; 1-7 from the 3DS (see NTR-HR source/menu/memread.c);
 *           0x100 no connection to the 3DS, 0x101 timeout (old NTR-HR without memory commands?),
 *           0x102 bad request
 */
#ifndef MEM_PROXY_H
#define MEM_PROXY_H

#include <stdint.h>
#include <stdbool.h>

#define MEM_PROXY_PORT 45988

#define MEM_PROXY_CMD_LISTPROCESS 5
#define MEM_PROXY_CMD_READMEM 9
#define MEM_PROXY_CMD_WRITEMEM 10

#define MEM_PROXY_ST_OK 0
#define MEM_PROXY_ST_NO_LINK 0x100
#define MEM_PROXY_ST_TIMEOUT 0x101
#define MEM_PROXY_ST_BAD_REQUEST 0x102

#ifdef __cplusplus
extern "C" {
#endif

/* Start / stop the local UDP server (sockets must already be initialised). */
int mem_proxy_init(void);
void mem_proxy_shutdown(void);

/* --- called by the port 8000 control-link thread only --- */

/* The control link to the 3DS went up or down. Down fails everything pending. */
void mem_proxy_link(bool up);
/* Commands are waiting to be sent, or replies are outstanding. */
bool mem_proxy_has_work(void);
/* Waits up to timeout_ms for a new command to be queued (returns early when one is). */
void mem_proxy_wait(int timeout_ms);
/* Next command to send, or -1. The caller sends a header with the given cmd, args
   and data_len (and seq of its choosing), then data_len bytes of data, then calls
   mem_proxy_sent(token, seq). */
int mem_proxy_next(uint32_t *cmd, uint32_t args[16], const uint8_t **data, uint32_t *data_len);
void mem_proxy_sent(int token, uint32_t seq);
/* A reply with cmd 5, 9 or 10 arrived. Returns true if it answered a pending command. */
bool mem_proxy_reply(uint32_t seq, uint32_t cmd, const uint32_t args[16], const uint8_t *data, uint32_t data_len);

#ifdef __cplusplus
}
#endif

#endif
