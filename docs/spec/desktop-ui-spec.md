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
| Decisions | Only when the core requires one | Full plan review before every build |
| Settings used | Defaults from Settings | Defaults, overridable per job |

- Switch via View menu, keyboard shortcut, or a "Open in Full mode" link on any Simple-mode result.
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

**Window states**

1. Idle: drop target with a one-line hint.
2. Running: progress ring, file name, throughput, ETA, Cancel. Further drops queue.
3. Done: result card with savings (e.g. "4.2 GB → 2.3 GB, 45% smaller"), warning count, Reveal in Finder/Explorer, Open in Full mode.
4. Error: what failed, which file, Retry / Open in Full mode.

A system notification is sent when a job finishes while the window is not focused.

## Full mode window

One window: archive browser on the left, Inspector on the right, archive summary and Jobs in the status bar. All commands live in the native menu bar (File, Archive, View, Help) rather than a toolbar. Simple mode has a View menu with Switch to Full Mode (Ctrl/Cmd+Shift+M); in Full mode the same shortcut switches back.

&#91;embedded content: Full mode main window · browser, inspector, status bar\]

Selecting a row fills the Inspector; a multi-mono set is one row that expands to its channel files. With no archive open, the browser area shows a drop zone and recent archives.

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
- Tree view (the default) and flat list toggle; sorting on every column; column visibility saved.
- Search field filters by name; filter chips by method and by "restorable only".
- Third-party entries the core can't read (unsupported method, symlinks) are shown greyed with the reason as a tooltip.
- The table model is virtualized, so 100,000+ entries scroll smoothly.

**Archive summary** (status bar, expandable): created by (app version from the archive comment, or "unknown tool"), total size → packed size and % saved, entry counts per method, Zip64 yes/no.

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

## Creating archives

In Full mode every build goes through a plan review, so the user sees exactly what will happen before any data is written.

1. **Add**: New Archive asks whether to create a blank archive (choose a location; an empty zip opens, then drag in files/folders or use Add) or to compress a folder (choose the folder, then review the plan and choose output name and location).
2. **Plan review** (sheet over the window): one row per planned entry with output name, chosen method, and reason ("FLAC: 24-bit, 6 ch", "Stored: 32-bit float", "Deflate"). Totals at the top: file count, input size, entries per method.
3. **Issues panel** in the same sheet:
   - Collisions (blocking): each conflict group with Rename / Skip / Store unconverted controls; Build stays disabled until all are resolved.
   - Skipped symlinks (warning) and FLAC fallbacks (info), listed with paths.
4. **Options** (collapsed by default): FLAC on/off, deflate level, FLAC level, threads.
5. **Build**: progress per file and overall, throughput (MB/s), ETA, Cancel. Runs in the background job queue; the window stays usable.
6. **Result**: savings, per-method breakdown, warnings, Reveal, Open archive.

Adding files to an archive that is already open uses the same review sheet, then the editor's rewrite-and-swap.

## Extracting

Extract All, Extract Selected, or drag entries out to Finder/Explorer; all three use the same extraction plan.

- **Options dialog** (Extract All/Selected): destination, "Restore original audio" (default on) vs keep FLAC, include generated readme (default off), overwrite policy (Ask / Skip / Replace).
- **Drag-out** uses the saved defaults without a dialog.
- **Pre-flight**: the extraction plan's warnings and errors (unsafe paths, symlinks, incomplete multi-mono groups, case-only duplicates) are shown before writing; existing files prompt per the overwrite policy.
- **Open with default app**: double-click extracts that one entry (restored if audio) to a temp folder and opens it.
- **Result**: files written, restored and hash-verified count, any failures with reasons; hash mismatch is shown as an error with the file name and the partial output already deleted.

## Verify

Verify checks an archive's integrity without writing any files: CRC32 for every entry, plus full restore-in-memory and SHA-256 check for every restorable FLAC.

- Run on the whole archive or a selection; progress and Cancel as for builds.
- Results per entry: OK / CRC mismatch / hash mismatch / incomplete group / unreadable, shown as a column and in the Inspector.
- Summary: "1,204 entries verified, 0 errors" with the time taken.
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
| Integration | Associate with .zip | Off (offered, never forced) |
| Updates | Check automatically | On |

"Copy as CLI command" (on the plan review and Settings) produces the equivalent CLI invocation for the current options.

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

- **Jobs**: one background queue for builds, extracts, verifies and edits; a Jobs panel lists running and finished jobs with Cancel and Reveal. Closing the window while a job runs asks whether to cancel or keep running.
- **Keyboard shortcuts**:

| Action | macOS | Windows / Linux |
| --- | --- | --- |
| New archive | ⌘N | Ctrl+N |
| Open archive | ⌘O | Ctrl+O |
| Add files | ⌘⇧A | Ctrl+Shift+A |
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

- **Unit**: Simple-mode drop rule (zips only / mixed / none), output naming and `  2 ` suffixing, multi-mono row grouping, timecode formatting, Settings defaults, "Copy as CLI command" output.
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
