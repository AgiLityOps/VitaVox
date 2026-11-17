# Vita Vox — Changelog

History taken from the project’s git log. Commit subjects and messages are the source; nothing later has been invented. All of the commits below are dated 2026-11-17 in this repository. Version **00.39** is the current release.

The app began as a Vita Audiobooks homebrew skeleton (title ID `VABS00001`). It was then replaced with an offline fork of ABSvita and developed as 0.2.x (the system `param.sfo` version went up to 00.30 with 0.2.12). From **00.31** there is a single version number everywhere. **00.34–00.35** renamed the app to Vita Vox (title ID `VVOX00001`, data `ux0:data/VitaVox/`).

## 00.39 — list view (`192d458`)

`v00.39: list view, hold SELECT to toggle, remembered per library/search/series`

- In the Library, Search, a series’ books, or an author’s books, hold SELECT for two seconds to switch between the five-column grid and a list (thumbnail, title, author, progress or *Finished*).
- A fill grows on the SELECT hint while you hold; at two seconds a toast says *List view* or *Grid view*. Releasing earlier is a short press (Library and a series’ books still cycle sort). The later release after a hold is ignored.
- The Library and Search each remember their own choice in `config.json`. A series remembers its choice in `series.json` (with its sort mode). An author’s books always open in the grid; list mode is only for that visit.
- The Series and Authors tabs themselves stay as lists; holding SELECT there does nothing. Home, Settings and the player are unchanged (SELECT in the player still turns the screen off on press).
- Manual reorder of a series in list mode moves up/down by one book. Switching keeps the focused book. Menus still use 256 px thumbnails.

## 00.38 — status bar and descriptions (`ec9c644`)

`v00.38: clock and battery indicator, descriptions from file comment tag`

- The bottom-left of the hint bar shows the system clock (12/24-hour setting), a battery icon and the percentage.
- Book descriptions come from the comment/description tags of the **first** audio file only (with fallbacks). Text is cleaned and capped at 8 KB. Stored in the probe cache and in `books/<id>.bin` version 2; older records are re-read once (first file only).

## 00.37 — faster scan, covers and refresh (`a62fbc6`)

`v00.37: fast MP3 lengths in scan with fallback, early covers, no wait on rescan, fast big-cover thumbs`

- Fast MP3 header lengths in the background scan as well, with an FFmpeg fallback when the header is unusable; the probe cache records the header kind.
- A many-file book can show its cover and title as soon as the first file is read.
- Refresh lists the folder on its own worker, so opening a book never waits on a rescan.
- libjpeg-turbo scaled decode (1/2, 1/4, 1/8) for large JPEG covers; progressive JPEG thumbnails can stop after DC scans.

## 00.36 — priority open, partial catalog, fast exit (`50db496`)

`v00.36: open tapped book first during scan, save scan progress, fast exit`

- Opening or playing a book pauses the enrich scan until that book is loaded. Many-file MP3 books get quick header lengths (or a short CBR estimate) so playback can start; lengths refine in the background.
- The first scan’s catalog is saved every few seconds (partial), so a quit mid-scan resumes.
- Shutdown cancels scans and probes promptly.

## 00.35 — full Vita Vox rename (`dc4870b`)

`v00.35: full rename to Vita Vox (title ID VVOX00001, data ux0:data/VitaVox)`

- Title ID `VVOX00001`, data folder `ux0:data/VitaVox/`. No migration from the previous id or data path (a fresh install was expected). The ABSvita MIT credit lines stay in the README.

## 00.34 — name and art (`5ab8f19`)

`v00.34: Vita Vox rename, new LiveArea art and in-app logo`

- The visible name became Vita Vox. New LiveArea art and the in-app logo (orange circle, dark headphones around an open book). LiveArea description: “Audiobook Player for PS VITA”.

## 00.33 — two-glyph hints and library folders (`18c4d21`)

`v00.33: (L) (R) Skip and (L) (R) Page hints (two-glyph hints); library folder fix: one catalog per folder (libraries/<folder>.json, old library.json migrated), startup loads only the current folder's list, empty folder shows/saves empty, missing folder shows Can't open with an empty library (no silent mkdir); folder entry validated (known drive, mounted, exists) and refused otherwise, keeping the old folder; playback fully stopped before a folder change; progress stores book length and ignores another book's progress with the same path; host tests`

## 00.32 — log cap, touch, menus (`ab23bae`)

`v00.32: debug.log capped at 4 MB with one rotated debug.old.log (8 MB max, checked at startup, async low-priority writer); drag-to-scrub player progress slider with one seek on release; touch lists scroll freely with momentum and no snap-back; Chapters menu and option menus close on a tap outside, menu rows tappable; host tests for rotation and touch logic`

## 00.31 — native-rate output and one version number (`5b4ba09`)

`v00.31: native-rate output, ~1 s decode-ahead with a separate audio output thread, underrun/late/clip diagnostics, hidden debug WAV recorder; single version number 00.31`

- Books play at their own sample rate when the BGM port supports it. A decode thread keeps about a second of audio ready ahead of a separate, higher-priority output thread.
- From this release, the in-app version, `param.sfo` `APP_VER`, the VPK name, commit messages and the README share one `MM.mm` number.

## Earlier 0.2.x (and the first commits)

These builds used 0.2.x names. 0.2.12 maps to system version **00.30** in `param.sfo`.

| Commit | Subject / what changed |
|---|---|
| `6c878b2` | **v0.2.12:** stall watchdog only times blocked device I/O, reopen-once on mid-track errors; PNG alpha on player cover (`pa_` cache); sfo 00.30 version mapping |
| `b6bd058` | **v0.2.11:** skip icon arrows point the right way; number-aware title sort for Library and Series |
| `65837dd` | **v0.2.10:** user-managed series (`series.json`) with alphabetical/manual order |
| `3b1f01d` | **v0.2.9:** per-book detail files (`books/<id>.bin`) for a fast many-file book open; sleep timer turns the display off |
| `3478dfe` | **v0.2.8:** lazy MP4/M4B demuxer, 256 px thumbs on build/Refresh, 500 px cached player cover with cross-fade |
| `41ff2b1` | **v0.2.7:** Fast FLAC opens (STREAMINFO; no `find_stream_info` when the codec is known; next track preopened). Menus use only 256 px thumbs; the full-size player cover loads after audio starts, on a lowest-priority worker, and is cancelled if the player is left |
| `f32510c` | **v0.2.6:** Fix close freeze (cancel probes, join threads, release audio, `sceKernelExitProcess`). Cache the library in `library.json`; a full rescan is Settings > Refresh (or no cache). Covers prefer a folder image, else extract art once during Refresh. Book open uses a 2 MiB readahead and preloads `moov` without reading `mdat` |
| `85144d8` | Add an idempotent VitaSDK toolchain setup script; copy each built VPK into `releases/` as well as `build/` |
| `082b8a8` | **v0.2.5:** Scan mixed library layouts and covers. Merge disc-like subfolders into one book; pick folder images more flexibly; force a demuxer from the extension; pick audio streams even when codecpar is incomplete; extract APIC / FLAC / Ogg pictures; unlock enrich per-path so play is not blocked. Host fixtures recreate a large sample library tree. VPK `vita_audiobooks_v0.2.5.vpk` |
| `2d73857` | Fix playback ending after the first decoded frame (`store_converted` was treated as EOF). Disarm the open deadline after `stream_open`, stall watchdog for later I/O, (re)build swr from the actual frame so HE-AAC is not locked to codecpar. Enrich and `load_detail` share a mutex. Host decode tests on a 32 kHz mono m4b |
| `5a09a05` | Skip `find_stream_info` on m4b play when codecpar is complete; cache covers on enrich; reuse the probe cache so playback opens the file once; show Opening book… / a timeout instead of an infinite spinner |
| `ab1f026` | Keep the library responsive while probing large m4b files: list filename titles immediately, enrich in the background with a 15 s AVIO interrupt timeout and a path+size+mtime cache; large-buffer sceIo AVIO |
| `4564d6f` | Fix empty library: treat filenames as paths (root-level `.m4b` was missed), open `ux0:` via `file:`. Scanner logs every entry; VPKs named `vita_audiobooks_v${APP_VERSION}.vpk` (0.2.1) |
| `97da312` | Replace the skeleton with an offline Vita audiobook player. Fork ABSvita (MIT), drop all network/download/login code, decode local files with a project-built LGPL FFmpeg. Library scan of `ux0:audiobooks/`, resume and speed persistence, VPK with TITLEID `VABS00001` |
| `0c6e024` | Initial Vita Audiobooks homebrew skeleton: buildable VPK (TITLEID `VABS00001`) with vita2d UI, `sceAudioOut` thread, and a sceNet/libcurl + jansson stub for a future Audiobookshelf client (later removed) |
