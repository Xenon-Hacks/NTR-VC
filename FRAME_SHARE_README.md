# NTRViewer-HR with frame sharing

This is NTRViewer-HR (MIT licence -- see LICENSE) plus one small feature:
every decoded top-screen frame, and the connection quality, is published
to shared memory, so another program (the Sweet Scent bot) can read the
game picture **while this window is minimised**. Everything else behaves
exactly like the normal viewer.

What changed (also in `frame_share.patch`):
- new files `frame_share.c` / `frame_share.h`
- `ntr_rp.c`: publish each finished top-screen frame
- `ui_common_sdl.c`: publish the connection quality
- `main.c`: set up / clean up the shared memory
- `Makefile`: build `frame_share.o`

## Build it on GitHub (recommended, nothing to install)

1. Create a new repository on GitHub (private is fine), e.g.
   `ntrviewer-hr-frame-share`.
2. Put the contents of this folder in it, **including the hidden
   `.github` folder** (it contains the build recipes). The browser upload
   can skip hidden folders, so the easiest way is GitHub Desktop: clone the
   empty repository, copy these files into it, then Commit and Push.
3. On the repository's page: **Actions** tab -> enable workflows if asked
   -> pick **windows_app_lite.yml** -> **Run workflow**.
4. After roughly 5-10 minutes, open the finished run; under **Artifacts**
   download **NTRViewer-HR-Lite-Win-x64.zip**. It contains `ntrviewer.exe`.
5. Use that `ntrviewer.exe` instead of the original, with the same
   settings (3DS IP address etc.).

## Build it locally (advanced)

Install MSYS2, open the **MINGW64** shell, then:

    pacman -S make zip mingw-w64-x86_64-sdl3 mingw-w64-x86_64-gcc mingw-w64-x86_64-nasm
    mkdir lib
    ln -s "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64\dxcompiler.lib" lib/dxcompiler.lib
    make -j$(nproc) LITE=1

(The `dxcompiler.lib` line needs the Windows 10/11 SDK installed; the
version number in the path may differ on your PC.)

## Updating to a newer NTRViewer-HR later

Get the new source, then from its folder, in the MSYS2 or Git Bash shell:

    patch -p2 < frame_share.patch

and build it again as above.

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

Changes in this build:
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

## Audio sharing

The game audio NTR-HR sends (NTR-HR+ audio, turned on in the viewer's
Debug window) is also published to shared memory, `Local\NTRViewerHR_Audio`
on Windows or `/ntrviewer_hr_audio` on Linux: a ring of the last ~8 s of
16-bit stereo samples at 32728 Hz. It is copied as each audio packet
arrives, before playback, so the PC's volume, mute and output device make
no difference to it, and it keeps working with no audio device at all.
Small gaps are filled with silence, as for playback. Layout and how to read
it safely are at the top of `audio_share.h`. Files: `audio_share.c` /
`audio_share.h`, hooked in `ntr_audio.c` and `main.c` (not in
`frame_share.patch`).

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
receiver without a console, run `tools/ds_test_sender.py` from the NTR-VC
repository (`python ds_test_sender.py`, or add the PC's IP if the viewer runs
on another machine): the top screen shows colour bars with a sweeping white
bar, the bottom screen a scrolling checkerboard.

New files `ntr_ds.c` / `ntr_ds.h`; `ntr_rp.c` gains `rp_publish_frame()`;
`main.c` starts the receiver thread; `Makefile` builds `ntr_ds.o`.
