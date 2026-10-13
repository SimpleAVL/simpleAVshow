Second development prerelease of SimpleAVShow, a native timeline editor and player for stage performances. This release focuses on keeping a running show alive when something goes wrong, plus a round of editing and workflow improvements. See the [review findings](https://github.com/staubichsauger/simpleAVshow/blob/v0.2.0/docs/review-findings.md) for the full list.

## Live-show resilience

- A damaged video freezes its last frame instead of stopping the show; audio and MIDI cues continue.
- Clips that already played no longer drop out after seeking back (for example, rehearse, return to the start, enter Show mode).
- A briefly unreadable audio file (antivirus lock, USB hiccup) is retried for up to 2 seconds instead of stopping playback.
- A lost USB-MIDI destination is detected and reconnects by name when it returns, even during playback. A failed or stalled audio interface is reported with the driver's reason.
- Shows with missing media open with those files offline; **File → Relink missing media** finds them by name in a folder.
- Show mode:
  - a checklist on entry: unsaved changes, missing media, audio/MIDI destinations, program output
  - a confirmation before leaving Show mode or quitting while the show runs
  - Stop All by Escape needs two presses; Space still plays and pauses everywhere
- The program output hides the cursor, remembers its display, and closes and reopens with that display.

## Audio

- Short (2.5 ms) fades remove clicks at holds, pauses, seeks, Stop All, and cut clip ends.
- Stereo sources on a single-output track are mixed to mono instead of losing the right channel. Mono sources on one output with pan at 0 now play at full level (3 dB louder than before).
- Mono files whose channel is marked front-left now play centred. Existing audio caches are rebuilt once.
- The audio cache is capped (about 20 GiB) and reopening unchanged media skips rehashing.

## Editing and workflow

- The transport bar shows the next cue, the time until it, and the time left.
- Inspector fields apply when you press Enter or leave the field, and times use timecode like the clock.
- Copy/paste, duplicate (Ctrl+D), split at the playhead (Ctrl+B), nudge (Alt+arrows), draggable cue markers, previous/next cue (Ctrl+arrows), and file drops onto the window.
- A video placed on a picture track brings its sound as a linked clip; **Edit → Unlink clip** separates them.
- The timeline follows the playhead, zooms around the cursor (Ctrl+wheel), has a Fit button, shows more tracks, and supports keyboard navigation.
- Autosave offers recovery when a newer autosave exists, removes it after saving, and also protects untitled shows.

## Downloads

| Platform | Package | Launch |
| --- | --- | --- |
| macOS 15+ Apple Silicon | `SimpleAVShow-macos-arm64.zip` | Extract; move `simpleavshow.app` to Applications |
| Windows 11 x64 | `SimpleAVShow-windows-x86_64.zip` | Extract the whole ZIP; run `bin/simpleavshow.exe` |
| Linux x64 | `SimpleAVShow-linux-x86_64.AppImage` | `chmod +x`, then run; Ubuntu 24.04 / glibc 2.39 or newer |

Runtime dependencies and third-party license notices are bundled. `SHA256SUMS.txt` contains package checksums. If FUSE is unavailable on Linux, set `APPIMAGE_EXTRACT_AND_RUN=1`.

These builds are unsigned; macOS uses ad-hoc signing without notarization. Downloaded-app security prompts may appear.

## Validation and limits

Native builds, transport tests (including a ThreadSanitizer build), media/project/UI tests, and packaged-app launch smoke tests run on GitHub Actions for macOS, Windows, and Linux. Unplugging an ASIO interface or USB-MIDI device, Windows timer behaviour, MIDI bridge timing, and full shows still need rehearsal testing on the target hardware. This is a prototype prerelease, not a validated production show system.

Video decoding and composition use software FFmpeg/QPainter. Import mono/stereo stems separately. NDI is outside this version. See [hardware validation](https://github.com/staubichsauger/simpleAVshow/blob/v0.2.0/docs/hardware-validation.md) and the [README](https://github.com/staubichsauger/simpleAVshow/blob/v0.2.0/README.md).
