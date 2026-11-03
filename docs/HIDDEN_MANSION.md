# PAL Hidden Mansion on the US build

What the PAL release (GLMP01, rev 0) changes in the Hidden Mansion compared to the US release
(GLME01, rev 0), where each change lives, and how the port brings it to the US-based build.
Everything below was found with `tools/pal_diff.py` against the user's own dumps; the commands
to reproduce each finding are given inline. Addresses are virtual (0x80......); "US" is GLME01,
"PAL" is GLMP01_00. The Japanese release, the normal-mansion differences of all three and the
planned per-version lab menu: docs/VERSION_DIFFERENCES.md.

## Tools

| Tool | Purpose |
|------|---------|
| `tools/pal_diff.py code [--tsv f]` | Matches all functions of the two DOLs and lists what changed |
| `tools/pal_diff.py show <US fn> [--pal ADDR] [--side pal]` | Side-by-side disassembly; PAL calls/data are labelled with US names |
| `tools/pal_diff.py var <us\|pal> <ADDR>` | Maps a variable to the other version and lists every use in both |
| `tools/pal_diff.py xref <us\|pal> <ADDR> [--exact]` | Code that references an address (or the object containing it) |
| `tools/pal_diff.py field <us\|pal> <OFFSET>` | Struct-field uses of a displacement (PAL shown with US function names) |
| `tools/pal_diff.py data` | Same-shaped data objects whose bytes differ (pointers masked) |
| `tools/pal_diff.py files [--deep]` | Disc files that differ, and the members inside differing archives |
| `tools/pal_diff.py arcdiff A.szp B.szp [--path RE]` | Two archives member by member; JMP tables row by row with field names |
| `tools/pal_diff.py strings <regex>` | Strings in both DOLs |
| `tools/hidden_mansion_data.py` | Builds the data overlay the port serves in PAL Hidden Mansion mode |

How the matcher works: every function in `decomp/config/*/symbols.txt` is reduced to a
fingerprint of opcodes and registers (branch targets and 16-bit immediates masked). The two
address-ordered lists are aligned with difflib, then moved functions are paired by identical
fingerprint and the rest by similarity between the same matched neighbours. The PAL DOL was
built with a different compiler patch level that schedules some instructions differently, so
fingerprints compare the *multiset* of instructions. Pairs are then compared strictly:
immediates directly, address operands by the value they load from `.rodata`/`.sdata2` (or
ignored for writable data), and `bl` targets through the function map. Functions whose only
difference is struct field offsets are reported as `layout` (several classes gained members in
PAL, e.g. Luigi gained fields at +0x134C/+0x1350, shifting later fields by 4).

Current result: 10,984 US / 11,055 PAL functions; 10,432 identical, 51 layout-only, 164 with
constant/call differences, 282 modified, 55 US-only, 126 PAL-only. Almost all of the non-HM
differences are PAL infrastructure: language selection, 50/60 Hz display setup
(`LMDisplayUtil`), the PAL boot/file-select screens, and an updated SDK (CARD/DVD/GX).

## The hidden-mansion flag

Both versions keep the mode in the same `.sbss` word:

| | US | PAL |
|---|---|---|
| "ura" (hidden mansion) flag | `0x804D80C0` (r13+0x160) | `0x804C5784` (r13+0x164) |
| deferred "enter ura" request (PAL only) | – | `0x804C5780` (r13+0x160) |
| current map number `sCurMapNo` | `0x804D80C8` | `0x804C578C` |
| language (PAL only; 0=J 1=E 2=G 3=F 4=S 5=I) | – | `0x804BD658` (.sdata, initial 1) |

`python tools/pal_diff.py var pal 804C5784` prints the mapping and both use lists: **9 uses in
US, 44 in PAL** (36 PAL functions). The flag is set by the `<URALUIGI>` event command and
cleared by `<OMOTELUIGI>` and on new game (US `fn_8000B978`). The event scripts also keep save
flag 8 ("ura") in step with it.

## Code differences (every use of the flag)

"Both" means US already does the same thing; everything else is new in PAL. Function names
are the US symbols; most game functions are still unnamed in the decomp.

### Mode switching

| PAL | US | Change |
|-----|----|--------|
| `8006656C` | `fn_8006385C` (event command interpreter) | `URALUIGI` now sets the deferred request `0x804C5780 = 1` instead of the flag; `OMOTELUIGI` clears the flag (both) |
| `8000CDE8` | `fn_8000BF38` (scene start) | New: if the request is set, clear it and set the flag, before the scene's subsystems are created |
| `8000C050` | `fn_8000B978` (new game) | Both: flag = 0 |
| `800BDF64` | `init__Q24Koga11MissionModeFv` | New: when flag && map 2, load `/Map/map2u.szp` (`"%s%du.szp"`) instead of `/Map/map2.szp` |

### Mirror (the PAL hidden mansion is left/right mirrored)

The same mirror is available in every mode as *Mirrored Mansion* (docs/ENHANCEMENTS.md section 5).

The 3D scene is mirrored, the HUD is not, and everything that ties the two together is patched:

| PAL | US | Change |
|-----|----|--------|
| `80062388` | `fn_8005F8B8` (game scene draw, called from `fn_8000BCE8`) | The full-screen quad that redraws the EFB copy of the 3D scene uses flipped U texcoords when flag == 1 — this is the mirror |
| `80082838`, `80083724`, `80083934` | `fn_8007F9B8`, `fn_800807EC`, `fn_800809CC` (Luigi input, called from Luigi update `fn_800B2084`) | Main/C stick X (pad +0x44 / +0x54) negated |
| `80042720` | `fn_80041214` (HUD, lifemeter/`dmman` unit) | Screen X of positions the game projects itself: `x = 640 - x` (two sites) |
| `80057B94` | `fn_80055C54` (iris wipe, `res_crcl.szp`) | Wipe rectangle centred at x = 328 instead of 312 |
| `8002ADA0` | `fn_80029A74` (Game Boy Horror map init) | New variable `0x804C5A10` = -1 (hidden) / +1 |
| `8002B124`, `8002B3E4` | `fn_80029DD4`, `fn_8002A01C` (GBH map draw) | Concatenates `PSMTXScale(-1, 1, 1)` onto the map's view matrix |
| `8002B9C8`, `8002BB98`, `8002C2DC` | `fn_8002A5A0`, `fn_8002A74C`, `fn_8002AE74` (GBH map markers) | X multiplied by the sign variable |
| `8018BAFC`, `8018C500` → `8019F8E0` | `fn_8018865C`, `fn_80189054` | Set the JAudio flag `0x804C6A21` = ura |
| `8019F900` | `Jac_VframeWork` (`8019C360`) | Swaps the left/right mix buffers in `Jac_imixcopy` when that flag is set (stereo swap) |

### Difficulty and tuning

| PAL | US | Change (normal → hidden) |
|-----|----|--------------------------|
| `800BCA98` | `fn_800B9680` (Luigi takes damage) | Both: damage x2 |
| `800B7534` | `fn_800B41E0` (Luigi, damaging floor) | Hidden: damage on 5 of every 8 frames (table `0x804C8D5C` = 1,1,0,1,1,0,1,0) instead of every 2nd frame; each hit still x2 |
| `800EA4F8`, `800EAAD0` | `fn_800E70BC`, `fn_800E7698` (heart items, `Koga/800E634C.cpp`) | Threshold on Luigi field +0x6C8: 200 normal, **400 in US hidden, 1000 in PAL hidden** |
| `800179F8` (new, split out of `80017BC8`) | `fn_800168EC` (room ambient colour) | Ambient RGB additionally x0.2 (`0x804BD728`) — darker rooms |
| `800A2204` | `fn_8009F1DC` (`KtEnBaby`, Chauncey's ball spawner) | Balls 0 and 1 of each volley spawn at z = +700 (900-200) instead of -700 |
| `800A2CFC` | `fn_8009FCA8` (Chauncey) | Sets field +0xD8 = 2.0 |
| `800A2DBC` | `fn_8009FD54` (Chauncey) | Threshold 20 → 40 |
| `800A3A50` | `fn_800A09D0` (`KtEnMoku`, Chauncey's rocking horses) | Scale 1.8 → 3.6 |
| `800A48E4` | `fn_800A184C` (`KtEnMoku2`, rocking-horse charge) | Horse weaves side to side, faces its travel direction, hit shapes x2 (see *Bosses*) |
| `800A4DC4` | `fn_800A1BCC` (`KtEnMoku2`) | Argument to `fn_8008907C` 50 → 100 |
| `800A7640` (+ `800A79FC`, `800A8E10`, new `800AC584`) | `fn_800A441C` (+ `fn_800A47BC`, `fn_800A5BA0`) (Bowser / King Boo) | Alternating fast bomb volleys (see *Bosses*) |
| `800BABC8` | `fn_800B783C` (Luigi) | Map 11 (Boolossus) + hidden: after 15 frames enables Luigi's Boolossus movement mode (see *Bosses*) |
| `8006FD0C`, `8006FE44`, `8006FF88`, `800702A0`, `80071240` | `fn_8006CFBC`, `fn_8006D0DC`, `fn_8006D1B8`, `fn_8006D4B8`, `fn_8006E414` (enemy strategy, state table `0x8034BB48`; not yet identified) | State timer 400 → 250, early exit below 130, colour words, extra parameters |
| `8003F444` | `fn_8003DF88` (GBH screen) | Both: picks `fn_8003B668` instead of `fn_8003B700` |
| `800DF2CC` | `fn_800DBE8C` | Both: value x0.5 |
| `80084B64` | `fn_80081BCC` | Both |

No other PAL code reads the flag, and no PAL code checks save flag 8 through
`InGameFlagsUtil` with a constant argument. The portrait-ghost/boss parameter files
(`Game/game*.szp` `param/ctp/*.prm`) are identical between US and PAL except `banaoba.prm`,
so there is no separate boss HP table: boss differences are the code changes above and the
map data below.

## Data differences

### `/Map/map2u.szp` (PAL only) vs `/Map/map2.szp`

`python tools/pal_diff.py arcdiff "<PAL>/files/Map/map2.szp" "<PAL>/files/Map/map2u.szp"`.
Collision (`col.mp`), polygon, room, object, group and event tables are identical. Changed:

| Table | Change |
|-------|--------|
| `jmp/telesa` (the 35 Boos) | Higher `str_hp` (e.g. 40 → 100, 50 → 200, 150 → 300), much higher `max_speed` (up to 24 → 132), longer `move_time`, `attack` 0 → 1 on several, an unnamed field (`#16abce3`) 3 → 5..15 |
| `jmp/enemyinfo` | 186 → 258 rows (92 added): more ghosts (`obake`, `mapoo`, `topoo`, `mopoo`, `tenjyo`, `skul`, ...), many existing ghosts changed to other types |
| `jmp/teidenenemyinfo` | 172 → 175 rows (blackout ghosts) |
| `jmp/generatorinfo` | 84 → 90 rows; spawn counts changed |
| `jmp/iyapootable` | Speedy spirits / gold mice drops: coins 20 → 30, bills → 20, gold 1; emerald/ruby/sapphire columns replaced by `diamond` |
| `jmp/characterinfo` | Six treasure chests (`rtakara1`, `btakara1`, `gtakara1`, `wtakara1`) moved to other spawn slots |
| `jmp/keyinfo` | `key01` spawn slot `38_4` → `38_5` |
| `jmp/furnitureinfo` | 47 furniture item tables changed |
| `jmp/itemappeartable` | 16 → 21 rows |
| `jmp/railinfo`, `jmp/observerinfo`, `path/*` | Paths for the added ghosts; 18 new `path/` entries |
| `jmp/itemtable` | Dropped (no code references it) |

Every ghost type named in `map2u` already appears in `map2`, so the US code can spawn all of
them. JMP tables are looked up by field-name hash, so the US code also reads PAL's extra
`roominfo` fields harmlessly.

### Event scripts

Event archives (`/Event/eventNN.szp`) hold a language-neutral script (`text/eventNN.txt`) and
message tables. PAL changed three scripts for the hidden mansion:

* `event08`: adds `<OMOTELUIGI>` + `<FLAGOFF>(8)` at the start and `<FLAGON>(8)` right before
  `<URALUIGI>` (a dated "bug fix" comment).
* `event16`, `event73`: add a `<CHECKFLAG>(8)"ura""omote"` branch; in the hidden mansion the
  Boo-count checks use `<CHECKTELESA>(40)(41)(42)(43)(44)` instead of `(19)(29)(34)(38)(39)`
  (event16) and `(35)..(39)` (event73).

The US interpreter already knows every tag used (`CHECKFLAG`, `CHECKTELESA`, `COUNTJMP`,
`OMOTELUIGI`, ...). The "omote" branches are byte-for-byte the US logic, so the PAL scripts
are safe in the normal mansion too. `event48`'s `entrdemo.cmn` (camera demo) also differs but
is not hidden-mansion specific and is left alone.

### Text and languages

PAL ships every language: `message/<language>/...` inside each event archive, `Game/game_X.szp`
+ `sdata_X.arc`, `Kawano/<e|f|g|i|s>/res_*.szp`, `Ending/<language>.szp`, and a language
select screen (`Kawano/res_lngg.szp`). The language index lives at `0x804BD658` and is read by
22 functions. None of this is needed: the PAL hidden-mansion changes add no new message lines,
and the PAL English message tables are identical to the US ones in 100 of 102 event archives
(events 39 and 80 differ by two bytes). The port keeps the US text and only swaps the scripts.

### Frame timing

PAL data uses the same per-frame units as US: the normal-mansion `map2` tables match US apart
from the few rows `arcdiff` lists, so nothing was rescaled for 50 Hz. PAL frame counts (e.g.
Boo `move_time`) are therefore used unchanged. On a 50 Hz PAL console they last 20% longer in
wall-clock time than they will in the 60 Hz US build; scaling them is a design choice, not a
porting requirement. (No PAL60 option was found in the PAL DOL.)

## Bosses

Found from the flag uses above plus `pal_diff.py field` (struct-field uses) and the boss
actors' state tables. Boss maps: 9 = Bowser/King Boo, 10 = Chauncey, 11 = Boolossus.

### Boolossus: Luigi's "Boolossus movement mode"

The US Luigi already contains a complete alternative movement mode behind byte **US +0x134C**
(PAL +0x1350; PAL inserted a float at +0x134C, shifting it by 4). Eight Luigi functions read
it: run speed 38 instead of 17/18 (`fn_800B9848`), state selection (`fn_800B5324` → state 9,
`fn_800B5C14` → state 5), movement physics with momentum (`fn_800B63FC`: 0.995 damping, 1.5 /
-1.4 accel; `fn_800B6B50`: 20 / 0.9), animation selection (`fn_800AE9DC`) and a looping sound
(`fn_800B79B8`, sound 0x43). **US never sets it** (only Luigi init clears it). PAL adds the
enabling code in `fn_800B783C` (PAL `800BABC8`): in map 11 with the hidden-mansion flag set, a
frame counter (new PAL field +0x134C) increments every frame and the mode turns on once it
exceeds 15 (`0x804C032C` = 15); elsewhere it is forced off. In-game (port, PAL variant) Luigi
slides with momentum and keeps gliding after the stick is released; with the US variant he
stops dead.

### Chauncey (map 10)

| PAL | US | Change in the PAL hidden mansion |
|-----|----|----------------------------------|
| `800A2204` | `fn_8009F1DC` | Balls 0/1 of each volley spawn at z = +700 instead of -700 (ball 2 unchanged) |
| `800A2CFC` | `fn_8009FCA8` | "POYON" state entry also sets the model's +0xD8 = 2.0 |
| `800A2DBC` | `fn_8009FD54` | Approach state: distance test 15 → 40 and move speed 20 → 40 (PAL normal: 20/20) |
| `800A3A50` | `fn_800A09D0` | `KtEnMoku` rocking-horse scale 1.8 → 3.6 |
| `800A48E4` | `fn_800A184C` | `KtEnMoku2` charge: weaves (x velocity 100·sin(20°·t), t = `0x804D8BF8`), yaw follows travel (`fn_8000965C`), hit shapes vs Luigi x2 |
| `800A4DC4` | `fn_800A1BCC` | `KtEnMoku2`: value passed to `fn_8008907C` 50 → 100 |

`map2u`/event data for Chauncey's room is unchanged; his parameter files are identical.

### Bowser / King Boo (map 9)

PAL adds byte **+0x1EE** to the boss strategy (cleared in init `fn_800A47BC`). In the chase
state (`fn_800A441C`), when all three bombs are gone it flips the byte (hidden mansion only)
before switching to state 0x12. In the throw function (`fn_800A5BA0`, `bom_throw_timing`), the
first bomb of a volley is thrown as before; the following bombs normally enter the "thrown"
state, but while the byte is set they enter a **new bomb state** that PAL inserted into the
bomb strategy's state table (PAL `0x8034A0D8`, new init `fn_800AC584`; later states shift by
one, which is why several bomb functions differ only in state numbers). The new state is the
thrown state with timers +0x1D0/+0x1D4 = 100/30 instead of 300/90 (PAL's normal thrown state
also sets +0x1D4 = 90, US leaves it). So every other volley in the PAL hidden mansion is a fast
volley.

### Other bosses and portrait ghosts

No other boss or portrait-ghost code reads the flag (Bogmire, the portrait ghosts and the
Boos' actors are identical apart from field-offset shifts), and their parameter files are
identical. Boo differences are data (`jmp/telesa` in `map2u`) and event scripts (Boo-count
gates).

## Lighting

The only lighting change is the room ambient colour x0.2 (`fn_800168EC` / PAL `800179F8`),
for every room including hallways and the exterior, lit or unlit (lit rooms already use
x0.6). `map2u`'s `roominfo` values are identical to `map2`'s, no other code reads the flag,
and no fog, light-colour or light-table data differs. Verified in the foyer: the PAL variant is
markedly darker than the US one (candle/lamp light sources unaffected).

## Hearts

* **Heal amount, not spawn chance.** `fn_800E70BC` is called from the heart item's init
  (`fn_800D1458`, `EnHeartItem`): a small heart (type 0xCD) heals 20 instead of 10 when the
  player's counter at status +0x6C8 reaches the threshold; big hearts (0xCE) always heal 50. The
  same threshold in `fn_800E7698` picks between item 0x19 and 0x1A for a drop. The counter is
  copied from Luigi +0x12F4 (incremented by the treasure-pickup message in `fn_800AEEA8` and
  shown by the money/rank screens), i.e. a treasure total. Normal 200, US hidden 400, PAL
  hidden 1000 — so in the PAL hidden mansion small hearts practically always heal only 10.
* **Fewer hearts (data).** `map2u` `itemappeartable` replaces many `sheart`/`move_sheart`
  slots with `mkinoko` (poison mushroom), `money` or `nothing`, and changes 47 furniture item
  tables; Speedy Spirits/gold mice drops change (`iyapootable`). These come with the overlay.

## Implementation in the port

**Variant.** PAL rules are a runtime *variant* separate from the game's own flag:
`hm::variant()` / `hm::setVariant()` (`src/recomp/hidden_mansion.h`). Everything below is
active only while the game's flag is set **and** the variant is PAL; with the US variant the
overlay is off, patched literals are restored and every hook passes straight through to the
original. With both discs the player picks the variant in E. Gadd's lab (below); the lab
records it in game flag 8 and `variant()` reads it from there. Without the lab menu (one
disc, or an overlay built before it) the variant is F1 > Enhancements > *Hidden Mansion
version*. `--hidden-mansion-variant pal|us` (alias `--hidden-mansion`, env
`LM_HIDDEN_MANSION_VARIANT`) overrides both, for testing; `LM_HM_FORCE=1` sets flag 8 from
the F1/override default when it forces the hidden-mansion flag.

**Data overlay** (`tools/hidden_mansion_data.py` → `orig/hidden_mansion/`, served by
`hle_dvd.cpp` through `hm::overlayEntry`/`overlayRead`): `/Map/map2.szp` → PAL `map2u.szp` while
PAL rules apply and map 2 loads; `/Event/event{08,16,73}.szp` → US archives with the PAL
scripts while the variant is PAL.

**Hooks** (`src/recomp/hidden_mansion_hooks.cpp`, listed in `tools/recomp/hooks.txt`, all via
`callOriginal`):

| Hook | PAL behaviour |
|------|---------------|
| `fn_800168EC` | Room ambient x0.2 (current room's RGB scaled for the call) |
| `fn_800E70BC`, `fn_800E7698` | Heart threshold 1000 |
| `fn_800B41E0` | Damaging-floor 5-of-8 pattern |
| `fn_800B783C` | Boolossus movement mode in map 11 |
| `fn_8009F1DC`, `fn_8009FCA8`, `fn_8009FD54`, `fn_800A184C` | Chauncey balls, POYON, approach, rocking-horse weave and hit shapes |
| `fn_800A47BC`, `fn_800A441C`, `fn_800A5BA0`, `fn_800A926C` | Bowser volley alternation and fast bombs (state kept host-side) |
| `fn_8006D1B8` | Enemy strategy (`0x8034BB48`) state timer 400 → 250 |
| `fn_8005F8B8` | Mirror: S texcoords of the screen-copy quad flipped (FIFO filter for the call) |
| `fn_8007F9B8`, `fn_800807EC`, `fn_800809CC` | Mirror: main/C stick X negated for Luigi's input |
| `fn_80041214` + `fn_80009148` | Mirror: projected HUD positions x = 640 - x |
| `fn_80055C54` + `set__7JUTRectFiiii` | Mirror: iris wipe centred at 328 |
| `fn_80029DD4`, `fn_8002A01C` + `fn_80015BDC`, `fn_8001D894`; `fn_8002A5A0`, `fn_8002A74C`; patch 60 + `fn_80164F58` | Mirror: Game Boy Horror map (map model view x Scale(-1,1,1) with the mirrored cull row; map focus, camera reset and Luigi's map effect with x negated; `src/recomp/mirror_world.cpp`) |
| `fn_8000C188` | Dev only: `LM_START_MAP=<n>` loads map n instead of the mansion (now in `dev_tools.cpp`, docs/UNUSED_CONTENT.md) |

**Literal patches** (`hm::syncConstants`, every frame): `0x804DB278` 1.8 → 3.6 (rocking horse
scale), `0x804DB2B0` 50 → 100.

**Verified in-game** (`LM_HM_FORCE=1` sets the flag; `LM_START_MAP` jumps to a boss map):
`map2u` loads and the foyer works; darker rooms; mirrored picture with unmirrored HUD and
correct stick direction; Boolossus movement mode (position log `LM_HM_TRACE_LUIGI=1` shows
momentum vs US); Chauncey fight runs with the ball/horse rules applied; Bowser fight runs
with the alternation applied; US variant logs no PAL rule.

**Not done yet:**
* The rest of the `0x8034BB48` enemy strategy (`fn_8006CFBC`, `fn_8006D0DC`, `fn_8006D4B8`,
  `fn_8006E414`: early exit below 130, colour words, extra parameters).
* PAL's deferred `URALUIGI` (only matters for scene-change edge cases).

## The reverse: USA rules on the European game

With both discs, the European game can run the USA Hidden Mansion (F1 > Enhancements >
*Hidden Mansion version*): the USA `map2.szp` and event scripts are served from
`hidden_mansion/usa/`, and every European function in the tables above whose flag use is
PAL-only runs with the flag hidden; the USA heart threshold (400), room ambient (1.0) and no
Boolossus movement mode are restored. Details: docs/SETUP.md, *USA Hidden Mansion on the
European game*.

## E. Gadd's lab menu (both Hidden Mansions)

**Where it is.** The destination menu is event script `text/event08.txt` in
`/Event/event08.szp`, not code. Its choice list is the `<CHOICE>` command:

```
<CHECKFLAG>(5)"5on""5off"
<CASE>"5on"   <CHOICE>"back"(3)"urayasiki"(38)"minigame"(1)"gallary"(2)<LISTEND>
<CASE>"5off"  <CHOICE>"back"(3)"minigame"(1)"gallary"(2)<LISTEND>
```

Each entry is a case label plus a line index into the same archive's message table
`message/message8.csv` (line 0 "Where will you go, Luigi?", 1 "The Training Room", 2 "The
Gallery", 3 "The Mansion", 38 "The Hidden Mansion"). Game flag 5 (Hidden Mansion unlocked)
selects the 4-entry list. The `"urayasiki"` case plays the send-off line and runs
`<URALUIGI>`, which sets the RAM hidden-mansion flag (`0x804D80C0`); `"back"` runs
`<OMOTELUIGI>`. Game flags live in a bitfield read by `getFlagValue__FUc` (`0x80065FA4`):
flag n is bit (n & 7) of byte `0x803D2D40 + 0x659 + n/8`; that block is part of the save
data, so flag 5 persists, while the hidden-mansion flag itself is RAM-only and is set again
each time the player picks the Hidden Mansion in the lab (PAL additionally mirrors it in save
flag 8).

**Implementation: a follow-up choice.** No game build uses more than four `<CHOICE>`
entries, so picking "The Hidden Mansion" asks a second `<CHOICE>` (`tools/hidden_mansion_data.py`
`lab_script`/`lab_table`, C++ twin in `src/setup/hm_overlay.cpp`; both produce identical
archives):

```
<CASE>"urayasiki"
<CHOICE>"ura_us"(39)"ura_pal"(40)<LISTEND>
<ANYKEY>
<CASE>"ura_us"   ...original send-off... <FLAGOFF>(8) <URALUIGI> ...
<CASE>"ura_pal"  ...original send-off... <FLAGON>(8)  <URALUIGI> ...
```

- **Script**: the European `event08` script (it already clears flag 8 and the hidden-mansion
  flag when the lab is entered, and sets flag 8 before `<URALUIGI>`), with the
  `"urayasiki"` case split as above. Served on both games (`Event/event08.szp` on the USA game,
  `usa/Event/event08.szp` on the European one; manifest tag `[always]`) whichever variant is
  selected, as long as both discs are installed (`hm::labChoiceAvailable()`).
- **Text**: two lines appended to every message table in the archive (`message8.csv`; the
  European archive's `message/<language>/book1.txt`, padded with empty entries so the
  indices match): that language's own "Hidden Mansion" line (index 38) plus " (USA)" /
  " (PAL)" (full-width in the Japanese table). English: "The Hidden Mansion (USA)" /
  "The Hidden Mansion (PAL)". Drawn by the game's own choice box and font.
- **Choice box**: its left edge is a constant (`.sdata` 0x804D0558 = 368) and it grows to the
  right, so the longer entries ran off the screen; `HLE(fn_80043104)` (the `<CHOICE>` box
  draw, both releases) moves a box that would cross x = 624 to the left for the draw, only
  while the lab menu is installed.
- **Where the answer is kept: game flag 8**, the European release's hidden-mansion save
  flag. Flags 85..255 are cleared on every map load (`fn_80066084`: 85..169,
  `fn_8006628C`: 170..255), so an unused high flag cannot carry the choice into the mansion,
  and the low persistent flags are used by scripts, code or map data. Flag 8 is set for PAL /
  cleared for USA right before `<URALUIGI>`, is saved with the file, and is cleared again
  whenever the lab is entered; it is exactly "currently in the PAL Hidden Mansion". The USA
  game never reads it; the European scripts (event16/73) use it for their Boo-count gates,
  which is what the PAL rules need, and with it cleared they take the USA branch.
  `hm::variant()` = flag 8 while the lab menu is installed. The choice is therefore asked
  on every visit (like the hidden-mansion flag itself, which the lab sets each time), and a
  save made inside a Hidden Mansion keeps its variant.
- **F1**: with the lab menu the F1 combo is replaced by a note ("chosen in E. Gadd's lab",
  and the current visit's version while inside); the setting stays as the fallback.
- **Retail unchanged** with one disc: nothing is served and the menu is the original one.

Verified (US base, `lmrecomp`, and European base, `LuigisMansionRecomp` with both discs and
a setup-built overlay; scratch AppData/card, `LM_START_MAP=1` + `LM_SKIP_TEST` flags 5/7):
the follow-up list shows both lines on screen; "(PAL)" gives the mirrored, darker mansion
with `map2u` (US base) / the European data (European base); "(USA)" gives the unmirrored
USA mansion (US base: retail data; European base: `usa/Map/map2.szp`).
