# Desktop App UI Specification

Sep 30, 2026

## Purpose and scope

One Qt 6 Widgets desktop app for Linux, macOS and Windows with two modes: Simple (drop and go) and Full (browse, inspect, edit, verify). It is a presentation layer over the core C API; every behavior described here maps to an existing core operation (plan, build, extract, edit, verify).

- Companion to the main spec (Sections 6, 7, 12) and the zip module design doc.
- Also covers OS integration: Quick Look (macOS), preview handler (Windows), context menus, file association.
- The CLI is out of scope here; it is specified in the main spec, Section 12.1.

## Modes

The app opens in the mode last used; first launch opens Simple mode.

|  | Simple mode | Full mode |
| --- | --- | --- |
| Purpose | Compress or extract in one drop | Browse, inspect, edit, verify |
| Window | Small drop window | Main window with browser and inspector |
| Decisions | Only when the core requires one | Changes are staged and reviewed in the browser, then written in one step |
| Settings used | Defaults from Settings | Defaults, overridable per job |

- Switch via View menu, keyboard shortcut, or the **View Contents** button on a Simple-mode result.
- Both modes share one job queue, so a job started in one mode stays visible in the other.

## Simple mode

Drop files on the window (or the app icon) and the job starts immediately: zips are extracted, everything else is compressed. No dialogs unless the core requires a decision.

**Drop rule**

| Dropped | Action |
| --- | --- |
| Only zip archives | Extract each, restoring original audio and verifying hashes |
| Anything else, including a mix with zips | Compress everything into one archive; zips are added as files |

**Outputs**

| Job | Output | Location |
| --- | --- | --- |
| Compress one item | `<item name>.zip` | Next to the item |
| Compress several items | `<parent folder name>.zip` | Next to the items |
| Extract | Folder `<archive name>/`, or, when the archive holds exactly one top-level folder, that folder alone with no enclosing folder (numbered if the name is taken) | Next to the archive |

- Existing outputs are never overwritten: the app appends `  2 `, `  3 `… (`Recordings 2.zip`). This is the app naming its own output, not an in-archive collision, so it needs no decision.
- Output location can be changed in Settings to a fixed folder.

**When Simple mode stops to ask** (only what the core makes mandatory):

- Name collisions inside the archive (main spec 6.2): a compact resolution sheet, defaulting to "rename with suffix".
- Extraction: unsafe paths, incomplete multi-mono groups, hash mismatches are reported as errors on the result card.
- Skipped symlinks and FLAC fallbacks don't stop the job; they're listed on the result card.
- The "Leave out" settings apply to new archives and to files added to an archive (Simple mode uses them as saved).

**Window states**

1. Idle: drop target with a one-line hint.
2. Running: progress ring, file name, throughput, ETA, Cancel. Further drops queue.
3. Done: result card with savings (e.g. "4.2 GB → 2.3 GB, 45% smaller"), warning count, Reveal in Finder/Explorer, and after compressing View Contents (opens the archive in Full mode). Counts use proper singular and plural ("1 file extracted", "44 files extracted"). Cancel leaves no trace: a cancelled extraction removes everything it wrote (including its folders) and the card reads "Cancelled. Nothing was extracted from <archive>." (compression: "Cancelled. <archive> was not created.").
4. Error: what failed, which file, Retry.

A system notification is sent when a job finishes while the window is not focused.

## Full mode window

One window: archive browser on the left, Inspector on the right, archive summary in the status bar (with a progress bar while an extract or verify runs). All commands live in the native menu bar (File, Archive, View, Help) rather than a toolbar. Simple mode has a View menu with Switch to Full Mode (Ctrl/Cmd+Shift+M); in Full mode the same shortcut switches back.

&#91;embedded content: Full mode main window · browser, inspector, status bar\]

Selecting a row fills the Inspector; a multi-mono set is one row that expands to its channel files. With no archive open, the browser area shows a drop zone. Dropping one zip there opens it. Dropping anything else asks for a name and location, as New does (the default name follows the Simple-mode output rule, e.g. `<item>.zip` next to the items), then starts a new archive with those items added (see Building and editing archives).

## Archive browser

The center pane lists the open archive's entries, read from the central directory only, so archives of any size open instantly.

| Column | Content |
| --- | --- |
| Name | Entry name; folders expandable in tree view |
| Size | Uncompressed size |
| Packed | Stored size |
| Ratio | Packed / size, as % saved. For FLAC, size is the original audio file's, derived from the stored metadata (`zp_reader_flac_original_size`); a multi-mono set shows it on its group row. Blank where it cannot be known, and for folders, selections and totals containing such entries |
| Method | Store · Deflate · FLAC · FLAC multi-mono |
| Restores to | Original filename for converted audio (e.g. `take1.wav`), blank otherwise |
| Modified | Entry date |

- Converted audio carries a "restorable" badge.
- A multi-mono set is one row (`take2.wav — 16 channels`), expandable to its `_chNN.flac` members. Selecting, extracting, renaming or deleting the row acts on the whole group.
- A folder tree (the only view); sorting on every column; column visibility saved. Left on an entry or a collapsed folder selects the enclosing folder.
- Right-click menu: Extract Selected, Rename, Move To…, New Folder with Items, Delete (on a row), then New Folder, Add Files…, Add Folder….
- Search field filters by name; filter chips by method and by "restorable only". While any of them is active, every folder holding a match is expanded; clearing them restores the earlier expansion.
- Third-party entries the core can't read (unsupported method, symlinks) are shown greyed with the reason as a tooltip.
- The table model is virtualized, so 100,000+ entries scroll smoothly.

**Archive summary** (status bar, expandable): "Created by" with the app version from the archive comment (left out when unknown), total size → packed size and % saved, entry counts per method, Zip64 yes/no.

## Inspector

The right-hand panel shows everything known about the selected entry, read without extracting it (FLAC entries: metadata blocks only).

| Section | Shown for | Fields |
| --- | --- | --- |
| File | All entries | Name, sizes, ratio, method, CRC32, modified, Unix permissions |
| Audio | FLAC entries | Sample rate, bit depth, channels, duration, layout (standard / multi-mono / private) |
| Original | Restorable FLAC | Original filename and container (WAV, RF64, AIFF, CAF, Wave64), stored SHA-256, restorable with `flac -d` yes/no |
| Production metadata | Restorable FLAC | Mirrored tags: timecode (TIME\_REFERENCE shown as HH:MM:SS:FF using the iXML timecode rate, or HH:MM:SS.mmm if none), scene, take, tape, project, note, circled, originator |
| Tracks | Multichannel and multi-mono | Channel number, track name from iXML |
| Chunks | Restorable FLAC | Ordered chunk list of the original file: ID, size, before/after audio; unknown chunks marked |
| Verification | All entries | Last verify result and time, or "not verified" |

Multiple selection shows combined totals (count, size, packed, savings).

Field labels use a secondary text color: the text color blended up to 40% toward the background, as far as the result keeps a WCAG contrast ratio of at least 4.5:1 against the background. It follows the current light or dark palette and updates when the system appearance changes.

## Building and editing archives

Full mode works like a desktop archive manager (e.g. GNOME File Roller), except that nothing is written right away: changes are staged in the window, the browser shows the result they will have, and one Go action writes the archive. Creating a new archive and editing an existing one use the same staging model and differ only in their starting point and Go action.

### Sessions

The window holds one **session** at a time: a base archive (none for a new archive), a list of pending changes and per-session options.

| | New archive | Edit archive |
| --- | --- | --- |
| Start | File ▸ New (⌘N) asks for the archive's name and location (a save dialog; default `Archive.zip` in the last-used folder), then opens an empty session. Files or folders dropped on an empty window start a session without asking, written next to them by the Simple-mode rule (`<item>.zip`). The action bar shows the destination ("Save as *name* in *folder*…"); clicking it changes it. While the dropped items are scanned, the bar shows "Scanning files…" with an indeterminate progress bar. Nothing is written yet | File ▸ Open (⌘O), double-click a zip, or drop one zip on an empty window |
| Browser shows | Only the staged additions | The archive's entries with pending changes overlaid |
| Go action | **Create Archive** | **Apply Changes** (also **Save As…**) |
| Writes | A new zip at the destination | Rewrites the archive in place (temp file, then atomic swap), or writes a copy for Save As |
| Afterwards | The window opens the new archive as an Edit session with no pending changes | Same archive reopened, no pending changes |

- The window title shows the archive name and the platform's edited marker (macOS: dot in the close button; elsewhere: `*`) while there are pending changes. A new archive's title is its chosen name, marked as edited from the start.
- **Save As…** (⌘⇧S) on a new archive changes its target path, still without writing anything.
- If a file appears at a new archive's target path before Go, Create asks whether to replace it.
- Closing the window, quitting, starting New, or opening another archive with pending changes asks **Apply / Discard / Cancel** (new archives: **Create / Discard / Cancel**).
- A new archive with no additions is discarded silently.

### Staging changes

| Change | How |
| --- | --- |
| Add files / folders | Archive ▸ Add Files… (⌘⇧A) or Add Folder…, or drag from Finder/Explorer onto the browser. They go into the selected folder (or the selected entry's folder), or the folder or entry row they are dropped on; dropped on empty space, to the top level |
| New folder | Archive ▸ New Folder (⌘⇧N): an empty folder next to the selection (beside a selected folder, not inside it), or at the top level with nothing selected, named in place |
| New folder with items | Archive ▸ New Folder with Items (⌃⌘N; Ctrl+Alt+N elsewhere): a new folder next to the selection with the selected items moved into it, named in place |
| Rename | F2 / Enter on macOS, edited in place; works on folders, which renames everything below them. A multi-mono set is renamed through a dialog asking for the channel files' stem. A name containing `/` is refused (use Move To…) |
| Move | Drag rows onto a folder (into it), an entry (beside it) or empty space (top level); a folder cannot go into itself. Or Archive ▸ Move To…: pick an existing folder or type a new path. Stored as renames |
| Delete | Delete key; no confirmation, because it is only staged and can be undone |
| Replace | Drop a single file onto a file row with the same name and confirm "Replace *name*?" |

- Dropping a single zip onto an open archive asks **Add to This Archive** or **Open** (default Open).
- **Existing entries** (session option, Options popover; default Ask): what happens when an added file has the same archive path as an entry already there. Ask shows it as a collision in the Issues panel. Replace and Keep apply to every match. Replace if Newer replaces only where the source's modification time is later than the entry's and keeps the rest. Replacements are staged as Replace changes.
- **Undo / Redo** (⌘Z / ⌘⇧Z) step through pending changes. **Discard Changes** clears them all.
- Additions are planned in the background with the same planner and session options as a build. While they are planned, the rows show "Scanning…" and Go stays disabled.
- **Leave out** patterns (Settings defaults, overridable per session) apply when files are added. Changing them re-plans every pending addition.

### How pending changes look

| Row state | Display |
| --- | --- |
| Added | "New" badge, right-aligned in the Name column; Size from the source file; Packed blank; Method from the plan. For files that are not converted to FLAC, Store or Deflate is predicted by the build's own sampling (`zp_plan_predict_store`), so already-compressed media shows Store before anything is written |
| Renamed / moved | New name, with the old name in the tooltip and Inspector |
| Replaced | "Replaced" badge; columns show the new source |
| Deleted | Struck through and dimmed; View ▸ Show Deleted (on by default) hides them |
| Unchanged | As today |

- The Inspector shows the plan for an added entry (source path, method) instead of archive metadata. For a deleted or replaced entry it still shows the archive metadata.
- Extract, Open and Verify act on the archive as stored on disk: they ignore staged additions (and are disabled when the selection contains only additions), and still include entries staged for deletion.

### Action bar

A bar at the bottom of the browser, visible while the session has pending changes (always visible for a new archive):

- Summary: "+12 files (4.2 GB) · −3 · 2 renamed". New archive: file count, input size, entries per method.
- **Issues** button with a count, which opens the issues panel (a popover over the bar):
  - Collisions (blocking): each conflict group with Rename / Skip / Store unconverted controls. The rows involved get a warning badge in the browser.
  - Skipped symlinks and leave-out matches (warning), FLAC fallbacks (info), listed with paths.
- **Options** button, which opens a popover with FLAC on/off, FLAC level, deflate level, Existing entries and the Leave out controls. These are per-session overrides of Settings and are not saved. OK applies them (an invalid Leave out pattern is shown in the popover and keeps it open); Cancel, Esc or clicking elsewhere discards them. Threads stay a Settings value (one thread pool for the app).
- **Discard** (all staged changes; for a new archive it reads **Cancel** and leaves it without creating it, asking first if files were added, then shows the start page) and the Go button (**Create Archive** / **Apply Changes**, ⌘S). Go is disabled while blocking issues remain or additions are still being planned.
- While the archive is written, the bar shows the summary, a progress bar and **Cancel** instead of the other controls.

### Go

1. Create and Apply need no dialog. For an Edit session, **Save As…** (⌘⇧S) asks for a path and writes a copy, leaving the original untouched.
2. The job runs in the background queue, with progress and Cancel in the action bar. A job cancelled while still queued is reported as cancelled. The session's browser is read-only while its job runs; other archives can be opened in Simple mode or by other jobs.
3. On success: a result card in the action bar (savings, per-method breakdown, warnings, Reveal). Added files that changed on disk after they were added are left out of the written archive; the card is marked with a warning and a dialog lists them, and the session reopens the written archive. With "verify after build" on, Verify runs on the written archive.
4. On failure or Cancel: the original archive is untouched, and the pending changes stay staged so the user can retry.

### Implementation model

- The session (`desktop/gui/session`, no widgets, unit-tested) owns an `ArchiveEditor` opened on the base archive (an empty zip in memory for a new archive) with the session's planner options, and a history of changes. A change is one undo step: a list of operations that address entries by key, not index. The key is the entry's source (`archive:<original name>` for base entries, the file path for additions) plus its current name. Operations: add(paths, folder), new folder, rename, rename folder, remove, remove folder, replace(entry, path), resolve(entry, Rename / Skip / Store unconverted).
- A new change is applied to the live editor. Undo, a change of options, or a failed change rebuilds the editor from the base archive and replays the applied history. A change that no longer applies during a replay (for example, its source file was deleted) is dropped and reported.
- A rename or folder rename that would collide with another entry is refused (`ZP_NAME_COLLISION`) and leaves the session unchanged. Collisions from additions are left for the Issues panel unless the Existing entries option settles them.
- Replace is staged as remove + add into the entry's folder, so restorable audio is planned like any addition. Adding a folder that already exists in the archive merges into it.
- Row states come from comparing the merged plan with the base: a base entry still present under its original name is Unchanged, under another name Renamed; a base entry missing is Deleted, unless an addition took its name, which makes that row Replaced. Everything else is Added. The generated readme is Unchanged if the base has one, Added otherwise. So the browser always shows exactly what Go will write.
- Go commits the editor to `zp_stream_create_file(output)`. The editor and base stream are closed before `zp_stream_commit` swaps the file into place (zip module design, Section 10), then the session reopens on the output with an empty history. On failure the session keeps its changes.
- The session is not thread-safe. The GUI runs planning and commits on its job thread and does not touch the session meanwhile.

**Core additions:** `zp_input_set_prefix` (items placed under an archive folder), planner options on `zp_editor_open` (for additions), `zp_editor_resolve` (conflict decisions on an edit), build options and a build result on `zp_editor_commit` (temp folder; files left out because they changed), `base_folder` in `zp_extract_options_t` (extract relative to a folder).

## Extracting

Extract All, Extract Selected, or drag entries out to Finder/Explorer; all three use the same extraction plan. Selected items are extracted relative to the deepest folder holding all of them (`zp_extract_options_t.base_folder`): selecting `docs/b.txt` writes `b.txt`, selecting the folder `docs` writes `docs/…`. Cancelling an extraction removes the files and folders it created; files that already existed are left alone.

- **Options dialog** (Extract All/Selected): destination, "Restore original audio" (default on) vs keep FLAC, include generated readme (default off), overwrite policy (Ask / Skip / Replace).
- **Drag-out** uses the saved defaults without a dialog. The entries are extracted to a temporary folder only when the drop target asks for the files, so a drag that ends inside the browser (a move) extracts nothing. No events are processed during that extraction (a busy cursor shows instead).
- **Pre-flight**: the extraction plan's warnings and errors (unsafe paths, symlinks, incomplete multi-mono groups, case-only duplicates) are shown before writing; existing files prompt per the overwrite policy.
- **Result**: files written, restored and hash-verified count, any failures with reasons; hash mismatch is shown as an error with the file name and the partial output already deleted.

## Verify

Verify checks an archive's integrity without writing any files: CRC32 for every entry, plus full restore-in-memory and SHA-256 check for every restorable FLAC.

- Purpose: a test of the archive as it is on disk, for archives received from elsewhere, copied to other media, or before deleting the originals.
- Runs on the whole archive; progress in the status bar.
- Results per entry: OK / CRC mismatch / hash mismatch / incomplete group / unreadable, shown in the Inspector and in the Verified column (hidden by default; shown from the header's column menu).
- Summary: a dialog, "All 1,204 files intact", or a warning listing the damaged files.
- Optional setting: verify automatically after every build (off by default; it re-reads the whole archive).

## Settings

| Group | Setting | Default |
| --- | --- | --- |
| General | Start in | Last used mode |
| General | Simple mode output location | Next to source |
| Compression | FLAC conversion | On |
| Compression | FLAC level | 5 |
| Compression | Deflate level | 6 |
| Compression | Threads | All cores |
| Compression | Temp location (multi-mono spill) | System temp |
| Extraction | Restore original audio | On |
| Extraction | Include generated readme | Off |
| Extraction | Overwrite policy | Ask |
| Verification | Verify after build | Off |
| Leave out | System files (main spec 6.3.1, system preset) | On |
| Leave out | Waveform and analysis caches of audio software (DAW preset) | Off |
| Leave out | Custom name patterns (list: add, edit in place, remove) | Empty |
| Integration | Associate with .zip | Off (offered, never forced) |
| Updates | Check automatically | On |

Folder settings (Simple mode output location, temp location) are dropdowns: the default, the chosen folder, and "Choose Folder…".

Full mode's Options popover has the same "Leave out" controls as Settings, as a per-archive override that is not saved. Items left out are counted in the action bar and listed in its Issues popover ("Left out: …"). Settings refuses to close while a custom pattern is invalid.

## OS integration

Each platform gets a native previewer and context-menu entries; these are separate native components that call the core C API, not part of the Qt app.

| Feature | macOS | Windows | Linux |
| --- | --- | --- | --- |
| Preview without opening the app | Quick Look Preview Extension (Swift, `QLPreviewingController`) | Preview handler (C++ COM `IPreviewHandler`) | Optional: Nautilus/Dolphin previewer plugin |
| Compress / Extract from the file manager | Finder Quick Actions (Services) | Explorer context menu (`IExplorerCommand`) | Nautilus scripts, Dolphin service menu |
| Drop on app icon | Dock icon → Simple mode rule | Drop on shortcut / exe → Simple mode rule | `.desktop` `%F` → Simple mode rule |
| File association | Registered handler for `.zip`, not default unless chosen in Settings | Same, via "Open with" | Same, via MIME `application/zip` |

**Previewer content** (Quick Look and Windows preview handler):

- For a `.zip`: entry list (name, size, method), total savings, "created by" version, restorable audio count. Reads only the central directory, so it is instant for any size.
- For a `.flac` made by this app: audio format, original filename and container, mirrored production tags, track names, multi-mono group info.

**Platform notes**

- macOS: the extension is sandboxed and embedded in the `.app`; it links the core as a static universal (arm64 + x86\_64) library. Whether it takes precedence over the system's zip preview must be checked early.
- Windows 11: top-level context menu entries need package identity (a sparse MSIX package alongside the MSI). Without it, entries only appear under "Show more options".
- All previewers are read-only and never decode audio, to stay fast and within OS time limits.

## General

- **Jobs**: one background queue for builds, extracts, verifies and edits; Simple and Full mode show their own jobs (Simple: the progress page; Full: the action bar for writes, the status bar for extract and verify). Files sent from the file manager (Compress/Extract with Satchel, the Dock icon) are handled by the Simple window, opened alongside a visible Full window, which keeps its session; the app never quits on its own while Full mode has unwritten changes. Closing the window while a job runs asks whether to cancel or keep running.
- **Keyboard shortcuts**:

| Action | macOS | Windows / Linux |
| --- | --- | --- |
| New archive | ⌘N | Ctrl+N |
| Open archive | ⌘O | Ctrl+O |
| Add files | ⌘⇧A | Ctrl+Shift+A |
| New folder | ⌘⇧N | Ctrl+Shift+N |
| New folder with items | ⌃⌘N | Ctrl+Alt+N |
| Create archive / Apply changes | ⌘S | Ctrl+S |
| Save as | ⌘⇧S | Ctrl+Shift+S |
| Undo / Redo | ⌘Z / ⌘⇧Z | Ctrl+Z / Ctrl+Shift+Z |
| Extract selected | ⌘E | Ctrl+E |
| Verify | ⌘⇧V | Ctrl+Shift+V |
| Find | ⌘F | Ctrl+F |
| Switch mode | ⌘⇧M | Ctrl+Shift+M |

- **Accessibility**: full keyboard navigation, screen-reader names on all controls (Qt accessibility), no information conveyed by color alone.
- **Appearance**: follows the system light/dark setting; native platform style.
- **Localization**: all strings translatable (Qt Linguist); English at launch.
- **Updates**: Sparkle on macOS, WinSparkle on Windows, signed update feeds; Linux updates through the package or AppImage.
- **Performance targets**: open and list a 100,000-entry archive in under 1 s; UI never blocks during jobs.

## Dependencies

All are compatible with the dual license; Qt stays dynamically linked (main spec, Section 13).

| Dependency | Use | License |
| --- | --- | --- |
| Qt 6 Widgets | App UI | LGPL-3 |
| Sparkle | macOS updates | MIT |
| WinSparkle | Windows updates | MIT |
| Swift / Quick Look framework | macOS previewer | System |
| Windows Shell COM APIs | Preview handler, context menu | System |

## Testing

UI logic that decides anything lives outside the widgets so it can be unit-tested; the widgets themselves get a manual pass per release.

- **Unit**: Simple-mode drop rule (zips only / mixed / none), output naming and `  2 ` suffixing, multi-mono row grouping, timecode formatting, Settings defaults.
- **Integration**: each Full-mode flow (create with collisions, extract with every pre-flight error, verify with injected CRC and hash errors, edit) driven through the C API with the same fixtures as the core.
- **Automated UI smoke tests** (Qt Test): app launches in both modes and opens a 100,000-entry archive within the target.
- **Previewers**: Quick Look and Windows preview handler render zip and FLAC fixtures, including third-party zips and corrupt files, without crashing or timing out.
- **Context menus and icon drops** on each OS, including Windows 11 with and without the sparse package.
- **Manual pass per release per OS**: both modes, dark mode, screen reader, keyboard-only use.

## Open questions

- [ ] Simple mode with a mixed drop (zips plus other files): compress all, as specified, or ask once?
- [ ] Product name and icon, needed for the previewers, context-menu labels and readme.

## File-manager actions quit when clean

When a job started by a file-manager action ("Compress with Satchel", "Extract with Satchel", including the macOS Finder services and the `--compress`/`--extract` flags) finishes with status OK and no warnings or per-entry problems, and no other job is running and Full mode is not open, Satchel quits instead of showing the Done page. Any warning, failure, cancellation or changed source keeps the window open on the result page. Drag-and-drop onto the window never quits.
