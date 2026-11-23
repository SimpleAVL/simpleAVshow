# 9. Reference and troubleshooting

[Manual contents](README.md) · Previous: [8. Agent collaboration](agent-collaboration.md)

Use this chapter as a quick desk reference for shortcuts, exact numeric limits, health indicator meanings, troubleshooting, media caching, and current v1 boundaries.

---

## Keyboard shortcuts

Shortcuts below use Windows and Linux naming. On macOS, **Ctrl** in menu shortcuts is generally **Command (⌘)** and **Alt** is **Option (⌥)**—the shortcut shown in your platform's menu bar is always authoritative (especially for **Redo** and **Save as…**).

### Playback & cues

| Shortcut | Action |
| --- | --- |
| **Space** | Play, Pause, or Continue a hold *(when focus is outside text/number/dropdown/table editors)*. |
| **Escape** | **Stop All** in edit mode *(outside editing controls)*. |
| **Escape × 2** *(within 600 ms)* | **Stop All** in Show mode. |
| **Ctrl+Left / Ctrl+Right** | Jump to previous / next cue *(edit mode)*. |
| **Home / End** *(timeline focused)* | Seek to project start / end *(edit mode)*. |

### Editing & timeline

| Shortcut | Action |
| --- | --- |
| **Ctrl+Z** / **Edit → Redo** | Undo (up to 32 steps) / Redo. |
| **Ctrl+C / Ctrl+V** | Copy selected clip / paste at playhead on selected compatible track *(normal text clipboard inside fields)*. |
| **Ctrl+D** | Duplicate selected clip immediately after its end. |
| **Ctrl+B** | Split selected non-looping clip at the playhead. |
| **Delete** or **Backspace** | Delete selected clip or cue *(outside editing fields; deletes linked partners together)*. |
| **Alt+Left / Alt+Right** | Nudge selected clip or cue by 1 project frame. |
| **Alt+Shift+Left / Right** | Nudge selected clip or cue by 1 second. |
| **Up / Down** *(timeline focused)* | Select previous / next track. |
| **Left / Right** *(timeline focused)* | Select previous / next clip on the current track. |
| **Ctrl+mouse wheel** | Zoom timeline around the cursor. |

### Files & windows

| Shortcut | Action |
| --- | --- |
| **Ctrl+N / Ctrl+O / Ctrl+S** | New show / Open / Save. |
| **File → Save as…** | Save under a new filename *(platform shortcut)*. |
| **Ctrl+I** | Import media. |
| **Tab / Shift+Tab** | Move keyboard focus across controls. |
| **Ctrl+W / Command+W** *(program window focused)* | Hide the fullscreen audience display window. |

> **Accessibility note:** Buttons accept keyboard focus, and the timeline draws a clear focus outline while announcing selected tracks, clips, and cues to assistive technologies.

---

## Numeric ranges and units

| Setting | Valid range / unit |
| --- | --- |
| **Project width × height** | `320–7680` × `240–4320` pixels. |
| **Project frame rate** | `1–120` fps (default `24`). |
| **Project duration** | Positive seconds in Project settings (no fixed 24-hour cap; must contain all clips and cues). |
| **Clip & cue time fields** | Display `HH:MM:SS:FF`; accept seconds (`12.5`), `MM:SS`, or `MM:SS:FF`. |
| **Automation time** | Nonnegative seconds relative to clip **Start**; no duplicate times. |
| **Automation shape & bend** | `Linear`, `Hold`, `Curve`, or `S-curve` per segment; bend `−1…+1` (`0` = straight). |
| **Track & clip volume** | `0–4` linear gain (`1` = unity; same range for Volume automation). |
| **Track & clip pan** | `−1…+1` *(ignored on single-leg mono routes)*. |
| **Picture X / Y** | `−2…+2` project frame widths / heights from center. |
| **Picture scale** | `0.01–8` (`1` = normal fit). |
| **Picture opacity** | `0–1`. |
| **Audio output channels** | 1-based physical channel numbers, or **Off**; capped by open interface channels. |
| **MIDI channels** | `1–16`. |
| **MIDI notes / CC / velocity / pressure** | `0–127` *(Note On velocity `0` acts as note-off)*. |
| **MIDI program numbers** | Displayed `1–128`; transmitted on the wire as `0–127`. |
| **MIDI pitch bend** | `−8192…+8191` (`0` = centered). |

Inspector spin boxes respect your operating system's decimal separator, whereas automation table cells always parse a period (`.`). Timecode counts frames of **show time**—it pauses during holds and does not track wall-clock elapsed time.

---

## Health indicators

The bottom-right readout summarizes engine status and turns **orange** whenever a fault counter increments.

| Indicator | What it means & what to do |
| --- | --- |
| **Audio · connects on Play** | Idle—pressing **Play** will open the remembered (or default) audio device. |
| **N ch · 48 kHz** | Audio engine is running with `N` open physical channels. *(Verify cabling separately.)* |
| **N async playing** | Async voices are active. Pause won't silence them—use **Stop All** before editing or switching devices. |
| **MIDI needs a device** | The show contains MIDI cues, but no MIDI destination is selected. Pick one in **Outputs**. |
| **MIDI reconnects on Play** | A destination is remembered and will reconnect automatically on **Play**—check that it's plugged in/running. |
| **CLIPPING** | Summed audio exceeded full scale (or hit invalid samples) and was clamped. Lower gains and click **Stop All** to clear. |
| **underruns** | The audio driver missed a buffer deadline. Try a larger buffer size and check CPU/driver load. |
| **audio waits** | Audio wasn't ready in time while **If audio is late** is set to **Freeze**; audio and show time waited together instead of skipping frames. Choose **Prepared cache** audio sources, check disk speed, and reduce simultaneous sources. |
| **audio gaps** | Audio wasn't ready in time while **If audio is late** is set to **Gap**; show time kept running while the late sound dropped out briefly and faded back in. Choose **Prepared cache** audio sources, check disk speed, and reduce simultaneous sources. |
| **disk retries** | Source file reads had to retry. Check drive stability, cables, or other apps locking the files. |
| **video waits** | A video frame decoded late; the previous frame stayed on screen. Lower decode/layer load and rehearse with final media. |

*Counters track software events and may reset when playback or devices rebuild—watch for increments during a full rehearsal rather than assuming zero guarantees physical alignment. Brief misses right after jumping on the timeline are normal and are not counted as audio waits or gaps.*

---

## Troubleshooting

### Playback & audio

| Symptom | Likely cause & fix |
| --- | --- |
| **Play does nothing or shows a warning** | Check the banner under **PROGRAM** and the status bar: wait for media preparation, relink missing timeline media, select a MIDI destination if cues need one, or check audio device/routing. |
| **Remembered audio device is missing** | SimpleAVShow won't silently fall back to built-in speakers. Reconnect your interface, click **Refresh devices**, and retry **Play**—or **Stop All**, leave Show mode, and explicitly choose another device in **Outputs**. |
| **"A track needs more output channels"** | Open enough channels in **Outputs**, reroute the track to an open channel, or mute the unused track. |
| **No sound from Guide or Click** | Both tracks start with outputs **Off**. Route them to physical outputs, open enough channels in Outputs, check mute/gain/clip timing, and verify your cabling. |
| **Stereo audio loses one side on a mono feed** | With one connected leg, stereo sums automatically as `(L + R) / 2`. Check that the other leg is truly **Off**, and check whether your stereo file has phase-inverted channels canceling out. |
| **Audio or video keeps playing after Pause or at a hold** | Normal behavior for **async** tracks (**Keep playing through pauses / holds**). Check the `async playing` indicator and click **Stop All** to silence everything. |
| **Status bar counts audio waits or gaps** | A source was not read in time. Choose **Prepared cache** audio sources in **Outputs**, use fast local storage, and reduce simultaneous long sources. **Freeze** counts waits; **Gap** counts gaps. |
| **Audio crackles or video stutters** | Increase the audio buffer size in Outputs, reduce overlapping layers/voices, run media from fast local storage, and inspect the health counters on the actual show PC. |
| **Audio source changed or unreadable** | Restore any disconnected drive/file and retry **Play**. If the file was modified on disk or its cache was damaged, reopen the show to reload/rebuild and rehearse again. |

### Editing, timeline & automation

| Symptom | Likely cause & fix |
| --- | --- |
| **Can't edit, save, or test MIDI** | Click **Stop All**, leave **Show mode**, and wait for media preparation. Even a paused show with active async voices locks editing. |
| **Outputs / Audience display button is greyed out** | Leave **Show mode** or wait for preparation. During edit-mode playback, Outputs opens read-only with a **Stop All to change connections** button inside. |
| **Space types a space or triggers a control** | Focus is inside a text box, number field, dropdown, or table cell. Click the timeline, preview, or a transport button first. |
| **Escape didn't stop playback in Show mode** | Show mode requires **Escape twice within 600 ms** (or one click on **Stop All**). |
| **Copy / Paste / Duplicate won't place a clip** | Make sure a clip and a compatible destination track are selected, picture layers have room without overlapping, and there is space inside project duration. *(Copies are always unlinked.)* |
| **Split is rejected** | Put the playhead strictly *inside* the clip. Looping clips (and clips linked to a looping partner) cannot be split. |
| **Can't overlap two picture clips** | Picture clips can't overlap on the same track. Add another picture layer and animate the upper layer's opacity to crossfade. |
| **After hold does nothing** | The selected clip must contain a hold cue at or after the current playhead position. |
| **Changing clip Gain or Opacity has no effect** | A non-empty automation curve overrides the clip's base value. Edit the curve, or remove all its points and click **Apply curve**. |

### Cues & MIDI

| Symptom | Likely cause & fix |
| --- | --- |
| **Send test works, but the cue doesn't fire in the show** | Click **Apply cue** to save your MIDI settings into the cue, check **Send** timing (arrival vs. after hold), verify the port/channel, and play *across* the cue (seeking never sends MIDI). |
| **Program number is off by one** | SimpleAVShow displays programs as **1–128** and transmits **0–127** on the wire. Check whether your synth or console numbers from 0 or 1. |
| **External synth note or light stays on after Stop All** | **Stop All** never sends external MIDI stop/panic commands. Send a matching Note Off / cleanup cue, or use your receiver's own panic button. |

### Picture & displays

| Symptom | Likely cause & fix |
| --- | --- |
| **Picture is black** | Check clip timing, whether the picture layer is hidden, opacity/automation curves, offline media, and which screen is chosen in Outputs. *(Letterbox/pillarbox black bars are normal when aspect ratios differ.)* |
| **Fullscreen output covered the editor** | Focus the output window and press **Ctrl+W** (Windows/Linux) or **Command+W** (macOS) to hide it, then reopen it on a separate audience monitor. |
| **Audience display disconnected** | Reconnect the cable. SimpleAVShow hides the window on disconnect and tries to reopen automatically if the same named display returns on a screen other than the editor—always verify physically. |
| **Fatal video warning with frozen picture** | Audio and MIDI cues continue safely. After stopping, check the video file and reopen/reprepare the show. |

### Files, packages & recovery

| Symptom | Likely cause & fix |
| --- | --- |
| **Files go missing after moving a project** | **Save** / **Save as…** keeps external media in place. Move the full media folder tree with the `.savshow`, use **File → Relink missing media…**, or transfer a `.savpack` package instead. |
| **Package export or open fails** | All Media library items (even unused ones) must be online to export, and the drive needs enough free space. Don't open manually modified/re-zipped archives. |
| **Edits to an opened package disappeared after restart** | Opened `.savpack` files live in temporary storage and don't autosave until your first normal **File → Save**. Always save a permanent `.savshow` copy after opening a package. |
| **Prompted about a newer autosave on open** | Choose **Recover autosave** to inspect your recovered changes, then **Save** (to update the project) or **Save as…** (to keep a separate copy). |

---

## Media preparation and audio cache

- **Direct streaming WAVs:** 48 kHz mono or stereo PCM WAV files (8/16/24/32-bit integer or 32-bit float, including standard extensible headers) always stream directly from their source with zero conversion.
- **Other formats and sample rates:** Controlled by **Audio sources** in **Outputs** (see [Playback reliability](audio-and-outputs.md#playback-reliability)). Multichannel source files must still be split into separate mono/stereo stems beforehand.
  - **Stream from media files:** Decodes the original file during playback. Import still decodes each source once to draw its waveform and measure its exact length, then keeps only a small waveform file (~**30 KB per source hour**). Originals must stay available while the show plays.
  - **Prepared cache:** Decodes into a reusable 48 kHz stereo float PCM cache (~**1.29 GiB per source hour**).
- **Cache location and cleanup:** Caches and waveform files share Qt's application cache directory under `audio-pcm-v1-le` (or `audio-pcm-v1-be`). They are local, never packed into `.savpack` files, and reused automatically when identical source content moves or arrives in a package (even a reused prepared cache can take time to validate for large files; missing or damaged entries rebuild). SimpleAVShow trims old unused caches on startup and after conversions (targeting at most 20 GiB, or less on tighter drives) while retaining in-use entries. To reclaim disk space, use **Clear audio cache** in **Outputs**, or—with the app closed—remove that cache folder; sources needing preparation will prepare again next time you open them. Keep your originals and saved projects.
- **Memory usage:** Playback streams through bounded read-ahead buffers instead of loading entire audio files into RAM. There is no hard cap on audio bytes, tracks, clips, cues, or 24-hour duration—practical limits depend on your show computer's disk speed, memory, simultaneous voice count, and video decoding load.

---

## Current limitations

Keep these v1 scope boundaries in mind when planning a production:

- **Hardware validation required:** Windows 11 is the primary stage target. ASIO routing, MIDI bridge latency, multi-display behavior, and real-world show stability must be verified on your actual show PC.
- **Single audio output device:** One audio interface at a time. NDI, MIDI input, Wings import, audio plugins, and advanced video effects are not included in v1.
- **Mono/stereo audio files only:** Split multichannel audio files into separate mono or stereo stems before importing.
- **Software video decoding and compositing:** Picture clips cannot overlap on the same layer, and video decoding/compositing currently runs on the CPU via FFmpeg and QPainter (hardware acceleration is future work). Always stress-test multi-layer 1080p24 shows on your real show PC.
- **No automatic latency compensation:** All audio outputs share one callback, but physical driver buffering, projector lag, and MIDI bridge latency are not automatically compensated—measure alignment during rehearsal.
- **Fixed text styling:** Text uses centered, white system Arial; appearance can vary slightly between operating systems.
- **Async state resets on edits:** Async media positions survive seeks inside an active clip, but reset on **Stop All**, project end, edits, or device changes.

For a structured test run on your stage machine, see the [Show-machine validation checklist](hardware-validation.md).
