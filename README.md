# NTR-VC

NTR-VC is a modified version (fork) of **[NTRViewer-HR](https://github.com/xzn/ntrviewer-hr) by xzn**, the PC viewer for wireless screen streaming from a New 3DS / New 2DS running [NTR-HR](https://github.com/xzn/ntr-hr). All credit for the viewer itself goes to the upstream project. Please report problems with this version here, not upstream.

## What's different from upstream

- **Frame sharing:** every decoded top-screen frame and the connection quality are published to shared memory (`Local\NTRViewerHR_TopScreen` on Windows), so another program (for example an OpenCV bot) can read the picture even while the viewer window is minimised.
- **Audio sharing:** the 3DS game audio is published to shared memory too (`Local\NTRViewerHR_Audio`, a ring of the last ~8 s of 16-bit stereo at 32728 Hz), copied as it arrives, so a bot can listen to the game whatever the PC's volume, mute or output device.
- **Saved settings:** every setting in the Remote Play and Debug windows (including Audio), the 3DS IP and the window size and position are remembered in `ntrviewer_settings.ini` next to the exe. Delete that file to go back to the defaults.
- **Diagnostic log:** each run writes `ntrviewer_log_<date>_<time>.txt` next to the exe, with heartbeat lines and a stall watchdog, to help track down long-run freezes.

The full source, including a `frame_share.patch` against upstream and `FRAME_SHARE_README.md` with details, is in `ntrviewer-hr-frame-share.zip`.

## Download and build

Prebuilt Windows builds are on the [Releases](../../releases) page. To build, run the **Build NTRViewer-HR (frame sharing, lite)** workflow from the Actions tab; enter a `release_tag` to publish the result as a release.

## License

NTRViewer-HR and the NTR-VC changes are under the MIT License, see [LICENSE](LICENSE). Third-party code bundled in or linked into the viewer keeps its own license; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Release zips include all of these license files.
