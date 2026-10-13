SimpleAVShow 0.4.0 adds direct keyframe editing on the timeline and shaped automation curves. This is a development prerelease.

## Keyframes on the timeline

- Drag automation points directly on the timeline to change their time and value. Times follow Snap and stay between neighbouring points; hold Shift to change only the value.
- Click the selected clip's curve to add a point, double-click a point to delete it, and right-click for the point and segment menu.
- The selected clip shows the property chosen in the inspector (Volume, Pan, Position X/Y, Scale or Opacity), with a dashed base-value line when it has no keyframes yet. Every change is one undo step and respects the existing editing locks.

## Curve shapes

- Each segment can be **Linear**, **Hold** (step), **Curve** or **S-curve**. Curve and S-curve have an adjustable bend from −1 to 1.
- Alt/Option-drag a segment on the timeline to bend it, or set shape and bend in the inspector's automation table.
- Shapes are saved per keyframe. Unshaped curves keep the original `[time, value]` format; agents can read and write shapes through MCP. Trimming and splitting keep the segment shape at the cut.
- The demo's ambience fades now use an S-curve fade-in and a curved fade-out.

See [automation](https://github.com/staubichsauger/simpleAVshow/blob/v0.4.0/docs/editing-a-show.md#automation-and-fade-helpers) in the manual.

## Fixes

- Windows: the fullscreen audience display now opens on the selected screen.

Projects that use the new shapes cannot be opened by SimpleAVShow 0.3.0 or earlier.

## License and source

SimpleAVShow is free software under the GNU GPL, version 3 or later. Every package includes `LICENSE`, third-party notices and an About dialog license notice. This release includes `SimpleAVShow-0.4.0-source.tar.gz`. Windows ASIO support uses Steinberg's ASIO SDK 2.3.4 under its GPLv3 option.

## Downloads

| Platform | Package | Launch |
| --- | --- | --- |
| Windows 11 x64 | `SimpleAVShow-windows-x86_64.zip` | Extract the whole ZIP; run `bin/simpleavshow.exe` |
| macOS 15+ Apple Silicon | `SimpleAVShow-macos-arm64.zip` | Extract; move `simpleavshow.app` to Applications |
| Linux x64 | `SimpleAVShow-linux-x86_64.AppImage` | `chmod +x`, then run; Ubuntu 24.04 / glibc 2.39 or newer |

Runtime dependencies and the user manual are bundled. `SHA256SUMS.txt` contains package and source checksums. If FUSE is unavailable on Linux, set `APPIMAGE_EXTRACT_AND_RUN=1`.

These builds are unsigned; macOS uses ad-hoc signing without notarization. Downloaded-app security prompts may appear.

## Validation and limits

The release workflow publishes only after transport tests on all three platforms, ThreadSanitizer, desktop builds and tests on Windows/macOS/Linux, and packaged-app launch smoke tests pass. The MCP suite covers authentication, discovery, atomic edits/undo, conflicts, linked trimming, background imports and operator controls. An official TypeScript MCP SDK 1.32.1 client was also checked against the server.

This remains a development prototype. Rehearse ASIO routing, audio/MIDI equipment, audience output and complete shows on the actual show hardware. Video decoding and composition use software FFmpeg/QPainter; import mono/stereo stems separately. NDI is outside this version. See [hardware validation](https://github.com/staubichsauger/simpleAVshow/blob/v0.4.0/docs/hardware-validation.md).
