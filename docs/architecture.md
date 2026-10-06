# Architecture

## Transport and clocks

`Transport::render` is independent of Qt and runs in one audio device callback at 48 kHz. Before each output sample, it checks markers at the current integer show position. Holds consume their markers and freeze show time before rendering that sample. Normal clips become silent; async sources keep advancing and evaluate automation at the frozen show time. The audio device remains running.

An async clip beginning exactly at a hold starts on arrival. Loops wrap to the source-in point. One-shots become Finished at source end and do not retrigger because show time remains inside them. Play from Held sends resume MIDI once and advances show time; same-time holds are consumed together. Manual pause cannot consume a held cue.

Seek changes show time and rearms markers without sending MIDI. It preserves running/finished async state inside the clip's half-open interval and clears it outside. Dormant sources start at the sought source position during playback. Stop All clears voices without rewinding. Show end clears all voices; a final hold can wait for the operator to resume into the end.

## Thread boundaries

The armed Show is immutable and references predecoded PCM. UI edits validate a candidate before committing, close the device, then swap the snapshot. Fixed SPSC queues carry commands and MIDI events. Published status uses lock-free atomics. The callback does no allocation, disk I/O, mutex locking, Qt work, or MIDI sending.

Audio legs mix into one interleaved device buffer. Tracks map L/R legs to physical channels; −1 disconnects a leg. Pan is constant-power for mono and balance attenuation for stereo. Overload clamps at the device boundary and latches an indicator. Tracks align sample-exactly because there is one callback/clock. Bounded clip scans cap work but are not a performance guarantee at maximum size.

## Media and picture

QtConcurrent import/open jobs use FFmpeg to probe and predecode audio. Audio/video share the container timestamp origin; leading silence or start trimming preserve embedded-stream offsets. Mono/stereo audio resamples to 48 kHz; larger channel layouts are rejected.

Video workers have bounded lookahead caches. At most the active and closest upcoming video per layer are retained. Frames downscale to project bounds before caching. The GUI composites images/text in layer order using show-time transformations. Preview and physical output receive the same final QImage. If a requested frame is not ready, the previous composition remains and the wait counter increments; audio continues independently.

Software picture composition, GUI presentation, refresh, and device/display latency are remaining performance risks. GPU composition and hardware decoding should follow profiling on the show PC; output is not genlocked.

## MIDI

A dedicated 1 ms polling worker sends RtMidi messages from an immutable snapshot. Seek/Stop All advance an epoch to invalidate stale queued events. The worker joins before reconfiguration. The bridge is a native MIDI port; no network protocol is assumed. Complete MIDI 1.0 messages, including bounded SysEx, are validated.

Scheduling has OS/driver jitter and may precede audible buffer presentation. Measurement and compensation are future work.

## Projects

`.savshow` is versioned JSON with integer audio-frame timing and relative media paths. Validation checks IDs, ranges, routing, MIDI, envelope ordering, picture overlaps, and limits. QSaveFile replaces files atomically. Generated demo assets export on first save; other media stays external. Undo/redo retains 32 snapshots sharing media. Editing/audio-MIDI connection changes/saving require no active playback or async voices; Show mode additionally locks seeks and Outputs access. In edit mode, Outputs remains accessible during playback/holds for inspection and program display controls. Connection controls show their locked state and offer Stop All to unlock; inspecting a live show avoids probing audio drivers.

## Next milestones

1. Windows/ASIO build and physical routing verification; disconnect/reconnect and full rehearsal runs.
2. Measure display and MIDI bridge latency, then add compensation.
3. Profile four-layer 1080p24 and long shows; add GPU/hardware decoding and streamed/prebuffered audio.
4. Crash logging, signed installers, release metadata, and third-party notices.
5. NDI using the composed picture.
