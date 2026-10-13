# Review findings (October 2026)

A review of the UI, operator workflow, playback engine, persistence, packaging, and CI. Each finding has an ID, a severity, and a status. Paths and line numbers refer to the code at commit `9d01abb`.

The real-time core is sound. The audio callback does no allocation, locking, I/O, or Qt work. The SPSC queues and the `AudioBuffer` slot handoff use correct acquire/release ordering. Hold, seek, and retry semantics do not double-fire markers. Most risk lies in failure handling: one fault can stop or stall a running show.

**Design constraint:** Space toggles play/pause everywhere, including Show mode. Pausing a show with Space is essential behaviour and must not change.

Status: `open`, `fixed`, `wontfix` (with reason), `deferred`.

## Live-show blockers

| ID | Sev | Status | Finding |
|----|-----|--------|---------|
| E1 | High | fixed | **A video decode error stops all audio, repeatedly.** `VideoReader::run` exits permanently on any FFmpeg error, including a single invalid packet (`src/media/Media.cpp:240-277`). `Controller::compose` then calls `stopAll()`, killing async audio as well (`src/app/Controller.cpp:277-282`). The dead reader stays cached while the clip is on screen, so every Play stops again. **Fix:** skip recoverable packet/decode errors; keep the last frame on fatal errors; never stop audio from the picture path; surface a persistent warning. |
| E2 | High | fixed | **Disk buffers are not repositioned after a seek.** Only `AudioBuffer::sample` updates `requested_` (`src/core/AudioBuffer.cpp:9`). After a clip plays, a seek back before it leaves the read-ahead slots at the old position. When playback reaches the clip again, the first sample misses and the rest of that device block is silent on every track; show time slips. This hits rehearse → seek to 0 → Show mode. **Fix:** retarget each inactive buffer to its entry block on seek/stop/locate; add a regression test. |
| E3 | High | fixed | **A single disk read error permanently stops playback.** Any exception in `AudioBuffer::service` latches `failed_` (`AudioBuffer.cpp:58-61`). Each 8192-frame block reopens the file and re-checks size/mtime (`src/media/AudioSources.cpp`). Transient AV/backup locks or USB hiccups end the show. **Fix:** retry transient failures with backoff inside the read-ahead window. Latch only after repeated failures or a content mismatch. |
| U1 | High | fixed | **Quitting during a show is unguarded.** `closeEvent` checks only busy/dirty (`src/app/MainWindow.cpp:728`). Cmd+Q, Alt+F4, or the close button stops a saved show mid-performance and closes the program output. **Fix:** in Show mode or while the transport is not stopped, require explicit confirmation. |
| U2 | High | fixed | **One missing media file prevents opening the project.** `readProject` throws on the first missing asset (`src/app/Project.cpp:207`) and names only that file. **Fix:** load missing assets as offline placeholders, list all of them, and offer relinking. |
| U3 | High | fixed | **A missing video blocks Continue.** `Controller::toggle` checks that every video asset in the library exists on every Play/Continue (`Controller.cpp:116-118`), including unused assets. **Fix:** check only when starting from Stopped/Paused and only for videos used on the timeline, warning instead of blocking when resuming a hold. |
| E4 | Med-High | fixed | **A lost MIDI port is never detected.** `MidiOutput::connected()` is `isPortOpen()` (`src/media/MidiOutput.h:13`), which stays true after a USB device disappears, so the documented reconnect-on-Play never runs. **Fix:** mark the port lost on send failure and reconnect by name from Play/Continue; verify per backend. |
| L1 | High | deferred (owner decision, 2026-10-13: leave for now) | **macOS (and likely Linux) packages ship GPL FFmpeg.** Homebrew's ffmpeg is built with `--enable-gpl --enable-version3` (verified locally) and bundled by `build.yml:34`. Ubuntu's libavcodec is very likely GPL too (unverified). Windows uses an LGPL build. The repository has no license for the app itself. **Decision needed:** use LGPL FFmpeg builds plus a CI license check, or distribute under GPLv3. |

## Engine and backend

| ID | Sev | Status | Finding |
|----|-----|--------|---------|
| E5 | Medium | fixed (stream pauses instead of reopening; unchanged disk clips keep their read-ahead) | **Every edit re-arms everything on the GUI thread.** `Controller::arm` closes and reopens the audio device, drops all video readers, and synchronously prebuffers every disk-backed clip (`Controller.cpp:71-80`, `Transport.cpp:34-39`). Edits stall, and ASIO reopen is slow and can click. **Fix:** keep the device open across re-arms. Pause the callback via an atomic swap or stop/start the stream only; prebuffer only changed clips. |
| E6 | Medium | fixed | **Undo after Save restores stale media paths.** `Controller::save` rewrites paths in the current project only. Undo brings back temporary package paths (deleted on exit) or pathless generated demo assets. Autosave then writes useless paths or fails every minute, and the next save copies all media again. **Fix:** rewrite asset paths in undo/redo snapshots after a successful save. |
| E7 | Medium | fixed (verified on macOS CoreAudio; Windows/ASIO unplug still needs a hardware check) | **Audio device stall may be undetected.** Device loss is detected only via `isStreamRunning()` (`Controller.cpp:15`); no RtAudio error callback is installed. **Fix:** add a callback heartbeat; treat "running but no progress for ~250 ms" while playing as a failure. Install an error callback. |
| E8 | Medium | fixed | **Stereo sources on a mono route lose a channel.** The "Mono · output N" preset disconnects the right leg and pans hard left; balance panning drops the right channel. **Fix:** when exactly one leg is connected, sum stereo sources to mono on it. |
| E9 | Medium | fixed | **Looping video stalls at the wrap.** The loop target falls before the cached frames, forcing a keyframe seek with no preroll (`Controller.cpp:273`, `Media.cpp:228-235`). **Fix:** preroll the loop start before the wrap. |
| E10 | Medium | fixed (untested on Windows; measure on the show PC) | **Coarse Windows timer resolution.** MIDI and disk workers sleep for 1 ms without `timeBeginPeriod(1)`, so jitter may reach ~15.6 ms. **Fix:** raise timer resolution while armed, then measure. |
| E11 | Low | fixed | **NaN samples reach the driver.** `std::clamp(NaN)` returns NaN (`Transport.cpp:180-182`). **Fix:** replace non-finite samples with 0 and latch the clip indicator. |
| E12 | Low | fixed (one tail per clip; async voices starting mid-source still start abruptly) | **Hard cuts click.** Holds, Stop All, seeks, and clip boundaries cut mid-waveform. **Fix:** a short (2–5 ms) per-voice fade. |
| E13 | Low | fixed (reuse still hashes the whole prepared .pcm to detect silent corruption) | **Audio caches grow forever and opening is slow.** The cache key includes FFmpeg versions; each open SHA-256s the entire source and cache. **Fix:** size-bounded LRU cleanup; cheaper validation before the full hash. |
| E14 | Low | fixed | **Some mono files go hard left.** A mono source declaring a front-left layout is resampled to left only (`Media.cpp:105,139`). **Fix:** treat any one-channel input as centre mono. |
| E15 | Low | fixed | **Per-sample envelope and trig work.** Each voice does a binary search plus `cos`/`sin` per sample. **Fix:** evaluate per block (or per 32 samples) with smoothing. |

## UI and workflow

| ID | Sev | Status | Finding |
|----|-----|--------|---------|
| U4 | Medium | fixed | **The operator readout is thin.** The clock shows elapsed time only. There is no next cue, no time to the next hold, and no remaining time. The timeline does not follow the playhead. Errors go to a status bar that the next message overwrites. PLAYING and STOPPED share a colour. **Fix:** a "Next: cue · in mm:ss" readout, follow-playhead, a sticky colour-coded problem banner, and per-state colours. |
| U5 | Medium | fixed | **Inspector edits are silently lost.** Changes need one of four Apply buttons. Enter does nothing, and a selection change or any `changed` signal discards them. **Fix:** apply on `editingFinished`/toggle (one undo step each), or mark the form dirty and prompt. |
| U6 | Medium | fixed | **No preflight on entering Show mode; one click unlocks it.** Show mode does not check that the show is saved, the output is open, audio/MIDI are ready, and media is online. Leaving it takes one click next to Outputs. Once out of Show mode, a ruler click seeks the live show. **Fix:** a preflight checklist on entry and a confirmation to leave while a show is running. |
| U7 | Medium | fixed | **Program output is fragile.** No `screenRemoved` handling (after an HDMI dropout the frameless window can cover the editor). The cursor is not hidden, the chosen display is not remembered, and choosing the editor screen covers the editor. **Fix:** handle screen changes, hide the cursor, remember the display, and warn for the editor screen. |
| U8 | Medium | fixed | **Autosave recovery is manual and error-prone.** Opening a show never offers a newer `.autosave` file, and the sidecar is never deleted after Save. Saving an opened `.autosave` writes `x.savshow.autosave.savshow`. Unsaved shows and opened packages never autosave. **Fix:** offer recovery on open, delete the sidecar after save, force Save As for `.autosave`, and autosave untitled shows to app data. |
| U9 | Medium | fixed (button spacing; Escape cancels edits outside Show mode; Show mode needs two presses within 600 ms) | **Stop All is too easy to hit in Show mode.** The Stop All button sits next to Play/Continue, and a single Escape fires it. Escape inside a keyframe cell editor also fires Stop All instead of cancelling the edit. **Fix:** separate the button. In Show mode require a double Escape (or Shift+Escape); let editors handle Escape. |
| U10 | Medium | fixed | **The GUI repaints constantly.** Every 16 ms tick repaints the whole timeline viewport, and `compose()` scans tracks × clips even when stopped (`src/app/Timeline.cpp:23`). This competes with software video composition on the GUI thread. **Fix:** repaint only the playhead strips; skip work when nothing changed. |
| U11 | Medium | fixed | **Trims can extend past the source.** Non-looping clips can be trimmed or lengthened beyond the source end (`Timeline.cpp:202`, `MainWindow.cpp:428`), ending in silence or a frozen frame. **Fix:** clamp to `source length − offset` unless looping. |
| U12 | Low | fixed | **Timeline editing basics are missing.** Copy/paste, duplicate, split at playhead, frame nudge with the arrow keys, draggable markers, Ctrl+wheel zoom anchored at the cursor, fit-to-window, next/previous cue keys, drag-and-drop from Finder/Explorer, and linked video+audio clips. |
| U13 | Low | fixed | **Zoom jumps.** Zoom keeps the pixel offset instead of anchoring at the playhead or cursor; ruler labels crowd at low zoom. |
| U14 | Low | fixed | **Mixed units.** The inspector uses decimal seconds; the clock uses timecode. |
| U15 | Low | fixed | **Lists rebuild on every change.** Media and Cues lose their scroll position; the cue selected on the timeline is not highlighted. |
| U16 | Low | fixed | **Delete track confirmation is vague.** The prompt does not name the track, and it deletes the current track even while a clip is selected. |
| U17 | Low | partly fixed (Tab-reachable buttons, no missing font; timeline keyboard/accessibility support open) | **Accessibility gaps.** Buttons are `Qt::NoFocus`; the timeline has no keyboard or accessibility support; hints use 10 px text. The stylesheet requests an uninstalled "Inter" font while `main.cpp` sets Arial. |
| U18 | Low | fixed | **MIDI editor: velocity 0.** A Note On with velocity 0 loads as "Play a note" but its summary says "Stop" (`MidiMessageEditor.cpp:37,125`). |
| U19 | Low | fixed | **Duplicate cue names.** New cues are named "Hold N" from the current count, so names repeat after deletions. |
| U20 | Low | fixed | **Track rows are tall.** 76 px track rows show only four of the demo's six tracks at 960 px window height. |

## Persistence and packaging

| ID | Sev | Status | Finding |
|----|-----|--------|---------|
| P1 | Medium | fixed (worker + progress; rename-instead-of-copy not done) | **Saving an opened package freezes the GUI.** `materializeProjectMedia` copies all extracted media and re-hashes audio on the GUI thread with no progress (`Project.cpp:252-258`). **Fix:** run it on a worker with progress; try rename before copy. |
| P2 | Medium | fixed | **Package extraction has no disk budget.** Compressed entries (zip bombs) and unreferenced `media/*` entries are accepted, and free space is not checked. Extraction folders in `%TEMP%` leak after a crash. **Fix:** accept only stored `media/<uuid>.<ext>` entries that the manifest references; check sizes against free space; sweep stale `simpleavshow-package-*` folders at startup. |
| P3 | Medium | fixed | **Windows leaks extracted video folders.** `Controller::replace` releases the old project's `QTemporaryDir` before `arm(0)` closes video readers, so Windows cannot delete open files. **Fix:** close readers before releasing the old project. |
| P4 | Low | fixed | **Windows device names pass the package filename check** (`media/CON`, `media/NUL.wav`; `ProjectPackage.cpp:85`). Fixed by the UUID-only rule in P2. |
| P5 | Low | fixed | **Loose JSON value types.** Wrong types are coerced (`"gain":"x"` → 1, `"left":1e10` → disconnected). Every asset is decoded before the structure is validated, and unknown fields are dropped on re-save. **Fix:** strict type checks; validate structure before loading media. |
| P6 | Low | fixed | **Failed saves leave partial files.** `materializeProjectMedia` leaves earlier copies behind when a later step fails. |
| P7 | Low | fixed (history takes the previous state by move; the validated candidate and armed snapshot remain) | **Every edit copies the whole project three times** (candidate, undo, armed). This is acceptable now but scales with show size. |

## CI and build

| ID | Sev | Status | Finding |
|----|-----|--------|---------|
| C1 | Medium | fixed (Windows builds and tests on PRs; packaging stays main-only) | **The Windows app never builds on pull requests.** Windows and macOS app jobs are skipped on PRs, so MSVC/ASIO/Windows path behaviour first runs after merge. |
| C2 | Medium | fixed | **`cancel-in-progress: true` also cancels `main` builds.** **Fix:** `cancel-in-progress: ${{ github.event_name == 'pull_request' }}`. |
| C3 | Medium | fixed | **Unpinned supply chain.** linuxdeploy comes from the `continuous` tag without a checksum; actions are pinned by tag, not SHA; RtAudio is cloned by tag; core jobs use floating `*-latest` runners. |
| C4 | Low | partly fixed (Linux collects only packages owning bundled libraries — unverified until the next main build; ASIO SDK terms still undocumented) | **License collection over-collects.** It copies every `/usr/share/doc/*/copyright` on the runner rather than what ships; ASIO SDK obligations are undocumented. |
| C5 | Low | fixed | **No ThreadSanitizer job.** A TSan build of the transport tests would check callback/worker/UI interactions. |

## Test gaps

- Replay a disk-backed clip after seeking back, expecting `bufferWaits() == 0` (E2).
- Transient read failure and retry (E3); NaN source samples (E11); stereo source on a one-leg route (E8).
- Video reader error and loop behaviour (E1, E9).
- Autosave: sidecar contents, recovery, save after undo across a save (E6, U8).
- Packages: compressed, unreferenced, oversized, and device-named entries (P2, P4); cross-OS round trip.
