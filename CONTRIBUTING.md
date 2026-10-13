# Contributing to SimpleAVShow

Thanks for your interest in improving SimpleAVShow. This guide covers building from source, running the tests, and submitting changes.

Bug reports and feature requests go to [GitHub Issues](https://github.com/staubichsauger/simpleAVshow/issues). For bugs, include the app version (`simpleavshow --version` or **Help → About**), the platform, the audio/MIDI equipment involved, and steps to reproduce.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/core` | Qt-independent transport, mixing, and real-time queues. Runs in the audio callback. |
| `src/media` | FFmpeg decoding, audio caches, RtAudio/RtMidi devices. |
| `src/app` | Qt Widgets editor, project model, persistence, packages. |
| `tests` | `transport_tests` (no native dependencies) plus `app_tests` and `mcp_tests` (Qt Test). |
| `docs` | The user manual. CI copies this whole folder into every package, so keep it user-facing. |
| `development` | Developer documentation: [architecture](development/architecture.md), [CI and releases](development/ci-and-releases.md), [screenshot maintenance](development/screenshots.md). |
| `packaging` | Packaging scripts, the release README and notes, third-party license texts. |
| `ports/rtaudio` | vcpkg overlay port building RtAudio against the GPLv3 ASIO SDK. |

## Building

The app needs Qt ≥ 6.4 (Widgets, Concurrent, Network, Test), FFmpeg ≥ 5.1, RtAudio ≥ 6, RtMidi, libarchive ≥ 3.6, CMake ≥ 3.24, and a C++20 compiler. Launch any build with `--demo` to open the demonstration show.

### macOS

Install Xcode Command Line Tools and Homebrew, then:

```sh
brew install cmake ninja qtbase ffmpeg rtaudio rtmidi libarchive pkgconf
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build
ctest --test-dir build --output-on-failure
open build/simpleavshow.app --args --demo
```

### Windows 11

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

The vcpkg manifest pins dependencies and enables ASIO. By default it builds Qt and FFmpeg from source, so the first build takes a while. To stage the app with its dependency DLLs and Qt plugins:

```powershell
cmake --install build --config Release --prefix dist
dist\bin\simpleavshow.exe
```

CI avoids the long source build by using prebuilt Qt 6.8.3 and a checksum-pinned FFmpeg shared SDK from [BtbN](https://github.com/BtbN/FFmpeg-Builds) (pinned in [packaging/ffmpeg-windows.json](packaging/ffmpeg-windows.json)). To do the same locally, run `python packaging/fetch_ffmpeg_windows.py ffmpeg-sdk`, then configure with `-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON`, `-DCMAKE_PREFIX_PATH=<Qt SDK>`, and `-DSAV_FFMPEG_ROOT=<extracted FFmpeg SDK>`. Copy the SDK's `bin/*.dll` into `build/Release/` before running tests, and add its `bin` directory to PATH for the fixture generator. See the `windows` job in [.github/workflows/build.yml](.github/workflows/build.yml) for the exact steps.

### Linux

Install the dependencies listed above. Ubuntu 24.04 packages an older RtAudio; install version 6 from source as the `linux` job in [.github/workflows/build.yml](.github/workflows/build.yml) does, or use the vcpkg manifest.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
build/simpleavshow --demo
```

## Tests

The transport tests build without Qt, FFmpeg, or device libraries:

```sh
cmake -S . -B build-core -DSAV_BUILD_APP=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

They cover hold boundaries, async looping/finish, pause/resume, seeks, MIDI timing, routing isolation, pan, clipping, Stop All, project end, large timeline scheduling, disk-buffer handoffs, cache failures, and callback allocation/I/O checks.

The Qt suite (`app_tests`, built with the app) adds media decoding and caching, direct WAV streaming, video seeks, project and package round-trips, malformed input, undo/redo, Show-mode UI, device selection, and the MIDI cue editor. The FFmpeg CLI generates its fixtures; media tests skip when it is not on PATH.

On a Windows desktop with two extended displays, run the opt-in audience-display check using the native Qt platform plugin:

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:SAV_TEST_DISPLAYS = '1'
build\Release\app_tests.exe programOutputUsesSecondaryScreen
```

It opens and reopens fullscreen output on the second display and checks that the editor stays on its original display. Run it in the signed-in desktop session; an SSH session alone does not provide that desktop.

The MCP suite (`mcp_tests`) exercises the local HTTP endpoint, authentication, protocol discovery, shared edits, revision conflicts, atomic rollback/undo, linked trimming, asynchronous media preparation, and the operator controls. It runs offscreen alongside the app tests and needs no external agent or service.

Clang/GCC builds support `-DSAV_SANITIZE=ON` (AddressSanitizer + UBSan) or `-DSAV_TSAN=ON` (ThreadSanitizer); the two cannot be combined. CI runs the transport tests under ThreadSanitizer.

For a quick visual check, render the demo headlessly:

```sh
QT_QPA_PLATFORM=offscreen build/simpleavshow.app/Contents/MacOS/simpleavshow --demo --preview-time 6 --screenshot editor.png
```

On Linux use `build/simpleavshow`; on Windows `build\Release\simpleavshow.exe`.

## Design rules

Changes must keep these properties; reviewers will check them.

- **The audio callback stays real-time safe.** `Transport::render` performs no allocation, locking, disk I/O, Qt calls, or MIDI sending. Hand work to it through the SPSC queues and prepared snapshots described in the [architecture](development/architecture.md).
- **A fault must not stop a running show.** Recoverable media, MIDI, or disk errors are reported and retried; they never silence unrelated audio or cues.
- **Space plays and pauses everywhere, including Show mode.** Pausing a show with Space is essential and must not change. Text fields keep normal Space behavior.
- **Missing devices never fall back.** A missing remembered audio or MIDI destination blocks playback; the app does not substitute another output.
- **Project files stay compatible.** `.savshow` is versioned JSON; new optional fields need defaults that keep older files loading.

## Submitting changes

1. Fork the repository and create a branch from `main`.
2. Keep changes focused, and add or update tests for behavior changes. Real-time and threading changes should also pass the ThreadSanitizer build.
3. Update the [user manual](docs/README.md) when user-visible behavior changes, and refresh affected screenshots (see [screenshot maintenance](development/screenshots.md)).
4. Open a pull request against `main`. CI must pass before merging; see [CI and releases](development/ci-and-releases.md) for what runs.

By contributing, you agree that your contributions are licensed under the [GNU GPL, version 3 or later](LICENSE).
