# 9. Phone remote

A phone or tablet on the same network can run playback from its web browser. Nothing needs to be installed on the phone. The remote shows the clock, state, next cue, time left, the current hold and any playback problem. It has two buttons: **Play / Pause / Continue** and **Stop All**.

## Connect a phone

1. Open **Project → Phone remote…**, or click **Remote · off** in the top bar.
2. Check **Enable phone remote**. The first time, Windows and macOS may ask whether SimpleAVShow may accept incoming network connections; allow it for the network you use for the show.
3. Open one of the links under **Open on the phone** in the phone's browser, for example `http://192.168.1.20:48124/k7m2-x9pq-4hnt/`. **Copy link** puts the first link on the clipboard so you can send it to the phone. A computer with several network connections lists one link per address; use the one on the phone's network.
4. Bookmark the page or add it to the home screen. The top-bar button changes to **Remote · 1 connected** while the page is open.

The link stays the same between sessions, including the port, so a bookmark keeps working. The remote itself is off after each launch until you enable it again. Choose another port before enabling if 48124 is in use.

## Operate from the phone

The large button follows the editor's Play button: **Play** when stopped or paused, **Pause** while playing, and **Continue** at a hold. Each tap states what it means, so a tap that arrives late does nothing instead of the opposite. For example, Pause pressed just as a hold arrives does not continue past the hold, and a second Play tap does not pause the show.

**Stop All** needs two taps within three seconds: the first arms it, the second stops all playback including async audio and video. This matches pressing Escape twice in Show mode.

The remote works in Show mode, like Space and the Stop All button. It cannot seek, edit, save, or change outputs. Play from the phone uses the same checks as the editor: missing media, unavailable audio, or a missing MIDI destination are reported on the phone and in the editor. The editor's status bar names each remote action and the phone's address.

If the phone loses its connection, the page dims, shows **Connection lost**, and disables both buttons until it reconnects. Keep the phone charged, set its screen not to lock during the show, and keep the editor's keyboard and mouse within reach as the primary control.

## Security

Anyone who has the link and is on the same network can control playback. The remote uses plain HTTP, so use a show network you trust rather than public Wi-Fi.

- **Change link…** creates a new link. Phones using the old link show **This link is no longer valid** until they open the new one.
- Uncheck **Enable phone remote** to stop accepting connections immediately.

## Troubleshooting

- **The page does not load:** the phone must be on the same network as the computer, not on mobile data or a guest network that isolates devices. Check that the remote is enabled and that the firewall allows SimpleAVShow.
- **No network connection found:** connect the computer to the network, then reopen the dialog.
- **Cannot start the phone remote:** another program is using the port. Uncheck, choose another port, and enable again. Update the phone's bookmark.
- **This link is no longer valid:** the link was changed. Open the new link from the dialog.

[Back to the manual](README.md)
