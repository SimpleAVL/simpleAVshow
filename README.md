<img src="assets/icon.svg" width="64" height="64" alt="">

# SimpleAVShow

A native timeline editor and playback engine for stage performances. Windows 11 is the primary show target; macOS and Linux share the C++20 codebase.

This is a **development prototype**. CI builds and tests the app on macOS, Windows, and Linux, then launches the packaged app with a demo screenshot as a smoke test. Physical channel routing, MIDI equipment, and full rehearsal runs still need validation on the show hardware.

## Downloads

Download the packages from [GitHub Releases](https://github.com/staubichsauger/simpleAVshow/releases):

- macOS 15 or newer, Apple Silicon (`arm64`): extract the ZIP and move `simpleavshow.app` into Applications. Intel Macs are outside the supported targets.
- Windows 11 x64: extract the entire ZIP and run `bin/simpleavshow.exe`.
- Linux x64: AppImage built on Ubuntu 24.04 (glibc ≥ 2.39). Make it executable with `chmod +x SimpleAVShow-linux-x86_64.AppImage`, then run it. Set `APPIMAGE_EXTRACT_AND_RUN=1` if FUSE is unavailable.

Packages include runtime libraries and third-party notices; no SDK or Homebrew installation is required. These first prereleases are unsigned (macOS uses ad-hoc signing without notarization), so downloaded-app security prompts may appear. Read the included `README.txt` and [hardware validation](docs/hardware-validation.md) before using a build for a performance.

## Features

- Audio, video, images, and text on a scrollable timeline. Drag to place/move, drag edges to trim, and snap to frames, clips, markers, and the playhead.
- Four picture layers, composed into one program picture; project resolution/frame rate default to 1920 × 1080 / 24 fps.
- Holds freeze show time and normal media; Space continues. Manual pause holds the picture and leaves async audio playing.
- Async audio can loop or finish naturally. Its source keeps advancing through holds while its automation freezes with show time.
- Seeking inside an active async clip preserves its source cursor; seeking outside its `[start, end)` interval stops it. Inactive clips start at the sought source position on playback.
- One audio interface, configurable channel count, and per-track routing, gain, pan, and mute. Main, ambience, guide, and click tracks are created by default; guide/click outputs start disconnected.
- Clip automation for volume, pan, picture X/Y, scale, and opacity; linear keyframes and fade helpers.
- Native MIDI output ports, including MIDI bridges. Channel/system messages and SysEx; each cue can fire on arrival or resume.
- Physical fullscreen program output, editing/seek lock in Show mode, atomic save, relative media paths, autosave sidecars, and 32-step undo/redo.

NDI, multiple audio devices, MIDI input, Wings import, plugins, and advanced effects are outside v1.

## macOS

Install Xcode Command Line Tools and Homebrew, then:

```sh
brew install cmake ninja qtbase ffmpeg rtaudio rtmidi pkgconf
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build
ctest --test-dir build --output-on-failure
open build/simpleavshow.app --args --demo
```

The demonstration contains generated ambience, click, text, a backdrop, three holds, and a fade after the first hold. Saving exports generated WAV/PNG assets into a sibling `.media` directory. Nothing plays until you connect an audio device and press Play.

## Windows 11

Install Visual Studio 2022 with Desktop development with C++, CMake, and Git. In a Visual Studio developer PowerShell:

```powershell
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release --parallel 4
$env:PATH = "$pwd/build/vcpkg_installed/x64-windows/tools/ffmpeg;$env:PATH"
ctest --test-dir build -C Release --output-on-failure
build\Release\simpleavshow.exe --demo
```

The vcpkg manifest pins dependencies and enables ASIO. The initial Qt/FFmpeg build can take a while. Choose the interface's ASIO driver in Outputs, choose the interface, and open the needed number of output channels.

Stage the development app with dependency DLLs and Qt plugins:

```powershell
cmake --install build --config Release --prefix dist
dist\bin\simpleavshow.exe
```

CI uses the prebuilt Qt 6.8.3 SDK with `-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON`; the default manifest builds Qt from source for local development. Release packaging includes dependency DLLs, Qt plugins, and third-party notices. Developer ID/code signing and notarization are not configured.

## Linux

Requires Qt ≥ 6.4, FFmpeg ≥ 5.1 development libraries, RtAudio ≥ 6, RtMidi, CMake, and a C++20 compiler. Ubuntu 24.04 packages an older RtAudio; install version 6 from source as shown in [.github/workflows/build.yml](.github/workflows/build.yml), or use the vcpkg manifest.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
build/simpleavshow --demo
```

## Operating a show

1. Set title, resolution, frame rate, and duration in Project settings.
2. Open Outputs and connect the audio interface. Last selections are remembered by name; devices are not automatically connected on startup.
3. Route tracks in the inspector. For four channels: Main 1/2, Guide 3/Off, Click 4/Off. Mono pan is constant-power; use Pan −1 on a left-only mono route for unity gain.
4. Import and drag media onto compatible tracks. Video with embedded audio creates an audio companion on the first synchronous audio track. These are independent clips: trim/move both when changing their timing.
5. Add hold/MIDI cues at the playhead and edit them in the inspector. Connect MIDI output before playing a project with MIDI messages.
6. Select a clip to edit transformations and automation. Keyframe times are relative to clip start and follow show time. Apply changes with the relevant Apply button.
7. Choose a physical program display, save, and enable Show mode. Space plays/pauses/resumes; Escape or Stop All silences internal audio. The project end also stops voices.

Use Stop All and leave Show mode before editing, saving, or changing audio/MIDI connections. Outputs can be opened during playback or a marker hold in edit mode; locked connection controls are visibly disabled with an explanation and a Stop All button to unlock them. The program display can be opened without stopping playback. Outputs buttons and the menu item are disabled in Show mode or during media preparation, with explanatory tooltips. Text fields keep normal Space behavior. Seeking never sends MIDI; landing exactly on a marker arms it for Play. Non-hold cues fire on crossing, including those configured for resume, because there is no hold to resume.

Autosave writes `<project>.savshow.autosave` every minute after changes to a saved show. Open that sidecar and Save As a `.savshow` to recover. Imported media remains external; move the project and media together.

## Prototype limits

- Audio predecodes to 48 kHz stereo float PCM, with a 1 GiB project audio budget. Import mono/stereo stems separately; multichannel source files are rejected. Long shows need streamed/prebuffered audio in a later iteration.
- Limits: 64 tracks/output channels, 256 clips/assets, 512 markers, 256 keyframes per curve, and 24 hours of show time.
- Picture clips cannot overlap on one track; use separate layers and opacity automation for crossfades. Loops wrap to the clip's Source in point.
- Video decoding/composition currently use software FFmpeg/QPainter. Four-layer 1080p24 performance is unverified on the show PC; hardware decoding/GPU composition remain future work. Delayed video frames retain the previous composition and increment a visible wait counter.
- Audio legs share one callback and align sample-for-sample. Video follows that clock, but display refresh and audio/monitor buffering need measurement and compensation.
- MIDI runs on a separate worker without sample-timestamped hardware scheduling. Bridge latency needs measurement. Stop All does not command external MIDI receivers.
- Source cursors persist across seeks, not across edits/device changes. Editing/saving are disabled while playing/held or while async voices remain active.

## Tests

Transport tests run without native dependencies:

```sh
cmake -S . -B build-core -DSAV_BUILD_APP=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

They cover hold boundaries, async looping/finish, pause/resume, seeks, MIDI timing, routing isolation, pan, clipping, Stop All, and project end. The Qt suite adds decoding/resampling, embedded A/V offsets, video seeks, save/reload/moved media, malformed projects, undo/redo, and Show-mode UI checks. FFmpeg CLI generates fixtures; media tests explicitly skip when it is unavailable. Clang/GCC builds support `-DSAV_SANITIZE=ON` for address/undefined-behavior checking.

Visual QA:

```sh
QT_QPA_PLATFORM=offscreen build/simpleavshow.app/Contents/MacOS/simpleavshow --demo --preview-time 6 --screenshot artifacts/editor.png
```

See [architecture](docs/architecture.md) and [hardware validation](docs/hardware-validation.md).
