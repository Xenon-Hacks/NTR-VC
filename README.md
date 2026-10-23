# NTR-VC

NTR-VC is a fork of **[NTRViewer-HR](https://github.com/xzn/ntrviewer-hr) by xzn**, the PC viewer for wireless screen streaming from a New 3DS / New 2DS running [NTR-HR](https://github.com/xzn/ntr-hr). It adds what you need to run the viewer next to a bot, or for hours unattended. All credit for the viewer itself goes to the upstream project. Please report problems with this version here, not upstream.

For setting up NTR-HR and the viewer's normal settings, see the upstream README ([README-upstream.md](README-upstream.md)) and the [3DS wireless streaming guide](https://wiki.hacks.guide/wiki/3DS:Wireless_streaming).

## What's different from upstream

- **Frame sharing:** every decoded top-screen frame and the connection quality are published to shared memory (`Local\NTRViewerHR_TopScreen` on Windows), so another program (for example an OpenCV bot) can read the picture even while the viewer window is minimised. An event, `Local\NTRViewerHR_TopScreenFrame`, is set after each frame so a reader can wait instead of polling.
- **Audio sharing:** the 3DS game audio is published to shared memory too (`Local\NTRViewerHR_Audio`, a ring of the last ~8 s of 16-bit stereo at 32728 Hz), copied as it arrives, so a bot can listen to the game whatever the PC's volume, mute or output device. Turn on Audio (NTR-HR+) in the Debug window.
- **Saved settings:** every setting in the Remote Play and Debug windows (including Audio), the 3DS IP and the window size and position are remembered in `ntrviewer_settings.ini` next to the exe. Delete that file to go back to the defaults.
- **Stream-drop recovery:** the viewer notices when the 3DS stops answering or stops sending video, reconnects on its own with backoff, asks for the stream again, and logs whether the 3DS still answers ping (to tell a Wi-Fi drop from a hung NTR). Windows power throttling is turned off for the viewer so it keeps full speed while minimised.
- **Diagnostic log:** each run writes `ntrviewer_log_<date>_<time>.txt` in the folder it was started from, with heartbeat lines and a stall watchdog, to help track down long-run freezes. Set `NTRVIEWER_NO_LOG=1` to turn it off.
- **DS mode receiver:** the viewer also listens on UDP port 8300 for the two 256x192 screens of a Nintendo DS game, sent as raw lines, and shows them in the normal top and bottom views. Nothing on the 3DS sends this stream yet; this is the PC half of DS mode streaming. To try it without a console, run `python ds_test_sender.py` (in the release zip and in [tools/](tools/)) while the viewer is open. Set `NTRVIEWER_DS_PORT` to use another port.

The shared-memory layouts, the log format and every change are described in [NTR-VC.md](NTR-VC.md). NTR-VC branches from upstream commit 627c018, so `git diff 627c018` (or [this comparison](../../compare/627c018...main)) shows exactly what changed.

## Download and build

Prebuilt Windows builds are on the [Releases](../../releases) page: download `NTR-VC-Win-x64.zip`, unzip it and run `ntrviewer.exe`. Releases 2.0.0 to 2.5.0 were made before NTR-VC became a fork and are named `NTRViewer-HR-FrameShare-Lite-Win-x64.zip`.

To build, run the **Build NTR-VC** workflow from the Actions tab; enter a `release_tag` to publish the result as a release. Local build steps are in [NTR-VC.md](NTR-VC.md).

## License

NTRViewer-HR and the NTR-VC changes are under the MIT License, see [LICENSE](LICENSE). Third-party code bundled in or linked into the viewer keeps its own license; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Release zips include all of these license files.
