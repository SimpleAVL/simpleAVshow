# 6. Phone remote

[Manual contents](README.md) · Previous: [5. Running a show](running-a-show.md) · Next: [7. Saving and recovery](saving-and-recovery.md)

The phone remote lets you run playback from any phone or tablet browser on the same local network—no app to install. It displays the live show clock, transport state, next cue, time remaining, active hold name, and any playback warnings, alongside two large buttons: **Play / Pause / Continue** and **Stop All**.

---

## Connect a phone or tablet

1. Open **Project → Phone remote…**, or click **Remote · off** in the top bar.
2. Check **Enable phone remote**.
   - The first time you turn it on, Windows or macOS may ask whether SimpleAVShow can accept incoming network connections—allow it for your show network.
3. Under **Open on the phone**, open one of the listed links in your phone's browser (for example, `http://192.168.1.20:48124/k7m2-x9pq-4hnt/`).
   - Click **Copy link** to copy the first link and send it to your device.
   - If your computer has multiple network interfaces (such as Wi-Fi and Ethernet), SimpleAVShow lists one link per address—use the one on the same network as the phone.
4. Bookmark the page or add it to your phone's home screen. While the page is open, the editor's top bar updates to **Remote · 1 connected**.

> **Good to know:** Your link and port stay the same across restarts so your phone bookmark keeps working. For safety, the remote always starts **off** when you launch SimpleAVShow until you turn it back on. If port `48124` is already taken by another program, pick a different port before enabling.

---

## Run the show from your phone

- **Smart primary button:** The main button mirrors the editor—showing **Play** when stopped or paused, **Pause** while playing, and **Continue** at a hold. Every tap sends your *intended action* rather than a blind toggle, so network lag can't invert what you meant: pressing **Pause** a split second after a hold arrives won't accidentally **Continue** past the hold, and double-tapping **Play** won't pause the show.
- **Two-tap Stop All:** Stopping requires **two taps within three seconds**—the first arms the button, the second silences all internal audio and video (including async tracks), matching the double-Escape safeguard in Show mode.
- **Works inside Show mode:** The remote stays active in Show mode just like **Space** and the **Stop All** button, and obeys the exact same safety rules. It cannot seek, edit, save, or change outputs. If a missing file, audio device, or MIDI port blocks **Play**, both the phone and the editor report the error. Every remote tap is also logged in the editor's status bar along with the phone's IP address.
- **Drop-out protection:** If the phone loses Wi-Fi or the link drops, the page dims, shows **Connection lost**, and disables both buttons until the connection returns.

> **Show-day tip:** Keep the phone plugged in or fully charged, turn off its auto-lock screen timer during the performance, and keep the show computer's keyboard and mouse within easy reach as your primary control.

---

## Security and network safety

Because the remote uses plain **HTTP** over your local network, **anyone on the same network who has the URL can control playback**. Only enable it on a private, trusted show network—never on public or venue-guest Wi-Fi.

- **Revoke an old link:** Click **Change link…** to generate a brand-new URL. Any phone holding the old link immediately shows **This link is no longer valid** until it opens the new one.
- **Shut it off immediately:** Uncheck **Enable phone remote** to stop accepting connections right away.

---

## Troubleshooting the remote

| Problem | What to check |
| --- | --- |
| **Page won't load on the phone** | Make sure the phone is on the **same local network** as the computer—not cellular data or a guest Wi-Fi with client isolation enabled. Confirm the remote is enabled and your OS firewall allows SimpleAVShow. |
| **"No network connection found"** | Connect the computer to Wi-Fi or Ethernet, then reopen the Phone remote dialog. |
| **"Cannot start the phone remote"** | Another program is using the configured port. Uncheck **Enable phone remote**, choose a different port number, re-enable it, and update your phone's bookmark. |
| **"This link is no longer valid"** | The link was rotated with **Change link…**. Open the new URL shown in the Phone remote dialog. |
