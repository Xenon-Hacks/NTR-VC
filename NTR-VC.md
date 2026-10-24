# NTR-VC

NTR-VC is NTRViewer-HR by xzn (https://github.com/xzn/ntrviewer-hr,
MIT License, see LICENSE) with additions for running the viewer next to a
bot or for hours unattended:

- every decoded top-screen frame, and the connection quality, is published
  to shared memory, so another program (for example an OpenCV bot) can read
  the game picture while this window is minimised
- the game audio is published to shared memory too
- settings are saved between launches
- a diagnostic log, and automatic recovery when the 3DS stream drops
- a DS mode receiver (PC side only for now)

Everything else behaves like the normal viewer. All credit for the viewer
itself goes to xzn; please report problems with this version to the
NTR-VC repository, not upstream.

NTR-VC is a GitHub fork of upstream, branched from commit 627c018
(2026-09-18), so `git diff 627c018` shows every change.

New files: `frame_share.c/.h`, `audio_share.c/.h`, `diag.c/.h`,
`settings.c/.h`, `ntr_ds.c/.h`. Changed upstream files: `Makefile`,
`const.h`, `main.c`, `ntr_audio.c`, `ntr_common.h`, `ntr_hb.c`, `ntr_rp.c`,
`ntr_rp.h`, `ui_common_sdl.c`, `ui_main_nk.c`.

## Build it

The easiest way is GitHub Actions: in the NTR-VC repository, run the
**Build NTR-VC** workflow from the Actions tab and download the zip
from the finished run.

To build locally on Windows, install MSYS2, open the **MINGW64** shell, then
from the repository folder:

    pacman -S make zip mingw-w64-x86_64-sdl3 mingw-w64-x86_64-gcc mingw-w64-x86_64-nasm
    mkdir -p lib
    make -j$(nproc) LITE=1

This builds the "lite" `ntrviewer.exe` (SDL renderer only), which is what the
releases contain.

## Updating to a newer NTRViewer-HR later

Merge upstream's `full-width` branch into `main` and fix any conflicts.

## For programs reading the frames

Shared memory `Local\NTRViewerHR_TopScreen` (Windows) or
`/ntrviewer_hr_top_screen` (Linux): a 64-byte header then the pixels. The
exact layout, and how to read it without catching a half-written frame, is
at the top of `frame_share.h`.

## Diagnostic log (added for long-run hang hunting)

Every run now writes `ntrviewer_log_<date>_<time>.txt` in the folder you
start `ntrviewer.exe` from (all of the viewer's normal error messages go
there too, with a time-since-start stamp). Set the environment variable
`NTRVIEWER_NO_LOG=1` to turn it off.

- `HB` lines every 10 s: total/new UDP packets, decoded top/bottom frames,
  render iterations, `shared_idx` (frames published to shared memory),
  how many ms ago each stage last ran (`age_ms`), which stage each thread is
  in right now, and process memory / handle / GDI / USER / thread counts.
  A leak shows up as one of those numbers climbing for hours.
- `STALL` / `RECOVER` lines when no packets arrive, packets arrive but nothing
  decodes, or the render loop stops for more than 10 s. A STALL of the decoder
  or renderer also writes `ntrviewer_stall_*.dmp` (a Windows minidump).
- A crash writes `ntrviewer_crash_*.dmp`.
- Logs rotate at 64 MB (4 files max).

## Stream performance and reconnect (New 3DS XL tuning)

How the picture gets here: NTR-HR on the 3DS encodes each top-screen frame
as JPEG on up to 3 cores (New 3DS), sends it over UDP using KCP (reliable,
with FEC), and a TCP link on port 8000 carries control (the remote play
request, heartbeats and the 3DS's debug log). The viewer acks/nacks KCP,
reassembles the slices, decodes with libjpeg-turbo and publishes to shared
memory.

Changes:
- The process opts out of Windows power throttling (EcoQoS) and keeps 1 ms
  timers, so receive, KCP replies and decoding keep full speed while the
  window is minimised.
- The UDP receive thread runs at highest priority and the decode thread
  above normal, so a busy bot or game on the PC can't delay acks or frames.
- Each frame slot is cleared by the bytes it actually used (tens of KB)
  instead of all 2 MB, and the JPEG decoder is created once instead of
  every frame.
- If the TCP link drops while streaming, the viewer reconnects on its own
  (after 1 s, backing off to every 15 s) and asks for the stream again,
  instead of waiting for UDP packets that a reset 3DS won't send. Pressing
  Disconnect stops this. Uses the existing "Auto reconnect" setting.
- Auto-reconnect no longer misses a still-streaming 3DS after a TCP drop.
- When a reconnect attempt times out, the viewer pings the 3DS once and logs
  whether it answers ("3DS answers ping ... NTR isn't answering on port
  8000" or "3DS doesn't answer ping: it's off the network"), only when that
  changes, and "reconnected to the 3DS" once the link is back. That tells a
  3DS that fell off Wi-Fi apart from one whose NTR stopped answering.
- Windows: an auto-reset event `Local\NTRViewerHR_TopScreenFrame` is set
  after every published frame, so a reader can wait instead of polling.

Added in 2.5.0 (drop recovery):
- Heartbeat timeout: the 3DS answers every heartbeat, so 5 s without a
  reply now counts as a disconnect ("3DS stopped answering heartbeats") and
  starts the reconnect and ping logic above. Before, a 3DS that vanished
  from Wi-Fi without closing the connection was never noticed.
- The TCP send/receive wait now waits for the right socket event, gives up
  after 3 s and returns on a socket error, so a half-dead link can't park the
  control thread forever or spin a CPU core.
- If video stops for 5 s while the control link is still up, the viewer asks
  the 3DS for the stream again ("no video for N s, asking the 3DS for the
  stream again"), backing off to once a minute, and resets when video
  returns. Whether a hung NTR restarts its stream on that request is up to
  the 3DS side.
- A refused connection is logged as "connection refused" right away instead
  of as a 2 s timeout.
- A UDP receive error (adapter reset, driver restart) waits 2 s before
  rebinding instead of looping, and oversized datagrams are dropped.
- With no audio playback device the open is retried every 5 s instead of on
  every audio packet (shared audio for the bot works either way).
- Log rotation no longer appends to the first log file again after part 5.

## Audio sharing

The game audio NTR-HR sends (NTR-HR+ audio, turned on in the viewer's
Debug window) is also published to shared memory, `Local\NTRViewerHR_Audio`
on Windows or `/ntrviewer_hr_audio` on Linux: a ring of the last ~8 s of
16-bit stereo samples at 32728 Hz. It is copied as each audio packet
arrives, before playback, so the PC's volume, mute and output device make
no difference to it, and it keeps working with no audio device at all.
Small gaps are filled with silence, as for playback. Layout and how to read
it safely are at the top of `audio_share.h`. Files: `audio_share.c` /
`audio_share.h`, hooked in `ntr_audio.c` and `main.c`.

## Saved settings

The viewer remembers its settings between launches in `ntrviewer_settings.ini`, next to the exe (`settings.c`).

- **What's saved:** everything in the Remote Play window (view mode, upscaling filter, 3DS IP, viewer port, Prioritize Top Screen, Priority Screen Factor, compression format, JPEG quality or colour bias, Auto Quality, bandwidth limit, input redirection face-button swap, bottom-screen cursor size, ambience quality and blur), the Debug-window toggles (Stats, Audio (NTR-HR+), Full Width, Auto-Reconnect, Auto-Update Params), and the size and position of the top window (and the bottom one in Separate Windows mode).
- **When:** about a second after something changes, and again on exit. The file is written to a `.tmp` and then renamed, so a crash can't leave half a file. A missing file is created on the first launch with the defaults.
- **Bad or missing values** fall back to the built-in default for just that setting; delete the file to reset everything. A window saved on a monitor that's no longer connected keeps its size but not its position.
- **3DS IP:** if Auto-Detect finds a 3DS at launch, the detected address wins (the saved one is picked when it's among those found). The saved IP is used when nothing is detected.
- **Auto-Reconnect:** only the Debug-window checkbox is remembered. Clicking Disconnect turns Auto-Reconnect off for that session only, so the next launch still reconnects.
- Not saved: the Viewer IP adapter, the input-redirection controller, and fullscreen.

Hooks: `settings_load()` and `settings_apply_windows()` in `main_windows()` (`ntr_config_set_default()` moved there from `main_ntr()` so the saved values aren't overwritten), `settings_apply_ip()` after `ntr_detect_3ds_ip()`, `settings_tick()` in `main_loop()`, `settings_save()` after the main loop, and `settings_auto_*` updates around the two Debug-window checkboxes in `ui_main_nk.c`.

## DS mode receiver

The viewer also listens on **UDP port 8300** for a "DS mode" stream: the two
256x192 screens of a Nintendo DS game, sent as raw lines. Frames that arrive
there are shown in the normal top and bottom screen views (and the top screen
is published to frame sharing too, with width 192 and height 256). Set the
environment variable `NTRVIEWER_DS_PORT` to use another port.

Nothing on the 3DS sends this stream yet; it is the PC half of DS mode
streaming. The packet layout is at the top of `ntr_ds.h`. To check the
receiver without a console, run `ds_test_sender.py` (in the release zip and in
the repository's `tools` folder; add the PC's IP if the viewer runs on
another machine): the top screen shows colour bars with a sweeping white
bar, the bottom screen a scrolling checkerboard.

New files `ntr_ds.c` / `ntr_ds.h`; `ntr_rp.c` gains `rp_publish_frame()`;
`main.c` starts the receiver thread; `Makefile` builds `ntr_ds.o`.

## Game memory for other programs (mem_proxy)

Needs the NTR-HR build with memory commands (ntr-hr-memread). With it, other
programs on the PC can read and write the running game's memory through the
viewer, which holds NTR-HR's only control link (TCP port 8000).

Local UDP server on `127.0.0.1:45988`, in Azahar's RPC format: scripts written
for Azahar (`scripting/citra.py`, `AzaharRPC.py`, port 45987) work on a real
3DS by changing the port. Request: `u32 version (1 or 2), u32 id, u32 type,
u32 data size, data`; the reply has the same header. Types:

| type | request data | reply data |
|---|---|---|
| 1 read | u32 addr, u32 size (up to 0x8000) | the bytes; empty on failure |
| 2 write | u32 addr, u32 size, bytes | empty on success; no reply on failure |
| 3 / 0x100 processes | u32 start, u32 max | u32 count, then {u32 pid, u64 title id, char name[8]} each |
| 4 process | u32 mode (0 get, 1 set), u32 pid | get: u32 pid; set: empty. Default: the game |
| 0x101 read | u32 pid, u32 addr, u32 size | u32 status, u32 pid, u32 detail, bytes |
| 0x102 write | u32 pid, u32 addr, u32 flags, bytes | u32 status, u32 pid, u32 detail |
| 0x103 status | none | u32 link up, memread supported, requests, failures, last and average round trip (us), game pid |

pid `0xFFFFFFFF` means the current game. Write flag bit 0 allows read-only
pages (code patches). Status 0 is OK; 1-7 come from the 3DS (1 size, 2 no
game, 3 open failed, 4 unmapped (detail = first bad address), 5 not writable,
6 DMA failed, 7 data lost); 0x100 no connection to the 3DS, 0x101 timeout
(an NTR-HR without memory commands), 0x102 bad request.

NTR-HR commands on port 8000 (NTR 3.6 numbering, replies keep the request's
seq): 5 list processes (reply args status, count, game pid; data 24-byte
entries {pid, reserved, title id, name[8]}), 9 read (args pid, addr, size up
to 0x4000; reply args status, pid, detail and the bytes), 10 write (args pid,
addr, size, flags plus the data; reply args status, pid, detail).

How it fits in the control loop (`ntr_hb.c`, menu link only): every waiting
packet is read each pass instead of one per 250 ms; new memory commands are
sent at once, and while replies are outstanding the socket is checked every
2 ms. The heartbeat keeps its own 250 ms timer. TCP_NODELAY is on. Writes go
out only once this connection has answered a memory command (otherwise a
process-list probe goes first), because an NTR-HR without the commands would
not read the write's data and the link would break. A request with no answer
fails after 2 s; a dropped link fails everything pending at once.

Test without a 3DS: `tools/mem_proxy_test/harness.c` runs `mem_proxy.c` in the
same loop shape against a fake 3DS (see the comment at its top).

New files `mem_proxy.c` / `mem_proxy.h`; `ntr_hb.c` services the commands;
`main.c` starts and stops the server; `Makefile` builds `mem_proxy.o`.
