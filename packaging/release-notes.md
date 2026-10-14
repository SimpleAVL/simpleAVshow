SimpleAVShow 0.5.0 adds async video on picture layers, fixes the connected Windows ASIO device list, and reduces CI packaging and temporary artifact storage. This is a development prerelease.

## Async video layers

Use **Add track → Async picture layer**, or enable **Keep playing through pauses / holds** on a picture track, to keep video advancing while the show transport is paused or held. Async videos can loop or finish independently, support interior seeks, and stop with **Stop All** or at project end. A one-shot video that finishes during a hold retains its final frame.

Videos with linked sound must use an async audio track too, so sound and picture keep advancing together. The app validates this pairing and offers an async audio track when placing video with sound. Async picture activity contributes to the activity indicator and editing lock.

## Windows ASIO and validation

Reopening **Outputs** while an ASIO stream is open now keeps the connected interface available, including while paused. Device enumeration uses the stream owner's cached inventory rather than trying to probe another ASIO instance.

The opt-in Lighthouse demo validation now includes Windows ASIO, MIDI, OBS, and secondary-display evidence. Physical routing and complete shows still need rehearsal on your actual interface, driver, and cabling.

## CI and packaging

Platform build/test coverage is retained for code changes. Documentation-only changes use a lightweight required CI check. Release packaging runs on version tags or manual runs, temporary Actions artifacts expire after one day, superseded PR/main runs are cancelled, and builds/tests have runtime limits. Published release downloads remain available independently of temporary Actions artifacts.

## Downloads and source

| Platform | Package | Launch |
| --- | --- | --- |
| Windows 11 x64 | `SimpleAVShow-windows-x86_64.zip` | Extract the whole ZIP; run `bin/simpleavshow.exe` |
| macOS 15+ Apple Silicon | `SimpleAVShow-macos-arm64.zip` | Extract; move `simpleavshow.app` to Applications |
| Linux x64 | `SimpleAVShow-linux-x86_64.AppImage` | `chmod +x`, then run; Ubuntu 24.04 / glibc 2.39 or newer |

Runtime dependencies and the [user manual](https://github.com/SimpleAVL/simpleAVshow/blob/v0.5.0/docs/README.md) are bundled. `SHA256SUMS.txt` contains package and source checksums. If FUSE is unavailable on Linux, set `APPIMAGE_EXTRACT_AND_RUN=1`.

SimpleAVShow is free software under the GNU GPL, version 3 or later. Every package includes `LICENSE`, third-party notices and an About dialog license notice. The release includes `SimpleAVShow-0.5.0-source.tar.gz`. Windows ASIO support uses Steinberg's ASIO SDK 2.3.4 under its GPLv3 option.

These builds are unsigned; macOS uses ad-hoc signing without notarization. Rehearse complete shows on the actual show hardware. See [hardware validation](https://github.com/SimpleAVL/simpleAVshow/blob/v0.5.0/docs/hardware-validation.md).
