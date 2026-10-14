# Windows device-loopback rehearsal

Build the desktop app with `-DSAV_BUILD_DEMO_VALIDATION=ON`, then build the
`lighthouse_validation` target in Release. Run from Tim's signed-in desktop:
SSH service sessions cannot host the editor or OBS capture windows. Qt's `bin/`
and FFmpeg's `bin/` must be on `PATH`.

Install Python dependencies from `tools/demo/requirements.txt`, OBS Studio,
loopMIDI and VB-Audio Matrix from their official vendors. Driver installation
can require a restart; arrange it with the operator first. In loopMIDI create
one port named `lighthouse`, and keep loopMIDI running in this desktop session.

In Matrix enable VASIO64A and VASIO64B at 48 kHz, 512 samples, with the internal
clock. Route VASIO64A inputs 1–16 to VASIO64B outputs 1–16 individually at 0 dB.
Keep all other crosspoints disabled. The application writes to A; a separate
PortAudio process reads B. The receiver records sixteen channels without
remapping, mixing, or echoing audio/MIDI back to the application.

Enable OBS's authenticated local WebSocket server (default port 4455). The
client reads its local configuration; no credential belongs in a report or
repository. OBS creates a separate **Lighthouse validation** profile and scene
collection. It records the real editor and independent input meters/MIDI logger
side by side, plus six audio tracks: full mix, score, guide, click, sea, effects.
Desktop/microphone inputs are excluded; only independently received stems feed
these tracks. No network-facing firewall exception is needed.

```powershell
python tools/demo/run_validation.py --api asio `
  --device 'VB-Matrix VASIO-64A' --input-device 'VB-Matrix VASIO-64B' `
  --midi-port lighthouse --virtual-device --obs --long-hold `
  --output artifacts/last-lighthouse/windows-rehearsal
```

Use a fresh output directory. `--scenario routing`, `async`, `show`, or
`long-hold` runs one exercise; add `--obs` when recording `show`. The ASIO-enabled
PortAudio wheel is selected through `SD_ENABLE_ASIO=1` before import. The native
runner rejects an unavailable audio API instead of falling back to another.
Reports record the **actual** driver buffer and channel count.

For ASIO4ALL select only the desired VBMatrix WDM output in its control panel,
then enable the matching Matrix VAIO slot and route its input channels to
VASIO64B. Substitute `--device 'ASIO4ALL v2'`. Its channel order follows the
selected WDM endpoints; repeat the frequency line check after changing them.
ASIO4ALL through virtual WDM endpoints exercises the ASIO/WDM path, without
establishing physical audio-interface compatibility.

A passing `verification.json` establishes device loopback, channel isolation,
MIDI reception and the selected transport behavior. `--virtual-device` keeps
`hardware_audio_verified` false. Physical ASIO interfaces, analog cabling,
external MIDI equipment and production latency still require a separate test
with that equipment connected. Autologon configuration is separate from this
workflow and is not verified by a rehearsal.
