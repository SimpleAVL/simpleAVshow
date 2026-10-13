SimpleAVShow 0.3.0 adds MCP agent collaboration: an agent can build the show currently open in the editor alongside an operator. This is a development prerelease.

## MCP agent collaboration

- Enable the local MCP server under **Project → Agent collaboration…**, or use the Agent button in the editor. Copy the connection configuration into a Streamable HTTP MCP client, then allow editing.
- Agents can read the shared show and media library; create or edit project settings, tracks, clips, automation, holds and MIDI cues; and prepare local media imports in the background.
- Changes appear immediately and use the operator's existing Undo history. Each batch validates and commits atomically.
- Revision checks reject stale agent edits, including conflicts with pending inspector typing, so the agent must reread and adapt to the operator's current show.
- The operator can switch the agent to read-only access or disconnect it. Playback, async voices, busy operations and Show mode retain their editing locks.
- Playback, audio/MIDI equipment, audience displays, saving and package export remain under operator control.

The server is opt-in, bound to localhost and protected by a session bearer token. It supports MCP versions 2025-11-25, 2025-06-18 and 2025-03-26. The connection requires a client running on the show computer; a hosted agent cannot reach its loopback address. See the [connection guide](https://github.com/staubichsauger/simpleAVshow/blob/v0.3.0/docs/agent-collaboration.md).

## Documentation and packaging

- The illustrated [user manual](https://github.com/staubichsauger/simpleAVshow/blob/v0.3.0/docs/README.md) now covers editing, routing, MIDI, Show mode, saving/recovery and agent collaboration. It ships inside each package.
- macOS bundle versions are generated from the project version and verified before packaging.
- The repository includes contributor documentation, build instructions and the release workflow.

## License and source

SimpleAVShow is free software under the GNU GPL, version 3 or later. Every package includes `LICENSE`, third-party notices and an About dialog license notice. This release includes `SimpleAVShow-0.3.0-source.tar.gz`. Windows ASIO support uses Steinberg's ASIO SDK 2.3.4 under its GPLv3 option.

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

This remains a development prototype. Rehearse ASIO routing, audio/MIDI equipment, audience output and complete shows on the actual show hardware. Video decoding and composition use software FFmpeg/QPainter; import mono/stereo stems separately. NDI is outside this version. See [hardware validation](https://github.com/staubichsauger/simpleAVshow/blob/v0.3.0/docs/hardware-validation.md).
