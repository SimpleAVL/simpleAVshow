# Manual screenshots

Developer notes for the [user manual](../docs/README.md) screenshots in `docs/screenshots/`.

Captured on 13 October 2026 from the native Qt widgets rebuilt at commit `460ef1d`. These are actual application renders, not mockups. The capture used macOS, the application's Fusion style/palette, an isolated settings directory, and Qt's offscreen platform. The desktop editor images are 1600 × 1100; dialog/control images use their own dimensions. No physical audio output or MIDI messages were started for capture.

Offscreen capture exposes a virtual 800 × 800 display in Outputs. The audio list comes from the capture host. These images illustrate controls and state, not validation of physical routing or fullscreen display behavior. Menus, fonts, decimal separators, device names, and shortcut labels can differ on another platform.

| File | State illustrated |
| --- | --- |
| [01-editor-overview.png](../docs/screenshots/01-editor-overview.png) | Built-in demo at 6 s, Fit enabled, Media and track inspector visible. |
| [02-project-settings.png](../docs/screenshots/02-project-settings.png) | Demo project settings: 1920 × 1080, 24 fps, 70 s. |
| [03-track-routing.png](../docs/screenshots/03-track-routing.png) | Guide selected and routed through the mono-output-3 preset; right leg Off. This is an example edit, not the default route. |
| [04-picture-and-text.png](../docs/screenshots/04-picture-and-text.png) | Demo title clip selected, timing/text/picture properties visible. |
| [05-audio-automation.png](../docs/screenshots/05-audio-automation.png) | Demo ambience selected, Loop source and Volume keyframes visible. |
| [06-midi-cue.png](../docs/screenshots/06-midi-cue.png) | Example edit to the first demo hold: Program 5/channel 1 after Continue. No destination/test send. The stock demo has no MIDI messages. |
| [07-outputs.png](../docs/screenshots/07-outputs.png) | Outputs with driver/latency controls expanded and audience-display controls visible. |
| [08-show-preflight.png](../docs/screenshots/08-show-preflight.png) | Unsaved example show with MIDI unconfigured and program output closed. |
| [09-hold-playback.png](../docs/screenshots/09-hold-playback.png) | Unmodified demo rendered to its 12 s hold through the transport without a physical device; one async voice remains active. |
| [10-show-mode.png](../docs/screenshots/10-show-mode.png) | Same held transport with Show mode locked. |
| [11-automation-controls.png](../docs/screenshots/11-automation-controls.png) | Entire native automation group, including Apply curve and fade helpers normally reached by inspector scrolling. |

## Refreshing the screenshots

Rebuild the current app before capturing. For a basic overview, the app has a built-in screenshot mode:

```sh
# macOS, from the repository root:
QT_QPA_PLATFORM=offscreen build/simpleavshow.app/Contents/MacOS/simpleavshow \
  --demo --preview-time 6 --screenshot manual-overview.png

# Linux: substitute build/simpleavshow for the executable.
# Windows: run build\Release\simpleavshow.exe with the same arguments;
# set QT_QPA_PLATFORM=offscreen in the shell if a headless render is wanted.
```

This command captures the editor at its default size and exits. For the other states, open **File → Open demonstration**, follow the states listed above, and capture the native editor/dialog. Resize the editor and move splitters for readable panels; scroll the inspector/Outputs to include lower controls. Use a separate temporary demo copy and leave real equipment disconnected when capturing example MIDI edits.

When updating images, check every label at readable resolution, keep the same filenames/relative Markdown links, and update the capture date above. Review captures for user paths/private device names before committing. Do not use a hold screenshot to claim physical playback validation; use the [hardware-validation procedure](../docs/hardware-validation.md) for that.
