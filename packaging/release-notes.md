SimpleAVShow 0.7.0 adds an embedded agent chat, streamed audio, a recent-shows menu and timeline-style layer stacking. This is a development prerelease.

## Picture layers stack like other editors

The topmost picture track is now the front layer, and tracks below it sit further back. New shows start with **Overlay** above **Background**. Shows saved by earlier versions have their picture tracks reordered when opened, so they look the same as before; save them to keep the new order. See [editing a show](https://github.com/SimpleAVL/simpleAVshow/blob/v0.7.0/docs/editing-a-show.md).

## Open recent

**File → Open recent** lists the last 10 shows and packages you opened or saved. Missing files are marked and can be removed from the list, and **Clear recent shows** empties it.

## Agent chat and MCP tools

**Project → Agent chat** (Ctrl+Shift+A) opens a chat panel that runs an ACP coding agent, such as Claude Code, Codex or Antigravity, against the open show. The MCP server gains an app guide, environment inspection and the full set of operator controls, behind the same edit locks as the editor. See [agent collaboration](https://github.com/SimpleAVL/simpleAVshow/blob/v0.7.0/docs/agent-collaboration.md).

## Audio

- **Audio sources** (experimental, in **Outputs**): **Stream** decodes compressed audio from the original files during playback instead of preparing a PCM cache; **Prepared** keeps the cache. Show mode warns while audio still streams.
- **If audio is late**: **Freeze** holds show time as before; **Gap** keeps time running and silences only the late voices.

See [audio and outputs](https://github.com/SimpleAVL/simpleAVshow/blob/v0.7.0/docs/audio-and-outputs.md).

## Editing

- Placing, moving, trimming, pasting or duplicating clips and cues past the end of the show now lengthens it instead of being refused.
- Drag in the timeline ruler to scrub the playhead; it snaps to clip edges and cues.
- The user manual was rewritten for readability and accuracy.

## Downloads and source

| Platform | Package | Launch |
| --- | --- | --- |
| Windows 11 x64 | `SimpleAVShow-windows-x86_64.zip` | Extract the whole ZIP; run `bin/simpleavshow.exe` |
| macOS 15+ Apple Silicon | `SimpleAVShow-macos-arm64.zip` | Extract; move `simpleavshow.app` to Applications |
| Linux x64 | `SimpleAVShow-linux-x86_64.AppImage` | `chmod +x`, then run; Ubuntu 24.04 / glibc 2.39 or newer |

Runtime dependencies and the [user manual](https://github.com/SimpleAVL/simpleAVshow/blob/v0.7.0/docs/README.md) are bundled. `SHA256SUMS.txt` contains package and source checksums. If FUSE is unavailable on Linux, set `APPIMAGE_EXTRACT_AND_RUN=1`.

SimpleAVShow is free software under the GNU GPL, version 3 or later. Every package includes `LICENSE`, third-party notices and an About dialog license notice. The release includes `SimpleAVShow-0.7.0-source.tar.gz`. Windows ASIO support uses Steinberg's ASIO SDK 2.3.4 under its GPLv3 option.

These builds are unsigned; macOS uses ad-hoc signing without notarization. Rehearse complete shows on the actual show hardware. See [hardware validation](https://github.com/SimpleAVL/simpleAVshow/blob/v0.7.0/docs/hardware-validation.md).
