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
