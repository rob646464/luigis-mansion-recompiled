# Texture packs

Replacement textures in Dolphin's format, and a Dolphin-compatible texture dumper for pack makers.
Ported from our City Folk port (same module and Aurora patch there). Code:
`src/recomp/texture_packs.cpp` (packs folder, menu, settings, test aids) on top of Aurora's texture
replacement (`extern/aurora/lib/gfx/texture_replacement.cpp`, upstream) and Aurora patch **0360**
(dumper, cache budget, statistics, use log; generic, identical file in the City Folk repo).

Off by default, and nothing is created on disk until the feature is used: the packs folder only by
"Open texture packs folder", the dump folder only by dumping. With "Use texture packs" off (or no
pack installed) Aurora builds no texture source keys, exactly as before the feature existed.

## For players: installing a pack

1. F1 > Visuals > Texture packs > **Open texture packs folder**. It is `texture_packs` in the data
   folder: `%APPDATA%\Luigi's Mansion Recompiled\texture_packs` (Linux:
   `~/.local/share/Luigi's Mansion Recompiled/texture_packs`; portable mode:
   `<program folder>\portable\texture_packs`).
2. Put each pack in its own folder there. Packs made for Dolphin are copied as they are; any of
   these layouts work (the game ID is the running version's: GLME01 USA, GLMP01 Europe, GLMJ01
   Japan):
   - `texture_packs\<pack>\GLME01\...` (what Dolphin packs usually ship: the folder you would put
     in Dolphin's `Load\Textures`)
   - `texture_packs\GLME01\...` (that folder dropped in directly)
   - `texture_packs\<pack>\Load\Textures\GLME01\...`, `...\Textures\GLME01\...`,
     `...\User\Load\Textures\GLME01\...` (a copy of a Dolphin user folder)
   - `GLM` instead of the full ID, or a marker file `GLME01.txt` / `GLM.txt` in the pack folder
     (Dolphin's conventions for "this game, every region" and "the whole folder")
   - a folder with the texture files and no ID at all: used for every region.
   A pack whose ID folders are all for other regions is listed greyed out ("other region") and not
   loaded. Subfolders inside a pack are fine (searched recursively).
3. Switch on **Use texture packs** and press **Reload packs** (or restart). No restart is needed
   for adding, removing, editing, enabling or reordering packs.

The list shows each pack with an on/off box, its number of textures and up/down arrows: where two
packs replace the same texture, the one higher in the list wins. "Use texture packs" turns them all
off (every texture is the game's own again). Settings: `[texture_packs]` in settings.ini
(`enabled` (default 0), `budget_mb`, `dump_mips`, `pack<N>=<0|1>,<folder>` in priority order; new
packs are added enabled at the bottom).

Files: `tex1_<w>x<h>[_m]_<texture hash>[_<palette hash>]_<format>.png` or `.dds` (DDS: BC1-BC7
and uncompressed RGBA8; mip levels inside the DDS or as `<name>_mip<N>.png/.dds` sidecars;
`_arb` arbitrary-mipmap suffix accepted; `$` wildcards for either hash). Replacements may be any
size (upscaled packs are the usual case). They stream in on background threads: for the first
frames after a texture appears the original is drawn.

The US and European games share most textures (same hashes), so a GLME01 pack mostly works for
GLMP01 too when it is put in a `GLM` folder or has no ID folder; text/logo textures differ.

### Memory

"Texture memory for packs" (512 MB .. 8 GB, default 2 GB) bounds the GPU memory replacement
textures may hold; past it the least recently drawn ones are released and stream in again when
they are next drawn (a short pop-in of the original). Every registered DDS keeps a small
thumbnail (mip tail up to 64x64) in memory for instant pop-in; PNGs have none. The `[mem]` lines
in last_run.log (every 60 s) have a `texture packs:` line: registrations, replacements on the GPU
and their MB against the budget, pending loads, failed files, texture loads replaced, dump counts.

## For pack makers: dumping

F1 > Visuals > Texture packs > For pack makers > **Dump textures** writes every texture the game
loads, once each, as PNG into `<data folder>\texture_dump\<game id>\` (Open dump folder). The
textures already on screen are written at once (the texture cache is re-resolved when dumping is
switched on). The file names are exactly Dolphin's "Dump Textures" names, so an edited (recoloured,
upscaled, saved as DDS) copy dropped into a pack replaces that texture here and in Dolphin.
"Include mipmaps" also writes `<name>_mip<N>.png` for mipmapped textures (Dolphin's "Dump Mip
Maps"). Dumping is not saved: it is off at the next start. Existing files are never overwritten.
Dumping works with "Use texture packs" off. EFB copies (screen copies, the flashlight/self-shadow
maps, the GBH view, the Portrificationizer's screen captures) are not textures and are not dumped,
as in Dolphin.

## How a texture is matched (and why Dolphin's names work)

Aurora computes the same key Dolphin does when a GX texture is first resolved (not per draw; the
texture object and content caches keep it): XXH64 (seed 0) of the **base level** as the game stores
it in memory (block-padded size of the format), the size, the GX format number, and for C4 / C8 /
C14X2 an XXH64 of the palette entries the texture actually uses (the range between its smallest and
largest index). `_m` is Dolphin's "the min filter uses mipmaps" flag (TexMode0 mip bits); the
loader ignores it, the dumper writes it as Dolphin does. The City Folk port checked the names and
pixels against Dolphin's dump (98 of 98 common textures identical, every format but C14X2 covered);
the code is the same here.

## Interactions with the port's other features

- **Silly Luigi colours** (cap/shirt/overalls/shoes/Poltergust, src/recomp/silly.cpp) hand
  Aurora a recoloured copy of Luigi's CMPR textures (GXInitTexObj): the recoloured texture has another
  hash, so a pack's Luigi texture applies only with those colours at their defaults (as in Dolphin
  with a recolour cheat).
- **EFB copies / copies to RAM**: never replaced (resolved before replacements, as in Dolphin).
- **Interpolation**: frames replayed at high frame rates resolve textures from a capture-time copy
  of the texels; the hash is of the content, so the same replacement is chosen.
- **Textures the game rewrites in memory**: their hash changes with their content, so only a pack
  entry for that exact content matches.

## Test aids

- `LM_TEXPACK_DIR=<dir>` packs folder instead of `<data folder>\texture_packs`
- `LM_TEXPACKS=0|1` master switch (overrides the setting)
- `LM_TEXDUMP=1|<dir>` dump from the start (into `<dir>`)
- `LM_TEXPACK_LOG=<s>` the `[mem] texture packs:` line every s seconds (1 = 5 s) and an Aurora info
  line for the first use of each replacement (`texture_replacement: using <file> for <key>`)
- `LM_TEXPACK_AT=<frame>:<action>[,...][;<frame>:...]` (or `|` between groups) at guest frames:
  `reload`, `on`, `off`, `dump`, `nodump`, `budget=<MB>`, `pack=<folder>:<0|1>`, `top=<folder>`
  (live changes as the menu makes them)
- Scratch tools used for the checks (not in the repo): a pack builder that recolours / upscales a
  dump (solid-colour packs for priority checks).

## Verified (2026-11-02, branch lm-116-extras-2, scratch APPDATA / packs, never committed)

Route: `LM_INPUT_CLOCK=frames LM_START_MAP=2 LM_START_APPEAR=6 LM_TEST_STATE=poltergust
LM_DVD_SPEED=1`, Original 30 (`target_fps=30`), the scenarios.sh menu input to the foyer.

- **Off = unchanged**: with the feature off (default), the nursery route's frame dumps 2580-2960
  are byte-identical to main's build; a default run creates no `texture_packs` / `texture_dump`
  folder and writes no `[texture_packs]` section (nothing changed in the menu).
- **Dumper** (`LM_TEXDUMP`): 277 textures from boot to the foyer (I4, I8, IA4, IA8, RGB565,
  RGB5A3, RGBA8, CMPR; LM uses no palette or mipmapped textures in these scenes).
- **Packs** made from that dump: `Swap\GLME01` (all 277, colour-swapped, 2x), `Magenta` (no ID
  folder: the 20 RGB5A3 textures in magenta, e.g. the heart and the GBH icons), `PalOnly\GLMP01`
  and, copied in while the game ran, `Added\GLM` (RGB565 in green). Every replacement used (log);
  Magenta above Swap gave the magenta heart, `top=Swap` at frame 2620 switched it to Swap's,
  `off` at 2680 gave the original frame (dump at 2700 byte-identical to the run without packs),
  `on` at 2740 brought them back, `reload` picked up the pack added mid-run (listed at the
  bottom, enabled), `pack=Swap:0` at 2900 showed Magenta + Added. PalOnly: "not for GLME01".
- **European game** with the same folder: Swap "not for GLMP01", PalOnly/Magenta/Added loaded
  (14 shared textures replaced on the title screens).
- **Budget**: a 4x pack (203 MB on the GPU at the default 2 GB) held at 64 MB of 64 MB after
  `budget=64` (143 resident, no failures); process private memory flat over the run (2090 MB vs
  ~1960 MB without packs on the same route).
- **Menu**: F1 > Visuals > Texture packs passes the `LM_MENU_ID_CHECK=exit` sweep (15 pages, 0
  conflicts, with four packs listed); `LM_MENU_CLICK` "Use texture packs" saved `enabled=1` and
  the pack order to settings.ini.
- Not tested: a real third-party pack, DDS files, the Japanese game (same code path: the ID
  comes from the running disc), Linux/macOS folder opening.
