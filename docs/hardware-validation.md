# Show-machine validation checklist

[Manual contents](README.md) · [5. Running a show](running-a-show.md) · [9. Reference and troubleshooting](reference-and-troubleshooting.md)

Automated tests confirm software logic, not how your specific audio interface, ASIO driver, MIDI bridge, and projector behave together. Run through this checklist on your actual show computer (typically Windows 11) before your first live performance.

---

## 1. Audio interface and physical routing

- [ ] **ASIO driver & persistence:** Open **Outputs**, check **Show driver and latency settings**, choose your interface's native **ASIO** driver, and open all channels your show needs. Quit, relaunch, and verify pressing **Play** restores the exact same device and channel count.
- [ ] **Channel isolation:** Place distinct, time-aligned test stems on **Main audio** (outputs `1/2`), **Guide** (`3/Off`), and **Click** (`4/Off`). *(With one leg Off, mono stems play at unity gain and stereo stems sum automatically—no panning trick required.)*
- [ ] **Physical verification:** Listen to and/or record every physical output. Confirm **Guide** and **Click** never bleed into `1/2`, unused channels stay completely silent, and aligned impulses stay in sync over a long run.
- [ ] **Buffer stability:** Repeat playback at **128, 256, 512, and 1024 samples**. Pick a buffer that runs cleanly with zero **underruns**, and note the confirmed settings.

## 2. Holds, pauses, async media, and Stop All

- [ ] **Long hold + post-hold fade:** Hold a looping async ambient bed for **five minutes**, then press **Continue** into an **After hold** fade. Confirm the loop stays smooth throughout the hold and begins fading only after **Continue**.
- [ ] **Async one-shot:** Repeat with a short non-looping async clip that finishes during the hold; verify it stays finished after **Continue**.
- [ ] **Async pause & seek:** Pause inside an active async clip, seek *inside* its interval (source phase should stay unbroken), then seek *outside* or directly to its end (voice should stop cleanly).
- [ ] **Stop All everywhere:** Trigger **Stop All** from Playing, Holding, and Paused states. Confirm every internal audio and video voice stops immediately, and verify your plan for silencing external MIDI gear separately.

## 3. MIDI cues and audience picture

- [ ] **Real MIDI delivery:** Select your real MIDI port/bridge and verify every message type your show uses (Note On/Off, CC, Program Change—checking `1–128` vs. `0–127` numbering—Pitch Bend, and SysEx). Confirm automatic reconnection on **Play**, and confirm unplugging the port raises an error without falling back to another port.
- [ ] **Cue timing rules:** Confirm arrival and hold-resume messages each fire exactly once, seeking sends nothing, backward seeks rearm cues cleanly, and same-time or `00:00:00:00` holds behave as expected.
- [ ] **Audience display:** Open fullscreen program output on the real projector/monitor. Compare framing against **PROGRAM**, confirm the picture freezes cleanly at holds, and verify resuming playback.
- [ ] **Physical sync offset:** Measure actual A/V and MIDI timing offset with a camera or capture setup, since driver, projector, and MIDI bridge latency are not automatically compensated.
- [ ] **Real-media stress test:** Rehearse your heaviest layered video section (for example, four simultaneous 1080p24 layers in your final codecs) for a full run while watching CPU, memory, **video waits**, and **underruns**.

## 4. Show mode, files, and fault recovery

- [ ] **Show mode lock:** Enter **Show mode** and confirm edits and timeline seeks are blocked while **Space** and **double-Escape** (plus the **Stop All** button and phone remote, if used) work reliably.
- [ ] **File transfer & recovery:** Save, reopen, move a show with its media (or round-trip via `.savpack`), and verify opening a `.savshow.autosave` sidecar works cleanly.
- [ ] **Device disconnect drill:** Unplug the audio interface mid-playback and verify the show halts cleanly. Press **Play** while still unplugged and confirm it stays blocked (no silent switch to built-in speakers). Reconnect, click **Refresh devices** (or pick a backup interface after **Stop All** and leaving Show mode), and verify recovery. Repeat with a MIDI disconnect so you recognize the warning banner.
- [ ] **Log your baseline:** Record the SimpleAVShow version (`Help → About SimpleAVShow`), OS build, ASIO driver version, buffer size, and health-indicator counts from your clean full rehearsal.
