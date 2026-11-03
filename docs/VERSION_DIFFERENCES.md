# Version differences: USA, Japan and PAL, normal and Hidden Mansion

What separates the base mansion (and the Hidden Mansion) of the three retail releases, where
each difference lives, and what it takes to offer every version's rules on any base, as
E. Gadd's lab already does for the two Hidden Mansions (docs/HIDDEN_MANSION.md). This is
the basis of the lab menu "The Mansion (USA) / (Japan) / (PAL)" and "The Hidden Mansion
(USA) / (Japan) / (PAL)", implemented on branch `japan-support` (section 6, *Implementation*).

Releases: **USA** GLME01 rev 0, **Japan** GLMJ01 rev 0 (the first build), **PAL** GLMP01
rev 0. "US" names and addresses are the USA symbols (`decomp/config/GLME01`); a Japanese or
PAL function is named by its USA counterpart.

Sources:

* The TCRF page "Luigi's Mansion (GameCube)/Version Differences", read only through the
  user-approved mirror (`https://tcrf.tiago.zip/wiki/Luigi's_Mansion_(GameCube)-2fVersion_Differences`)
  as untrusted data. It contained no instructions aimed at us. Each of its claims is marked
  below as **verified** (seen on the discs), **consistent** (the data or code differs where the
  claim says, but we have not heard or watched it) or **unverified**.
* Our own comparison of the user's dumps (`Luigis Mansion US/`, `Luigis Mansion JP/`,
  `Lugis Mansion PAL/`): every disc file, every JMP table of the differing map archives,
  every event script, the music sequence list, and all of main.dol, function by function.

## Tools

| Command | What it does |
|---------|--------------|
| `python tools/version_diff.py files` | Three-way table of disc files (same letter = same bytes, `-` missing) |
| `python tools/version_diff.py arc <path> A B` | Archive members; JMP tables row by row (rows paired by name / content, so inserted rows don't cascade) |
| `python tools/version_diff.py events A B [--diff]` | Event scripts (`text/eventNN.txt`) that differ |
| `python tools/version_diff.py event NN A B` / `msg NN A B` | One script as a unified diff / one message table side by side |
| `python tools/version_diff.py seqs A B` | Music and sound sequences (`JaiInit.aaf` BARC list, `JaiArcS.arc`) |
| `python tools/version_diff.py fnsum jp\|pal [--range LO-HI]` | Every changed main.dol function vs US: calls, `li`/`cmpwi` immediates and constants that differ |
| `python tools/pal_diff.py --jp code\|show\|var\|xref ...` | The PAL diff tool with the Japanese game as the second side |

A and B are `us`, `jp`, `pal`. The Japanese function table (`decomp/config/GLMJ01/`, from
decomp-toolkit's analysis of the Japanese DOL plus the USA/PAL matcher) comes from branch
`japan-support` (`tools/recomp/gen_jp_symbols.py`); `--jp`/`fnsum jp` need it. Code result:
10,984 USA / 10,886 Japanese functions, 10,690 identical, 19 layout-only, 22 constant-only,
141 modified, 112 USA-only, 14 Japanese-only.

## Summary

* **Japan vs USA** is mostly *presentation*: Japanese text, a reworked sound bank and
  music (two new US tracks, ~25 re-edited sequences, JAudio sequencer fixes), the Pikmin
  trailer, cutscene pacing, and a handful of map-data and gameplay-code changes. The
  gameplay-relevant ones are few: **small hearts heal 20 from a treasure counter of 100**
  (USA 200), **no double damage in the Japanese Hidden Mansion**, Chauncey's approach
  distance, a few extra props/dust clouds/paths in the mansion data, and unidentified changes
  in several ghost/boss state machines.
* **Japan's Hidden Mansion** is the USA one (same map data, same scripts, no mirror) minus
  the two USA additions above: damage x2 and the raised heart threshold (USA 400). The
  Hidden Mansion's light-bind x0.5 (faster stun window), vacuum drain change and GBH star
  exist in all three releases.
* **PAL vs USA (normal mansion)**: on top of the language/50 Hz infrastructure
  (docs/HIDDEN_MANSION.md), the PAL normal mansion has **fewer heart drops**, an **A rank
  that needs 150,000,000 G instead of 100,000,000 G** (in both modes), Chauncey and Bowser
  tweaks that also apply outside the Hidden Mansion, and a few data rows.
* **PAL Hidden Mansion**: unchanged from docs/HIDDEN_MANSION.md, already offered.

Rule of thumb for "<version> rules": the gameplay data (map JMP tables, ghost parameters) and
gameplay code come from the chosen version; text, event-script wording and (by default)
sound stay with the base disc, because event scripts index their own release's message
tables and sound ids (below).

## 1. Japan vs USA

### 1.1 Gameplay code (main.dol)

| Where (US) | Japan | USA (and PAL) | Rules item | How to offer it |
|------------|-------|----------------|------------|-----------------|
| `fn_800E70BC`, `fn_800E7698` (heart items) | small heart heals 20 once the treasure counter (status +0x6C8) is **>= 100**, in both modes | 200 normal, 400 Hidden (PAL: 200 / 1000) | **yes** (verified, disassembly) | Existing heart hooks: add the JP threshold (100) |
| `fn_800B9680` (Luigi takes damage) | does not exist: every damage site subtracts the raw damage (`fn_800B9488(hp - dmg)`) | damage x2 while the hidden-mansion flag is set; called from `fn_800B41E0`, `fn_800B47B0`, `fn_800B4F2C`, `fn_800B5110`, `fn_800B9124` | **yes**, Hidden Mansion only (verified) | Hook `fn_800B9680` (already in hooks.txt for the reverse overlay): JP rules = no doubling |
| `fn_8009FD54` (Chauncey approach state) | stops approaching at distance **10** (two compares) | 15 (PAL normal 20, PAL Hidden 40); USA also guards the direction normalisation against a zero vector (`VECMag` < 0.0001) | yes (verified) | Existing hook; the two `lfs 15f` loads need an instruction `patch` to a 10.0 literal (the third 15f load in the function is shared) |
| `fn_800B9488` (Luigi's health setter) | at 0 HP only the game-over request | additionally `fn_8018B7C8`: Luigi voice 0x280F and music handles 1/4 faded | cosmetic | Hook: skip the call |
| `fn_800A16C0` (Chauncey's rocking horses) | one sound | picks SE 0x4857 when actor +0x1B9 is set | cosmetic | TCRF "rocking horse sound changed": **consistent** |
| `fn_800A8E5C`, `fn_800A909C`, `fn_800A9844`, `fn_800A99A4`, `fn_800A9500`, `fn_800A5960`, `fn_800A6630` (Bowser / King Boo and his bombs) | | USA adds a looping SE 0x10D1 with distance volume to the bombs, SE 0x18D2, messages to Luigi (51), an extra transition to state 11 when the sub-object's +0x2A0/+0x2A1 is set (`fn_800A5960`), and a vertex offset (-1700 vs JP -2134, `fn_800A6630`) | partly gameplay, **unidentified** | Identify in-game before offering (A/B the fight on both discs) |
| `fn_80073D40`, `fn_8007A368`..`fn_8007B278`, `fn_8007B05C` (US only) | | ghost strategy with one more state (state numbers 27/29/31/32 vs 26/28/30/31), extra `setFlagOn`, message 221 to Luigi | **unidentified** | Identify first |
| `fn_8009AAEC`..`fn_8009D938`, `fn_8009C2D8`/`fn_8009C2E8`/`fn_8009B450`/`fn_8009B454` (US only, tiny) | | small changes in an enemy family next to Chauncey's code | **unidentified** | Identify first |
| `fn_800AE1A4`, `fn_800AE9DC`, `fn_800AEEA8`, `fn_800B2A18`, `fn_800B36B8`, `fn_800B6B50`, `fn_800B976C` (Luigi) | | message handler sets an extra flag and handles messages 5/6/56; map entry refreshes the low-health music (`fn_80188D04`); JP's movement physics calls `fn_800952D8` where US does not; US clears a field (+0x248) of the object from `fn_800255E4` when +0x1150 > 0 | mostly cosmetic, **unidentified** in detail | Identify first |
| `fn_800CE0D8`..`fn_800D1C78` (items: Mario's items, treasure) | `fn_800D114C` sends a `CharacterEvent` (messages 21/45) itself | delegated to `fn_800D00BC` with extra checks | presentation (TCRF: some text boxes, e.g. Mario's items, appear later in JP: **consistent**) | Not needed for rules |
| `fn_8013199C`, `fn_80137310`..`fn_8013A0A0`, `fn_80149C28`..`fn_801516DC`, `fn_8015066C` | | treasure / money grains (bills, coins), speedy spirits' escape paths (`escape_%d`, `fn_801338C0`); SE 0x4849 added; `fn_801516DC` (US only) | **unidentified** | Identify first |
| `fn_80189858` (music on room change) | special cases for rooms 21, 24, 41 | also rooms 57 and **70 (Secret Altar)** | cosmetic | TCRF "Luigi no longer whistles in the Secret Altar": **consistent**; hook if JP sound is offered |
| `fn_8004225C`..`fn_8004CED4`, `fn_8004DB9C` (US) / `8004D3CC` (JP) | text box open/close constants 1.3 / 1.5 / 2.5, account-screen code | 0.9 / 1.2 / 2.2, rewritten area-clear screen (`lbl_803CFA50`) | presentation | Not needed for rules |
| HVQM4 decoder, `jaudio/hvqm_play.c`, `syncstream.c`, `fn_8000EF58`..`fn_8000F074`, `fn_800671E8`..`fn_8006775C`, title scene | no Pikmin trailer | Options > Pikmin movie (`Movie/pikminS.h4m`) | menu | TCRF: **verified**. Stays with the base |
| JAudio sequencer (`Jam_SeqmainNote`, `Cmd_Process`, `Init_Track`, `Jaq_CloseTrack`, `Nas_HeapAlloc`, `DspExtraTaskCheck`, JAI `fn_80188594`..`fn_80196058`) | debug `OSReport`s, older sequencer and stream code | fixed | bugs | TCRF audio bugs (delays, tempo, Conservatory loop, music not reset): **consistent** (the code differs there). Not offered: they are bugs |
| `fn_8018865C`, `fn_80189054` | compare 8 | compare 9 | none | one more music handle in the USA game |

The hidden-mansion flag itself (`0x804D80C0` US, `0x804A0C40` JP) has 6 uses in the
Japanese game (new game, the two event commands, the GBH screen `fn_8003DF88`, the capture
drain `fn_80081BCC`, the light-bind frames `fn_800DBE8C`) against 9 in the USA game
(`pal_diff.py --jp var us 804D80C0`): the USA game added the damage x2 and the two heart
thresholds.

### 1.2 Map data (`Map/map2.szp`, the mansion)

`version_diff.py arc Map/map2.szp us jp`. Ghosts (`enemyinfo`, `teidenenemyinfo`,
`generatorinfo`), Boos (`telesa`), keys, events and item tables are **the same rows**; only
the byte layout differs. Changed (Japan has / USA has):

| Table | Japan vs USA |
|-------|--------------|
| `characterinfo` | one more `kdust` (dust cloud) in room 61; two `kdust` moved (z 190 vs 120/130) |
| `furnitureinfo` | one more picture, `gakub01`, in room 12 (PAL has it too); four `omark` rows in room 63 `not_transparent` 0 (USA 1) |
| `railinfo`, `path/*` | escape paths `escape_17`, `escape_20` (USA removed them); `escape_12`, `escape_18` one point longer |
| `treasuretable` | an entry `key_72` in room 57 (USA removed it) |
| `roominfo` | room 1 and 15 `sound_room_code` 1 / 0 (USA 2 / 2), room 3 `Lv` 2 (USA 3) |
| `groupinfo` | `2_f_47` `VRbox` 0 (USA 1) |
| `objinfo` | `eldoor10` z -5460 (USA -5410) |
| `soundpolygoninfo`, `col.mp` | 147 floor polygons with footstep material 0 (USA 4 or 1); collision differs |

Other data: `Map/map4.szp` (Portrificationizer room): `dluige03` visible in Japan (USA
`invisible`). `Iwamoto/map2/room_70.arc` (Secret Altar room model, 285 KB vs 209 KB) and
`room_08.arc` (`reizo.bin`) differ. `model/kopabody.szp` (Bowser's fire animation `fire.key`),
`model/boy.szp` and `model/father.szp` (animation sound tables `.bas`) differ.
`Game/game.szp`: all ghost and boss parameter files (`param/ctp/*.prm`) are identical; only
`model/dummy2.arc` differs.

**Offering it:** serve the Japanese `map2.szp`, `map4.szp`, `room_70.arc`, `room_08.arc` and
`kopabody.szp` from a setup-built overlay (as `map2u` for the PAL Hidden Mansion). The JMP
tables are read by field-name hash and use the same fields, so the USA/PAL code reads them;
nothing in them is text. Keep the `.bas` files with the base's sound (they hold sound ids).

### 1.3 Event scripts

`version_diff.py events us jp --diff`: 36 scripts differ, but most lines are `<SE>(n)`
renumbering. The numbers are not one global table (USA 175 is Japan 132, 133, 135 or 181
depending on the event), so a Japanese script played with the USA sound data would play wrong
sounds, and the message tables are laid out differently (e.g. event08: 98 vs 109 lines, entry
13 "So your name's Luigi?" vs 「・・・そうか。」), so a Japanese script cannot show the USA
text. Real content changes:

* `event09`/`event10` (training results): Japan waits 90 frames and starts the result music
  after the line, USA plays it first and waits 120; one result waits for a key in Japan.
* `event18`-`21`, `event41` (portrait-room pacing): waits 50/60 in Japan vs 20/90.
* `event17`, `event32`: Japan adds a 28-frame wait (and Luigi's `WAIT0` animation in 17).
* `event28`: E. Gadd has two more talk animations in Japan.
* `event14`: Japan adds `<CAMERAEND>`. `event72`: USA adds `<HLIGHTOFF>`.
* `event52` (ending): USA adds `<SE>(98)` after Mario hits Luigi (TCRF: **verified**).
* `event68`: USA returns to Luigi's humming (`<BGM>(7)`) after the puzzle sound.
* `event02`: USA adds two 30-frame waits commented 「アメリカ版より」 ("from the American version").
* `event03`/`38`/`65`/`69`/`89`: USA adds sound effects (commented with a sound designer's name).

None of these touch progress rules (flags, Boo counts, `CHECKTELESA`); `event16`/`event73`
(the Boo gates) are identical in Japan and USA.

**Offering it:** keep the base's scripts. The pacing changes could be carried over by a
setup-time transform that applies only the Japanese `WAIT`/`ANYKEY`/`BGM`/`ACT` lines to the
base's script (both discs needed, aligned by the diff above); low value, optional.

### 1.4 Sound and music

`version_diff.py seqs us jp`: the USA game has 89 sequences, Japan 87. USA-only:
`HeyHoo.com` (index 87, the Shy Guy ghosts' Ball Room music: TCRF **verified**) and
`Demo_Star.com` (index 88, probably the portrait-ghost music TCRF describes: **consistent**).
Re-edited in the USA game: `LuigiSe.bms` (sound-effect sequence), `Mario02.com`,
`PianistQuiz01-03`, `PianistBattle`, `LuigiSings` (humming), `GoodNight`, `Fishing_Event`
(Japan: a 96-byte stub), `DanceHall`, `BabyDemo`/`BabyBattle` (Chauncey), `BlueDemo`/
`BlueBattle`, `TelesaDemo01`/`TelesaBattle` (Boolossus), `BattleClear` (boss victory: TCRF
drums **consistent**), `KuppaDemo01-03`/`KuppaBattle` (final boss: TCRF instruments
**consistent**), `Event_Speak`, `Training`, `GetKey_Heart`. The three wave banks
(`AudioRes/Banks/LuiSec*.aw`) differ too (TCRF: Japan's damage grunt; **consistent**: the code
plays SE 0x839 in both, the sample differs). Unverified: King Boo whimper, Game Over length,
Mr. Luggs' panic music, observatory music, credits "PRODUCER".

**Offering it:** "Japanese sound" is separable from the rules: serve the Japanese
`JaiInit.aaf`, `JaiArcS.arc` and banks. Risks: the USA/PAL code asks for sequences 87/88 and
sound ids the Japanese tables may not have, and the USA JAudio expects its own data. Make it
a separate option (F1 > Sound, "Japanese soundtrack", needs the Japanese disc), only after a
listening test; the rules menu should not switch sound.

### 1.5 Presentation only (stays with the base)

Japanese text (event message tables, `Kawano/res_*`, `Ending/*.bti`, title/banner), the
Pikmin trailer, text-box timing, the area-clear screen code, the opening camera. Not part of
"rules".

## 2. Hidden Mansion: Japan vs USA vs PAL

| | Japan | USA | PAL |
|-|-------|-----|-----|
| Map data | Japanese `map2.szp` (no `map2u`) | USA `map2.szp` | `map2u.szp` (more ghosts, faster Boos, fewer hearts, diamonds) |
| Event scripts | same as USA (Boo gates `CHECKTELESA(19)(29)(34)(38)(39)`) | | `event08/16/73` with the `CHECKFLAG(8)` "ura" branches (45 Boos) |
| Damage | **x1** | x2 | x2 |
| Small heart heals 20 from treasure counter | **100** (as normal) | 400 | 1000 |
| Light-bind frames x0.5, capture drain, GBH star | yes | yes | yes |
| Mirror, darker rooms, stereo swap | no | no | yes |
| Chauncey / Boolossus / Bowser / Bogmire changes | no | no | yes (docs/HIDDEN_MANSION.md) |
| A rank | 100,000,000 G | 100,000,000 G | 150,000,000 G |

TCRF (European Hidden Mansion): mirrored, ghosts earlier, Boos more agile, bigger/zigzagging/
faster rocking horses, faster Chauncey, Boolossus riding, shorter bomb fuses, 45 Boos, fewer
hearts, darker rooms, more ghosts (five in the Artist's Studio), Speedy Spirits and gold mice
always dropping diamonds, A rank only possible there: all **verified** in
docs/HIDDEN_MANSION.md and above, except "shorter time to catch Bogmire", which matches the
still unidentified enemy-strategy timer 400 -> 250 (`fn_8006D1B8`, table `0x8034BB48`):
probably Bogmire's (**consistent**, to confirm). TCRF's "Japan/North America" Hidden Mansion
screenshot is one picture: the two differ only in the damage and heart rules.

**Offering "Hidden Mansion (Japan)":** Japanese map data (as 1.2) + hidden-mansion flag +
damage x1 + heart threshold 100. Needs the Japanese disc for the map data; without it, the
code rules alone (on the USA map data) would be a "Japanese rules" approximation, so grey
it out instead.

## 3. PAL vs USA, normal mansion

Beyond docs/HIDDEN_MANSION.md (which covers the Hidden Mansion and the infrastructure):

| Difference | Where | Verified | How to offer |
|------------|-------|----------|--------------|
| A rank needs **150,000,000 G** (ranks: 5M, 20M, 40M, 50M, 60M, 70M, 100M/150M) | `.rodata` rank table read by `fn_8000D0AC` (US `0x8020BE00`, PAL `0x80210EB8`) | yes | Rewrite the one word at runtime (like `hm::syncConstants`) |
| Fewer heart drops: 5 of 16 `itemappeartable` probability rows lose `sheart`/`move_sheart` slots | PAL `map2.szp` | yes | PAL `map2.szp` overlay |
| Event 45 trigger `EventIf` 4 (USA/Japan 5) | PAL `map2.szp` `eventinfo` | yes (effect unseen) | same overlay |
| Picture `gakub01` in room 12 (as Japan) | PAL `map2.szp` | yes | same overlay |
| `roominfo` has extra pos/dir/scale fields | PAL `map2.szp` | yes | harmless (read by hash) |
| `banaoba.prm` normal item table 2 -> 3 | PAL `Game/game.szp` | yes | serve that member / archive |
| Chauncey approach distance 20 (USA 15) | `fn_8009FD54` | per docs/HIDDEN_MANSION.md | existing hook |
| Bowser's thrown-bomb state sets +0x1D4 = 90 (USA leaves it) | `fn_800A926C` | per docs/HIDDEN_MANSION.md | existing hook |
| `map4` event area 4999 (5000), opening camera `opcn.scd`, `event48` camera demo | data | yes | cosmetic, optional |
| `LuigiSings`, `Change_Water`, `KuppaDemo03`, `LuigiSe.bms` re-edited; `Kawano` European screens, language select | sound / UI | yes | stays with the base |

PAL frame counts are 60 Hz-equivalent (docs/HIDDEN_MANSION.md "Frame timing"): nothing to
rescale.

## 4. Feasibility per base

The rules of a version = its gameplay data (map archives above, `game.szp` params) + its
gameplay code differences. Everything is built at setup from the user's discs (no game data
in the repo), like the PAL Hidden Mansion overlay.

| Wanted | On USA base | On Japanese base | On PAL base | Discs needed |
|--------|-------------|------------------|-------------|--------------|
| Mansion (USA) | retail | USA `map2`/`map4`/rooms overlay + USA code rules (thresholds 200/400, damage x2 in Hidden, death voice) | USA rules (reverse overlay `hidden_mansion/usa/` already has `map2.szp`; add 150M -> 100M) | base + USA |
| Mansion (Japan) | Japanese map overlay + hooks: heart threshold 100, Chauncey 10; optional: death voice off | retail | Japanese map overlay + the same hooks + PAL-only changes off (150M -> 100M, Chauncey, Bowser) | base + Japan |
| Mansion (PAL) | PAL `map2.szp` + `game.szp` overlay + 150M + Chauncey 20 + Bowser 90 | same overlay + hooks, Japanese text kept | retail | base + PAL |
| Hidden (USA) | retail | USA map overlay + damage x2 + threshold 400 | done (reverse overlay) | base + USA |
| Hidden (Japan) | Japanese map overlay + damage x1 + threshold 100 | retail | Japanese map overlay + USA-rules mode (flag hidden) + damage x1 + threshold 100 | base + Japan |
| Hidden (PAL) | done | `map2u` + PAL scripts (`event16/73` are identical in Japan and USA, so the European scripts work; the European archive's Japanese message tables equal the Japanese disc's in 84 of 89 events) + PAL hooks on Japanese addresses (`VA()` table from japan-support) | retail | base + PAL |
| Japanese soundtrack | possible, experimental (1.4) | retail | possible, experimental | + Japan |
| Japanese cutscene pacing | possible via script transform (1.3), low value | retail | same | + Japan |
| JAudio sequencer bugs, Pikmin trailer, text timing | not offered | | | |

Not feasible / not wanted: playing a Japanese event script with USA/PAL text (different
message layout), playing the Japanese sound sequences through the USA sequencer without a
listening test, the unidentified state-machine changes in 1.1 until each is identified (they
are probably bug fixes; a rules variant should not reintroduce a bug that breaks progress).

Code hooks on the Japanese base need its version map (`tools/recomp/GLMJ01_map.txt`,
`version_addrs_jp.inc` on japan-support); hooks tagged `us pal` today need a `jp` tag.

## 5. Lab menu design

### What the player sees

The lab's destination list (`event08`, docs/HIDDEN_MANSION.md "E. Gadd's lab menu") keeps its
four retail entries. Picking **The Mansion** or **The Hidden Mansion** asks a follow-up
choice, built at setup from the installed discs:

```
Where will you go, Luigi?          ->  The Mansion
  The Training Room                     The Mansion (USA)
  The Gallery                           The Mansion (Japan)
  The Mansion                           The Mansion (PAL)
  The Hidden Mansion
```

* Only installed versions are listed (the game's choice box cannot grey out an entry, and a
  selectable entry that refuses is worse); with one disc there is no follow-up at all
  (retail menu, nothing served). F1 > Enhancements > Gameplay shows the missing versions
  greyed with "needs the Japanese/PAL/USA disc" (`port::Avail`), which is where the user
  learns how to get them.
* The base disc's own version is the first entry, so pressing A twice keeps today's
  behaviour. Order after it: USA, Japan, PAL.
* Text: the base's own "The Mansion" / "The Hidden Mansion" line plus " (USA)", " (Japan)",
  " (PAL)" (full-width 「（ＵＳＡ）」「（日本）」「（ＰＡＬ）」 in Japanese tables), appended to every
  message table of the archive as now. The choice-box shift hook (`fn_80043104`) covers the
  longer lines.
* The Hidden Mansion follow-up appears only once flag 5 (Hidden Mansion unlocked) is set, as
  now.

### How the choice is stored

* **Version field: two persistent game flags**, set by the lab script right before
  `<URALUIGI>` / the normal send-off, so they work from event scripts and are saved with the
  file: `(a, b)` = `(0,0)` the base disc's own version, `(1,0)` USA, `(0,1)` Japan, `(1,1)` PAL.
  Candidates: flags **60** and **76**, the only isolated low flags (0..84 persist; 85..255
  are cleared on every map load) that no event script, no JMP table of any release and no
  constant flag call in the USA code uses (`flags 22/25/28/31/34` also look free but sit in
  `event36`'s groups of three and may be addressed by computed index). Before using them,
  confirm with `LM_WATCH` on the flag bytes (`0x803D2D40 + 0x659 + n/8`) over a full
  playthrough on each base that nothing else writes them.
* The same field serves both mansions; the game's own hidden-mansion flag says which one.
  **Flag 8** keeps its meaning (set exactly when in the PAL Hidden Mansion), because the
  European `event16/73` read it for the 45-Boo gates. A future `hm::variant()` returns
  Us/Jp/Pal from the field (flag 8 alone still means PAL, for saves made with the current
  menu).
* Cleared when the lab is entered (as flag 8 is now) and on new game, so a new file and the
  first mansion visit (the opening goes to the foyer before the lab) use the base's own
  version, and every lab visit asks again. A save made inside a mansion keeps its version.
* F1 fallback (one disc, or an old overlay without the menu): "Mansion rules" and "Hidden
  Mansion version" combos (`src/port/settings.h`), replaced by a "chosen in E. Gadd's lab"
  note while the menu is installed, as today. A test override
  `--mansion-variant us|jp|pal` / `LM_MANSION_VARIANT` alongside `LM_HIDDEN_MANSION_VARIANT`.

### Implementation order (after japan-support lands)

1. Overlay builder: per installed disc, the rule files of 1.2/3 (`map2.szp`, `map4.szp`,
   `room_70.arc`, `room_08.arc`, `kopabody.szp`, PAL `game.szp`) under `versions/<id>/`, plus
   the lab script with both follow-ups (`tools/hidden_mansion_data.py` and its C++ twin
   `src/setup/hm_overlay.cpp`), overlay format bump.
2. `hle_dvd.cpp` redirect by (version field, hidden flag, base).
3. Code rules as one table per version (heart thresholds, damage x2, rank table, Chauncey
   distance, Bowser +0x1D4, death voice) applied by the existing hooks.
4. Identify the "unidentified" rows of 1.1 by A/B play on the Japanese and USA discs; add
   only what turns out to be a rule, not a bug.
5. Optional: Japanese soundtrack option, script pacing transform.

## 6. Implementation (branch japan-support)

**Data.** With two or more discs, setup builds `<data>/hidden_mansion/versions/`
(`src/setup/hm_overlay.cpp` `buildVersionsOverlay`, key `versions.key` over every disc's DOL
hash, rebuilt with the Hidden Mansion overlay): `<ID>/Map/map2.szp`, `Map/map4.szp`,
`Iwamoto/map2/room_70.arc`, `room_08.arc`, `model/kopabody.szp` of each installed disc, and
`lab/<base ID>/Event/event08.szp` for each possible base. The PAL `game.szp` (one item table) is
not served yet.

**Lab script** (per base, from that base's own archive and every language table in it): the
`"back"` (The Mansion) and `"urayasiki"` (The Hidden Mansion) cases become a follow-up
`<CHOICE>` "The Mansion (USA) / (Japan) / (PAL)" (the base's own first, then USA, Japan, PAL;
Japanese tables 「（ＵＳＡ）」「（日本）」「（ＰＡＬ）」), each branch writing `<FLAGON|OFF>(60)`/`(76)` (and
`(8)` for the Hidden Mansion: on only for PAL) right before `<OMOTELUIGI>` / `<URALUIGI>`;
flags 8, 60, 76 are cleared at the top of the script (entering the lab). With one disc nothing
is served. The PAL Hidden Mansion is not offered on the Japanese base. The versions menu
supersedes the two-variant lab menu (docs/HIDDEN_MANSION.md) whenever it is installed.

**Flags 60/76.** No event script of any release uses them (all three discs' `Event/*.szp`
scanned), and `LM_WATCH` on their bytes over the opening -> lab -> mansion routes showed only
the new-game clear and the lab script's own writes. A full playthrough with the watch has not
been run.

**Runtime** (`src/recomp/hidden_mansion.cpp`): `hm::versionField()` (the flags),
`hm::mansionVariant()` (normal mansion), `hm::variant()` (Hidden Mansion; flag 8 alone still
means PAL for older saves), `hm::rulesVariant()`, `hm::rules(v, hidden)` (the table below);
`redirect()` serves the chosen release's files (European base, hidden: for `map2u.szp`); the PAL
Hidden Mansion on the USA base and the USA one on the European base keep their overlays; the
Japanese Hidden Mansion on the European base uses the "USA rules" flag hiding (its `event16/73`
are the USA scripts; `map2u.szp` -> the Japanese `map2.szp`). `[rules]` log lines name the rules
on every change (the line prints the release's table, including items not implemented on that
base, below).

| Rule | USA | Japan | PAL | Implemented on |
|------|-----|-------|-----|----------------|
| Small heart threshold (normal / hidden) | 200 / 400 | 100 / 100 | 200 / 1000 | every base (`fn_800E70BC`/`fn_800E7698` hooks correct the base's own compare) |
| Damage x2 in the Hidden Mansion | yes | no | yes | USA/PAL base: `fn_800B9680` with the flag hidden; Japanese base: `fn_800B9488` doubles the hit |
| A rank | 100 M | 100 M | 150 M | every base (`VA(0x8020BE18)`, rewritten while the rules differ) |
| Chauncey stop distance | 15 | 10 | 20 (hidden 40) | USA base (patches 61/62; PAL hidden: existing hook), Japanese base (patches 62/63); not undone on the European base |
| Bowser thrown bombs +0x1D4 = 90 | - | - | yes | USA base only (`fn_800A926C`, a USA-only hook); not undone on the European base |
| Map data (ghost/item/heart tables, props, paths) | | | | every base (versions overlay) |

Not done: the PAL `game.szp` item table, the USA death voice, the unidentified rows of 1.1, the
European Hidden Mansion on the Japanese base, the Chauncey/Bowser undo on the European base,
Bowser's 90 on the Japanese base.

**F1 fallback** (one disc, or no versions overlay): Enhancements > General > *Mansion version*
and *Hidden Mansion version* list USA / Japan / PAL, greyed with "needs the X disc"
(settings `enhancements.mansion_version` 0 own / 1 USA / 2 Japan / 3 PAL, `hidden_mansion_jp`).
With the lab menu installed they become a note ("chosen in E. Gadd's lab", this visit's version,
missing discs). Test aids: `LM_MANSION_VARIANT=us|jp|pal`, `LM_HIDDEN_MANSION_VARIANT=us|jp|pal`.

**Verified** (LuigisMansionRecomp with all three discs, scratch AppData/card, route
`LM_START_MAP=1` + `LM_SKIP_TEST` flags 5/7, the follow-up picked with the stick): every
combination enters the foyer, logs the expected `[rules]` line and serves the expected files:
USA base: Mansion Japan / PAL, Hidden Japan / PAL; Japanese base: Mansion USA / PAL, Hidden USA /
own; European base: Mansion USA / Japan, Hidden USA / Japan / own (only its own is mirrored).
The base's own entry is the retail behaviour (nothing served). Saving and loading inside another
release's mansion keeps the choice by construction (flags 0..84 are saved with the file); not
run-tested. F1 ID sweep (`LM_MENU_ID_CHECK=exit`): 0 conflicts.
