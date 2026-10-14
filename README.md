<img src="assets/icon.svg" width="64" height="64" alt="">

# SimpleAVShow

A native timeline editor and playback engine for stage performances. Build a show from audio, video, images, text, holds, and MIDI cues, then run it from a locked Show mode with a fullscreen audience display.

Windows 11 is the primary show target; macOS and Linux run the same C++20 codebase.

![SimpleAVShow editor with the demonstration show](docs/screenshots/01-editor-overview.png)

> **Status: development prerelease.** Automated transport, media, persistence, and UI tests pass on all three platforms, but physical channel routing, MIDI equipment, and complete shows still need validation on your own hardware. Rehearse with the [show-machine checklist](docs/hardware-validation.md) before using a build for a performance.

## Downloads

Download the latest package from [GitHub Releases](https://github.com/staubichsauger/simpleAVshow/releases):

| Platform | Package | Install |
| --- | --- | --- |
| Windows 11 x64 | `SimpleAVShow-windows-x86_64.zip` | Extract the entire ZIP and run `bin/simpleavshow.exe`. |
| macOS 15+, Apple Silicon | `SimpleAVShow-macos-arm64.zip` | Extract the ZIP and move `simpleavshow.app` into Applications. Intel Macs are not supported. |
| Linux x64 | `SimpleAVShow-linux-x86_64.AppImage` | `chmod +x` the AppImage, then run it. Built on Ubuntu 24.04 (glibc ≥ 2.39). Set `APPIMAGE_EXTRACT_AND_RUN=1` if FUSE is unavailable. |

Packages include their runtime libraries and third-party notices. Prereleases are unsigned (macOS uses ad-hoc signing without notarization), so the usual downloaded-app security prompts may appear. Each release lists package checksums in `SHA256SUMS.txt`.

Start with `--demo` (or **File → Open demonstration**) to explore a sample show.

## Documentation

The [illustrated user manual](docs/README.md) covers installation, timeline editing, audio routing, MIDI cues, Show mode, portable packages, recovery, and troubleshooting. It also ships inside every package.

## Features

- Audio, video, images, and text on a scrollable timeline with snapping, trimming, copy/paste, split, nudge, and 32-step undo/redo.
- Picture layers composed into one program picture, shown on a fullscreen output that remembers its display.
- Holds that freeze show time until the operator continues; async audio and video (ambience, loops, projections) keep playing through holds and pauses.
- Per-track routing to physical output channels, with presets for stereo speakers, mono feeds (guide, click), or silence. ASIO on Windows.
- Clip automation for volume, pan, position, scale, and opacity, with fade helpers.
- MIDI cues sent on arrival or on resume, with a guided editor for notes, controls, programs, pitch bend, and raw/SysEx messages.
- Show mode with a preflight check, editing/seek lock, next-cue countdown, and confirmations before leaving or quitting.
- Atomic saves, autosave recovery, relinking of missing media, and portable `.savpack` packages that open on any supported platform.
- Opt-in [MCP agent collaboration](docs/agent-collaboration.md): build the same live show with an agent, with shared undo, conflict checks, and operator-controlled editing.

NDI, multiple audio devices, MIDI input, plugins, and advanced effects are not part of v1. See [current limitations](docs/reference-and-troubleshooting.md#current-limitations).

## Building from source and contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions on macOS, Windows, and Linux, the test suites, and how to submit changes. Developer documentation (architecture, CI and releases) lives in [development/](development/).

## License

SimpleAVShow is free software: you can redistribute it and/or modify it under the terms of the [GNU General Public License](LICENSE) as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. It is distributed in the hope that it will be useful, but without any warranty.

Each release attaches `SimpleAVShow-<version>-source.tar.gz` (a `git archive` of the release tag) next to the packages, and every package includes `LICENSE` and third-party notices in `licenses/`. Third-party components keep their own licenses:

- **FFmpeg:** the macOS (Homebrew) and Linux (Ubuntu) packages bundle GPL-configured builds; Windows bundles BtbN's LGPL build pinned in [packaging/ffmpeg-windows.json](packaging/ffmpeg-windows.json).
- **ASIO (Windows):** RtAudio is built through the overlay port in [ports/rtaudio](ports/rtaudio) against Steinberg's ASIO SDK 2.3.4 under its GPLv3 option, instead of the 2013 SDK copies RtAudio bundles, which were licensed only under Steinberg's proprietary agreement. "ASIO" is a trademark of Steinberg Media Technologies GmbH.
- **Qt** (LGPLv3), **RtAudio** and **RtMidi** (MIT), and **libarchive** (BSD) are bundled as replaceable shared libraries.
