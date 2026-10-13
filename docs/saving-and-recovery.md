# 6. Saving and recovery

[Manual contents](README.md) · Previous: [Running a show](running-a-show.md) · Next: [Reference and troubleshooting](reference-and-troubleshooting.md)

## Choose the right file type

| File | Contains | Best use |
| --- | --- | --- |
| `.savshow` | Timeline, tracks, clips, curves, cues, project settings, and references to media. | Normal editable working project. |
| `.savpack` | Show plus **every Media library item**, including unused items. | Transfer/backup of a self-contained show. |
| `.savshow.autosave` | Recovery snapshot with media references. | Recover unsaved changes; not a media backup. |

Stop All and leave Show mode before saving/exporting. Wait until imports or other preparation finish. Apply pending MIDI/automation edits with **Apply cue / Apply curve**, and apply text with **Apply clip**, before saving.

## Save a normal project

Choose **File → Save**. An untitled show asks for a location; `.savshow` is added if omitted. **Save as…** writes to another location. The title's unsaved indicator clears after success; check the status bar for **Saved …** rather than assuming a dialog close means success.

The project file is written atomically. Media paths are stored relative to the project where possible, so a folder tree can move together. **Imported media stays at its original file location** during normal Save/Save as. Saving does not collect every imported file into one folder.

Generated demonstration media is materialized as PNG/WAV in a sibling folder based on the project filename. Saving an opened package also copies its extracted media into that sibling folder:

```text
OpeningScene.savshow
OpeningScene.media/
    ...media files...
```

Keep that `.media` folder with its project. For an ordinary project with other external sources, keep those files in their relative locations too. Save as does not automatically duplicate all externally imported media. Use Export package for a complete portable copy.

Copying package media can take time; a progress dialog keeps the editor responsive and locked until it completes. Leave enough free disk space for the new copy. Undo after a successful save retains the saved media locations; it does not intentionally restore temporary package paths.

## Export a portable package

1. Stop All, leave Show mode, and resolve missing library media.
2. Choose **File → Export package…**.
3. Choose a `.savpack` filename and confirm replacement only if intended.
4. Wait while the app collects the show/media. Verify **Exported …** in the status bar.
5. Copy the package to the destination computer and open it there before relying on the transfer.

Export includes **current unsaved timeline/settings edits**, all library sources, original imported media bytes, and generated PNG/WAV media. Multiple references to the same original source are stored once. Export does not save, rename, or clear the dirty state of the current `.savshow` project.

Because unused library items are included, a missing unused asset can prevent packaging even when timeline playback does not need it. Keep library sources available when preparing a portable copy.

The package is a ZIP/Zip64 archive containing `project.savshow` and `media/` files with generated portable names. Media is stored without additional compression, so the package is roughly the size of the included source media plus project data. Audio caches are local and are not included.

## Open a package on another computer

Use **File → Open…** or drop the package onto the editor. Opening extracts an editable copy into temporary storage, validates the archive, and prepares/reuses audio as necessary. Allow space for extracted originals, any converted audio caches, and a later permanent saved copy. Extraction checks available space, but you still need room for preparation and saving.

After opening, choose **Save** to keep a `.savshow` with a sibling `.media` folder, or **Export package…** to share another package. The extracted copy is temporary: autosave starts after the first normal save. Temporary extraction alone is not a permanent working copy.

Audio driver/device, MIDI destination, and program display are chosen on the receiving computer. Track channel routing travels with the show and can refer to channels that computer does not have. Reconfigure, physically verify routing, and rehearse. Text uses the destination system's Arial and can look different.

A ZIP utility can also extract a package for opening its `project.savshow`. The app's package reader expects the app's own archive structure; recompressed/modified archives may be rejected. Keep the original package intact for transfer.

## Recover an autosave

Changed projects autosave every minute when media preparation is not active:

- Saved projects use `<project>.savshow.autosave` beside the project.
- Untitled projects use `Untitled.savshow.autosave` in the app's data folder once every asset has a file path.
- Generated unsaved demo media cannot autosave until materialized by normal Save.
- Opened packages start autosaving after their first normal Save, because extraction is temporary.

Opening a project with a newer sidecar offers **Recover autosave** or **Open saved show**. Recovery loads the snapshot as unsaved changes to the original show; Save then replaces that original project's contents. You can use Save as to preserve the recovered version separately.

You can also open `.savshow.autosave` directly with File → Open. It is treated as recovery data, not as a new filename ending in `.autosave.savshow`. At startup, an untitled sidecar offers recovery or discard.

Opening the saved version does not immediately delete its newer sidecar; a subsequent autosave/save can replace or remove it. Successful saving removes relevant sidecars. Discarding unsaved changes removes the current recovery sidecar. Recovery does not restore deleted/moved source media: the snapshot still references files.

## Relink missing media

A show can open with missing/unreadable assets. They appear **OFFLINE** in Media; their clips keep their timing but are silent/blank until restored. The opening warning lists problems and offers **Locate folder…**.

1. Restore the original files, or choose **File → Relink missing media…**.
2. Select a folder containing replacements. The app searches that folder and its subfolders by source filename, ignoring case.
3. Wait for matched sources to load. Read the summary and any remaining errors.
4. Inspect affected clips and check source length, timing, linked sound, and picture.
5. Save the corrected show and rehearse the affected sections.

Filename matching is not a content identity check. Avoid folders containing multiple different files with the same name; use the intended originals and verify results. Missing files with different replacement names will not be found by this search. Restoring the original path and reopening is another recovery route.

Media used on the timeline can block a new Play when offline. During an already-live hold/pause resume, some media/MIDI problems can warn without blocking the rest of the show. Resolve them before a performance.

## Protect the working copy

Keep original media unchanged while the app uses it. Direct WAV playback still reads the original; editing or replacing it on disk can halt playback. Preserve your saved project, required external media, and a verified package backup. An autosave is protection for recent project edits, not a substitute for those files.
