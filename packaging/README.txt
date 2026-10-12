SimpleAVShow 0.1.0 — development prerelease

Windows: extract the entire ZIP, then run bin/simpleavshow.exe.
macOS 15 or newer, Apple Silicon: extract the ZIP, then drag simpleavshow.app
       to Applications. Intel Macs are not a supported target.
Linux: chmod +x SimpleAVShow-*.AppImage, then run it. Built on Ubuntu 24.04
       (x86_64, glibc 2.39 or newer). If FUSE is unavailable, run with
       APPIMAGE_EXTRACT_AND_RUN=1.

The packages include their runtime dependencies. No development SDK is needed.
These builds are unsigned; macOS is ad-hoc signed, without notarization.
Windows and macOS may display their usual downloaded-app security prompts.

Press Play to prepare the remembered audio output, or default speakers on first
use. Choose show equipment in Outputs; selecting a device connects it immediately.
Choose a MIDI destination once if your show sends MIDI cues; Play reconnects it.
Missing remembered devices block playback. Use the track inspector routing
presets for speakers, mono feeds, or silence. Guide and Click start silent.
Use --demo to open the demonstration, or create a fresh project in the editor.
File > Export package collects the show and all media into a portable .savpack.
Open it on another computer, then Save to keep an editable project/media folder.
Choose audio/MIDI outputs and the program display on the receiving computer.

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
libarchive: https://www.libarchive.org (BSD, additional dependency notices)
Linux libraries: distribution source packages corresponding to licenses/.
The Windows manifest, ffmpeg-windows.json, and CI workflow record versions and
build recipes. Windows uses BtbN's LGPL shared FFmpeg SDK:
https://github.com/BtbN/FFmpeg-Builds
The macOS package records Homebrew versions in licenses/homebrew.json.

The bundled shared libraries can be replaced with compatible builds. No signing
key is required: modified macOS bundles can be re-signed ad-hoc with codesign.
