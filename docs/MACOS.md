# macOS port

The port builds and runs natively on macOS (Apple silicon): the same runtime, recompiler and
setup as on Windows and Linux, Aurora on Dawn's Metal backend, SDL3 for the window, input,
audio and file dialogs. The game module that setup compiles from the user's disc is a Mach-O
`.dylib` built with clang + ld64.lld. macOS shares the POSIX code paths of the Linux port
(docs/LINUX.md); the differences are `#ifdef __APPLE__` and the `macos-clang` preset.

Status (2026-10-24, M3 Pro, macOS 27, tested by the PR author, NitrosGits; merged from PR #31
on 2026-10-26): `LuigisMansionRecomp` builds, setup recompiles the USA disc into
`module.dylib`, the game boots to the title screen; the CI package installs and builds the
module with its bundled clang. Not yet checked: gameplay beyond the title screen, the PAL disc,
the `lmrecomp` dev build.

**We (the project) never build or run macOS ourselves.** The macOS package is built and packaged
by GitHub Actions only, from the same commit as the Windows/Linux release, and attached to the
release by `tools/release_windows.ps1` ("Releases" below). It is unsigned and not notarized.

## Building

Xcode (Apple clang builds the program) and Homebrew:

```
brew install cmake ninja llvm lld
cmake --preset macos-clang          # build/macos-clang
cmake --build build/macos-clang --target LuigisMansionRecomp
```

The program links only macOS's own libraries (system zlib, sqlite3, libc++ and frameworks), so
it runs on a Mac without Homebrew. Aurora fetches Dawn (`dawn-darwin-arm64`) and nod
(`macos-arm64`) as prebuilt static libraries and builds SDL3, abseil, fmt, xxhash, libpng,
freetype and zstd from source: the preset sets `AURORA_SDL3_PROVIDER=vendor`,
`CMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local` (no Homebrew packages through
`find_package`) and `CMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON` (no pkg-config fallback; an
environment variable in the preset would not survive the re-configure `cmake --build` runs). Apple's clang has no lld, so the game module
is built with Homebrew's `clang` (llvm) and `ld64.lld` (lld): CMake finds them
(`LM_DEV_CLANG`, `LM_DEV_LD64_LLD`) and passes both folders to the program
(`LM_DEV_LLVM_DIR`, `LM_DEV_LLD_DIR`); `findToolchain` takes clang and ld64.lld from
different folders on macOS. `LM_LLVM_DIR` overrides at run time. The `clang` first on
`PATH` may be another toolchain (llvm-mingw): the preset names `/usr/bin/clang` explicitly.

## Runtime differences from Linux

| Area | Linux | macOS |
|------|-------|-------|
| Game module | `module.so`, `ld.lld -shared` | `module.dylib`, `ld64.lld -dylib -arch arm64 -platform_version macos 13.0 13.0 -undefined error -dead_strip` (ad-hoc signed by lld, required on Apple silicon) |
| Settings / install.ini, modules, cards | `~/.config`, `~/.local/share` | `~/Library/Application Support/Luigi's Mansion Recompiled` (both) |
| GPU caches | `~/.cache` | `~/Library/Caches/Luigi's Mansion Recompiled` |
| Own path | `/proc/self/exe` | `_NSGetExecutablePath` (`port::selfExePath`) |
| Crash reports | pc from `gregs[REG_RIP]` | pc/sp from `uc_mcontext->__ss` |
| Thread dumps (`[hang]`, `LM_HANG_DUMP`) | `/proc/self/task` + `tgkill` | `task_threads` + `pthread_kill(SIGUSR2)` |
| Thread policy (`[cpu]`) | none | guest threads QoS user-interactive (performance cores); Aurora's threads keep SDL's pthread priority (macOS allows no QoS class after one: logged as such) |
| Updater | libcurl.so, `-linux-x86_64.tar.gz` | `/usr/lib/libcurl.4.dylib`, `-macos-arm64.tar.gz` (attached to releases from CI from the first release after the merge) |
| `LM_OFFSCREEN=1` | window moved off every display | window made transparent and click-through (macOS clamps positions onto a display) |

The XDG variables still override the macOS folders, so test runs use scratch folders the same
way as on Linux: `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME`, `LM_CARD_DIR`.

### Retina: "Match window" renders the window's size in points

Aurora creates the window with `SDL_WINDOW_HIGH_PIXEL_DENSITY`, so on a Retina panel the drawable is
the point size times the display scale (2x) in each direction. "Match window" (`resolution_scale=0`)
used to hand that drawable size to aurora, i.e. the default 1280x960 window rendered 2560x1920, 4x
the pixels of every 1x display at the same window size. On a base M1 (7-core GPU) that is the whole
GPU budget: the foyer held 30 fps but a heavier room fell to 20 (GPU busy ~105 ms per frame at
2560x1920 against ~22 ms at 1280x960), and the menu showed only "Match window". The port now passes
the window's point size (`src/recomp/menu.cpp`, `applyResolution`), keeps it in step when the window
is resized, logs the buffer in use (`[video] rendering 1280x960, match window (window 1280x960
points, 2560x1920 pixels)`) and shows it in the F1 slider label. 1x displays are unaffected; a fixed
Nx still renders N x 480 lines, so the drawable's own pixels remain one slider step away (a
1280x960-point window needs 3x).

### Floating point on arm64

arm64 compilers fuse `a*b+c` into one `fmadd` by default (`-ffp-contract=on`); x86-64 builds
without `-mfma` never do. The PowerPC rounds a separate `fmul`/`fadd` twice, and the recompiler
emits the real `fmadd` instructions as `fma_` explicitly, so the game module and the runtime
(HLE PSMTX/PSVEC math, lmrecomp's generated code) are built with `-ffp-contract=off` on macOS.
Float-to-int conversions (`fctiw`, quantized stores) clamp explicitly, so arm64's saturating
converts change nothing.

### Thread stacks

Every guest call is a host frame. `std::thread` gets 512 KiB on macOS (Windows: 1 MiB, Linux:
8 MiB), so the threads that run guest code (guest threads, boot thread, interrupt timer) are
started by `sched::startHostThread` with 8 MiB stacks on POSIX.

## Packages

`tools/package_macos.sh <LLVM bin dir> [out dir]` builds, installs (`cmake --install
--strip`; the ad-hoc signature survives) and packs `LuigisMansionRecompiled-macos-arm64`
(.tar.gz/.zip): `LuigisMansionRecomp`, `pipelines.bin`, `README.txt`
(`packaging/macos/README.txt`), `NOTICE.md` and `llvm/bin/clang` + `llvm/bin/ld64.lld` from the
official LLVM release for macOS arm64 (`LLVM-<ver>-macOS-ARM64`; Homebrew's clang needs
`libLLVM`/`libclang-cpp` next to it). The script fails when any binary links a library outside
`/System/Library` or `/usr/lib` or has an invalid signature, and it builds a test game module
with the bundled clang/ld64.lld the way setup does and loads it.

GitHub Actions (`.github/workflows/build.yml`) builds the three platforms on pushes to `main`,
on `v*` tags, on pull requests and on demand (`gh workflow run build.yml --ref <branch>`; other
branch pushes do not run it), and uploads one artifact per package, named like the file:
`LuigisMansionRecompiled-<version>-windows-x64.zip`, `-linux-x86_64.tar.gz`,
`-macos-arm64.tar.gz` (the updater's asset suffix). Windows uses the runner's LLVM; Linux and
macOS the official LLVM 23.1.4 archives, checked against their sha256 (GitHub's digests of the
release assets, `LLVM_SHA256_*` in the workflow; update them with `LLVM_VERSION`) and cached.
The workflow's token is read-only (`permissions: contents: read`), third-party actions are
pinned to a commit (`ilammy/msvc-dev-cmd`), `ninja` to a version.

The packages are **not signed and not notarized** (no Apple developer account). Gatekeeper
stops a downloaded copy until the player allows it once: right-click `LuigisMansionRecomp` >
Open > Open, or `xattr -dr com.apple.quarantine <folder>` (packaging/macos/README.txt, and the
release notes say so). Setup then clears the quarantine flag of the bundled clang/ld64.lld before
starting them (`removexattr` in `buildModule`, only for tools in the package's own `llvm/bin`;
a toolchain from `LM_LLVM_DIR`, Homebrew or `PATH` is left alone).

## Releases

The owner releases from Windows with `tools/release_windows.ps1`; nobody has to build on a Mac:

1. The release commit has to be on GitHub: the script compares `origin/<branch>` with `HEAD` and
   offers to push (a push to `main` starts the build workflow, which then runs while Windows
   and Linux build locally).
2. Windows zip (local build), Linux tar.gz (WSL, `tools/release_linux.sh`), as before.
3. macOS: the script finds the `build.yml` run of that commit (`gh run list --workflow build.yml
   --commit <sha>`; none: starts one with `gh workflow run`), waits for it (one status line per
   minute), and requires the `macOS arm64` job to have succeeded and the run to have built
   exactly that commit. It downloads the artifact `LuigisMansionRecompiled-<ver>-macos-arm64.tar.gz`
   (`gh run download`) and checks it: one file of that name, 20 MB - 1 GB, a readable .tar.gz
   whose entries all sit in `LuigisMansionRecompiled-macos-arm64/` and include
   `LuigisMansionRecomp`, `pipelines.bin`, `llvm/bin/clang`, `llvm/bin/ld64.lld`, `README.txt`
   and `NOTICE.md`. Its sha256 is printed, written to `out/<package>.sha256` and goes into the
   release's download table (row "macOS 13+, Apple silicon (M1 or newer), Metal; unsigned").
4. If the run failed, the artifact is missing or a check fails, the script says why and asks
   whether to publish without macOS (the notes then say "macOS: no <ver> package").
5. `gh release create` tags the built commit (`--target <sha>`) and uploads all packages.

The release notes template gets the macOS note through `{{MAC_NOTE}}` where it has one, else
right after `{{LINUX_NOTE}}` (older templates): with a package, that it is unsigned/not
notarized and how to allow it; without, that there is no macOS package for this version.
`-SkipMac` leaves macOS out; `-MacCheckOnly` only fetches and checks the macOS package of `HEAD`
(nothing built or published; a dry run, also for a branch: push it, then the script starts the
workflow there).

## Open

- Setup's module build: 28.5 s with the package's bundled clang (official LLVM 23.1.4,
  measured from the CI package on an M3 Pro); 352 s with Homebrew's clang in a dev build (slower
  clang, plus setup's nice 10 keeps it on the efficiency cores). Package tested end to end from
  the CI artifact: quarantined tools cleared, module built, game ran.
- Shader preparation on Metal: about 180 s at a start where most pipelines came from the Dawn
  cache (`[precache]` lines: roughly 60 per second loaded), against 0.4-0.6 s warm on Windows.
- Packaging: a plain folder, no `.app` bundle, not signed/notarized. Release upload is wired
  (`tools/release_windows.ps1`, "Releases"); the in-game updater on macOS has not been run.
- Intel Macs: the code has the x86_64 branches but was not built.
