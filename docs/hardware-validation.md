# Show-machine validation

Validate the Windows 11 PC, real interface, bridge, and display before a performance. Local tests do not establish physical driver behavior.

## Routing

- Choose the interface's native ASIO driver and open the needed channels.
- Place distinct aligned stems on main, guide, and click. Route main 1/2, guide 3/Off, click 4/Off; pan left-only mono routes left for unity gain.
- Inspect/record outputs: guide/click must not leak onto 1/2, unused channels must be silent, and impulses must stay aligned during a long run.
- Repeat at 128/256/512/1024 samples. Choose a stable buffer; inspect reported settings and underruns.

## Holds and seeks

- Hold looping ambience for five minutes, then resume into a fade. The source continues through the hold; the fade starts on resume.
- Repeat with a one-shot ending during the hold; it must stay finished after resume.
- Pause inside ambience, seek inside its clip, then outside or exactly to its end. Expect uninterrupted source phase inside and silence outside.
- Stop All from Playing, Held, and Paused. All internal audio must stop; external MIDI receivers are controlled separately.

## MIDI and picture

- Connect the real bridge port and verify required Note/CC/Program Change/SysEx messages.
- Confirm arrival/resume each fire once, seeks send nothing, rehearsal backward seeks rearm cues, and same-time/time-zero holds behave correctly.
- Select the display; compare program and preview, hold the frame, and resume.
- Measure physical audio/video/MIDI offsets with a capture setup. Device/display/bridge buffering is not compensated yet.
- Run four-layer 1080p24 with actual formats. Inspect CPU/memory, video waits, and underruns over a full rehearsal.

## Workflow and failures

- Show mode must block edits/seeks while Space and Escape still work.
- Save/reopen, move media with the show, and open the autosave sidecar.
- Disconnect audio: the show must halt and require explicit reconnect without falling back to another device. Inspect MIDI disconnect errors too.
- Record app/driver versions and health indicators during long holds, repeated resumes, seeks, and stopped output changes.
