#pragma once
// Which game release is running, and USA -> European / Japanese address mapping for host code
// that reads or patches game data (docs/SETUP.md, "Version-aware code").
//
// Host code is written against the USA game (GLME01): its addresses come from the USA
// symbol map. Wrap each such address in VA(0x...): on the USA game it is the address itself,
// on the European game (GLMP01) the matching address from src/recomp/version_addrs.inc, on the
// Japanese game (GLMJ01) the one from src/recomp/version_addrs_jp.inc. Both tables are generated
// by tools/recomp/gen_version_map.py (--target pal|jp|all) by scanning the sources for VA(...)
// and mapping each one with tools/pal_diff.py's function/data matching (code addresses: the
// same instruction in the matched function). Re-run the script after adding a VA(...).
//
// Struct field offsets are not generated: write VOFF(usa, european) after checking the field
// with `tools/pal_diff.py field` (several classes gained members in the European build, e.g.
// Luigi from +0x134C on is shifted by 4). VOFF gives the USA offset on the Japanese game
// (checked for every VOFF with `tools/pal_diff.py --jp field`); where the Japanese layout
// differs, write VOFF3(usa, european, japanese).
//
// Game functions are called/hooked by their USA names on every version (gen_version_map.py
// names their European/Japanese counterparts the same); hooks need `pal` / `jp` tags in hooks.txt.
#include "recomp/ctx.h"

namespace recomp::ver {

bool pal();  // the European game is running
bool jpn();  // the Japanese game is running
bool usa();  // the USA game is running (code on raw USA addresses must check this, not !pal())

// USA address -> running version's address. 0 (and a log line) when the running game has no
// mapping for it: version_addrs[_jp].inc is stale (re-run gen_version_map.py).
u32 addr(u32 usAddr);

inline u32 off(u32 usOff, u32 palOff) { return pal() ? palOff : usOff; }
inline u32 off3(u32 usOff, u32 palOff, u32 jpOff) { return pal() ? palOff : jpn() ? jpOff : usOff; }

}  // namespace recomp::ver

// Looked up once per use site (the version never changes while the game runs).
#define VA(usAddr)                                                                  \
    ([]() -> ::recomp::u32 {                                                        \
        static const ::recomp::u32 a = ::recomp::ver::addr(usAddr);                 \
        return a;                                                                   \
    }())
#define VOFF(usOff, palOff) (::recomp::ver::off((usOff), (palOff)))
#define VOFF3(usOff, palOff, jpOff) (::recomp::ver::off3((usOff), (palOff), (jpOff)))
