# 7. Reference and troubleshooting

[Manual contents](README.md) · Previous: [Saving and recovery](saving-and-recovery.md)

## Keyboard reference

Shortcuts below use Windows/Linux names. For Qt menu shortcuts, **Ctrl generally appears as Command on macOS**, and Alt appears as Option. The menu's displayed shortcut is authoritative for your platform. Standard Redo/Save as combinations can differ.

| Key / menu action | Effect |
| --- | --- |
| Space | Play/Pause/Continue outside editing fields. |
| Escape | Stop All in edit mode outside editing fields. |
| Escape twice within 600 ms | Stop All in Show mode. |
| Delete or Backspace | Delete selected clip/cue outside editing fields; linked partners are deleted together. |
| Ctrl+N / Ctrl+O / Ctrl+S | New show / Open / Save. |
| **File → Save as…** | Save to another filename; use the platform's menu shortcut. |
| Ctrl+I | Import media. |
| Ctrl+Z | Undo; up to 32 undo steps. |
| **Edit → Redo** | Redo; use the platform's menu shortcut. |
| Ctrl+C / Ctrl+V | Copy selected clip / paste at playhead on selected compatible track. Editing fields retain their text clipboard behavior. |
| Ctrl+D | Duplicate selected clip after its end. |
| Ctrl+B | Split selected non-looping clip at playhead. |
| Alt+Left / Alt+Right | Nudge selected clip/cue one project frame. |
| Alt+Shift+Left / Alt+Shift+Right | Nudge selected clip/cue one second. |
| Ctrl+Left / Ctrl+Right | Previous / next cue and seek to it in edit mode. |
| Ctrl+mouse wheel | Zoom timeline around cursor. |
| Up / Down, timeline focused | Select previous/next track. |
| Left / Right, timeline focused | Select previous/next clip on that track. |
| Home / End, timeline focused | Seek to project start/end in edit mode. |
| Tab / Shift+Tab | Move keyboard focus through controls. |
| Standard Close shortcut, program window focused | Hide program output; typically Ctrl+W / Command+W. |

Buttons support keyboard focus. The timeline has a focus outline and announces selected track/clip/cue descriptions for assistive technology. Space in text/spin/combo/table-cell editing controls belongs to that control, not the transport. Modal dialogs also have their own keyboard behavior.

## Numeric ranges and units

| Setting | Range / unit |
| --- | --- |
| Project width | 320–7680 pixels. |
| Project height | 240–4320 pixels. |
| Project frame rate | 1–120 fps; default 24. |
| Project duration | Positive seconds in Project settings; no fixed 24-hour cap. Must contain all clips/cues. |
| Clip/cue time fields | Display HH:MM:SS:FF; also accept seconds, MM:SS, and MM:SS:FF. |
| Automation time | Nonnegative seconds relative to clip Start; unique keyframe times. |
| Track/clip volume | 0–4 linear gain; 1 is unity. Volume automation uses the same range. |
| Track/clip pan | −1…+1; one-output mono routing ignores pan. |
| Picture X/Y | −2…+2 project frame widths/heights from center. |
| Scale | 0.01–8; 1 is normal fitted size. |
| Opacity | 0–1. |
| Output channels | One-based physical channel numbers; Off disconnects a leg. Open channels are limited by the interface. |
| MIDI channels | 1–16. |
| Notes / CC / velocity / pressure | 0–127; note velocity 0 stops rather than plays. |
| Displayed MIDI programs | 1–128; transmitted values are 0–127. |
| Pitch bend | −8192…+8191; 0 centered. |

Numeric inspector fields may use your system decimal separator. Automation table entries are parsed as decimal-point numbers. Timecode counts frames at the project rate; it is not a wall-clock stopwatch and does not include time spent holding.

## Health indicators

The bottom-right readout reports open audio channels and 48 kHz, async activity, MIDI status when needed, and fault counters. A problem counter turns it orange. The program banner/status bar explain playback failures.

| Indicator | Meaning and response |
| --- | --- |
| **Audio · connects on Play** | No running output yet. Play prepares the default/remembered device. |
| **N ch · 48 kHz** | Audio output is running with N channels. Verify physical routing separately. |
| **N async playing** | Async voices are active; pausing does not necessarily silence them. Use Stop All before editing/connections. |
| **MIDI needs a device** | The show has MIDI messages but no remembered destination. Choose one in Outputs. |
| **MIDI reconnects on Play** | A destination is remembered but not currently connected. Check it is available. |
| **CLIPPING** | Output exceeded full scale or contained invalid samples. Lower summed levels; Stop All clears the latch. |
| **underruns** | Audio driver reported missed delivery. Try a larger buffer and investigate CPU/driver load. |
| **audio waits** | Disk-backed audio was not ready. Audio and show time wait together instead of skipping source frames. Investigate disk throughput and concurrent playback. |
| **disk retries** | Source reads needed retrying. Investigate file availability, storage, and programs locking the files. |
| **video waits** | A requested picture was delayed; the previous composition is retained when available. Reduce decode/composition load and rehearse the actual media. |

Counters describe software events; they do not establish physical audio/video/MIDI alignment. Some counters reset when playback/device state is rebuilt. Use changes during a rehearsal to assess stability.

## Troubleshooting

| Symptom | Check and recovery |
| --- | --- |
| Play does nothing or a warning appears | Read the banner/status. Wait for preparation, resolve missing timeline media, select needed MIDI, and check remembered audio/routing. |
| Remembered audio device is missing | Reconnect it, Refresh devices, and retry Play. To use another device: Stop All, leave Show mode, choose it explicitly in Outputs. |
| “A track needs more output channels” | Open enough channels on the interface or route/disconnect the offending track. Muting unused audio tracks also excludes their routes from the check. |
| No Guide/Click sound | They start Off. Route to intended physical channels, open those channels, check mute/gain/source timing, and inspect the cabling. |
| Stereo content loses a side on a mono feed | Exactly one connected leg sums stereo to mono. Verify the other leg is Off and check phase cancellation in the source. |
| Sound continues after Pause or at a hold | Check **Keep playing through pauses / holds** and async count. This is expected for async sources. Stop All silences them. |
| Cannot edit/save/test MIDI | Stop All, leave Show mode, and wait for media preparation. Paused async voices still lock editing. |
| Outputs/Audience display button is disabled | Leave Show mode or wait for preparation. In edit-mode playback, Outputs opens for inspection but connection controls stay locked. |
| Space types a space or changes a control | Move focus out of text/spin/combo/cell editors, then use Space or the transport button. |
| Escape did not stop Show mode playback | Press twice within 600 ms, or click Stop All once. |
| Copy/Paste/Duplicate has no useful result | Select a clip and a compatible destination track. Picture overlaps/project bounds can reject placement; copies are unlinked. |
| Split is rejected | Put the playhead strictly inside the selected clip. Looping clips and linked looping partners cannot be split. |
| Picture clips cannot overlap | Add another picture layer. Use opacity automation for a crossfade. |
| After hold does nothing | The selected clip needs a following hold inside its interval, at or after the current playhead. |
| Changing a clip's gain/opacity seems ineffective | A nonempty curve supplies that property instead of its base value. Edit the curve or remove its rows and Apply curve. |
| MIDI test works but cue does not | Click Apply cue, check arrival/resume timing and destination/channel, and rehearse across the cue. Seeking alone sends nothing. |
| External note/device remains active after Stop All | Send matching note-off/cleanup MIDI or use the receiver's stop/panic control. Stop All sends no external stop command. |
| Program selection is off by one | The UI displays 1–128; MIDI transmits 0–127. Check the receiver's numbering convention. |
| Missing files after moving a project | Move the required media tree too, use Relink missing media, or transfer a .savpack. Save as does not gather all imported sources. |
| Audio source changed/cannot be read | Restore temporarily unavailable originals and retry Play. Reopen the show to reload changed files or rebuild damaged caches; rehearse the result. |
| Fatal video warning with frozen picture | Audio/cues can continue. After stopping, verify the source and reopen/reprepare it; inspect the audience output. |
| Black picture | Check clip interval, picture-track hide, opacity/curves, offline media, and selected program display. Source/preview aspect ratios can produce black bars normally. |
| Program output covered the editor | Focus the output and use the standard Close shortcut shown in the warning. Reopen on a separate audience screen. |
| Program display disconnected | Restore the display/cable. Output attempts to reopen when the same named screen returns away from the editor; verify it physically. |
| Package export/open fails | Keep all library media available and ensure free disk space. Use an unmodified app-generated package; read the archive/source error. |
| Package edits were not recovered after restart | Opened packages need a normal Save before autosave starts. Keep the original package and save the extracted working copy. |
| Saved show has a newer autosave | Choose Recover autosave when opening; inspect it and Save or Save as. |
| Audio crackles or picture stutters | Increase audio buffer, reduce simultaneous media/composition load, use stable local storage, and investigate the health counters on show hardware. |

## Media preparation and disk usage

Compatible 48 kHz mono/stereo PCM WAV files stream directly from their source. Supported direct WAV sample types include 8/16/24/32-bit integer and 32-bit float, with compatible extensible headers. Other audio formats/sample rates prepare to a reusable 48 kHz stereo float PCM cache, about **1.29 GiB per source hour**. Multichannel source audio still needs separate mono/stereo stems.

Caches live in Qt's application cache directory under `audio-pcm-v1-le` (or `audio-pcm-v1-be`). They are local, excluded from packages, and can be reused when identical source content moves or arrives in a reopened package. Reuse validates source/prepared data and waveform metadata; missing or damaged entries rebuild. Even a reused cache can take time to validate for large files.

The current app trims old/unused caches at startup and after conversion, targeting at most 20 GiB and a lower budget on disks with limited free space. In-use cache entries are retained. With the app closed, you can remove its audio cache folder to reclaim space; sources needing conversion will prepare again on the next open. Keep originals and saved projects.

Playback uses bounded read-ahead buffers rather than keeping entire imported audio files in memory. There is no fixed project audio-byte budget, track/clip/cue count cap, or 24-hour duration cap. Actual capacity depends on memory, disk throughput, simultaneous voices, and picture work; very large projects still need target-machine testing.

## Current limitations

- Windows 11 is the primary show target. Physical routing/ASIO, MIDI bridge latency, display behavior, and complete rehearsal runs require validation on the actual machine.
- One audio output device is used at a time. NDI, MIDI input, Wings import, plugins, and advanced effects are outside v1.
- Audio is based on mono/stereo sources; prepare separate stems for multichannel material.
- Picture clips cannot overlap within one layer. Video decoding and picture composition use software FFmpeg/QPainter; hardware decoding/GPU composition are future work.
- Multi-layer 1080p24 capacity is not established for your show PC by automated tests.
- Audio legs share one callback, but physical driver/display buffering and MIDI timing are not automatically latency-compensated. Measure alignment.
- Text styling is fixed in the UI and system fonts can vary between computers.
- Async source position survives seeks within an active clip, but not edits/device changes. Stop All/project end clear voices.

See [hardware validation](hardware-validation.md) for a structured rehearsal and failure-test procedure.
