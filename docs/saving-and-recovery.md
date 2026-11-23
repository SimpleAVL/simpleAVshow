# 7. Saving and recovery

[Manual contents](README.md) · Previous: [6. Phone remote](remote-control.md) · Next: [8. Agent collaboration](agent-collaboration.md)

Understanding the difference between a **working project** and a **portable package** prevents the most common disaster in live production: arriving at the theatre and discovering your media files are still back on your laptop.

---

## Choose the right file type

| File extension | What's inside | When to use it |
| --- | --- | --- |
| **`.savshow`** | Timeline, tracks, clips, automation, cues, project settings, and **links** to your media files. | Everyday editing on one computer (or inside one self-contained folder). |
| **`.savpack`** | The show **plus every file in your Media library** (even unused items). | Moving a show to another computer, handing off to a colleague, or archiving a full backup. |
| **`.savshow.autosave`** | Automatic crash-recovery snapshot of the project (still references external media). | Recovering recent edits after an unexpected crash—not a backup of media files. |

Before saving or exporting:
- Click **Stop All**, leave **Show mode**, and wait for any media preparation to finish.
- Commit any pending changes in the inspector with **Apply clip** (for multiline text), **Apply cue** (for MIDI), or **Apply curve** (for automation table edits).

---

## Save a working project (`.savshow`)

Choose **File → Save** (or **Save as…** for a new copy). If the project is untitled, SimpleAVShow prompts for a filename and adds `.savshow` automatically. Writes are atomic so a power cut won't leave a half-written file. Watch the status bar for **Saved …** and confirm the unsaved dot next to the title disappears.

### Where your media lives after Save

- **External imported files stay where they are.** SimpleAVShow stores relative paths to them whenever possible so you can move a whole project folder together, but **Save** and **Save as…** do **not** copy external media into one folder.
- **Generated demo media and extracted package media** get written into a sibling `.media` folder named after your project:

```text
OpeningScene.savshow
OpeningScene.media/
    ...media files...
```

Always keep that `.media` folder right beside its `.savshow` file (along with any external media in their relative folders). While copying package media during a first Save, a progress dialog locks the editor until copying finishes—make sure your drive has enough free space. *(Note: pressing Undo after saving does not revert saved media back to temporary package paths.)*

---

## Export a portable package (`.savpack`)

Whenever you move a show to a show PC or make an archive backup, export a `.savpack`:

1. Click **Stop All**, leave **Show mode**, and resolve any **OFFLINE** items in your Media library.
2. Choose **File → Export package…**.
3. Pick a destination and `.savpack` filename.
4. Wait for the progress dialog to finish and confirm **Exported …** in the status bar.
5. Copy the `.savpack` to the target computer and **test-open it there** before relying on it for a show.

### What goes into a `.savpack`

- Your **current in-memory edits** (even if you haven't clicked Save on your `.savshow` yet). Exporting does not overwrite, rename, or clear the unsaved state of your open `.savshow` project.
- **Every item in the Media library** (including unused assets), stored as original uncompressed bytes inside a ZIP/Zip64 archive (`project.savshow` + `media/`). Duplicate references to the same file are stored once, and local audio caches are excluded—so file size is roughly the sum of your source media.
- Because *every* library item is packed, even an unused **OFFLINE** asset will block export until you restore or remove it.

---

## Open a package on another computer

Open a `.savpack` via **File → Open…** or by dragging it onto the editor window. SimpleAVShow validates the archive, extracts an editable working copy into temporary storage, and prepares any needed audio caches. Leave enough free disk space for the extracted originals, audio caches, and your final saved copy.

> **Important:** An opened `.savpack` runs from a **temporary folder** and **does not autosave** until you perform your first normal **File → Save** (which creates a permanent `.savshow` + `.media/` folder on disk). Always save a permanent copy right after opening a package on the show machine.

After opening on a new computer:
- **Re-select hardware in Outputs:** Track routing travels inside the package, but audio driver/interface, MIDI destination, and audience display are per-computer settings. Reconfigure and physically verify all three.
- **Check titles:** Text renders with the new computer's system Arial font, so review framing and line wraps in **PROGRAM**.
- **Keep packages untouched:** While a standard ZIP tool can peek inside a `.savpack`, SimpleAVShow validates its own package structure and may reject manually edited or re-zipped archives.

---

## Recover from an autosave

SimpleAVShow writes a recovery snapshot every minute whenever a project has unsaved changes and isn't busy preparing media:

- **Saved projects:** `<project>.savshow.autosave` right next to your project file.
- **Untitled projects:** `Untitled.savshow.autosave` inside the app's data folder (once every asset has a file path on disk—unsaved generated demo media cannot autosave until materialized by a normal Save).
- **Opened packages:** Start autosaving only after your first normal Save.

If SimpleAVShow finds a newer autosave when you open a project (or an untitled autosave at launch), it offers **Recover autosave** or **Open saved show**:

- **Recover autosave** loads the snapshot as unsaved changes on top of the original project. Clicking **Save** updates that original file; use **Save as…** if you want to keep both versions.
- You can also open a `.savshow.autosave` file directly with **File → Open…**—it loads as recovery data rather than renaming your project to `.autosave.savshow`.
- Opening the saved version does not immediately wipe the newer autosave, though a later save or autosave will overwrite/clear it. Saving normally or choosing to discard unsaved changes cleans up the sidecar automatically.

Remember that autosaves only protect your project edits—they cannot bring back deleted or moved media files.

---

## Relink missing media

If media files were moved or renamed outside SimpleAVShow, the project still opens: missing items appear as **OFFLINE** in the **Media** tab, and their timeline clips stay in place (playing silent/blank until restored). An opening warning lists what's missing and offers a **Locate folder…** button.

To relink manually at any time:

1. Either put the missing files back in their original location and reopen, or choose **File → Relink missing media…**.
2. Pick a folder containing the files. SimpleAVShow scans that folder and all subfolders by **exact filename (case-insensitive)**.
3. Wait for matched files to prepare, and read the summary for any remaining errors.
4. Inspect every relinked clip—verify length, timing, linked audio, and picture framing.
5. **Save** the fixed show and rehearse the affected cues.

> **Caution:** Relinking matches by **filename only**, not file contents. Avoid pointing it at a massive folder that might hold different takes with the same filename (like multiple `intro.wav` files), and rename replacement files to match if needed.

Offline media used on the timeline blocks starting a new **Play**. (If a file drops out mid-show while resuming a hold or pause, SimpleAVShow warns you and tries to keep the rest of the show running—always resolve missing media before curtain.)

---

## Protect your working files

- **Don't edit or overwrite active media on disk.** Direct 48 kHz WAV playback streams straight from your original file—modifying or replacing it while open can halt playback.
- **Keep a verified `.savpack` backup** before major tech-week edits or moving to the venue. Treat autosave as crash insurance, not a backup strategy.
