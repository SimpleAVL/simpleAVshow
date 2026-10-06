SimpleAVShow 0.1.0 — development prerelease

Windows: extract the entire ZIP, then run bin/simpleavshow.exe.
macOS 15 or newer: extract the ZIP, then drag simpleavshow.app to Applications.
Linux: chmod +x SimpleAVShow-*.AppImage, then run it. Built on Ubuntu 24.04
       (x86_64, glibc 2.39 or newer). If FUSE is unavailable, run with
       APPIMAGE_EXTRACT_AND_RUN=1.

The packages include their runtime dependencies. No development SDK is needed.
These builds are unsigned; macOS is ad-hoc signed, without notarization.
Windows and macOS may display their usual downloaded-app security prompts.

Open Outputs and connect your audio interface before pressing Play. Configure
track routing and your MIDI port there. Device connections are never automatic.
Use --demo to open the demonstration, or create a fresh project in the editor.

This is a development prototype. Automated transport, media, persistence, and
UI tests pass, but ASIO routing, MIDI equipment, and full shows still require
rehearsal testing on the actual hardware. See docs/hardware-validation.md.

Source and build instructions:
https://github.com/staubichsauger/simpleAVshow

Third-party library notices and license texts are included in licenses/.
Qt: https://code.qt.io/cgit/qt/qtbase.git/ (LGPLv3 / GPL options)
FFmpeg: https://ffmpeg.org/download.html (LGPL/GPL, according to build options)
RtAudio: https://github.com/thestk/rtaudio (MIT, additional backend notices)
RtMidi: https://github.com/thestk/rtmidi (MIT)
Linux libraries: distribution source packages corresponding to licenses/.
The Windows manifest and CI workflow record dependency versions/build recipes.
The macOS package records Homebrew versions in licenses/homebrew.json.

The bundled shared libraries can be replaced with compatible builds. No signing
key is required: modified macOS bundles can be re-signed ad-hoc with codesign.
