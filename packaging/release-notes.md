SimpleAVShow 0.4.1 fixes the Windows ASIO device list after connecting an output. This is a development prerelease.

## Windows ASIO

Reopening **Outputs** while an ASIO stream was open could incorrectly show the connected interface as missing and disable its audio settings. RtAudio's ASIO backend cannot probe a second instance while a stream is open. The app now reads the existing stream owner's cached device inventory, including while paused, so the connected interface remains selectable.

An opt-in Windows regression exercises device enumeration during an open stream, continued audio callbacks, pause and resume. Live Windows validation uses virtual multichannel ASIO drivers and independent input capture. A physical ASIO interface still needs rehearsal with its own driver and cabling.

## Downloads and source

| Platform | Package | Launch |
| --- | --- | --- |
| Windows 11 x64 | `SimpleAVShow-windows-x86_64.zip` | Extract the whole ZIP; run `bin/simpleavshow.exe` |
| macOS 15+ Apple Silicon | `SimpleAVShow-macos-arm64.zip` | Extract; move `simpleavshow.app` to Applications |
| Linux x64 | `SimpleAVShow-linux-x86_64.AppImage` | `chmod +x`, then run; Ubuntu 24.04 / glibc 2.39 or newer |

Runtime dependencies and the [user manual](https://github.com/staubichsauger/simpleAVshow/blob/v0.4.1/docs/README.md) are bundled. `SHA256SUMS.txt` contains package and source checksums. If FUSE is unavailable on Linux, set `APPIMAGE_EXTRACT_AND_RUN=1`.

SimpleAVShow is free software under the GNU GPL, version 3 or later. Every package includes `LICENSE`, third-party notices and an About dialog license notice. The release includes `SimpleAVShow-0.4.1-source.tar.gz`. Windows ASIO support uses Steinberg's ASIO SDK 2.3.4 under its GPLv3 option.

These builds are unsigned; macOS uses ad-hoc signing without notarization. This remains a development prototype. Rehearse complete shows on the actual show hardware. See [hardware validation](https://github.com/staubichsauger/simpleAVshow/blob/v0.4.1/docs/hardware-validation.md).
