// Gameplay / difficulty options as hooks on the game (see gameplay.h,
// docs/GAMEPLAY_OPTIONS.md). Every hook calls the original unchanged when its option is at the
// neutral default, so the original game is untouched unless the player changes something.
#include "recomp/gameplay.h"
#include "recomp/ghost_lights.h"

#include "recomp/coop.h"
#include "recomp/coop_test.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/hidden_mansion.h"
#include "recomp/hle.h"
#include "recomp/version.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace recomp {
namespace gameplay {
namespace {

// Guest memory: MEM1, or the host-made objects above it (hleAlloc, e.g. the AI messages the
// test aids send through fn_800C0F08).
bool inMem1(u32 a) { return (a >= 0x80000000u && a < 0x81800000u) || (a >= 0x81820000u && a < 0x81F00000u); }
s16 rds16(u32 a) { return s16(rd16(a)); }

#define GP_FIRST(...)                                       \
    do {                                                    \
        static bool sLogged = false;                        \
        if (!sLogged) {                                     \
            sLogged = true;                                 \
            log("[gameplay] " __VA_ARGS__);                 \
        }                                                   \
    } while (0)

struct Overrides {
    bool any = false;
    port::GameplaySettings v;
    bool has[9] = {};
};

const Overrides& overrides() {
    static const Overrides o = [] {
        Overrides r;
        const char* env = std::getenv("LM_GAMEPLAY");
        if (!env || !*env) return r;
        std::string s = env;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t end = s.find(',', pos);
            if (end == std::string::npos) end = s.size();
            const std::string item = s.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = item.substr(0, eq);
            const float f = float(std::atof(item.c_str() + eq + 1));
            if (k == "dark") r.v.palDarknessEverywhere = f != 0, r.has[0] = true;
            else if (k == "ambient") r.v.ambientScale = f, r.has[1] = true;
            else if (k == "damage") r.v.damageScale = f, r.has[2] = true;
            else if (k == "health") r.v.healthScale = f, r.has[3] = true;
            else if (k == "ghosthp") r.v.ghostHealthScale = f, r.has[4] = true;
            else if (k == "hearts") r.v.noHeartDrops = f == 0, r.has[5] = true;
            else if (k == "vacuum") r.v.vacuumStrength = f, r.has[6] = true;
            else if (k == "ride") r.v.alwaysRide = f != 0, r.has[7] = true;
            else if (k == "mirror") r.v.mirrorWorld = f != 0, r.has[8] = true;
            else continue;
            r.any = true;
        }
        if (r.any) log("[gameplay] LM_GAMEPLAY overrides: %s", env);
        return r;
    }();
    return o;
}

}  // namespace

port::GameplaySettings current() {
    port::GameplaySettings g = port::settings().gameplay;
    const Overrides& o = overrides();
    if (o.any) {
        if (o.has[0]) g.palDarknessEverywhere = o.v.palDarknessEverywhere;
        if (o.has[1]) g.ambientScale = o.v.ambientScale;
        if (o.has[2]) g.damageScale = o.v.damageScale;
        if (o.has[3]) g.healthScale = o.v.healthScale;
        if (o.has[4]) g.ghostHealthScale = o.v.ghostHealthScale;
        if (o.has[5]) g.noHeartDrops = o.v.noHeartDrops;
        if (o.has[6]) g.vacuumStrength = o.v.vacuumStrength;
        if (o.has[7]) g.alwaysRide = o.v.alwaysRide;
        if (o.has[8]) g.mirrorWorld = o.v.mirrorWorld;
    }
    return g;
}

bool rideAllowed(u32 luigi) {
    // Only in the mansion (map 2) and the boss arenas (9 Bowser, 10 Chauncey, 11 Boolossus):
    // E. Gadd's lab, the training room, the gallery and the ending maps position Luigi by
    // script and are left alone. Off while Luigi has no health (death sequence).
    const u32 map = rd32(VA(0x804D80C8));  // sCurMapNo
    if (map != 2 && map != 9 && map != 10 && map != 11) return false;
    return s16(rd16(luigi + 0xFC)) > 0;
}

float ambientFactor() {
    const port::GameplaySettings g = current();
    float base = (hm::palRulesActive() || g.palDarknessEverywhere) ? 0.2f : 1.0f;
    // The European game applies the PAL hidden-mansion 0.2 itself (fn_800179F8).
    if (ver::pal() && hm::palRulesActive()) base = 1.0f;
    return base * g.ambientScale;
}

}  // namespace gameplay

using gameplay::current;
using gameplay::inMem1;
using gameplay::rds16;

// ---- Damage ------------------------------------------------------------------------------
// fn_800B9488(luigi, newHp) is the one place Luigi's health (+0xFC, s16) is set: it clamps to
// [0, max (+0xFFC)] and plays the hurt/heal reactions. Every damage source (ghost attacks,
// floors, bombs, the hidden mansion's x2) arrives here as a lower value, so the damage taken is
// rescaled here; healing passes through.
HLE(fn_800B9488) {
    // Calls from Luigi's message handler (fn_800AEEA8: event/script commands such as "set
    // health", e.g. the new game's health = 100 on arriving in the mansion) are not damage.
    // Their values are scaled for "Luigi's health" in callLuigiMessage below.
    static const u32 sMsg = guestSymbol("fn_800AEEA8");
    const bool scripted = c.lr >= sMsg && c.lr < sMsg + VOFF3(0x2884, 0x2884, 0x26AC);  // size of fn_800AEEA8
    if (scripted) return callOriginal(c, "fn_800B9488");
    // The Japanese game has no damage x2 (its damage sites call this directly): the USA Hidden
    // Mansion rules on the Japanese base double the hit here (docs/VERSION_DIFFERENCES.md).
    if (ver::jpn() && hm::hiddenActive() && hm::rules(hm::rulesVariant(), true).doubleDamage) {
        const s32 cur = rds16(c.r[3] + 0xFC), next = s16(c.r[4]);
        if (next < cur) {
            c.r[4] = u32(s32(cur - 2 * (cur - next))) & 0xFFFF;
            GP_FIRST("(%s Hidden Mansion rules on the Japanese game) damage x2", hm::variantName(hm::rulesVariant()));
        }
    }
    const float k = current().damageScale;
    if (k != 1.0f) {
        const u32 luigi = c.r[3];
        const s32 cur = rds16(luigi + 0xFC);
        const s32 next = s16(c.r[4]);
        if (next < cur) {
            const s32 dmg = cur - next;
            s32 scaled = s32(std::lround(float(dmg) * k));
            if (k > 0.0f && scaled == 0) scaled = 1;  // never round a hit away completely
            c.r[4] = u32(s32(cur - scaled)) & 0xFFFF;
            GP_FIRST("damage x%.2f: hit %d -> %d", k, dmg, scaled);
        }
    }
    if (coop::fixes()) coop::onLuigiHealth(c);  // 2-player co-op: a downed player is revived by the partner
    callOriginal(c, "fn_800B9488");
}

// ---- Luigi's health ---------------------------------------------------------------------
// Luigi's health and maximum (+0xFC / +0xFFC) live in his actor during a map; between maps and
// in the save file they are kept in his persistent player record (Luigi +0x79C = lbl_803D5CD0
// + 0x16C * player: +0 health, +2 maximum, also saved to the memory card). The record is only
// written by Luigi's message 59 (leaving a map) and read by fn_800AE1A4 (entering one).
//
// The record always holds the ORIGINAL scale (maximum 100): the actor's values are scaled up
// when a map restores them and scaled back down when he leaves, so nothing scaled reaches the
// save, a slider change never compounds, and the slider back at 1 gives the retail values.
// (Before this, the scaled values went into the record, so a save made with the option kept
// the changed maximum after the slider went back to 1.)
namespace gameplay {
namespace {
s32 scaleUp(s32 v, float k, s32 hi) { return v <= 0 ? v : std::clamp(s32(std::lround(v * k)), 1, hi); }
s32 scaleDown(s32 v, float k) { return v <= 0 ? v : std::clamp(s32(std::lround(v / k)), 1, 32767); }
bool logOn() {
    static const bool on = std::getenv("LM_GAMEPLAY_LOG") != nullptr;
    return on;
}

// Health scale each live Luigi actor was set up with (a slider change applies at the next map,
// and a Luigi is always converted back with the factor it was converted up with).
struct LuigiScale {
    u32 actor = 0;
    float k = 1.0f;
};
LuigiScale sLuigiScale[4];
float luigiScale(u32 luigi) {
    for (const LuigiScale& s : sLuigiScale)
        if (s.actor == luigi) return s.k;
    return 1.0f;
}
void setLuigiScale(u32 luigi, float k) {
    LuigiScale* slot = nullptr;
    for (LuigiScale& s : sLuigiScale)
        if (s.actor == luigi) slot = &s;
    if (!slot) {
        static u32 sNext = 0;
        slot = &sLuigiScale[sNext++ % 4];
    }
    slot->actor = luigi;
    slot->k = k;
}

// Heal amount given to recovery hearts set up while "Disable recovery hearts" is on. A heal of
// 0 would mean "full health" to Luigi's message 13, so a marker is used and swallowed below.
constexpr u32 kNoHeal = 0x4E4F4845u;  // "NOHE"
}  // namespace

// Luigi's message handler fn_800AEEA8(luigi, id, data), wrapped by HLE(fn_800AEEA8) in
// luigi_states.cpp. Message ids are the same in both releases.
void callLuigiMessage(Ctx& c) {
    const u32 luigi = c.r[3], id = c.r[4], data = c.r[5];
    // 13: heal by data+0x44 (0 = to full); a heart set up with hearts disabled heals nothing.
    if (id == 13 && inMem1(data) && rd32(data + 0x44) == kNoHeal) {
        GP_FIRST("hearts disabled: heart heals nothing");
        c.r[3] = 1;
        return;
    }
    const float k = inMem1(luigi) ? luigiScale(luigi) : 1.0f;
    // 14 / 15: event script commands that set health / maximum health to data+0x48 / +0x4C
    // (e.g. health = 100 when a new game arrives in the mansion), in original units.
    u32 field = 0, fieldOrig = 0;
    if (k != 1.0f && inMem1(data) && (id == 14 || id == 15)) {
        field = data + (id == 14 ? 0x48 : 0x4C);
        fieldOrig = rd32(field);
        if (s32(fieldOrig) > 0) {
            wr32(field, u32(scaleUp(s32(fieldOrig), k, 999)));
            GP_FIRST("health x%.2f: scripted %s %d -> %d", k, id == 14 ? "health" : "maximum", s32(fieldOrig),
                     s32(rd32(field)));
        } else {
            field = 0;
        }
    }
    // 21: a capture; for the boss portraits (types 64, 213, 220, 221) the portrait's rank comes
    // from Luigi's health (>= 90 / >= 60), so the handler sees it in original units.
    s32 hpOrig = 0;
    const bool rank = k != 1.0f && id == 21 && inMem1(luigi);
    if (rank) {
        hpOrig = rds16(luigi + 0xFC);
        wr16(luigi + 0xFC, u32(scaleDown(hpOrig, k)) & 0xFFFF);
    }
    callOriginal(c, "fn_800AEEA8");
    if (rank) wr16(luigi + 0xFC, u32(hpOrig) & 0xFFFF);
    if (field) wr32(field, fieldOrig);
    // 59: leaving the map, health and maximum were copied into the record: store them in
    // original units.
    if (id == 59 && inMem1(luigi)) {
        const u32 rec = rd32(luigi + 0x79C);
        if (!inMem1(rec)) return;
        if (k != 1.0f) {
            wr16(rec + 0, u32(scaleDown(rds16(rec + 0), k)) & 0xFFFF);
            wr16(rec + 2, u32(scaleDown(rds16(rec + 2), k)) & 0xFFFF);
        }
        if (logOn())
            log("[gameplay] Luigi %08X leaves: hp %d max %d (x%.2f) -> record %08X hp %d max %d", luigi,
                rds16(luigi + 0xFC), rds16(luigi + 0xFFC), k, rec, rds16(rec), rds16(rec + 2));
    }
}

}  // namespace gameplay

// Luigi's constructor sets health and maximum to 100. Scaled here for a Luigi that no record
// restores (fn_800AE1A4 below normally follows and sets both from the record).
HLE(fn_800AD5AC) {
    const u32 luigi = c.r[3];
    callOriginal(c, "fn_800AD5AC");
    if (!inMem1(luigi)) return;
    const float k = current().healthScale;
    gameplay::setLuigiScale(luigi, k);
    if (k == 1.0f) return;
    const s32 maxHp = gameplay::scaleUp(rds16(luigi + 0xFFC), k, 999);
    wr16(luigi + 0xFFC, u32(maxHp));
    wr16(luigi + 0xFC, u32(maxHp));
    GP_FIRST("health x%.2f: Luigi starts with %d", k, maxHp);
}

// Entering a map: health and maximum come from the record (original units) and are scaled.
HLE(fn_800AE1A4) {
    const u32 luigi = c.r[3];
    callOriginal(c, "fn_800AE1A4");
    if (!inMem1(luigi)) return;
    const float k = current().healthScale;
    gameplay::setLuigiScale(luigi, k);
    const u32 rec = rd32(luigi + 0x79C);
    if (k != 1.0f && inMem1(rec)) {
        const s32 maxHp = gameplay::scaleUp(rds16(rec + 2), k, 999);
        const s32 hp = rds16(rec + 0) > 0 ? std::min(gameplay::scaleUp(rds16(rec + 0), k, 999), maxHp) : 1;
        wr16(luigi + 0xFFC, u32(maxHp) & 0xFFFF);
        wr16(luigi + 0xFC, u32(hp) & 0xFFFF);
        static const u32 sHudHealth = VA(0x80188D04);  // the original passes the restored health here
        const Ctx saved = c;
        guestCall(c, sHudHealth, u32(hp));
        c = saved;
        GP_FIRST("health x%.2f: restored Luigi %d / %d (record %d / %d)", k, hp, maxHp, rds16(rec), rds16(rec + 2));
    }
    if (gameplay::logOn())
        log("[gameplay] restore Luigi %08X hp %d max %d (x%.2f; record %08X hp %d max %d)", luigi,
            rds16(luigi + 0xFC), rds16(luigi + 0xFFC), k, rec, inMem1(rec) ? rds16(rec) : 0,
            inMem1(rec) ? rds16(rec + 2) : 0);
}

// ---- Ghost HP ---------------------------------------------------------------------------
// EnThought (the AI of every character, Luigi's included) sets its actor's HP (+0xFC) on
// messages 6 (HP from the spawn data) and 7 (HP from the per-type table / EnTypesManager::
// getEnemyLife), and mirrors it in +0x89C. Scaled right after, for ghosts only: Luigi (type 0)
// gets message 6 with his carried-over health from fn_800AE1A4 on every map load (the v1.0
// bug: "Ghost health" below 1 lowered Luigi's health at each map load, compounding).
//
// Portrait ghosts keep their HP between encounters in player 1's record (+4 + 2 * index, index
// from the portrait table fn_800553FC; saved to the card): message 7 reads it and the thought's
// update fn_800C08DC writes the current HP back every frame. The record keeps original units:
// scaled when read, scaled back when written, with the factor the ghost was set up with.
namespace gameplay {
namespace {
struct GhostScale {
    u32 thought = 0;
    float k = 1.0f;
};
GhostScale sGhostScale[64];
float ghostScale(u32 thought) {
    for (const GhostScale& s : sGhostScale)
        if (s.thought == thought) return s.k;
    return 1.0f;
}
void setGhostScale(u32 thought, float k) {
    GhostScale* slot = nullptr;
    for (GhostScale& s : sGhostScale)
        if (s.thought == thought) slot = &s;
    if (!slot && k == 1.0f) return;
    if (!slot) {
        for (GhostScale& s : sGhostScale)
            if (!s.thought) slot = &s;
        static u32 sNext = 0;
        if (!slot) slot = &sGhostScale[sNext++ % 64];
    }
    slot->thought = k == 1.0f ? 0 : thought;
    slot->k = k;
}
// Portrait-table index of a ghost whose HP the record keeps, else -1 (the same two tests as
// the original message 7 and fn_800C08DC).
s32 portraitIndex(Ctx& c, u32 actor) {
    static const u32 sIsPortrait = VA(0x800E9E48), sIndex = VA(0x800553FC);
    const u32 type = rd32(actor + 0x3C);
    const Ctx saved = c;
    const s32 idx = (guestCall(c, sIsPortrait, type) & 0xFF) == 0 ? -1 : s32(guestCall(c, sIndex, type));
    c = saved;
    return idx;
}
u32 portraitRecord(s32 idx) { return VA(0x803D5CD0) + 4 + 2 * u32(idx); }  // player 1's record
}  // namespace
}  // namespace gameplay

HLE(fn_800C0F08) {
    const u32 self = c.r[3];
    const u32 msg = inMem1(c.r[4]) ? rd32(c.r[4]) : 0;
    if (coop::active()) coop::onGhostMessage(c);  // 2-player mode: messages from player 2
    cooptest::onMessage(self, msg);                // coop_test.cpp test aids (AI objects of the map)
    if (inMem1(self)) ghostlights::onCharacterEvent(rd32(self + 0x808), msg, self, c.r[4]);  // ghost_lights.cpp
    if (msg == 2 && inMem1(self)) enh::onGhostInBeam(rd32(self + 0x808));  // realistic flashlight flare
    callOriginal(c, "fn_800C0F08");
    if (msg == 21 && inMem1(self) && ghostlights::testForceLit(rd32(self + 0x808))) c.r[3] = 1;  // LM_GHOST_HUNT_BEAM
    if ((msg != 6 && msg != 7) || !inMem1(self)) return;
    const u32 actor = rd32(self + 0x808);
    if (!inMem1(actor) || rd32(actor + 0x3C) == 0) return;  // type 0: Luigi (both players, Mario)
    const float k = current().ghostHealthScale;
    gameplay::setGhostScale(self, k);
    if (k == 1.0f) return;
    const s32 idx = msg == 7 ? gameplay::portraitIndex(c, actor) : -1;
    const s32 hp = rds16(actor + 0xFC);
    if (hp <= 0) return;
    const s32 scaled = gameplay::scaleUp(hp, k, 32767);
    if (gameplay::logOn())
        log("[gameplay] ghost HP msg %u thought %08X actor %08X type %u%s: %d -> %d", msg, self, actor,
            rd32(actor + 0x3C), idx >= 0 ? " (portrait, HP from the record)" : "", hp, scaled);
    wr16(actor + 0xFC, u32(scaled));
    wr32(self + 0x89C, u32(scaled));
    GP_FIRST("ghost HP x%.2f: %d -> %d", k, hp, scaled);
}

// The thought's update: after the original wrote a portrait ghost's current HP into the
// record, store it in original units.
HLE(fn_800C08DC) {
    const u32 self = c.r[3];
    callOriginal(c, "fn_800C08DC");
    if (!inMem1(self)) return;
    const float k = gameplay::ghostScale(self);
    if (k == 1.0f) return;
    const u32 actor = rd32(self + 0x808);
    if (!inMem1(actor)) return;
    const s32 idx = gameplay::portraitIndex(c, actor);
    if (idx < 0 || idx >= 23) return;
    const u32 rec = gameplay::portraitRecord(idx);
    const s32 hp = rds16(actor + 0xFC);
    const s32 orig = gameplay::scaleDown(hp, k);
    if (gameplay::logOn() && rds16(rec) != orig) {
        static s32 sLast[23];
        if (sLast[idx] != orig) log("[gameplay] portrait ghost %d HP %d (x%.2f) -> record %d", idx, hp, k, orig);
        sLast[idx] = orig;
    }
    wr16(rec, u32(orig) & 0xFFFF);
}

// ---- Vacuum strength --------------------------------------------------------------------
// The Poltergust capture update drains the captured ghost's HP by one on each drain tick
// (capture +0x98 = ghost HP - 1, ghost at capture +0x18). The strength scales the ticks: each
// tick removes `strength` HP, with fractions carried over (per ghost).
HLE(fn_80081BCC) {
    const float k = current().vacuumStrength;
    const u32 cap = c.r[3];
    const u32 ghost = inMem1(cap) ? rd32(cap + 0x18) : 0;
    if (inMem1(ghost)) ghostlights::onVacuumTick(ghost);  // ghost_lights.cpp
    if (k == 1.0f || !inMem1(ghost)) return hm::callWithFlag(c, "fn_80081BCC");
    const s32 before = rds16(ghost + 0xFC);
    hm::callWithFlag(c, "fn_80081BCC");
    const s32 after = rds16(cap + 0x98);
    if (before <= 0 || after != before - 1) return;  // no drain tick this frame
    static u32 sGhost = 0;
    static float sCarry = 0.0f;
    if (ghost != sGhost) sGhost = ghost, sCarry = 0.0f;
    sCarry += k;
    const s32 n = s32(sCarry);
    sCarry -= float(n);
    wr16(cap + 0x98, u32(std::max(0, before - n)));
    GP_FIRST("vacuum x%.2f: drain tick removes %d (ghost HP %d)", k, n, before);
    if (gameplay::logOn()) log("[gameplay] vacuum x%.2f: ghost %08X HP %d -> %d", k, ghost, before, std::max(0, before - n));
}

// ---- Recovery hearts --------------------------------------------------------------------
namespace {
bool isHeartName(const char* s) {
    static const char* kHearts[] = {"sheart", "mheart", "lheart", "move_sheart", "move_mheart", "move_lheart"};
    for (const char* h : kHearts)
        if (std::strcmp(s, h) == 0) return true;
    return false;
}
u32 guestNothing() {
    static u32 a = [] {
        const u32 p = hleAlloc(8, 4);
        std::memcpy(hostp(p), "nothing", 8);
        return p;
    }();
    return a;
}
}  // namespace

// Item drops are named in the map's JMP tables (furniture/ghost item tables, itemappeartable,
// iteminfotable) and read through ToolData::getValue(row, field, const char**). With hearts
// disabled, any heart item name read from a table is turned into "nothing".
HLE(getValue__Q24Koga8ToolDataCFiPCcPPCc) {
    const u32 out = c.r[6];
    callOriginal(c, "getValue__Q24Koga8ToolDataCFiPCcPPCc");
    // LM_HEART_TABLES=0 (testing): leave the table names alone, so the spawn-level block in
    // fn_800E8904 below is what stops the hearts.
    static const bool sTables = [] {
        const char* v = std::getenv("LM_HEART_TABLES");
        return !(v && v[0] == '0');
    }();
    if (!sTables || !current().noHeartDrops || !inMem1(out)) return;
    const u32 str = rd32(out);
    if (!inMem1(str) || !isHeartName(gstr(str))) return;
    GP_FIRST("hearts disabled: table item %s -> nothing", gstr(str));
    wr32(out, guestNothing());
}

// Recovery hearts are characters of type 205 (sheart: 10, or 20 past the heart counter) and 206
// (lheart: 50); "mheart" and the move_* names in the tables are the same two types. Every item
// that appears in the mansion (furniture searched or vacuumed, ghost drops, scripted items, the
// random heart/mushroom of fn_800A9DE0, boss items) is spawned by fn_800E8904(type, pos, dir),
// which takes a character of that type from the map's pool (EnManager::create) and places it.
// It returns the pool index, or -1 when the pool has none: a result every caller handles (the
// item simply does not appear). With hearts disabled it returns -1 for the two heart types
// before anything is created.
namespace {
bool isHeartType(u32 type) { return type == 205 || type == 206; }
bool heartLog() {
    static const bool on = std::getenv("LM_HEART_LOG") != nullptr;
    return on;
}
// The guest call chain (return addresses) for the logs.
void callChain(Ctx& c, char* out, size_t size) {
    int n = std::snprintf(out, size, "%08X", c.lr);
    u32 sp = c.r[1];
    for (int i = 0; i < 6 && inMem1(sp) && n > 0 && size_t(n) < size; i++) {
        sp = rd32(sp);
        if (!inMem1(sp)) break;
        n += std::snprintf(out + n, size - size_t(n), " %08X", rd32(sp + 4));
    }
}
}  // namespace

HLE(fn_800E8904) {
    const u32 type = c.r[3];
    if (isHeartType(type) && current().noHeartDrops) {
        if (heartLog()) {
            char chain[160];
            callChain(c, chain, sizeof chain);
            log("[hearts] blocked item spawn type %u: %s", type, chain);
        }
        GP_FIRST("hearts disabled: heart item (type %u) not spawned", type);
        c.r[3] = u32(-1);
        return;
    }
    callOriginal(c, "fn_800E8904");
}

// The character pools: EnManager::create(type, toolData, -1) and the map's own placements
// (vt_0C, characters listed in the map data) both take a character here. A map-placed heart
// is refused here (vt_0C treats 0 as "not placed"); a heart from any path that bypasses
// fn_800E8904 is logged (LM_HEART_LOG) so a new source shows up in testing.
HLE(fn_800E58D4__Q24Koga9EnManagerFUlPQ24Koga8ToolDatal) {
    const u32 type = c.r[4];
    if (isHeartType(type) && (heartLog() || current().noHeartDrops)) {
        static const u32 sPlace = guestSymbol("vt_0C__Q24Koga9EnManagerFP11ToolDataRef");
        const bool placed = c.lr >= sPlace && c.lr < sPlace + 0x3BC;  // size of vt_0C (both releases)
        if (heartLog()) {
            char chain[160];
            callChain(c, chain, sizeof chain);
            log("[hearts] create type %u (%s)%s: %s", type, placed ? "map placement" : "pool",
                current().noHeartDrops ? (placed ? ", refused" : ", NOT through fn_800E8904") : "", chain);
        }
        if (placed && current().noHeartDrops) {
            GP_FIRST("hearts disabled: map-placed heart not created");
            c.r[3] = 0;
            return;
        }
    }
    callOriginal(c, "fn_800E58D4__Q24Koga9EnManagerFUlPQ24Koga8ToolDatal");
}

// EnHeartItem setup (sets how much the heart heals, +0x238; 10/20/50). Safety net only: a heart
// that still appears with hearts disabled heals nothing. Its pickup (fn_800D1A24) sends the
// amount to Luigi as message 13, where 0 means "heal to full" (v1.0 wrote 0 here, so such a
// heart healed Luigi completely); a marker is swallowed in gameplay::callLuigiMessage instead.
HLE(fn_800D1458) {
    const u32 self = c.r[3];
    callOriginal(c, "fn_800D1458");
    if (!current().noHeartDrops || !inMem1(self)) return;
    wr32(self + 0x238, gameplay::kNoHeal);
    log("[gameplay] hearts disabled, but a heart item was set up (%08X): it heals nothing", self);
}

}  // namespace recomp
