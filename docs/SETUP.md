# First-run setup and the game module

End users run one program, `LuigisMansionRecomp.exe`. It contains the runtime (memory,
scheduler, HLE SDK, Aurora, menu, audio), the recompiler and a setup screen. The game
itself is a DLL that setup builds on the user's machine from the user's own disc, with a
bundled clang + lld. Nothing derived from the game is shipped or committed.

## Release layout

`cmake --install build/<preset> --prefix <dir>` produces:

```
LuigisMansionRecomp.exe   runtime + recompiler + setup UI
pipelines.bin             shader/pipeline precache list (data/pipelines.bin; docs/RENDERING.md)
*.dll                     SDL3, Dawn (+ dxcompiler/dxil), nod, libpng, zlib
llvm/bin/clang.exe        bundled compiler (from LM_LLVM_DIR, default: the LLVM that built the exe)
llvm/bin/lld-link.exe     bundled linker
llvm/LLVM-LICENSE.TXT     Apache-2.0 WITH LLVM-exception (packaging/LLVM-LICENSE.TXT)
NOTICE.md
```

The LLVM binaries are copied at install time, never committed. Toolchain lookup order:
`<exe dir>/llvm/bin`, `LM_LLVM_DIR`, the LLVM that built the exe (dev builds,
`LM_DEV_LLVM_DIR`), then `PATH`.

## Setup flow

1. **Pick discs.** The setup screen (Dear ImGui on the Aurora window; SDL3
   `SDL_ShowOpenFileDialog` for browsing) has a slot for the USA disc (GLME01) and one for
   the European disc (GLMP01). Any image nod reads works (ISO/GCM, RVZ, WIA, GCZ, CISO,
   WBFS, NFS, TGC). Each pick is validated at once (`setup/disc.cpp`): the image is
   opened with nod and the SHA-1 of its `main.dol` must match a supported retail build
   (`port::dol::identify`, hashes from `decomp/config/*/build.sha1`). A disc in the wrong
   slot, another game or another revision gets a message.
   One disc is enough; with both, the user picks the version to play (default USA).
2. **Install** (worker thread, progress bar, cancellable):
   - For each disc whose game module is missing or stale: recompile its `main.dol`
     (`src/recompiler`, in-process), write the 32 generated sources plus the shipped
     freestanding sources (`ctx.h`, `module_abi.h`, `module_rt.cpp`) to a scratch folder,
     compile them in parallel (one clang per core, largest first), link with lld-link,
     then delete the generated sources (set `LM_KEEP_BUILD=1` to keep them).
     A full build takes about 15-25 s on an 8+ core machine.
   - With both discs: build the Hidden Mansion overlays (`setup/hm_overlay.cpp`, the C++
     version of `tools/hidden_mansion_data.py`; files are read straight from the discs):
     the European rules' data for the USA game and the USA rules' data for the European
     game (`usa/`).
3. **Compile shaders.** Aurora compiles every pipeline in the precache list
   (`pipelines.bin`) plus any this PC used before, on worker threads, from the first setup
   frame on, so most of it overlaps the disc picking and the module build. When setup was
   shown (first run, `--setup`, a rebuild) and the compile is not finished yet, a
   "Compiling shaders n / N" step with a progress bar follows. The game itself only starts
   once every precache pipeline for the current settings is built (Aurora 0217, held before
   its first frame with a "Compiling shaders... n / m" screen; with a warm cache "Loading..."
   for 0.5-2 s, all threads but one loading, Aurora 0226/0227, docs/RENDERING.md "Warm starts"),
   and a settings change that needs other shader variants pauses it the same way, so
   the boot logos, the title and play never stutter on a compile. A draw never waits on
   anything but its own pipeline, and is never skipped (docs/RENDERING.md, "Pipeline precache").
4. **Launch** the chosen version: open its disc in Aurora's DVD layer, check the DOL
   again, load the module, boot.

Later launches skip the screen when the configured discs are readable and every module
and overlay is current; they are rebuilt automatically when stale. `--setup` (or F1 >
Game > *Add or change discs...*) opens the screen again.

### Disc image check

The DOL check above only covers `main.dol`. Issue #37: an image with a retail DOL but
damaged or modified game files started, then crashed on START at the title screen, at every
start (`call to null (lr=80047B20) r3=80B9A7E0 vtable=B04FFE00`: the file select screen,
`fn_80047A1C`, mounts `/Kawano/ENGLISH/res_slct.szp` and calls the archive's `getResource`
through a vtable read from the file's own bytes; the same log also showed JAudio falling back
to `Seqs/JaiSeInf.bst` / `JaiArcS.hed` and `open failed: /`, which is what a broken
`/AudioRes/JaiInit.aaf` does, and a failed 0x44-byte allocation in
`BootScene::dvdFileOpenCallback`, the game archive `/Game/game_usa.szp` not fitting the game
heap). Reproduced byte for byte on a retail image with only the first 0x40 bytes of
`res_slct.szp` zeroed.

Now setup compares every file of the image with the retail disc's (`src/setup/disc_check.cpp`:
size and CRC-32 per file, tables `src/setup/disc_files_<ID>.inc` generated from clean dumps by
`tools/gen_disc_files.py`; no game data, only the checksums). It runs on a worker thread
before the game starts, also for installs that were set up before (a "Checking the disc
image" step, 0.6-1 s for an ISO), and the result is cached in `<dataDir>/disc_check_<ID>.txt`
keyed by the image's path, size and modification time. A mismatch:

- opens the setup screen with a warning under the disc slot naming the files that differ and
  what to do (new dump, Dolphin's Properties > Verify); "Play anyway" starts it and is
  remembered for that image (`accepted` in the cache; `--setup-auto` accepts too);
- is logged as `[disc] USA image check: N of 847 game files differ ...` at every start;
- is appended to a fatal error (`[recomp FATAL] probable cause: the disc image is damaged or
  modified: ...`) and to the next start's `[recovery]` line (`port::install().discProblem`).

Test aids: `LM_DISC_CHECK=0` skips the check, `LM_DISC_CHECK=force` ignores the cache.

### What lives where (`%APPDATA%\Luigi's Mansion Recompiled`)

Linux: the XDG config/data/cache folders of the same name (docs/LINUX.md). Portable mode (below):
`<program folder>/portable`, same layout on both platforms. Up to 1.0.1 the
product was "Luigi's Mansion PC": at the first start of 1.1 or later, each folder of that old
name whose new-name folder does not exist yet is renamed to the new name (`port::configDir`
/ `dataDir` / `cacheDir`, `migrateOldFolder` in `src/port/install.cpp`), so settings, install.ini,
memory cards, game modules (their key does not depend on the folder), overlays and caches carry
over. A new folder that holds only the Linux launcher's logs counts as missing (the launcher
also moves the data folder itself before it writes its log). If the rename fails (a file held
open by another program, another volume) the folder is copied and the old one kept; the
`[install]` log line says which.

| Path | Contents |
|------|----------|
| `install.ini` | disc paths and the version to play |
| `settings.ini` | F1 menu settings |
| `game/GLME01/module.dll`, `game/GLMP01/module.dll` | game modules |
| `game/<id>/module.key` | cache key (below) |
| `game/<id>/recompiler.log` | recompiler summary |
| `hidden_mansion/` | European Hidden Mansion overlay for the USA game, `usa/`: USA Hidden Mansion overlay for the European game; `overlay.key` |
| `USA/Card A`, ... | memory cards (unchanged) |
| `pipeline_cache.db` | every pipeline config this PC has used (Aurora), rebuilt at startup |
| `dawn_cache.db` | Dawn's compiled shader blobs; stale entries pruned after the startup rebuild |
| `last_run.log`, `previous_run.log` | Windows: the log of this run and the one before (Linux: written by the launcher) |
| `last_run_state.ini` | how the last run ended (start-up recovery, below) |
| `settings.ini.bak-<time>`, `settings.ini.corrupt` | settings put aside by start-up recovery / a damaged settings.ini |

A module's key is the SHA-1 of: the module ABI version, the game ID and DOL SHA-1, the
compile and link flags, and the embedded inputs (symbol map, splits, PAL version map,
hooks.txt, ctx.h, module_abi.h, module_rt.cpp, recompiler.cpp). Any change to those in a
new release rebuilds the module on first launch. The overlay key covers both DOL hashes
and the overlay format version (3: lab menu choice in event08).

### Portable mode

Public issues #9 and #16: everything in one folder next to the program, so the game folder can be
moved to another drive or PC and deleting it removes everything (`port::dataLocation`,
`src/port/install.cpp`).

- **Opt in.** A file `portable.txt` or a folder `portable` next to `LuigisMansionRecomp(.exe)`, or
  the environment variable `LM_PORTABLE=1`, turns it on; `LM_PORTABLE=0` forces the system folders
  (testing). The data root is `<program folder>/portable/` (created when only `portable.txt`
  exists). Decided once, at the first path lookup (the start of `main`, before any file is opened).
- **Layout.** `configDir()`, `dataDir()` and `cacheDir()` all return the data root, so it holds the
  same flat tree as `%APPDATA%\Luigi's Mansion Recompiled` (table above) on both platforms: on
  Linux the three XDG folders are merged into it. The 1.0.x folder migration only runs for the
  system folders.
- **Read-only program folder** (e.g. Program Files, a read-only mount): the probe file fails, portable
  mode stays off, the data goes to the system folders, and the setup screen, F1 > Game and the log
  (`[data] Portable mode is requested ... cannot be written ...`) say why.
- **F1 > Game > Data folder** shows where the data lives (the portable root, or the system folder(s))
  and offers *Make this installation portable...* or *Move data back to the system folder...*
  (disabled when `LM_PORTABLE=1` forces it). The button first checks that the move can work (program
  folder writable; the target empty: a system folder holding another installation's data is never
  overwritten, one with only the Linux launcher's logs counts as empty; free space for a full copy
  + 64 MB) and shows the reason otherwise; then a confirmation (save first), settings are saved, a
  request file `.lm-data-move` (direction + PID) is written next to the program and the game
  restarts. The new process waits for the old one to exit (its game module and SQLite caches are
  only consistent then), deletes the request (a failed move is never retried by itself) and moves
  the data before it opens any file:
  - *to portable*: every system folder is copied into `portable.lm-tmp` (each file's size checked),
    which is renamed to `portable` (the switch), then the system folders are deleted;
  - *back*: each file goes to the system folder it belongs in (Linux: `settings.ini`/`install.ini`
    config, `pipeline_cache.*`/`dawn_cache.*`/`imgui.log`/`texture_dumps` cache, the rest data),
    built as `<folder>.lm-tmp` and renamed into place; then `portable.txt` is removed and
    `portable` renamed to `portable.lm-old` (the switch) and deleted.
  Any failure before the switch removes the copies and leaves the data where it was; the result
  (green, or red with the reason) is shown in F1 > Game and logged as `[data] ...`. Leftover
  `portable.lm-tmp` / `portable.lm-old` folders of an interrupted move are deleted at the next start
  (the first is an unfinished copy, the second's data is already in the system folders).
- **Moving the folder.** Nothing inside the data root depends on where it is: module and overlay
  keys hash the program and the disc's DOL, not paths (a moved folder, or another PC, keeps its
  game module: it is generic x86-64 code; only a different program version rebuilds it). Disc
  paths: `install.ini` keeps each disc's absolute path and, in portable mode, its path relative to
  the program folder (`us_disc_rel` / `pal_disc_rel`). A disc not found at its absolute path is
  looked for at the relative path, then by file name next to the program and in `portable/`
  (`[install] disc ... not found; using ...`); only when none exists does the setup screen ask for
  it. Keep the disc images inside the game folder to move everything together. Another GPU or
  driver only recompiles shaders: `pipeline_cache.db` holds GPU-independent configs (rebuilt into
  pipelines at startup) and Dawn's `dawn_cache.db` keys include the adapter.
- **Updates** keep a portable install portable: the updater installs into the program folder,
  skips any package entry named `portable`, `portable.txt`, `.lm-data-move` or `portable.lm-*`
  (`port::isPortableDataPath`), and stages in `upd` (next to `portable`, never inside it). In portable mode the toolchain links for deep
  paths (`<data folder>/llvm`, "Long paths") live in `portable/llvm` and move with it.
- **Linux launcher**: `LuigisMansionRecompiled.sh` makes the same decision (portable.txt, portable
  folder, `LM_PORTABLE`; writable) and then writes `last_run.log` / `previous_run.log` into
  `portable/`. Its exit status file is a `mktemp` file now (the data folder may move during the run).

Command-line helpers (testing): `--us-disc <path>`, `--pal-disc <path>` prefill the slots,
`--setup-auto` starts installing without a click, `--setup` forces the screen.
`LM_CACHE_DIR` separates the GPU caches of test runs; pointing `APPDATA` at an empty folder
gives a fresh install.

### When the game does not start (start-up recovery)

Issue #17: after a fatal error in play the player could no longer start the game, and nothing
in the program helped. Now (`src/port/recovery.cpp`):

- **Run state.** Each run writes `last_run_state.ini`: `starting` at once, `running` after 600
  game frames (`frame.cpp`), `exited` on a clean quit, `crashed` from the crash handler with the
  error text (Aurora `FATAL` or the recompiler's `fatal()`, else the exception) and the phase
  (`startup` or `play`).
- **Prompt.** After a crash (start-up or play), or after two starts in a row that never reached
  `running` (killed or hung), the next start shows a dialog before anything else loads, with the
  last error: **Start normally** (default), **Reset settings** (settings.ini kept as
  `settings.ini.bak-<time>`, then the defaults), **Rebuild game data** (LuigisMansionRecomp only:
  deletes `game/`, `hidden_mansion/` and the pipeline/shader caches, so setup recompiles the
  module and rebuilds the overlay) and **Put saves aside** (each `<region>/Card A|B` folder is moved
  to `Card A (put aside <time>)`; nothing is deleted). It appears again after each crash.
- **Run log (Windows).** Everything the game logs (recomp `log`/`fatal`, Aurora's log callback,
  the crash handler's stack) also goes to `last_run.log`; the run before it is kept as
  `previous_run.log`. The fatal-error dialog names the file.
- **No console window (Windows, since 1.1.4).** LuigisMansionRecomp is a windowed program
  (`WIN32_EXECUTABLE`, entry still `main`); `port::recovery::setupConsole` decides where output
  goes: redirected stdout/stderr (test scripts, a pipe) stay as they are; started from a terminal it
  attaches to that terminal; `--console` or `LM_CONSOLE=1` opens a console window (the old look);
  otherwise (a double-click) stdout and stderr are reopened onto `last_run.log` itself, so every
  `printf` line is in the log too, and `logLine` stops writing its copies (no duplicates).
  `LM_CONSOLE=0` (test aid) acts like a double-click. In a windowed program stdout/stderr start
  without a descriptor (fileno -2): stdout is `freopen`ed onto the log (which then refuses other
  writers, so the run log *is* stdout), stderr is bound to NUL and `_dup2`ed onto stdout's
  descriptor; a stream reopen that fails must not be written to (the CRT stops the program).
  The dev build lmrecomp keeps its console.
- **Damaged settings.ini.** One with binary data (NUL bytes) is moved to `settings.ini.corrupt`
  and the defaults are used. Bad values in a text file were already clamped and unknown keys ignored.
- The GX FIFO itself no longer stops on malformed data (docs/RENDERING.md "Malformed FIFO data").
- **Hung starts** (issue #14): the hang watchdog (`crash_handler.cpp`, always on) logs
  `[hang] ... (<phase>)` to stderr and `last_run.log` when the main thread makes no progress for
  15 s, and while the run is still `starting` records "the game stopped responding: no progress for
  N s during <phase>" as its reason, so the prompt after a killed start names it (cleared if it
  continues). A saved anti-aliasing level the graphics API cannot create no longer stops the start
  at all (docs/RENDERING.md "Anti-aliasing sample counts").

Test aids: `LM_RECOVERY=<0 normal | 1 reset settings | 2 rebuild | 3 saves aside>` shows no dialog
and takes that choice (automated runs, `LM_BACKGROUND`, never show it either);
`AURORA_FIFO_ERROR_LIMIT=3 LM_FIFO_CORRUPT=650:junk,651:junk,652:junk,653:junk` produces a fatal
error in play.

## Updates

`src/port/update.cpp` (UI in `src/recomp/menu.cpp`, F1 > Game > *Updates*). The player always
clicks: nothing is downloaded or replaced on its own.

1. **Version.** The CMake project version (`project(lmport ... VERSION x.y[.z])`) is compiled into
   `update.cpp` as `LM_VERSION` and shown in F1 > Game. Versions compare numerically by
   component, missing components are 0: 1.1 > 1.0.1 > 1.0 = 1.0.0.
2. **Check.** In the background at startup (setting *Check for updates at startup*,
   `[updates] check_at_startup`, default on) and with the *Check for updates* button: one
   unauthenticated `GET https://api.github.com/repos/rob646464/luigis-mansion-recompiled/releases/latest`
   (User-Agent `LuigisMansionRecompiled/<ver> (updater)`, 8 s connect/receive timeouts, 15 s total).
   Drafts and prereleases are ignored. Offline, an unknown host, HTTP errors and the rate limit
   (403 with `x-ratelimit-remaining: 0` or 429; the message names the reset time) end as a line
   in the menu and the log, nothing else. The startup check is skipped in build trees (no
   `llvm/bin/clang` next to the exe) and with `LM_UPDATE_CHECK=0`.
3. **Notice.** A newer release shows a small corner notice for 12 s once per session (takes no
   input) and, in F1 > Game, "Update available: vX.Y" with the release notes (markdown reduced to
   headings, bullets and text) and *Download and install*, *Skip this version* (remembered as
   `[updates] skip_version=<tag>`: no notice for that tag; *Check for updates* still shows it) and
   *Later* (hidden until the next start).
4. **Download.** The asset for this platform: `LuigisMansionRecompiled-<ver>-windows-x64.zip` /
   `LuigisMansionRecompiled-<ver>-linux-x86_64.tar.gz` / `LuigisMansionRecompiled-<ver>-macos-arm64.tar.gz`
   (macOS: built by GitHub Actions, docs/MACOS.md "Releases"; v1.0 and v1.0.1 used the prefix
   `LuigisMansionPC-`, still accepted). Its URL must be the repository's release download
   (`https://github.com/rob646464/luigis-mansion-recompiled/releases/download/<tag>/<name>`, or the
   old repository name); redirects are followed by hand, at most 6, HTTPS only, to `github.com`,
   `objects.githubusercontent.com` or `release-assets.githubusercontent.com`. Progress and *Cancel*
   in the menu. Integrity: the API asset `digest` (`sha256:<hex>`), else the SHA-256 in the release
   notes' download table row for that file; a mismatch or no hash at all refuses the package (it is
   deleted) and offers the release page.
5. **Stage.** Before downloading, the install folder must be a release folder and writable (a probe
   file); otherwise (e.g. Program Files) the menu explains and offers *Open download page*. The
   package goes to `<install>/upd/` (up to 1.1.1: `update.tmp/new/<package folder>/`, see "Long
   paths"), is listed and unpacked there with the system `tar` (Windows 10 1803+
   `System32\tar.exe`, bsdtar, reads zip; Linux GNU tar) with `--strip-components 1` when every
   member is in one top folder, so `upd` holds the install layout itself; any link in it is
   refused, and the folder holding the program (`upd`, or its single subfolder for a package that
   was not flattened) must contain the program, `pipelines.bin`, `llvm/bin/clang` and `lld-link` /
   `ld.lld`.
6. **Install** (*Install and restart*, confirmed in a popup that says to save first; the game state
   is not saved): every file of the package is moved over the install: an existing file is first
   renamed to `<name>.old` (Windows cannot overwrite a running exe or a loaded DLL, but can rename
   them), then the new one is moved in; any failure moves everything back. Linux deletes the `.old`
   files at once and sets the executable bits of the program, launcher, clang and ld.lld; Windows
   lists them in `<install>/update-cleanup.txt` and the next start deletes them (retrying for 10 s
   while the old process exits). `upd` is removed right after the move (a leftover one, or an old `update.tmp`, e.g.
   from a cancelled or failed run, at the next start). Then settings are saved and the program
   restarts with the same arguments: Windows starts the exe at its original path and exits; Linux
   `execv`s the installed file in place of the running process (same PID, so the launcher script
   keeps logging). Files the new package no longer has are left alone, except the 1.0.x Linux
   launcher `LuigisMansionPC.sh` (now `LuigisMansionRecompiled.sh`): at every start of a release
   folder (so right after the update's restart, and also when a new package was unpacked over an
   old folder) an unmodified one (SHA-256 of the shipped file) is replaced by a three-line
   forwarder to the new launcher, so desktop entries keep working; an edited one is kept as it is.
7. **Data stays.** `%APPDATA%` / the XDG folders (settings, saves, install.ini, game modules,
   overlays, shader caches), and in portable mode the `portable` folder next to the program (package
   entries under it are skipped, "Portable mode"), are never touched by the updater (the new version's first start only
   renames a 1.0.x "Luigi's Mansion PC" folder, see "What lives where"). The module key includes the program version, so the
   first start of a new version rebuilds the game module automatically (about 20 s, setup's
   progress screen, no clicks), even when the embedded sources it hashes did not change.

Every step logs a `[update]` line.

**Release contract** (tools/release_windows.ps1, tools/release_linux.sh): tags `v<version>`, asset
names as above (one top folder or the files at the top, the `cmake --install` layout), the
`| \`<asset>\` | <platform> | \`<sha256>\` |` download table in the notes (GitHub's own `digest`
covers assets uploaded since 2025, the table is the fallback), and no prerelease/draft flags for a
release players should get.

**Testing** (scratch `APPDATA`, `LM_CARD_DIR`, a scratch install from `cmake --install`):
`LM_UPDATE_VERSION=<version>` pretends to be that version; `LM_UPDATE_FEED=<file, file:// URL or
URL>` replaces the API answer with a saved release object or array (`http://` only for
`localhost`/`127.0.0.1`); assets must still come from the real repository. The menu can be driven
with `LM_MENU_CLICK="60:open=Game;90:Download and install;120:Install and restart;130:Install and restart##confirm"`.
Checked (October 2026), Windows: pretending 1.0 against the real v1.0.1 (detect, notice, download
87.7 MB via release-assets.githubusercontent.com, digest verified, staged, installed, restarted into
v1.0.1, which rebuilt its module and reached the title screen); our build installed over that
folder deleted the 14 `.old` files at its start; a feed with a wrong digest (refused, file deleted),
no digest but the notes' table (verified from the notes), no hash at all and a non-GitHub asset URL
(both refused before downloading); prerelease and draft entries with higher tags ignored; offline
(unresolvable host, refused connection), rate limit 403/429 from a mock server; *Skip this version*
(no notice at the next start, shown again by the button); cancel mid-download (staging removed);
read-only install folder (message + download page); the current version against v1.0.1 ("latest").
Linux (WSL Ubuntu 24.04, Xvfb, CPU Vulkan): a scratch install of this build pretending 0.9 against a
saved feed of the real v1.0 release (the one with a Linux package): libcurl.so.4 loaded, 151.5 MB
downloaded via release-assets.githubusercontent.com, digest verified, `tar -xzf` staged, 9 files
replaced (executable bits kept, no `.old` left), `execv` into v1.0 (same log), which rebuilt its
module and ran to frame 1500 (no `update.tmp` or cleanup list left). A run that hit a DNS outage in
WSL mid-way ended with "The download failed: could not resolve the server name (offline?)" and the
game kept running.

## Long paths (Windows)

The bug (1.1 -> 1.1.1 from a deep folder): "The package does not contain the game program." The
staged `<install>\update.tmp\new\LuigisMansionRecompiled-<ver>-windows-x64\LuigisMansionRecomp.exe`
was 267 characters; std::filesystem in a process without long path support reports such an
existing file as missing. The 1.1 exe had no long path manifest at all, so it failed even with the
system policy on.

What the program does now:
- **Manifest.** `packaging/windows/longpath.manifest` (`longPathAware`) is merged into the default
  manifest of the Windows executables (CMake `lm_runtime_target`). Windows honours it only when the
  policy `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled` is 1; the log says
  `Windows long paths on/off` (`port::longPathsEnabled`, ntdll `RtlAreLongPathsEnabled`).
- **Extended paths.** `port::longPath(p)` (`src/port/install.h`) gives `\\?\C:\...` /
  `\\?\UNC\...` (absolute, normalized, backslashes), which every file call takes at any length
  with the policy off too; `port::plainPath` / `displayPath` drop the prefix for messages, logs and
  other programs' arguments (clang, lld-link and tar reject `\\?\`). Used by the updater (all of
  it: staging, download, unpacking checks, `.old` renames, moves, rollback, cleanup; `installDir()`
  is extended, package-relative names are joined with `make_preferred`, "inside the install" is a
  lexical check because `weakly_canonical` drops the prefix), the module build (sources, objects,
  logs, `module.key`, the module rename; the module is loaded by its extended path), the Hidden
  Mansion overlay (setup's writer and the game's reader, now UTF-8), `install.ini`, `settings.ini`
  and `pipelines.bin` past MAX_PATH. A restart by the updater uses the extended program name only
  past MAX_PATH (a program started that way sees the longer name as its own path).
- **Short staging.** The package is unpacked into `<install>\upd\` with its top folder flattened
  (above): the deepest staged file is 4 characters longer than its installed path. tar.exe is not
  long-path aware and runs in `upd` (its working directory must stay below MAX_PATH): a game folder
  whose `upd` would be longer than 247 characters gets "The game folder's path is too long for the
  updater (...). Move the game to a shorter folder ..." and the download page, before downloading.
- **Compiler.** clang.exe and lld-link.exe (LLVM 22) take plain paths of any length as arguments
  (LLVM widens them itself) but stop with "The data area passed to a system call is too small" when
  their *own* path is too long (257 characters run, 259 fail; also when started by an extended
  name, which adds 4). Setup starts them by their plain path, and from a game folder where they are
  deeper than 250 characters it hard-links them (copies them, across drives) into
  `<dataDir>\llvm\` and runs them from there (refreshed when size or time differ, e.g. after an
  update). If that is still too long: "Move the game to a shorter folder".
- **Game folder limit.** From a folder deeper than 234 characters the prebuilt Dawn / Direct3D 12
  start-up stops without a word (measured on 1.1.1 and this build, policy on or off: 234 runs,
  236 and deeper end at the adapter request), so the program checks first and says "The game's
  folder ... is N characters long ... Move the game's folder to a shorter path" (message box, exit
  1; `port::installPathProblem`). Every installed file is then below MAX_PATH (deepest
  `llvm\bin\lld-link.exe`, folder + 22); staged and `.old` files may exceed it and use `\\?\`.
- **Data folder.** Setup's own files work in any `%APPDATA%` depth. Aurora (memory cards, Dawn and
  pipeline caches, imgui.ini) uses plain paths: with long paths off and a data folder longer than
  259 - 52 characters (deepest: `USA\Card A\01-GLME-LUIGI_MANSION_SAVEDATA_v3.gci`, the module build's
  obj files), the start shows a warning that saving and caches may fail and how to fix it
  (`port::dataPathProblem`; not in automated runs, logged always).

Checked (October 2026, Windows 11, policy on; "no manifest" = this build with the manifest
replaced by one without `longPathAware`, which behaves as policy off), all with the real v1.1.1
release and `LM_UPDATE_VERSION=1.1`:
- game folder of 222 characters (the old staging path would be 300): setup built the module, the
  update downloaded, verified, staged in `upd` (top folder flattened), installed 14 files and
  restarted into v1.1.1, which removed the `.old` files;
- no manifest, 234 characters (staged `upd\llvm\bin\lld-link.exe` 260): setup linked clang/lld-link
  into the data folder (253/256 characters) and built the module; the update installed and
  restarted. (The restarted 1.1.1, still without long path support, then failed its own next
  download in `update.tmp`, and left `.old` files past MAX_PATH it could not see: the 1.1.1 bug.)
- normal folder (145): unchanged behaviour, update installed and restarted;
- `%APPDATA%` of 215 characters (data folder 242), both discs: with the manifest everything works
  (two modules, Hidden Mansion overlay found, caches); without it setup still builds both modules
  and the overlay, the game runs, the warning is shown and Aurora's SQLite caches fail as it says;
- game folder of 240: the message, exit 1 (1.1.1 there: silent stop at the adapter request); 234
  runs.

## Module boundary

```
LuigisMansionRecomp.exe                     module.dll (no imports, no CRT)
  runtime, HLE, scheduler, Aurora   <---->    f_XXXXXXXX (recompiled functions)
  RuntimeApi table (module_abi.h)  ---init-->  module_rt.cpp: globals, forwarders
  g_module (funcs/syms/orig tables) <--ret--   func_table.cpp: kGameModule
```

- The module exports one function, `lm_module_init(const RuntimeApi*)`, and returns its
  `GameModule` (function table, symbol table, hooked originals). The runtime calls game
  code only through those tables (`Ctx::call`, `guestSymbol`, `callOriginal`).
- `ctx.h` is **freestanding**: no system headers, only compiler builtins
  (`__builtin_memcpy`, `__builtin_bswap32`, `__builtin_fma`, `__builtin_bit_cast`, ...),
  so the bundled clang needs no SDK or C++ library. Paired-single quantization is inline.
- Hot paths stay inside the module: memory access inlines against a module-local copy of
  `g_mem` (set once at init; it never changes), the trace/watch flags are copied, the
  interrupt flag is read through a pointer to the runtime's flag (one load at loop
  back-edges), and indirect calls binary-search the module's own table. Everything else
  (MMIO, interrupts, HLE lookups, SPRs, tracing, fatal errors) goes through the
  `RuntimeApi` function pointers.
- The module is compiled with `-ffreestanding -nostdinc -fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-math-errno -fno-stack-protector -funwind-tables` and
  linked `/dll /noentry /nodefaultlib`; `module_rt.cpp` provides `memcpy`/`memset`/
  `memmove`/`memcmp`, `_fltused`, `__chkstk`, and `fma`/`trunc`/`nearbyint`/`sqrt`
  (forwarded to the runtime's CRT).
- Unwind tables are required: the runtime ends guest threads (OSExitThread, OSCancelThread,
  shutdown) by throwing `sched::ThreadExit` through the module's frames, and Windows can only
  unwind frames that have `.pdata`. Without them closing the window ended the process with
  0xE06D7363 (an exception that could not be unwound). Stack probes stay on (`__chkstk` in
  `module_rt.cpp`, the CRT algorithm): a generated function with a frame above 4 KiB would
  otherwise step over the stack guard page. Both were found in the SMS port.
- On exit, `frame::stop` waits (at most 2 s) for the guest threads to unwind before Aurora
  and audio are torn down; the thread running guest code only leaves at its next present or
  scheduling point.
- Bump `kModuleAbiVersion` (ctx.h) when `Ctx`, `GameModule` or `RuntimeApi` change.

### Developer builds

- `lmrecomp` (unchanged workflow): `tools/build.ps1 -Target lmrecomp`. The build runs
  `lmrecompile` (the same recompiler) on `LM_DOL` and links the generated code statically
  (`LM_STATIC_MODULE`): full debug info, no setup. `-DLM_DOL_VERSION=GLMP01` with the
  European `main.dol` builds the European game.
- `LuigisMansionRecomp`: the shipped program; in a dev tree it finds the LLVM that built
  it, so it runs from `build/windows-clang` as is.
- The Python recompiler is gone; `src/recompiler/recompiler.cpp` is the only one
  (`tools/recomp/lmrecompile.cpp` is its command line).

## The European (PAL) game as the base

The European release is recompiled from its own `main.dol` with
`decomp/config/GLMP01_00/symbols.txt`. That config has no translation-unit splits and
names few functions, while the runtime finds SDK functions (HLE) and hooks by their USA
names. `tools/recomp/GLMP01_map.txt` (generated by `tools/recomp/gen_version_map.py`
from the user's dumps, committed like the decomp symbol maps, embedded in the exe)
supplies, per European function address, the canonical name and translation unit:

- SDK functions: matched to their USA counterparts with `tools/pal_diff.py`, then named by
  call-site agreement (matched callers whose `bl` sequences line up), plus a short list of
  hand-checked overrides for the SDK units the European release reorganised (PAD, VI
  callbacks, `OSGetLanguage`).
- Game functions the European map leaves unnamed but the USA map names (e.g. JAudio's
  `DspSync__Fv`, used by the audio HLE).
- Hooks tagged `pal` in `tools/recomp/hooks.txt`.

Differences the runtime handles for the European game:

- **Frame pacing.** The European `LMDisplayUtil` paces frames with an `OSAlarm` of N/60 s
  (the USA game counts N VI retraces) and re-submits the framebuffer from its post-retrace
  callback every retrace. The runtime HLEs `OSCreateAlarm`/`OSSetAlarm`/`OSSetAbsAlarm`/
  `OSSetPeriodicAlarm`/`OSCancelAlarm` (an alarm re-armed from its own expiry counts from the
  expiry, so host latency does not slow the game) and presents a frame at `GXCopyDisp` instead of
  `VISetNextFrameBuffer`. Alarm times are signed (OSTime): a delay of zero or less, or an
  absolute time already passed, fires as soon as possible (the European `SITransfer` passes
  `time - now` to `OSSetAlarm`, which can be negative; the USA one calls `OSSetAbsAlarm`, which
  the European SDK does not have); huge delays are capped (~115 days) before the conversion to
  host clock units, which overflowed before (x86: fired at once by luck, arm64: never). A
  periodic alarm whose start is in the past keeps its phase (first expiry at
  `start + period * ((now - start) / period + 1)`, missed periods skipped, as in the SDK). No
  known game path hits these cases (the pacer's delays are constant and positive; the SI and
  periodic alarms are unused with the PAD HLE'd). Test aid `LM_ALARM_TEST_PAST=<n>[,<ms>]`: the
  first n alarms are set ms (default 100) in the past; the alarm log marks them `past`.
- **50 / 60 Hz.** F1 > Visuals > *PAL refresh rate*: 50 Hz (default, the console's PAL
  video timing) or 60 Hz (PAL60). Because the European game times its frames with the
  alarm, the logic speed is the same either way (30 fps, like the USA game); the setting
  changes the VI retrace (`VIGetTvFormat`, retrace callbacks, `VIWaitForRetrace`).
- **Language.** The game asks `OSGetLanguage` when its save has no language yet; F1 >
  Game > *Language* answers it (English, German, French, Spanish, Italian). A save keeps
  its own language (changeable in the game's Options). The `.sdata` language variable
  (0x804BD658) is also initialised from the setting.
- **Boot values.** Disc ID, `.ctors` start, arena start (from `__init_registers`), arena end
  (below *Guest memory*), `VI_PAL` TV format.
- **Hidden Mansion.** The European game has its own; with both discs the USA rules can be
  chosen instead (below, *USA Hidden Mansion on the European game*).

### Guest memory (both releases)

| Range | Contents |
|-------|----------|
| 0x80000000-0x80003100 | OS globals (boot info, clocks, ...), written by `OSInit` (hle_os.cpp) |
| 0x80003100-arena start | the DOL's sections, the boot stack |
| arena (OSGetArenaLo..Hi) | the game's heaps (JKRHeap root and children) |
| (0x81800000 - FST max length) & ~31 .. 0x81800000 | the FST's place on the console (US 0x817FAE80, PAL 0x817FAA80): arena high as the apploader sets it (0x80000034/38/3C) |
| 0x81800000-0x81804000 | locked cache (0xE0000000) |
| 0x81810000-0x81812000 | model viewer scratch |
| 0x81820000-0x819A0000 | host-made guest objects (`hleAlloc`: boot thread, interrupt stack, card/DVD scratch, GX FIFO object, unused models) |
| 0x819A0000-0x81F00000 | unused content's host-built data (`unused2::extraAlloc`: the Mario model, ghost placements, pictures, strings). From 1.1.1 to 1.1.3 both allocators started at 0x81820000 and overlapped: Play as Mario's model was built over hleAlloc's objects (malformed FIFO data in the nursery). |
| 0x81F00000-0x84000000 | early Luigi's model copies, the model viewer's heap |

Up to 1.1 the arena ended at 0x81700000 (the top MiB of MEM1 held `hleAlloc`), so the game
had 1 MiB less heap than on the console. Most of the game fits, but the first E. Gadd event
(event01, after the Parlor door) was about 20 KiB short: the decompression buffer of
`/model/demobak1.szp` (0x4E280 bytes) could not be allocated, and the game's loader, which does
not check, decompressed it to address 0, over the OS globals. On the European game that zeroed
the bus clock (0x800000F8) its frame alarm is computed from, so from then on every frame ended at
once (60-180 frames/s of game logic, user reports of 100 -> 10 fps at that cutscene); on the USA
game the same low memory was overwritten. With the console's arena the event's allocation has
about 1 MiB to spare (largest free block 0xDBF30-0xEA290 at that point).

Safety nets (logged as `ERROR`): a failed game heap allocation (`alloc__10JKRExpHeapFUli` hook,
tags.cpp; `LM_HEAP_LOG=1` logs every allocation and free with the heap's free total and largest
block), a DVD read or the loaders' Yay0 decompression (`fn_800071D0`) aimed at low memory or
outside guest memory is refused (hle_dvd.cpp), and the OS globals the game reads at run time (bus
and core clock, memory size, TV format) are checked every frame and restored (`verifyLowMemory`).
`LM_ALARM_LOG=1` logs every OS alarm (signed ticks, delay, `past` / `periodic`); `LM_WATCH=80000000,100` shows any store to
the OS globals after `OSInit`.

### Version-aware code

Host code (hooks, HLE, menu features) is written once, against the USA game, and runs on
both releases:

- **Hooks.** `tools/recomp/hooks.txt` lines are USA symbols, optionally followed by
  versions: `<symbol>` (USA only, the default), `<symbol> us pal` (both), `<symbol> pal`
  (European only). A function only the European build has is written `palfn_<PAL address>`.
  `gen_version_map.py` maps each `pal`-tagged hook to its European address (the matched
  function, given the USA name in `GLMP01_map.txt`) and fails if it cannot. Tag a hook
  `pal` only after checking the European function (`tools/pal_diff.py show <fn>`): a
  `layout` or `modified` pair can move struct fields the handler uses.
- **Functions the runtime calls by name** (`guestSymbol`, `GU_SYM`, `callOriginal`) get the
  same USA name in the European module: `gen_version_map.py` scans `src/recomp` and
  `src/port` for those names and writes a `<PAL address> <USA name> -` line for each.
  The recompiler renames an unrelated European function that happens to carry the same
  name (`pal_fn_...`), so names stay unique.
- **Addresses.** Wrap every USA data or code address in `VA(0x...)`
  (`src/recomp/version.h`). On the USA game it is the address itself; on the European game
  it is looked up in `src/recomp/version_addrs.inc`, which `gen_version_map.py` generates
  by scanning the sources for `VA(...)`: a data address is mapped through the matched
  functions that reference it (or another address of the same object, a literal with the
  same value in the matched function, or the pointer table that points at it); a code
  address (e.g. a return address checked in a hook, or a function's last instruction) is
  the same instruction in the matched European function. The script fails on an address it
  cannot map. Each row says how it was mapped.
- **Struct offsets** are not generated: `VOFF(usa, european)` after checking the field with
  `tools/pal_diff.py field` / `show` (e.g. the camera's viewport fields moved by +0xC,
  Luigi's fields from +0x134C on by +4).
- `ver::pal()` tells which game runs. Code that still uses raw USA addresses must check
  `port::usCodeFeature()` and stay inert on the European game; F1 greys such options with
  "applies when playing the USA version" (or "needs the USA disc").

The recompiler fails (setup shows the error) when a `hooks.txt` line for the release being
built names no function of it (e.g. a hook tagged `pal` without a re-run of the script below),
or when a `patch` address lies outside every function: a hook that silently never runs cannot
ship.

Workflow after adding a hook or a `VA()`: `python tools/recomp/gen_version_map.py`
(needs both dumps; about 10 s), commit `GLMP01_map.txt` and `version_addrs.inc` (address
metadata only, like the decomp symbol maps). The module cache key covers the map and
hooks.txt, so users' European modules rebuild on the next launch.

**For new features (cheat-code ports, debug modes, unused content):** use `VA()` for every
address from the cheat/AR code and `VOFF()` for offsets, tag the hooks `us pal`, re-run the
script, and test on the European build (`-DLM_DOL_VERSION=GLMP01`, below). A feature that
cannot be mapped yet stays USA-only: gate its menu entry with `port::usCodeFeature()`.

### Enhancements on the European game

All enhancements now work on both releases. Checked in the European static build
(`lmrecomp` with `LM_DOL_VERSION=GLMP01`, scratch `APPDATA`/`LM_CARD_DIR`):

| Feature | European specifics | Verified |
|---------|-------------------|----------|
| Skippable cutscenes | scene table has a 9th (language select) scene; ids and event interpreter layout unchanged | opening + mansion entrance skipped; snapshot matches a watched run |
| HUD at the screen edges | element hooks (`fn_8003D634`, `fn_800417F8`, `fn_80045BDC`, `fn_800115F8`, ...) are `us pal`; GBH mode variable via `VA()` | 16:9: health at the left edge, GBH live camera inside the GBH frame at the right |
| Widescreen culling (`fn_80008D58`) | camera viewport x/width at +0x1F8/+0x200 (USA +0x1EC/+0x1F4) | hook runs for every 3D projection |
| Game Boy Horror camera | the European view-1 clear is `fn_8000B7CC` (the matcher pairs the USA `fn_8000B228` with an unrelated full-screen clear); it resets depth with a Z texture, which Aurora now implements, so no hook is needed (the old far-plane workaround is gone) | live view on the GBH screen in the foyer |
| Ghost shading Beta | `fn_800581E8` identical; call site mapped | lighting visibly changes (Boolossus balcony); check in an ordinary room pending |
| Gameplay options | Luigi's riding byte +0x1350; the European Luigi update writes it every frame, so it is set in `fn_800B84E4` (state update) instead; darkness: the game's own hidden-mansion 0.2 is not applied twice | health x2 HUD 200, darkness, hearts, riding |
| Debug: info overlay, particle report, depth blur, warps, model viewer | `LMChangeFrameBuffer` is the European `fn_80007940` (alarm-paced variant) | overlays, warps to maps 5 and 13, viewer (Chauncey) |

### PAL parity status

Every feature was checked on the European base (October 2026): table, fixes and remaining
gates in docs/PAL_PARITY.md. Only the leftover-room visits stay USA-only (greyed in F1).
European specifics found there: the double-buffered frame end drops frames unless the
previous buffer is reported as shown (`palfn_80007844` hook), interpolation tapes are stamped
with the game's N/60 s frame period, and instruction patches need their own `pal` lines.

### USA Hidden Mansion on the European game

With both discs installed, E. Gadd's lab offers both Hidden Mansions on either base
(docs/HIDDEN_MANSION.md, *E. Gadd's lab menu*); F1 > Enhancements > *Hidden Mansion
version* is the fallback when the lab menu is not installed. On the European base, the USA
rules work like the forward direction in reverse:

- **Data** (built by setup next to the forward overlay, `hidden_mansion/usa/`; dev builds:
  `tools/hidden_mansion_data.py`): the USA `/Map/map2.szp`, served when the European
  `MissionMode::init` loads `/Map/map2u.szp`; the European `event08/16/73.szp` with only
  the script (`text/eventNN.txt`) swapped for the USA one (the message tables of every
  language stay).
- **Code.** The European game reads the hidden-mansion flag in 36 functions. Those whose
  only flag use is a PAL change are hooked (`pal` tag) and run with the flag reading 0
  for the duration (`hm::callWithoutFlag`): the screen mirror, stick and HUD mirroring, the
  iris wipe, the GBH map mirror, the enemy strategy timers, Chauncey's balls / rocking
  horses / approach, Bowser's volley flip, the damaging-floor pattern and the JAudio
  stereo swap. Functions where the USA game reads the flag too (damage x2 `fn_800B9680`,
  vacuum `fn_80081BCC`, the GBH screen `fn_8003DF88`, `fn_800DBE8C`) and the heart hooks
  run with the real value even when reached from inside a hidden call
  (`hm::callWithFlag`). The heart thresholds go back to the USA 400 (European 1000), the
  room-ambient factor (European `.sdata` 0x804BD728, 0.2) is set to 1.0, and Luigi's
  Boolossus movement mode is kept off in his state update.
- The scene-start / event-interpreter / new-game code (which sets or clears the flag) and
  the map loader are left alone.

Limitation: the hiding is a write to the flag word, so a guest thread switch inside a
hidden call (an interrupt waking a higher-priority thread) lets that thread see the USA
value too; only flag readers with USA hidden-mansion behaviour could be affected, and the
ones found are all on the main game thread.

## Feature gating (F1 menu)

`port::Avail` (`src/port/install.h`):

| Feature | USA base | European base |
|---------|----------|---------------|
| Skippable cutscenes, HUD at screen edges, widescreen culling, GBH camera/clock/tracker, ghost shading, unused content, gameplay options, debug tools | yes | yes |
| Leftover-room visits (Unused content > Unused rooms) | yes | *not available on the European version yet* |
| Hidden Mansion: European rules | with both discs (overlay) | native |
| Hidden Mansion: USA rules | native | with both discs (reverse overlay) |
| PAL refresh rate, language | *European version only* (greyed on a USA-only install) | yes |

Rules: a single-disc install greys out what needs the other disc, with the reason
("needs the European disc: F1 > Game > Add or change discs"). With both discs nothing is
greyed out: options of the other version stay selectable and say "applies when playing
the USA/European version"; switch with F1 > Game > *Version to play* (restarts the game).

Runtime safety: hooks without a `pal` tag are not compiled into the European module, and
code with raw USA addresses checks `port::usCodeFeature()`.

## Not done yet / design notes

- Linux: supported (`module.so` via `ld.lld`, XDG folders, bundled official LLVM clang/lld;
  docs/LINUX.md). macOS: not done (the module build has no Mach-O path).
