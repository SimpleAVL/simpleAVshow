<img src="assets/icon.svg" width="64" height="64" alt="">

# SimpleAVShow

A native timeline editor and playback engine for stage performances. Windows 11 is the primary show target; macOS and Linux share the C++20 codebase.

This is a **development prototype**. CI builds and tests the app on macOS, Windows, and Linux, then launches the packaged app with a demo screenshot as a smoke test. Physical channel routing, MIDI equipment, and full rehearsal runs still need validation on the show hardware.

Full builds and packages run on pushes to `main` or manual workflow runs. Pull requests targeting `main` run playback-engine tests on all three platforms and the Linux media/UI tests, without packaging. Other branch pushes do not trigger CI, and newer runs cancel superseded runs on the same branch or pull request.

All app builds wait for the core tests on every platform. A core failure cancels the remaining core matrix jobs and skips the app builds. The final `CI` check fails if any required build/test job fails, is cancelled, or is unexpectedly skipped; macOS/Windows app jobs are intentionally skipped on pull requests.

### Merge requirements

The repository ruleset in [.github/main-ruleset.json](.github/main-ruleset.json) requires pull requests to `main`, an up-to-date branch, and a passing `CI` check from GitHub Actions, with no bypass actors. The JSON file must be applied to GitHub; committing it alone does not enforce merge requirements. Once this workflow has reported a successful `CI` check, an administrator can import the file under **Settings → Rules → Rulesets**, or create the ruleset from the repository root:

```sh
gh api --method POST repos/staubichsauger/simpleAVshow/rulesets --input .github/main-ruleset.json
```

If the ruleset already exists, update it rather than creating another copy. GitHub currently rejects ruleset/branch-protection requests for this private repository with HTTP 403 and an instruction to upgrade to GitHub Pro or make the repository public. Merge enforcement remains unavailable until the repository has a [plan that supports rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/about-rulesets).

## Downloads

Download the packages from [GitHub Releases](https://github.com/staubichsauger/simpleAVshow/releases):

- macOS 15 or newer, Apple Silicon (`arm64`): extract the ZIP and move `simpleavshow.app` into Applications. Intel Macs are outside the supported targets.
- Windows 11 x64: extract the entire ZIP and run `bin/simpleavshow.exe`.
- Linux x64: AppImage built on Ubuntu 24.04 (glibc ≥ 2.39). Make it executable with `chmod +x SimpleAVShow-linux-x86_64.AppImage`, then run it. Set `APPIMAGE_EXTRACT_AND_RUN=1` if FUSE is unavailable.

Packages include runtime libraries and third-party notices; no SDK or Homebrew installation is required. These first prereleases are unsigned (macOS uses ad-hoc signing without notarization), so downloaded-app security prompts may appear. Read the included `README.txt` and [hardware validation](docs/hardware-validation.md) before using a build for a performance.

## Features

- Audio, video, images, and text on a scrollable timeline. Drag to place/move, drag edges to trim, and snap to frames, clips, markers, and the playhead.
- Picture layers composed into one program picture; project resolution/frame rate default to 1920 × 1080 / 24 fps.
- Holds freeze show time and normal media; Space continues. Manual pause holds the picture and leaves async audio playing.
- Async audio can loop or finish naturally. Its source keeps advancing through holds while its automation freezes with show time.
- Seeking inside an active async clip preserves its source cursor; seeking outside its `[start, end)` interval stops it. Inactive clips start at the sought source position on playback.
- Automatic audio preparation on Play, remembered devices, configurable channel count, and per-track routing presets, gain, pan, and mute. Main, ambience, guide, and click tracks are created by default; guide/click outputs start disconnected.
- Clip automation for volume, pan, picture X/Y, scale, and opacity; linear keyframes and fade helpers.
- Native MIDI output ports, including MIDI bridges, with automatic reconnection on Play. A guided cue editor offers note names, named controls, programs, pitch bend, message previews, and test sending. Raw channel/system messages and SysEx remain available; each cue can fire on arrival or resume.
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

The demonstration contains generated ambience, click, text, a backdrop, three holds, and a fade after the first hold. Saving exports generated WAV/PNG assets into a sibling `.media` directory. Press Play to prepare your remembered audio output, or your default speakers on first use. Missing remembered devices block playback with an explanation; they do not fall back to other speakers.

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

The vcpkg manifest pins dependencies and enables ASIO. The initial Qt/FFmpeg build can take a while. In Outputs, enable Show driver and latency settings to choose the interface's ASIO driver, choose the interface, and set the needed number of output channels.

Stage the development app with dependency DLLs and Qt plugins:

```powershell
cmake --install build --config Release --prefix dist
dist\bin\simpleavshow.exe
```

CI uses prebuilt Qt 6.8.3 and a checksum-pinned FFmpeg 9.0 shared SDK from [BtbN](https://github.com/BtbN/FFmpeg-Builds). Only the app, RtAudio (with ASIO), and RtMidi are compiled; FFmpeg is not rebuilt. The pin is in [packaging/ffmpeg-windows.json](packaging/ffmpeg-windows.json). To use that SDK locally, run `python packaging/fetch_ffmpeg_windows.py ffmpeg-sdk`, then configure with `-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON`, `-DCMAKE_PREFIX_PATH=<Qt SDK>`, and `-DSAV_FFMPEG_ROOT=<extracted FFmpeg SDK>`. Copy its `bin/*.dll` into `build/Release/` before running tests, and add its `bin` directory to PATH for the fixture generator.

The default manifest builds Qt and FFmpeg from source for local development. Release packaging includes dependency DLLs, Qt plugins, and third-party notices. Developer ID/code signing and notarization are not configured.

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
2. Press Play to prepare audio automatically. On first use it chooses the default output; afterward it restores the remembered device by name. For a show interface, choose it in Outputs. Selecting a device connects it immediately; Use audio settings applies channel/buffer changes. Refresh devices after plugging in equipment. A missing remembered device blocks playback until it returns or you choose a replacement.
3. Use Quick routing in the track inspector, then Apply track: stereo speakers, a mono output, or silent. Guide and Click start silent. For four channels: Main stereo 1/2, Guide mono 3, Click mono 4. Mono presets set pan for unity gain on the connected leg. Custom left/right routing remains available.
4. Import and drag media onto compatible tracks. Video with embedded audio creates an audio companion on the first synchronous audio track. These are independent clips: trim/move both when changing their timing.
5. Add hold/MIDI cues at the playhead and edit them in the inspector. Choose the MIDI destination once in Outputs; selecting it connects immediately, and Play restores it next time. Choose an action such as Play a note, Change a control, or Select a program; only relevant fields appear. Programs display 1–128 (sent as MIDI 0–127); pitch bend displays −8192 to +8191 with 0 centered. Raw MIDI / SysEx accepts hex bytes and reports incomplete messages inline. Send test triggers the displayed message immediately without applying the cue or advancing show time. Note tests may need a matching Stop a note message; Stop All does not stop external equipment.
6. Select a clip to edit transformations and automation. Keyframe times are relative to clip start and follow show time. Apply changes with the relevant Apply button.
7. Choose a physical program display, save, and enable Show mode. Space plays/pauses/resumes; Escape or Stop All silences internal audio. The project end also stops voices.

Use Stop All and leave Show mode before editing, saving, testing MIDI, or manually changing audio/MIDI connections. Play may prepare remembered devices in Show mode while playback is stopped. Outputs can be opened during playback or a marker hold in edit mode; locked connection controls are visibly disabled with an explanation and a Stop All button to unlock them. The program display can be opened without stopping playback. Outputs buttons and the menu item are disabled in Show mode or during media preparation, with explanatory tooltips. Text fields keep normal Space behavior. Seeking never sends MIDI; landing exactly on a marker arms it for Play. Non-hold cues fire on crossing, including those configured for resume, because there is no hold to resume.

Autosave writes `<project>.savshow.autosave` every minute after changes to a saved show. Open that sidecar and Save As a `.savshow` to recover. Imported media remains external; move the project and media together.

## Capacity and current constraints

- Imported audio prepares to a temporary 48 kHz stereo float PCM cache on disk, then streams through worker-fed read-ahead buffers. There is no project audio byte budget. Preparation needs about 1.29 GiB of temporary disk space per hour of source audio; caches are shared by undo snapshots and removed when their last owner releases them. Opening a project rebuilds its caches. Mono/stereo stems are supported; multichannel source files still need separate stems.
- Tracks, picture layers, clips, assets, markers, and automation curves grow with the project; there are no fixed prototype count caps or 24-hour duration cap. Output channel choices follow the interface. Frame timing is bounded only by exact integer representation in the JSON format. Real capacity depends on available memory, disk throughput, and simultaneous playback/composition work.
- Playback keeps four 8192-frame stereo buffers (256 KiB) per armed disk-backed audio clip, plus small waveform summaries (about 1.4 MiB per source hour). The callback performs no disk reads, decoding, allocation, or locking. On a cache miss, all audio and show time wait together without skipping source frames; the audio wait counter records this. An unreadable cache stops playback and requires reopening the project.
- Picture clips cannot overlap on one track; use separate layers and opacity automation for crossfades. Loops wrap to the clip's Source in point.
- Video decoding/composition currently use software FFmpeg/QPainter. Multi-layer 1080p24 performance is unverified on the show PC; hardware decoding/GPU composition remain future work. Delayed video frames retain the previous composition and increment a visible wait counter.
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

They cover hold boundaries, async looping/finish, pause/resume, seeks, MIDI timing, routing isolation, pan, clipping, Stop All, and project end, plus large timeline scheduling, disk-buffer handoffs, cache failures, and callback allocation/I/O checks. The Qt suite adds decoding/resampling, an audio source expanding beyond 1 GiB with bounded resident PCM, cache lifetime/cleanup, embedded A/V offsets, video seeks, large project round-trips, save/reload/moved media, malformed projects, undo/redo, Show-mode UI checks, device selection without fallback, MIDI editor byte round trips, raw-message validation, and cue editing. FFmpeg CLI generates fixtures; media tests explicitly skip when it is unavailable. Clang/GCC builds support `-DSAV_SANITIZE=ON` for address/undefined-behavior checking.

Visual QA:

```sh
QT_QPA_PLATFORM=offscreen build/simpleavshow.app/Contents/MacOS/simpleavshow --demo --preview-time 6 --screenshot artifacts/editor.png
```

See [architecture](docs/architecture.md) and [hardware validation](docs/hardware-validation.md).
