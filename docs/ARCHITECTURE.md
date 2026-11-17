# Vita Vox — Architecture

Version **00.39**. This describes the source tree as it exists at this version. The UI and most of the engine run on the Vita (VitaSDK, vita2d). A large part of the core is pure C and is also built on the host for unit tests.

## Layout

| Path | Role |
|---|---|
| `src/core/` | Hardware-independent logic: scan, catalog, decode, engine, caches, series, config |
| `src/ui/` | App shell and views (vita2d drawing, covers, icons) |
| `src/vita/` | Vita entry point, input, IME, system services, BGM audio sink |
| `vendor/` | Bundled third-party code: cJSON, sonic, stb_image |
| `assets/` | Manrope fonts and the in-app logo |
| `sce_sys/` | LiveArea icon, background and startup images |
| `scripts/` | VitaSDK setup and the LGPL FFmpeg cross-build |
| `tools/` | LiveArea art generator and font-metric helper |
| `tests/` | Host unit tests and optional FFmpeg decode tests |
| `docs/` | This documentation |

The built FFmpeg prefix (`third_party/ffmpeg/`) and VPKs (`build/`, `releases/`) are not stored in git. See [BUILDING.md](BUILDING.md).

## Module map

Each source file, with its purpose.

### `src/core/` — library, decode, engine

| File | Purpose |
|---|---|
| `models.h` | Shared data types: tracks, chapters, `BookSummary`, `BookDetail`, play session |
| `config.c` / `config.h` | Persistent settings (`config.json`): library path, skips, speed, screen-off, list/grid flags |
| `scanner.c` / `scanner.h` | Walk a library folder; a subfolder or a root-level audio file is a book; priority open vs background enrich |
| `catbuild.c` / `catbuild.h` | Build and incrementally save a folder catalog after the filename listing (partial saves during the first scan) |
| `libcache.c` / `libcache.h` | On-disk catalog of books (atomic JSON write) |
| `libroot.c` / `libroot.h` | Library-folder normalisation and validation; one catalog file per folder; the “change folder” sequence |
| `library.c` / `library.h` | In-memory sort (title, author, recently added) and search |
| `groups.c` / `groups.h` | Author (and tag-based series) groupings from the in-memory list |
| `series.c` / `series.h` | User-managed series in `series.json`: name, sort, list/grid, manual book order |
| `probecache.c` / `probecache.h` | On-disk probe results, keyed by path + size + mtime (lengths, tags, description, header kind) |
| `bookcache.c` / `bookcache.h` | Per-book `books/<id>.bin`: tracks, durations, chapters, description, for a fast many-file open |
| `progress.c` / `progress.h` | Per-book resume position and speed under `progress/` |
| `mp3info.c` / `mp3info.h` | Fast MP3 duration from Xing/Info/VBRI or a checked CBR estimate (~4 KB), with a fallback to FFmpeg |
| `mp4lazy.c` / `mp4lazy.h` | Lazy MP4/M4B demuxer that avoids building FFmpeg’s full sample index at open |
| `tagart.c` / `tagart.h` | Embedded-art extractors (ID3 APIC, MP4, FLAC/Ogg picture blocks) without parsing audio samples |
| `image.c` / `image.h` | Cover-image helpers, including libjpeg-turbo scaled JPEG decode when available |
| `ffstream.c` + `stream.h` | One local file: FFmpeg demux and decode, swresample to s16 stereo PCM at a native or nearby rate |
| `reader.c` / `reader.h` | Random-access byte reader over memory and files (used by the lazy MP4 path) |
| `engine.c` / `engine.h` | Decode thread (sonic speed into a ~1 s ring) and a higher-priority output thread writing the sink |
| `player.c` / `player.h` | Playback domain on the UI thread: open/resume, chapters, skips, speed, sleep timer, saving progress |
| `policy.c` / `policy.h` | When to flush progress, and the sleep timer |
| `timeline.c` / `timeline.h` | Book time ↔ (track, local time) and chapter navigation |
| `audiodiag.c` / `audiodiag.h` | Underrun, late device-write and clip counters, with rate-limited log lines |
| `wavrec.c` / `wavrec.h` | Hidden debug recorder: PCM as sent to the device, written off the audio thread |
| `statusbar.c` / `statusbar.h` | Clock / battery / percentage layout maths (host-tested); the UI draws it |
| `hintlayout.c` / `hintlayout.h` | Right-aligned hint-bar layout, including two-glyph hints such as (L) (R) Skip |
| `holdbtn.c` / `holdbtn.h` | Short press vs a 2 s hold (SELECT sort vs list/grid) |
| `scroll.c` / `scroll.h` | Touch drag, momentum and no snap-back for lists and grids |
| `touchmath.c` / `touchmath.h` | Tap vs drag, and slider scrubbing |
| `textwrap.c` / `textwrap.h` | Word wrapping for descriptions and toasts (width via a callback; host-tested) |
| `strutil.c` / `strutil.h` | Safe string copy on a UTF-8 boundary; natural / case-insensitive compare |
| `pathutil.c` / `pathutil.h` | Path join, audio-extension checks, `file:` URLs for FFmpeg |
| `color.c` / `color.h` | Colour helpers used when extracting a cover accent |
| `osal.c` / `osal.h` | Portable threads, mutexes, time; Vita thread priority and audio-core affinity |
| `worker.c` / `worker.h` | Background job queues (`g_api`, `g_sync`, `g_covers`, `g_refine`, `g_scan`) |
| `dbg.c` / `dbg.h` | Thread-safe log with 4 MB rotation and an async low-priority writer |
| `quit.c` / `quit.h` | Process-wide quit flag; long-running work must abort when it is set |

### `src/ui/` — shell and views

| File | Purpose |
|---|---|
| `app.c` / `app.h` | App shell: sidebar, view stack, hint bar, toasts, hit-testing, book cell/row widgets, SELECT hold |
| `views.h` | Constructors for content views and the player screen |
| `view_home.c` | Home: Continue listening and Recently added |
| `view_books.c` | Library, Search, and an author’s books: 5-column grid or list |
| `view_groups.c` | Authors (and a generic group list): names, book counts, fanned covers |
| `view_series.c` | Series tab, one series’ books, Add Books, and Add to Series |
| `view_book.c` | Book details: cover, metadata, description, Resume / Play from start |
| `view_settings.c` | Library folder, skips, speed, Refresh, About |
| `scr_player.c` | Now Playing: cover, chapter and book progress, transport, speed, sleep, chapters, screen off |
| `libmodel.c` / `libmodel.h` | The library as the UI sees it: background load, sorted views, groups, continue/recent |
| `covers.c` / `covers.h` | Lazy 256 px thumbs and a 500 px player cover; decode on a worker, upload textures on the UI thread |
| `gfx.c` / `gfx.h` | vita2d drawing: theme, fonts, shapes, glyphs, logo |
| `icons.c` / `icons.h` | Procedural icons as signed-distance fields (also built on the host) |
| `font_metrics.h` | Generated cap-height / bearing tables for ink-centred text (`tools/gen_font_metrics.py`) |

### `src/vita/` — platform

| File | Purpose |
|---|---|
| `main.c` | Entry point: data folders, workers, engine, frame loop, shutdown |
| `input.c` / `input.h` | Buttons, key repeat, left stick as D-pad, front-touch gestures |
| `ime.c` / `ime.h` | System keyboard (non-blocking IME dialog) |
| `sys.c` / `sys.h` | Clocks (12/24 h), power, battery, screen off, enter-button setting, power-tick thread, quit |
| `vsink.c` / `vsink.h` | `AudioSink` over `sceAudioOut` (BGM port, native sample rate) |

### `vendor/`

| Path | Purpose |
|---|---|
| `vendor/cjson/` | JSON for catalogs, config, series and the probe cache |
| `vendor/sonic/` | Time-stretch for playback speed, with pitch kept |
| `vendor/stb/stb_image.h` | Image decode fallback when libjpeg-turbo is not used (PNG, and host tests) |

## Threads and priorities

On the Vita, `thread_start` maps a small relative priority onto the default user priority `0x10000100` (lower numeric value runs first): **2** audio output (−30), **1** decode (−20), **−1** background (+20), **−2** lowest (+40, covers, thumbs, log and debug WAV writes). Audio threads are pinned to user cores 1–2 so they do not share core 0 with the UI and GPU.

| Thread / worker | Priority | Role |
|---|---|---|
| UI / main | default | Frame loop, input, drawing, texture upload |
| Engine output (`out_th`) | 2 (highest) | Volume, write PCM to the blocking BGM port — this is the playback clock |
| Engine decode (`th`) | 1 | Demux/decode, sonic speed, fill the ~1 s decode-ahead ring |
| Next-track preopen (`pre_th`) | −1 | Gapless: open the next file of a multi-file book in the background |
| `g_api` | default worker (−1) | UI-driven work (opening details, and similar) |
| `g_scan` | −1 | Folder listing for Refresh, so an open never waits behind a rescan (00.37) |
| `g_refine` | −1 | Exact lengths of a book that was opened with estimates (00.36) |
| `g_covers` and the art worker | −2 | Thumbnails and the player cover; never competes with audio |
| `g_sync` | default worker | Progress flush |
| Debug log writer | lowest | Async write and rotation of `debug.log` |
| Debug WAV writer | lowest | Optional 60 s capture |
| Power tick | default | Keeps the process from suspending while a book plays |
| Shutdown watchdog | default | `sceKernelExitProcess` if shutdown sticks |

Opening or playing a book whose details are not ready **pauses the background enrich** until that book is loaded (`scanner_priority_*`). A Refresh during the first scan stops the running scan between files, saves what it has, and continues from the probe cache and partial catalog.

Audio always takes priority over artwork: menus use already-made or lazily-made 256 px thumbs; the 500 px player cover is requested only after audio output has started.

## Audio pipeline

1. `player.c` (UI thread) opens a `BookDetail` (from `books/<id>.bin` or a priority scan) and asks the engine to load it.
2. `ffstream.c` (and `mp4lazy.c` for MP4/M4B) demuxes and decodes. When the codec parameters are already complete, `avformat_find_stream_info` is skipped. The play path does not call it when the codec is known; sample rate and layout can be learned from the first frame. FLAC rate/channels/duration are filled from STREAMINFO (FFmpeg 9’s flac demuxer may leave them at 0).
3. PCM is converted to s16 stereo. The book plays at its own sample rate when the Vita BGM port supports it (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 Hz); other rates go to the nearest supported one, so common audiobook rates are not resampled.
4. The decode thread applies **sonic** for speed and writes ~1 s of audio into a ring (48 chunks: about 1.0 s at 48 kHz, about 1.5 s at 32 kHz).
5. The output thread applies volume and writes the ring to `vsink.c` (`sceAudioOut` BGM). Pause and seek still respond at once (the ring is discarded).
6. Diagnostics (`audiodiag.c`) log `audio: stats` every 5 s while playing, and `audio: underrun` / `audio: late device write` at most once per second of each.

## Library scan and caches

1. **List the folder** (filenames only) so the Library can appear at once.
2. **Enrich** book by book: title, author, series tags, art, lengths. During the first scan the catalog is written every few seconds and marked partial (00.36), so a quit mid-scan resumes instead of starting over.
3. **Lengths.** The first file of a book is probed with FFmpeg for tags and art. Later MP3 parts can be timed from their header (Xing/Info/VBRI frame count, or a checked constant-bitrate estimate, about 4 KB). The header is only trusted if the first frame chains, the length and bitrate are plausible, and (without a frame-count header) frames at the start and at 1/3 and 2/3 of the file share one bitrate. Anything else falls back to a full FFmpeg probe; the log says why. The probe cache records the header kind (`hdr`: 1 exact frame count, 2 checked CBR).
4. **Priority open.** For a many-file MP3 book that is opened during a scan, parts after the starting point may be estimated from file size after a short delay so playback can start; exact lengths are refined on `g_refine` and the seek bar updates. Parts up to a saved resume position are always timed exactly.
5. **Covers and titles early (00.37).** A many-file book can show its title and cover as soon as the first file or folder image is read; the length may still show as *Length: scanning…*.

## Covers

- Menus and lists use only **256 px** PNG thumbnails (`covers/t_<hash>.png`), made at Refresh or lazily once on the lowest-priority worker. JPEG sources are decoded with libjpeg-turbo at 1/2, 1/4 or 1/8 (the largest reduction that keeps the shorter side at least 256 px); a progressive JPEG thumbnail can stop after its DC scans. PNG still decodes at full size.
- The player shows the 256 px thumb immediately (scaled up). After audio output has started, a **500 px** cover is decoded on its own lowest-priority worker and cross-fades in. Leaving the player cancels that work.
- Folder images are preferred over embedded art. Embedded art is extracted from the first file only.

## UI views

The shell (`app.c`) draws a 220 px sidebar (Home, Library, Series, Authors, Search, Settings, plus a mini player) and a stack of content views to the right, with a 40 px hint bar at the bottom.

- **Home** — Continue listening, Recently added.
- **Library / Search / author’s books** (`view_books.c`) — 5-column grid or 76 px list rows; Library sort is Title / Author / Recently added.
- **Series tab and one series** (`view_series.c`) — the tab is always a list; books inside a series follow that series’ saved list/grid flag.
- **Authors** (`view_groups.c`) — always a list of authors; opening one uses `view_books.c`, which starts in the grid.
- **Book details** (`view_book.c`) — metadata and a scrollable description.
- **Settings** — folder, skips, speed, Refresh, About.
- **Now Playing** (`scr_player.c`) — full-screen player.

List rows reuse the same 256 px thumb cache, scaled down, and never block audio.

## Config and other persistence

`config.json` keys: `libraryPath`, `skipBack`, `skipFwd`, `speed`, `screenOff`, `listViewLibrary`, `listViewSearch`. Defaults: `ux0:audiobooks`, skip back 15 s, skip forward 30 s, speed 1×, screen-off on, both views as a grid.

`series.json` stores each series’ name, sort (`alpha` / `manual`), `listView`, created time, and book ids (path-stable). A missing book is hidden and returns when it is back.

See [OVERVIEW.md](OVERVIEW.md) for the full `ux0:data/VitaVox/` layout.

## Status bar (00.38)

The left side of the hint bar shows the system time (12-hour as `12:31 PM`, or zero-padded 24-hour), a battery outline whose fill follows the charge, and the percentage. Fill is white, orange at 30 % or less, red at 15 % or less, and green with a yellow bolt while charging. Values are cached about once a second. The 12/24-hour setting is re-checked every 10 s. If hints need the room, the clock is dropped first, then the percentage.

## Description tags (00.38)

The book details description comes from the **first audio file only**. Order tried:

| Format | Order |
|---|---|
| MP3 | comment, a COMM frame with “comment” in its descriptor, synopsis, long_description, description, TDES |
| M4B (mp4lazy) | `©cmt`, `ldes`, `desc` |
| FLAC / Ogg | COMMENT, SYNOPSIS, DESCRIPTION |

The text is cleaned once when read (HTML stripped, entities decoded, invalid UTF-8 replaced with `?`, spaces collapsed, at most one blank line in a row, capped at 8 KB). It is stored in the first file’s probe-cache record and in `books/<id>.bin` (version 2). Older records cause a one-time re-read of the first file only.
