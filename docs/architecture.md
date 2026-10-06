# Architecture

## Transport and clocks

`Transport::render` is independent of Qt and runs in one audio device callback at 48 kHz. Before each output sample, it checks markers at the current integer show position. Holds consume their markers and freeze show time before rendering that sample. Normal clips become silent; async sources keep advancing and evaluate automation at the frozen show time. The audio device remains running.

An async clip beginning exactly at a hold starts on arrival. Loops wrap to the source-in point. One-shots become Finished at source end and do not retrigger because show time remains inside them. Play from Held sends resume MIDI once and advances show time; same-time holds are consumed together. Manual pause cannot consume a held cue.

Seek changes show time and rearms markers without sending MIDI. It preserves running/finished async state inside the clip's half-open interval and clears it outside. Dormant sources start at the sought source position during playback. Stop All clears voices without rewinding. Show end clears all voices; a final hold can wait for the operator to resume into the end.

## Thread boundaries

The armed Show is immutable and references prepared audio sources. UI edits validate a candidate before committing, close the device, then swap the snapshot. SPSC queues carry commands and MIDI events; MIDI capacity is allocated while disarmed and scales to the project marker count. Published status uses lock-free atomics. The callback does no allocation, disk I/O, mutex locking, Qt work, or MIDI sending.

Voice state and active-clip storage are allocated while disarmed. Stable sorted indices schedule clip starts and markers; per-sample work visits active audio clips and the markers at the current boundary instead of scanning the entire show. Seeking rebuilds the active set and locates the marker cursor without allocating. Clip completion retires voices; held async voices continue under the same rules as resident sources.

Audio legs mix into one interleaved device buffer. Tracks map L/R legs to physical channels; −1 disconnects a leg. Pan is constant-power for mono and balance attenuation for stereo. Overload clamps at the device boundary and latches an indicator. Tracks align sample-exactly because there is one callback/clock. Playback cost depends on simultaneous voices and device channels rather than total timeline length; practical polyphony still requires hardware rehearsal.

## Media and picture

QtConcurrent import/open jobs use FFmpeg to probe and decode audio incrementally into temporary 48 kHz interleaved stereo float PCM files. Decoding retains one conversion chunk and 10 ms waveform peaks rather than the whole PCM in RAM. Audio/video share the container timestamp origin; a sparse silence prefix or incremental start trimming preserves embedded-stream offsets. Mono/stereo audio is supported; larger channel layouts still require separate stems.

Each armed disk-backed clip owns four 8192-frame read-ahead slots. One transport disk worker fills them; release/acquire ownership prevents it from overwriting a callback-held slot. The initial offset (or sought position when arming) is prebuffered before the device opens, including loop lookahead. During playback the callback requests source blocks using an atomic cursor, reads published slots, and advances async cursors only after all required sources are ready. A miss leaves the remaining device block silent and freezes show/source clocks for a retry; waits are visible in the editor. Disk errors are published to the controller, which stops playback and asks the operator to rebuild caches by reopening the project.

Sources and cache lifetime are shared by project/undo/armed snapshots. The last owner deletes the temporary file. Cache creation/write failures report the underlying error, including disk exhaustion; partial imports remove their temporary files. Project JSON still references original media paths and remains version 1 compatible. Reopening rebuilds caches. Generated demo/test sources can remain resident.

Video workers have bounded lookahead caches. At most the active and closest upcoming video per layer are retained. Frames downscale to project bounds before caching. The GUI composites images/text in layer order using show-time transformations. Preview and physical output receive the same final QImage. If a requested frame is not ready, the previous composition remains and the wait counter increments; audio continues independently.

Software picture composition, GUI presentation, refresh, and device/display latency are remaining performance risks. GPU composition and hardware decoding should follow profiling on the show PC; output is not genlocked.

## MIDI

A dedicated 1 ms polling worker sends RtMidi messages from an immutable snapshot. Seek/Stop All advance an epoch to invalidate stale queued events. The worker joins before reconfiguration. The bridge is a native MIDI port; no network protocol is assumed. Complete MIDI 1.0 messages, including bounded SysEx, are validated.

Scheduling has OS/driver jitter and may precede audible buffer presentation. Measurement and compensation are future work.

## Projects

`.savshow` is versioned JSON with integer audio-frame timing and relative media paths. Validation checks IDs, representable frame timing, routing, MIDI, envelope ordering, and picture overlaps. Dynamic project collections have no prototype count caps; picture overlap validation sorts by layer/time instead of comparing every pair. Frame fields must be exact nonnegative integers within the JSON double integer range; clip bounds use subtraction to avoid addition overflow. The timeline scales scroll coordinates when its pixel width exceeds the scrollbar integer range. QSaveFile replaces files atomically. Generated demo assets export on first save; other media stays external. Undo/redo retains 32 snapshots sharing media. Editing/audio-MIDI connection changes/saving require no active playback or async voices; Show mode additionally locks seeks and Outputs access. In edit mode, Outputs remains accessible during playback/holds for inspection and program display controls. Connection controls show their locked state and offer Stop All to unlock; inspecting a live show avoids probing audio drivers.

## Next milestones

1. Windows/ASIO build and physical routing verification; disconnect/reconnect and full rehearsal runs.
2. Measure display and MIDI bridge latency, then add compensation.
3. Profile four-layer 1080p24 and long shows; measure disk-cache read-ahead and add GPU/hardware decoding.
4. Crash logging, signed installers, release metadata, and third-party notices.
5. NDI using the composed picture.
