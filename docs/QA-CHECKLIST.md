# Manual QA Checklist

Run before each release, on each OS the release targets. Automated tests cover the core; this list covers what they can't: widgets, OS integration, real filesystems and third-party tools. Tick each box per platform (macOS / Windows / Linux) and note the OS version.

## 1. Setup

- [ ] Test on the oldest supported OS version as well as the newest (icon, Quick Look and shell integration have differed between versions)
- [ ] Fresh user profile (no saved settings) and an upgraded profile with settings from the previous release
- [ ] Both light and dark appearance
- [ ] A volume with little free space, a case-insensitive filesystem, a case-sensitive one, and a network or virtual-machine shared folder

## 2. Test data

- [ ] PCM audio: WAV, RF64, AIFF, AIFF-C, CAF, Wave64
- [ ] Bit depths 8, 16, 24, 32 (integer), and 32-bit float (must not become FLAC)
- [ ] Mono, stereo, 6, 8, 9 and 16+ channels (multi-mono boundary at 8)
- [ ] Odd cases: zero-length data, odd byte count, truncated file, wrong declared data size, unknown chunks, huge `iXML`/`bext`, chunk over 16 MiB, sample rate above 1,048,575 Hz
- [ ] Already-compressed media: H.264 `.mov`/`.mp4`, MP3, AAC, JPEG, PNG, nested `.zip`
- [ ] Incompressible data (random bytes) and highly compressible data (zeros)
- [ ] Tree with deep nesting, empty folders, 100,000+ small files, a file over 4 GiB (Zip64), over 65,535 entries (Zip64)
- [ ] Names: Unicode (NFC and NFD forms of the same name), emoji, RTL, spaces at start/end, very long names and paths, characters illegal on Windows (`:` `?` `*`), reserved Windows names (`CON`, `NUL`), leading dot
- [ ] Case-only collisions (`Take.wav` and `take.wav`), and `take1.wav` next to `take1.flac`
- [ ] Symlinks (file, folder, dangling, loop), Windows junctions, macOS aliases
- [ ] Read-only files, files with no read permission, executable bits, files modified while being compressed, files deleted while being compressed
- [ ] Third-party zips: Info-ZIP, Archive Utility, 7-Zip, Windows Explorer, zips with symlinks, zips with `..` or absolute paths, zips with unsupported methods (bzip2, LZMA), encrypted zips, corrupt central directory, truncated zips, zips with a comment

## 3. Simple mode

- [ ] Drop one file, one folder, several items, nothing but zips, and a mix of zips and other files (mix compresses everything; zips are added as files)
- [ ] Output names: `<item>.zip`, `<parent folder>.zip`, numbered suffix when taken (`  2`, `  3`), never overwrites
- [ ] Output next to a read-only source folder, and with a fixed output folder that no longer exists
- [ ] Extract: single top-level folder is not double-wrapped; several top-level items go in a folder named after the archive; name taken
- [ ] Drops during a running job queue up; Cancel mid-job leaves no partial output
- [ ] Result card: savings, warning count, Reveal, Open in Full mode; error card with Retry
- [ ] Notification when a job finishes while the window is not focused
- [ ] Collision sheet during compress, defaulting to rename with suffix
- [ ] Drop on the Dock/taskbar icon, on the exe/shortcut, and via `.desktop` `%F`
- [ ] Mode persists across restart; first launch opens Simple mode

## 4. Full mode

- [ ] Open a 100,000-entry archive: lists in under 1 s, scrolls smoothly
- [ ] Tree and flat view; sort every column; column visibility saved; search; method and restorable filters
- [ ] Multi-mono set is one row, expands to channels; select, extract, rename, delete act on the whole group
- [ ] Greyed rows with tooltip for unreadable third-party entries
- [ ] Ratio blank where unknown, and for folders or selections containing such entries
- [ ] Inspector for each entry type: plain file, FLAC, restorable FLAC, multi-mono, folder, multi-selection
- [ ] Inspector labels readable in dark mode
- [ ] Timecode display with and without iXML rate
- [ ] New (⌘N) asks for name and location first; nothing is written until Create Archive; title shows the edited marker
- [ ] Drop files on the empty window: name dialog defaults to `<item>.zip` next to them; drop one zip: opens it
- [ ] Drop one zip on an open archive: Add to This Archive / Open
- [ ] Add Files, Add Folder, drop into a folder row; New Folder (next to the selection, top level without one, named in place); New Folder with Items; rename in place (file, folder, Added entry); rename a multi-mono set; Move To… (existing and new folder); Delete (no confirmation)
- [ ] Row states: New / Replaced badges, renamed in italics (tooltip shows the old name), deleted struck through; View ▸ Show Deleted
- [ ] Undo / Redo through several changes; Discard
- [ ] Drop a file onto an entry with the same name: Replace?
- [ ] Collisions: Issues popover with Rename / Skip (Remove for the archive's entry) / Store unconverted; Apply disabled until resolved
- [ ] Options popover: FLAC off, deflate 1/9, FLAC level 0/8, Existing entries (Ask, Replace, Replace if newer, Keep), Leave out; staged additions re-plan
- [ ] Apply rewrites in place; Save As writes a copy and switches to it; result card with Reveal
- [ ] Close the window / open another archive / New with staged changes: Apply / Discard / Cancel
- [ ] Apply with a failure midway (disk full, cancel) keeps the original intact and the changes staged
- [ ] Cancel a new archive (with and without added files) returns to the start page; Cancel in the action bar while writing, also while the write waits behind another job
- [ ] Extract / Verify with staged changes act on the archive on disk; double-click does nothing
- [ ] Archive open in the app is modified or deleted by another program
- [ ] Search and filters expand folders with matches; clearing restores the tree
- [ ] Extract Selected / drag-out of a file inside a folder gives just the file; of a folder gives the folder
- [ ] Verify: dialog "All N files intact", Verified column appears
- [ ] Cancel an extraction (Simple and Full): nothing left behind; message names the archive
- [ ] Singular/plural: "1 file extracted" vs "2 files extracted"
- [ ] While planning, Compress with Satchel from Finder compresses on its own and leaves the plan and Full window open
- [ ] Change an added file on disk before Create: the archive is written without it and a warning names it
- [ ] Verify whole archive and selection, with injected CRC error, hash mismatch, incomplete multi-mono group
- [ ] Verify after build setting

- [ ] Settings: Simple mode output and Temporary files dropdowns (default, chosen folder, Choose Folder…, cancel keeps the previous choice); Threads shows "All cores" in full; spin box text clear of the frame; Leave out tooltips wrap

## 5. Extraction

- [ ] Extract All, Extract Selected, drag-out to the file manager
- [ ] Restore original audio vs keep FLAC; byte-identical to the original (compare hashes) for every container type
- [ ] Original filename and case restored
- [ ] Readme option
- [ ] Overwrite policy: Ask, Skip, Replace, with existing files and read-only existing files
- [ ] Pre-flight warnings: unsafe paths, symlink entries, incomplete groups, case-only duplicates on a case-insensitive disk
- [ ] Hash mismatch deletes partial output and names the file
- [ ] Destination without write permission, on a full disk, with a path too long for Windows
- [ ] Cancel mid-extract leaves no partial files
- [ ] Extract with plain `unzip`, 7-Zip, Archive Utility and Explorer: FLAC entries present, readme explains how to restore
- [ ] `flac -d --keep-foreign-metadata` restores single-file layouts

## 6. Multiple jobs and windows

- [ ] Both modes share one job queue; a job started in one mode is visible in the other
- [ ] Closing the window with a running job asks to cancel or keep running
- [ ] Quit with a running job
- [ ] Several jobs at once writing to the same output folder or the same archive name
- [ ] Two jobs on the same source, and one job reading an archive another is rewriting
- [ ] Temp folder (multi-mono spill) on a full or missing volume; temp files cleaned up after cancel, failure and crash
- [ ] Settings changed while a job is running do not affect it

## 7. File-manager integration

- [ ] macOS Finder services: Compress and Extract with one item, many items, and mixed selections
- [ ] Windows Explorer context menu, with and without the sparse package (Windows 11 top-level vs "Show more options")
- [ ] Linux Nautilus scripts and Dolphin service menu
- [ ] `--compress` / `--extract` flags: quits when clean; stays open on warnings, failure, cancel or changed source; never quits after drag-and-drop or when Full mode is open or another job is running
- [ ] File association for `.zip` is off by default; enabling and disabling it works and reverts cleanly
- [ ] Uninstall removes services, associations and shell extensions
- [ ] macOS: Satchel > Uninstall Satchel… appears once with both modes opened; it quits, then the app is in the Trash and (Delete Settings) the preferences are gone and stay gone; running from the dmg it explains instead

## 8. Previewers

- [ ] macOS Quick Look and Windows preview handler: Satchel zip, third-party zip, `.flac` made by Satchel, other FLAC, empty zip, corrupt zip, huge zip (instant), encrypted zip
- [ ] Preview does not crash or time out, and is read-only
- [ ] Satchel's Quick Look takes precedence over the system zip preview
- [ ] App icon in Finder/Explorer, Get Info/Properties, Dock/taskbar, Launchpad, Open dialogs, and the DMG window, at all sizes, on the oldest supported OS

## 9. Settings

- [ ] Every setting persists across restart and applies to the next job
- [ ] Reset to defaults
- [ ] Invalid values (levels out of range, thread count 0, missing folders)
- [ ] Settings and Temporary Files deletion item where available

## 10. Platform and packaging

- [ ] Install from DMG, MSI, deb, AppImage, wheel and npm package on a clean machine
- [ ] Unsigned-package first-run prompts are understood and documented
- [ ] Upgrade over the previous release keeps settings and associations
- [ ] Update check (Sparkle, WinSparkle), including offline and bad feed
- [ ] Apple silicon and Intel; Windows 10 and 11; at least two Linux distributions
- [ ] Web app: Chrome, Safari, Firefox; large file via OPFS; quota exceeded; tab closed or reloaded mid-job; mobile browser

## 11. Accessibility and localization

- [ ] Keyboard-only use of every flow; all shortcuts from the UI spec
- [ ] Screen reader names on all controls (VoiceOver, Narrator/NVDA, Orca)
- [ ] No information conveyed by color alone; contrast acceptable in light and dark
- [ ] High-DPI and mixed-DPI multi-monitor setups; window restored to a disconnected monitor
- [ ] Long strings, non-English system language and non-ASCII usernames/paths

## 12. CLI

- [ ] `create`, `extract`, `list`, `verify`, `edit`, `restore`, `preview` with relative paths, spaces, Unicode, stdin/stdout where supported
- [ ] Exit codes for success, warnings, failure and cancel (Ctrl+C)
- [ ] Output identical to the GUI for the same options
- [ ] Non-seekable output falls back to two passes and still verifies
