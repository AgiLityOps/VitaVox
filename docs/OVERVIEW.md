# Vita Vox — Overview

Version **00.39**. A fully offline, local-file audiobook player for the PlayStation Vita. There is no server, sync or streaming, and no network code in the app.

Books live on the memory card (SD2Vita as `ux0:` by default) under `ux0:audiobooks/` (changeable in Settings). The player follows the behaviour of the Android app Voice (a folder is a book, a loose audio file at the library root is a book, natural file sort, resume position, chapters, sleep timer, playback speed). Voice itself is GPLv3; this project copies that behaviour only, never Voice source.

The title is **Vita Vox**, title ID **VVOX00001**, data folder `ux0:data/VitaVox/`.

## Attribution and licence

This app is a fork of [ABSvita](https://github.com/DarthTigerson/ABSvita) (MIT License). The original LICENSE is included in this repository. The UI shell, playback engine thread, sonic speed/pitch, vita2d interface, background/screen-off playback (`ATTRIBUTE 0x88008`, BGM audio port, power tick), IME, sleep timer and chapter list come from that codebase. Audiobookshelf client code, downloads, and the Helix / custom MP4–MP3 decoders were removed and replaced with a local library scanner and an FFmpeg decode path.

The app is MIT, following ABSvita. Audio is decoded with a project-built, decoder-only **LGPL FFmpeg** (no GPL features, no encoders, muxers or network). Playback speed uses **sonic**. Fonts, cJSON, sonic and stb_image keep their own licences (see the repository `LICENSE`, `vendor/` and `assets/fonts/OFL.txt`).

## Features

- Local library of MP4/M4A/M4B, MP3, AAC, FLAC, Ogg/Opus, Matroska/WebM, WAV, AIFF, WMA/ASF, APE, WavPack, CAF and TTA (FFmpeg demuxers).
- Resume position and speed per book, Continue listening and Recently added on Home.
- Chapters (from the file or one track per file), skip back/forward, playback speed (0.75×–2× via sonic).
- Sleep timer (minutes or end of chapter); can turn the display off.
- Book details with a description taken from the first audio file’s comment/description tags.
- Cover art: 256 px thumbnails in menus, a 500 px cover on the player after audio has started.
- User-managed series (alphabetical or a manual order), an Authors list, and Search.
- Library and Search can be a five-column grid or a list (hold SELECT for two seconds). A series remembers its own list/grid choice.
- Clock, battery icon and percentage in the hint bar (follows the system 12/24-hour setting).
- Playback continues in the background and with the screen off (BGM audio port).
- First scans and large many-file books are built in the background; opening a book is prioritised over the scan; MP3 lengths can be read from the file header.

## Library layout

Default folder: `ux0:audiobooks/`.

- A **subfolder** is one book. Audio files inside it (recursively) are tracks, sorted naturally (`ch2.mp3` before `ch10.mp3`). Disc-like subfolders (`CD1`, `Disc 2`, `Part 1`, …) are merged into one book.
- A **single audio file** sitting in the library root is a book on its own.
- Optional folder art: `cover.jpg` / `cover.png` / `folder.jpg` / `folder.png` (case-insensitive; other jpg/png images in the folder, including inside disc folders, are also considered). Embedded cover art is used when no folder image is present.

Settings > **Library folder** checks the path before anything changes: the drive must be one of `ux0:`, `uma0:`, `imc0:`, `ur0:`, `grw0:`, `xmc0:`, it must be mounted, and the folder must exist. Otherwise the change is refused and the old folder is kept. Each folder has its own catalog under `ux0:data/VitaVox/libraries/`. Only a first install creates the default `ux0:audiobooks`. A missing folder at startup shows “Can't open <folder>” and an empty library; the folder is not created.

## Controls

OK and Back follow the console’s Enter-button setting (normally Cross / Circle). The left stick is treated as a D-pad.

### Everywhere in the main UI

| Control | Action |
|---|---|
| D-pad / left stick | Move focus |
| L / R | Previous / next sidebar section |
| Triangle | Jump to Search (unless the current view uses Triangle itself) |
| Start | Open Now Playing if a book is loaded |
| Left / Back | Leave a section for the sidebar, or pop a pushed view |

### Library, Search, and books inside a series or author

| Control | Action |
|---|---|
| OK | Open book details (or the search field) |
| Square | Play / resume the focused book |
| SELECT, short press (release before 2 s) | Library and a series’ books: cycle sort. Search and an author’s books: no sort action |
| SELECT, hold 2 s | Toggle list view and grid view. A fill grows on the SELECT hint; a toast says *List view* or *Grid view*. The release after a completed hold is ignored |
| Touch | Tap a cell or a full list row to open it; drag to scroll (flick for momentum) |

The Library and Search each remember their list/grid choice in `config.json`. A series remembers its choice in `series.json`. An author’s books always open in the grid; list mode lasts only for that visit. The Series and Authors tabs themselves stay as lists; holding SELECT there does nothing.

### Series tab

- OK on *New Series* names a series with the keyboard; OK on a series opens it.
- Triangle on a series: Open / Rename / Delete.
- Inside a series, Triangle opens Options (add books, sort, move, remove, rename, delete). In manual *Move book* mode, Left/Right move one place; Up/Down move one row in the grid, or one book in the list; OK, Back or Triangle = Done.
- Book details: Triangle = *Add to Series*.

### Player (Now Playing)

| Control | Action |
|---|---|
| Square | Play / pause |
| L / R | Skip back / forward (the hint is *(L) (R) Skip*) |
| Triangle | Chapters |
| SELECT | Turn the display off (on press, not on hold). Can be disabled in Settings |
| Back | Return to the library |
| Touch | Tap the progress bar to jump, or slide along it (one seek on release; slide far off the bar to cancel). Tap a chapter, or tap outside the Chapters panel to close it |

### Home and Settings

- Home: Continue listening and Recently added. On an empty or failed library, SELECT (or OK) can Refresh / retry.
- Settings: library folder, skip times, default speed, Refresh, About (`Vita Vox 00.39`). Press OK on About five times to show the hidden **Debug: record output (60 s)** option.

## Data on the memory card

All app state lives under **`ux0:data/VitaVox/`**:

| Path | Purpose |
|---|---|
| `config.json` | Library folder, skip times, speed, screen-off, Library and Search list/grid flags |
| `libraries/<folder>-<id>.json` | One catalog per library folder (the pre-00.33 single `library.json` is migrated on first start) |
| `books/<id>.bin` | Per-book details (tracks, durations, chapters, description) for a fast open |
| `covers/` | 256 px menu thumbnails and cached player covers |
| `probe/` | Probe cache (lengths, tags, description), keyed by path + size + mtime |
| `progress/` | Per-book resume position and speed |
| `series.json` | User-managed series (name, sort, list/grid, book ids) |
| `debug.log` | Log, capped at **4 MB**. When full it is renamed to `debug.old.log` (replacing the previous one) and a new `debug.log` starts, so the log never takes more than **8 MB**. The same check runs at startup. A low-priority thread writes the log so playback is never blocked; after a hard crash the last quarter-second of lines may be missing |
| `debug/` | Optional 60 s WAV capture of the exact PCM sent to the audio device |

Progress is flushed on pause/exit and every few seconds while playing. Progress also stores the book’s length: if a different book with the same path in another folder has a clearly different length, it starts from the beginning rather than using the other book’s position.

## Documentation

- [Architecture](ARCHITECTURE.md) — modules, threads, audio and library pipelines
- [Building](BUILDING.md) — toolchain, VPK, host tests, LiveArea assets
- [Changelog](CHANGELOG.md) — history from the git log
