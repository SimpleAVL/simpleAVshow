SimpleAVShow 0.6.0 adds a phone remote: run playback from a phone or tablet's web browser on the same network. This is a development prerelease.

## Phone remote

Open **Project → Phone remote…**, or click **Remote · off** in the top bar, and enable it. The dialog lists a link to open in the phone's browser; nothing needs to be installed on the phone. The page shows the clock, state, next cue, time left, the current hold and any playback problem, with one large **Play / Pause / Continue** button and a **Stop All** button that needs two taps within three seconds.

Each tap states what it means, so a late or repeated tap does nothing instead of toggling the show back. The remote works in Show mode, uses the same playback checks as the editor, and cannot seek, edit, save or change outputs. If the phone loses its connection, the page dims and disables its buttons until it reconnects. The top-bar button shows how many phones are connected, and the status bar names each remote action.

Anyone with the link on the same network can control playback, and the remote uses plain HTTP, so use a trusted show network. The link and port stay the same between launches so bookmarks keep working; **Change link…** revokes the old link immediately. The remote is off after each launch until enabled again. Windows and macOS may ask to allow incoming connections the first time. See the [phone remote chapter](https://github.com/SimpleAVL/simpleAVshow/blob/v0.6.0/docs/remote-control.md).

## Downloads and source

| Platform | Package | Launch |
| --- | --- | --- |
| Windows 11 x64 | `SimpleAVShow-windows-x86_64.zip` | Extract the whole ZIP; run `bin/simpleavshow.exe` |
| macOS 15+ Apple Silicon | `SimpleAVShow-macos-arm64.zip` | Extract; move `simpleavshow.app` to Applications |
| Linux x64 | `SimpleAVShow-linux-x86_64.AppImage` | `chmod +x`, then run; Ubuntu 24.04 / glibc 2.39 or newer |

Runtime dependencies and the [user manual](https://github.com/SimpleAVL/simpleAVshow/blob/v0.6.0/docs/README.md) are bundled. `SHA256SUMS.txt` contains package and source checksums. If FUSE is unavailable on Linux, set `APPIMAGE_EXTRACT_AND_RUN=1`.

SimpleAVShow is free software under the GNU GPL, version 3 or later. Every package includes `LICENSE`, third-party notices and an About dialog license notice. The release includes `SimpleAVShow-0.6.0-source.tar.gz`. Windows ASIO support uses Steinberg's ASIO SDK 2.3.4 under its GPLv3 option.

These builds are unsigned; macOS uses ad-hoc signing without notarization. Rehearse complete shows on the actual show hardware. See [hardware validation](https://github.com/SimpleAVL/simpleAVshow/blob/v0.6.0/docs/hardware-validation.md).
