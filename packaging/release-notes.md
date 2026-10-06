First development prerelease of SimpleAVShow, a native timeline editor and player for stage performances.

Includes audio/video/image/text tracks, composed picture layers, hold and MIDI cues, async ambience, multichannel audio routing, automation, fullscreen program output, and Show mode. Includes the fix allowing Outputs to open at a stop marker in edit mode, with visibly disabled connection controls and a Stop All action.

Video scaling now converts every source row, fixing playback that showed only the top portion of a frame.

## Downloads

| Platform | Package | Launch |
| --- | --- | --- |
| macOS 15+ Apple Silicon | `SimpleAVShow-macos-arm64.zip` | Extract; move `simpleavshow.app` to Applications |
| Windows 11 x64 | `SimpleAVShow-windows-x86_64.zip` | Extract the whole ZIP; run `bin/simpleavshow.exe` |
| Linux x64 | `SimpleAVShow-linux-x86_64.AppImage` | `chmod +x`, then run; Ubuntu 24.04 / glibc 2.39 or newer |

Runtime dependencies and third-party license notices are bundled. `SHA256SUMS.txt` contains package checksums. If FUSE is unavailable on Linux, set `APPIMAGE_EXTRACT_AND_RUN=1`.

These builds are unsigned; macOS uses ad-hoc signing without notarization. Downloaded-app security prompts may appear.

## Validation and limits

Native builds, transport tests, media/project/UI tests, and packaged-app launch smoke tests run on GitHub Actions. Physical ASIO routing, MIDI bridge timing, and full shows still need rehearsal testing on the target hardware. This is a prototype prerelease, not a validated production show system.

Audio prepares to temporary 48 kHz PCM caches on disk and streams through worker-fed playback buffers, removing the 1 GiB project RAM budget. Fixed project count caps and the 24-hour duration cap are removed; interface capabilities determine available output channels. Import mono/stereo stems separately. Video decoding and composition use software FFmpeg/QPainter. NDI is outside this version. See [hardware validation](https://github.com/staubichsauger/simpleAVshow/blob/v0.1.0/docs/hardware-validation.md) and the [README](https://github.com/staubichsauger/simpleAVshow/blob/v0.1.0/README.md).
