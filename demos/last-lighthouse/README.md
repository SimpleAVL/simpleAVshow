# The Last Lighthouse

A keeper waits through a storm for a reply from a ship. The audience hears a chamber score and the sea; performers receive a private click and the crew receives a spoken guide. A moving lighthouse beam, rain, a radio insert, and surtitles tell the story. The actor controls the duration of three stage waits; the sea continues while the picture and score hold.

Open **[last-lighthouse.savpack](last-lighthouse.savpack)** through File → Open for a self-contained copy, or open **[last-lighthouse.savshow](last-lighthouse.savshow)** with its adjacent `media/` directory. The package contains every library source, including calibration media. Output-device selection is local to the machine. Use edit mode throughout this demonstration.

This is a real-file demo: **20 media files, 11 tracks, 19 clips and 20 cues**, including 19 outgoing MIDI messages. The scene lasts 64 seconds of show time, plus operator waits. Normal rehearsal uses four seconds at each hold and a two-second manual pause.

## The scene

| Show time | Stage action | Operator / technical action |
| --- | --- | --- |
| 0–12 s | Night watch. An original D-minor arpeggio grows under the sea. | Program 5 and CC7 at zero. Note-on, note pressure and matching note-off. Bell one-shot starts at 10 s. |
| 12 s | The keeper enters: “If I hold the light, will you find your way home?” | Two same-time hold cues: arrival CC1 sends once; Program 6 waits for Continue. Sea loops; the bell finishes naturally during the wait. |
| 12–30 s | The keeper sings into the storm. Three flashes become a radio signal. | Pitch bend and reset, channel pressure, storm CC1. A trimmed MP4/audio pair appears at 27 s with scale, vertical-position and opacity automation. |
| 30 s | Wait for the signal operator. | Hold; Continue sends a six-byte educational SysEx using manufacturer ID 7D. The radio picture freezes with the stage. |
| 30–43 s | A thunder effect; then the ship answers. | Synchronous stereo effects, sustain on/off, isolated crew guide and click. |
| 43 s | “I see your light. Keep it burning.” Wait for the reply. | Arrival expression CC; Continue selects Program 9. Sea continues on outputs 5–6. |
| 43–64 s | Dawn and final chorus: “The morning belongs to those who waited.” | Sea fades to zero by 52 s. MP3 fades into a source-trimmed WAV reprise at 48 s. Explicit all-notes-off and lighting-blackout cues close the scene. |

MIDI represents a demonstration lighting/instrument rig. The validation sends to the virtual **Lighthouse MIDI Logger**, which receives and logs raw bytes without changing any external instrument. Select the intended port before Play. Stop All does not send MIDI panic; the show contains an explicit cleanup cue.

## Channel patch

Open at least eight output channels; the validation opens all sixteen BlackHole channels to check unused outputs too. Channel numbers below are one-based.

| Track | Output | Behavior |
| --- | --- | --- |
| Score MP3 / WAV reprise | 1–2 | Audience stereo; follows show time. Crossfade, gain and balance automation. |
| Crew guide WAV | 3 only | Spoken standby calls; right leg Off. |
| Click WAV | 4 only | 120 BPM, stronger downbeat; right leg Off. |
| Sea loop WAV | 5–6 | Async loop continues through holds and pause. Fade automation follows show time. |
| Signal film audio / thunder WAV | 7–8 | Synchronous effects; MP4 picture/audio remain linked. |
| Keeper bell WAV | 7–8 | Async one-shot; finishes during the first hold and remains finished on resume. |
| Muted rehearsal score | 1–2 | Intentionally muted duplicate. |
| Unused outputs | 9–16 | Must remain silent. |

For an actual production, patch the isolated sea/effects stems into the audience mixer along with 1–2. Outputs 3–4 are private feeds. This validation deliberately keeps stems separate so their behavior can be measured.

## Coverage and evidence

| Capability | Concrete exercise |
| --- | --- |
| MP4 / H.264 / embedded AAC | Night, storm and dawn projections; linked, trimmed radio film/audio. |
| MP3 and WAV decoding | Original score, source-offset WAV finale, sea, bell, thunder, speech and click. |
| Layer composition | Up to four simultaneous picture layers: background movie, insert movie, transparent PNG and native text. |
| Automation | Volume, pan/balance, horizontal/vertical position, scale and opacity. Fade-in/out and MP3/WAV crossfade. |
| Native MIDI out | Programs, notes, polyphonic/channel pressure, CC, pitch bend/reset and SysEx. Exact-order independent CoreMIDI receiver log. |
| Holds | Three waits; same-time arrival/resume cues; picture/time freeze while async sea continues. |
| Pause and seek | `async-check.savshow`: ordinary click stops, async loop survives pause/inside seek, one-shot ends; exact-end seek stops it; paused re-entry remains silent until Play. |
| Multitrack routing | `routing-check.savshow`: distinct 211/307/419/557/683/809/947/1103 Hz fingerprints on outputs 1–8 and a simultaneous impulse. |
| Stop/end | Captured silence after Stop All; no async voices after project end. |
| Persistence | Native save/reopen, native package export/reopen, full rehearsal from the package's extracted real sources. |
| OBS | Six audio tracks: full mix, audience score, crew guide, click, sea, effects. Stream count, decoded signal, visible/changing video are verified. |

See **[VALIDATION.md](VALIDATION.md)** for the results and the measured real BlackHole, MIDI and OBS results, with separately labeled earlier software evidence.

## Reproduce the validation on macOS

From the repository root, build the opt-in runner and install the Python tools in an isolated environment:

```sh
cmake -S . -B build -DSAV_BUILD_DEMO_VALIDATION=ON
cmake --build build -j 6
python3 -m venv artifacts/last-lighthouse/venv
artifacts/last-lighthouse/venv/bin/pip install -r tools/demo/requirements.txt
```

The native runner uses isolated Qt settings and a separate application window. It does not change the normal editor's remembered audio/MIDI configuration. It does send real MIDI to the named logger port. `run_validation.py` starts and stops that independent receiver automatically.

For complete device-loopback and screen-capture evidence:

1. Install **BlackHole 16ch**, with administrator access, and confirm its input/output appear in Audio MIDI Setup. The [official BlackHole instructions](https://github.com/ExistentialAudio/BlackHole) cover loading/restarting CoreAudio; arrange this when other audio work can stop.
2. Install **OBS Studio**, allow screen capture in macOS Privacy & Security, and enable its authenticated WebSocket server on port 4455. Allow microphone access for **Lighthouse Loopback Recorder** when macOS prompts. This independent signed native app opens the BlackHole input; its visible meters and MIDI log are included in OBS. The client reads the local OBS configuration; it does not embed a password in the repository or command line.
3. Run the command below. It creates/selects a dedicated **Lighthouse validation** OBS profile and scene collection, records the rehearsal and live MIDI/input-meter windows side by side, and receives BlackHole input without changing the system default output.

```sh
artifacts/last-lighthouse/venv/bin/python tools/demo/run_validation.py \
  --obs --long-hold --output artifacts/last-lighthouse/hardware-validation
```

The default refuses to proceed without the named BlackHole device. It records its actual 16-channel CoreAudio input, relays those received samples into five live stereo/mono OBS feeds, and logs actual MIDI input. OBS writes a six-track MKV containing both live windows. ScreenCaptureKit sources are muted and excluded from all audio tracks, so they cannot duplicate the separately received stems. This follows OBS's [multiple audio track workflow](https://obsproject.com/kb/multiple-audio-track-recording-guide); channels and recording tracks are distinct.

The software fallback is explicitly weaker and requires no BlackHole or macOS screen-recording permission:

```sh
artifacts/last-lighthouse/venv/bin/python tools/demo/run_validation.py \
  --software --obs --output artifacts/last-lighthouse/software-rehearsal
```

This runs the production file decoders, mixer, transport, compositor and MIDI worker. Its audio comes from `Transport::render` written to a sixteen-channel WAV, rather than an independently received device input. OBS records native Qt widget snapshots updated about once a second, with an explicit software-evidence label, rather than the OS screen. Its MIDI receiver remains a real separate CoreMIDI endpoint. No software result is marked as hardware audio verified.

Use `--scenario routing`, `--scenario async`, `--scenario show`, or `--scenario long-hold` to run one exercise. `--long-hold` adds a five-minute actor wait. Use a **fresh** output directory for each run; evidence is never silently overwritten. `--buffer 128/256/512/1024/2048` repeats the same exercises at a chosen device buffer; software mode has no hardware buffer.

Every scenario writes a native-runner summary, JSONL transport telemetry, MIDI reception log, WAV, screenshots and a fail-closed `verification.json`. The show additionally writes a native `.savpack`, save/reopen round trip, OBS recording metadata/probe and relay logs. The suite's `validation-summary.json` succeeds only when the selected runner and analyses pass.

For the corresponding Windows ASIO, loopMIDI and OBS workflow, see
[Windows device-loopback rehearsal](WINDOWS-VALIDATION.md).

## Run the logger during a manual rehearsal

After the build/environment setup above, double-click **[Launch Lighthouse Logger.command](../../tools/demo/Launch%20Lighthouse%20Logger.command)** in Finder, or run from the repository root:

```sh
artifacts/last-lighthouse/venv/bin/python tools/demo/run_loopback.py
```

This opens **Lighthouse Loopback Recorder** with live sixteen-channel BlackHole input meters and the MIDI receipt log. It stays open while you operate SimpleAVShow. In the editor's Outputs dialog, select **Lighthouse MIDI Logger** as MIDI destination and **BlackHole 16ch** as audio output. The logger receives MIDI without echoing it back. Close its window or press Ctrl+C in the launcher's terminal to finish. Every launch uses a fresh timestamped directory under `artifacts/last-lighthouse/manual-loopback/`, containing `midi-received.jsonl` and `loopback-16ch.wav`. Audio is recorded continuously while open, including silence. Add `--obs-relay` to feed the five existing OBS stem inputs too.

For a manual rehearsal, open `last-lighthouse.savshow`, choose at least eight output channels, press Play and Continue at the three waits. Pause during the sea, then Stop All. Open the line-check and async-check projects separately. Guide/click sound should reach only their private feeds.

## Regenerate the original assets

All music, effects, projections and calibration signals are generated locally. The optional spoken guide uses the machine's `say` voice; without it the generator substitutes simple guide tones. No downloaded commercial show content is required. The generation code fixes its random seeds; encoded bytes can vary with FFmpeg and voice versions.

```sh
artifacts/last-lighthouse/venv/bin/python tools/demo/generate_lighthouse.py
```

This regenerates the three `.savshow` files and twenty real media files. `asset-manifest.json` stores codec metadata, sizes and SHA-256 hashes. To refresh the `.savpack`, run the native show-validation scenario, then copy its `last-lighthouse.savpack` back here. The package itself is created by the app's real package writer, not a hand-made ZIP.
