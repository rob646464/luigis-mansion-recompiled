// PAL Hidden Mansion tuning on the US build: hooks (tools/recomp/hooks.txt) that run the
// original US function and adjust its inputs or results while hm::palActive().
// What each change is, and where PAL makes it: docs/HIDDEN_MANSION.md ("Difficulty and tuning").
#include "recomp/dev_tools.h"
#include "recomp/gameplay.h"
#include "recomp/hidden_mansion.h"
#include "recomp/hle.h"
#include "recomp/lightning_test.h"
#include "recomp/mirror_world.h"
#include "recomp/runtime.h"
#include "recomp/silly_world.h"
#include "recomp/version.h"
#include "recomp/widescreen.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace recomp {
namespace {

float rdf(u32 a) { return std::bit_cast<float>(rd32(a)); }
void wrf(u32 a, float v) { wr32(a, std::bit_cast<u32>(v)); }
bool inMem1(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }

// One log line the first time each hook changes something (verification aid).
#define HM_FIRST(what)                                      \
    do {                                                    \
        static bool sLogged = false;                        \
        if (!sLogged) {                                     \
            sLogged = true;                                 \
            log("[hm] PAL rule applied: %s", what);         \
        }                                                   \
    } while (0)

// Runs the original with some .sdata2 literals temporarily replaced (they are only read by
// that function in the call's dynamic extent; see the per-hook notes).
struct ScopedLiteral {
    u32 addr, saved;
    ScopedLiteral(u32 a, float v) : addr(a), saved(rd32(a)) { wrf(a, v); }
    ~ScopedLiteral() { wr32(addr, saved); }
};

// The player actor (fn_800AD39C(0)); the caller's registers are left untouched.
u32 player(Ctx& c) {
    static const u32 fn = guestSymbol("fn_800AD39C");
    const Ctx saved = c;
    const u32 p = guestCall(c, fn, 0);
    c = saved;
    return p;
}

// Calls a guest function with r3 and f1..f3 set; the caller's registers are left untouched.
u32 guestCallF(Ctx& c, u32 fn, u32 r3, double f1, double f2 = 0, double f3 = 0) {
    const Ctx saved = c;
    c.r[3] = r3;
    c.f[1] = f1;
    c.f[2] = f2;
    c.f[3] = f3;
    c.ps1[1] = f1;
    c.ps1[2] = f2;
    c.ps1[3] = f3;
    c.call(fn);
    const u32 r = c.r[3];
    c = saved;
    return r;
}

u32 guestCall1(Ctx& c, u32 fn, u32 a0) {
    const Ctx saved = c;
    const u32 r = guestCall(c, fn, a0);
    c = saved;
    return r;
}

#define kCurMapNo VA(0x804D80C8)  // sCurMapNo

constexpr u32 kHeartCounter = 0x6C8;  // player field compared by the heart-item logic



// Heart items (docs/VERSION_DIFFERENCES.md "Hearts"): a small heart heals 20 instead of 10 once
// the player's treasure counter reaches the threshold of the release whose rules apply (USA
// 200 / Hidden 400, Japan 100, PAL 200 / Hidden 1000). The running game compares with its own
// (base) threshold; where the two differ the result is corrected.
int baseHeart(bool hidden) { return hm::rules(hm::baseVariant(), hidden).heartThreshold; }
int wantHeart(bool hidden) { return hm::rules(hm::rulesVariant(), hidden).heartThreshold; }

#define RULE_FIRST(...)                                     \
    do {                                                    \
        static bool sLogged = false;                        \
        if (!sLogged) {                                     \
            sLogged = true;                                 \
            log("[rules] " __VA_ARGS__);                    \
        }                                                   \
    } while (0)

}  // namespace

// Heart-item check: true when the player's counter reaches the threshold.
HLE(fn_800E70BC) {
    hm::callWithFlag(c, "fn_800E70BC");
    const bool hidden = hm::hiddenActive();
    const int want = wantHeart(hidden), base = baseHeart(hidden);
    if (want == base) return;
    const u32 p = player(c);
    if (inMem1(p)) c.r[3] = u32(s32(rd32(p + kHeartCounter)) >= want);
    RULE_FIRST("fn_800E70BC heart threshold %d (%s rules; this game %d)", want, hm::variantName(hm::rulesVariant()), base);
}

// Item selection switch; one case picks between two items on the same threshold. That case
// reads the counter once, so it is presented on the chosen rules' side of the game's own
// threshold for the duration of the call.
HLE(fn_800E7698) {
    const bool hidden = hm::hiddenActive();
    const int want = wantHeart(hidden), base = baseHeart(hidden);
    const u32 p = want != base ? player(c) : 0;
    if (!inMem1(p)) return hm::callWithFlag(c, "fn_800E7698");
    const s32 v = s32(rd32(p + kHeartCounter));
    const s32 shown = v >= want ? std::max(v, s32(base)) : std::min(v, s32(base - 1));
    if (shown != v) wr32(p + kHeartCounter, u32(shown));
    hm::callWithFlag(c, "fn_800E7698");
    if (shown != v && rd32(p + kHeartCounter) == u32(shown)) wr32(p + kHeartCounter, u32(v));
    if (shown != v) RULE_FIRST("fn_800E7698 heart threshold %d (%s rules; this game %d)", want, hm::variantName(hm::rulesVariant()), base);
}

// Chauncey (KtEnBaby) approach state: PAL hidden uses 40 for both the distance test (US 15,
// literal 0x804DB240, only this function) and the move speed (US 20, literal 0x804DB234, also
// read by fn_8009F4AC, which this function does not call).
HLE(fn_8009FD54) {
    if (ver::pal()) return hm::callWithoutFlag(c, "fn_8009FD54");
    if (!hm::palActive()) return callOriginal(c, "fn_8009FD54");
    ScopedLiteral dist(0x804DB240, 40.0f), speed(0x804DB234, 40.0f);
    HM_FIRST("fn_8009FD54 Chauncey distance/speed 40");
    callOriginal(c, "fn_8009FD54");
}

// Chauncey "POYON" state entry: PAL hidden also sets the model's +0xD8 to 2.0.
HLE(fn_8009FCA8) {
    if (ver::pal()) return hm::callWithoutFlag(c, "fn_8009FCA8");
    const u32 self = c.r[3];
    callOriginal(c, "fn_8009FCA8");
    if (!hm::palActive()) return;
    const u32 actor = rd32(self + 4);
    const u32 model = inMem1(actor) ? rd32(actor + 0x808) : 0;
    if (inMem1(model)) {
        wrf(model + 0xD8, 2.0f);
        HM_FIRST("fn_8009FCA8 Chauncey +0xD8 = 2.0");
    }
}

// Enemy strategy (state table 0x8034BB48) state entry: timer 400 -> 250 in the PAL hidden mansion.
HLE(fn_8006D1B8) {
    if (ver::pal()) return hm::callWithoutFlag(c, "fn_8006D1B8");
    const u32 self = c.r[3];
    callOriginal(c, "fn_8006D1B8");
    if (hm::palActive() && rd32(self + 0x1CC) == 400) {
        wr32(self + 0x1CC, 250);
        HM_FIRST("fn_8006D1B8 state timer 250");
    }
}

// Room ambient colour. PAL multiplies the room's ambient colour by 0.2 in the hidden mansion
// (both the lit-room x0.6 path and the unlit path); the gameplay options add their own factor
// (gameplay::ambientFactor() combines both). The current room record's RGB bytes are scaled
// for the duration of the call.
static void roomUpdateAmbient(Ctx& c) {
    const float k = gameplay::ambientFactor();
    const bool disco = sillyworld::discoOn();  // disco mansion (F1 > Enhancements > Silly)
    if (k == 1.0f && !disco) return callOriginal(c, "fn_800168EC");
    // Same record layout in both releases (European: read by the PAL-only fn_800179F8).
    const u32 roomNo = rd32(VA(0x803A3CA0) + 0xC) >> 24;
    const u32 rooms = rd32(VA(0x804D8178));
    const u32 rec = inMem1(rooms) ? rd32(rooms + roomNo * 0x14C + 0x18) : 0;
    if (!inMem1(rec)) return callOriginal(c, "fn_800168EC");
    u8 saved[3];
    for (int i = 0; i < 3; i++) {
        saved[i] = u8(rd8(rec + 0x2C + i));
        const float v = float(saved[i]) * k;
        hostp(rec + 0x2C + i)[0] = u8(v > 255.0f ? 255.0f : v);
    }
    if (disco) sillyworld::discoAmbient(hostp(rec + 0x2C));
    if (hm::palActive()) HM_FIRST("fn_800168EC room ambient x0.2");
    callOriginal(c, "fn_800168EC");
    for (int i = 0; i < 3; i++) hostp(rec + 0x2C + i)[0] = saved[i];
}
HLE(fn_800168EC) {
    const bool strike = lightning::beginRoomUpdate();  // LM_LIGHTNING test aid
    roomUpdateAmbient(c);
    lightning::endRoomUpdate(strike);
}

// ---- Bosses ------------------------------------------------------------------------------

// Luigi's per-frame update. The US Luigi already has a "Boolossus" movement mode behind byte
// +0x134C (PAL +0x1350): faster run (38 vs 17/18), its own animations, vacuum handling and a
// sound loop, read by eight Luigi functions. US never enables it; PAL turns it on in map 11
// (the Boolossus fight) in the hidden mansion once Luigi has spent 15 frames there, and keeps
// it off everywhere else. The 15-frame counter (PAL Luigi +0x134C) is kept host-side.
HLE(fn_800B783C) {
    static u32 sLuigi = 0, sFrames = 0;
    static bool sRideSet = false;  // the "always ride" option turned the mode on
    const u32 self = c.r[3];
    dev::noteLuigi(self);  // info overlay position
    static const bool sTrace = std::getenv("LM_HM_TRACE_LUIGI") != nullptr;  // dev: position log
    static u32 sTick = 0;
    if (ver::pal()) {
        // The European update sets the mode byte itself every frame (on in map 11 of the hidden
        // mansion after 15 frames, else off); fn_800B84E4 below overrides it for the options.
        callOriginal(c, "fn_800B783C");
        if (sTrace && ++sTick % 30 == 0)
            log("[hm] luigi map %u pos %.1f %.1f %.1f mode %u", rd32(kCurMapNo), rdf(self + 0x44), rdf(self + 0x48),
                rdf(self + 0x4C), u32(rd8(self + 0x1350)));
        return;
    }
    const bool ride = gameplay::current().alwaysRide && gameplay::rideAllowed(self);
    if (hm::palActive() && rd32(kCurMapNo) == 11) {
        if (self != sLuigi) sLuigi = self, sFrames = 0;
        ++sFrames;
        const bool on = sFrames > 15 || ride;
        if (on) HM_FIRST("fn_800B783C Boolossus movement mode");
        hostp(self + 0x134C)[0] = on ? 1 : 0;
    } else {
        sFrames = 0;
        if (ride) {
            if (!sRideSet) log("[gameplay] always ride the Poltergust: on");
            hostp(self + 0x134C)[0] = 1;
            sRideSet = true;
        } else if (hm::palActive() || sRideSet) {
            if (sRideSet) log("[gameplay] always ride the Poltergust: suspended/off");
            hostp(self + 0x134C)[0] = 0;
            sRideSet = false;
        }
    }
    callOriginal(c, "fn_800B783C");
    if (sTrace && ++sTick % 30 == 0)
        log("[hm] luigi map %u pos %.1f %.1f %.1f mode %u", rd32(kCurMapNo), rdf(self + 0x44), rdf(self + 0x48),
            rdf(self + 0x4C), u32(rd8(self + 0x134C)));
}

// European game: Luigi's state update, called by fn_800B783C after it has set the riding mode
// byte (+0x1350) for this frame. "Always ride the Poltergust" turns it on, the USA Hidden
// Mansion rules off (no Boolossus movement mode in the USA release).
HLE(fn_800B84E4) {
    const u32 self = c.r[3];
    if (ver::pal() && inMem1(self)) {
        const u32 mode = self + VOFF(0x134C, 0x1350);
        static bool sRideSet = false;
        if (gameplay::current().alwaysRide && gameplay::rideAllowed(self)) {
            if (!sRideSet) log("[gameplay] always ride the Poltergust: on");
            sRideSet = true;
            hostp(mode)[0] = 1;
        } else {
            if (sRideSet) log("[gameplay] always ride the Poltergust: suspended/off");
            if (sRideSet || hm::usaOnPalActive()) hostp(mode)[0] = 0;
            if (hm::usaOnPalActive() && rd32(kCurMapNo) == 11) HM_FIRST("(USA rules on the European game) no Boolossus movement mode");
            sRideSet = false;
        }
    }
    callOriginal(c, "fn_800B84E4");
}

// Luigi on a damaging floor: US hurts him every 2nd frame of the floor timer (+0x11CC); PAL
// hidden uses the 8-frame pattern 1,1,0,1,1,0,1,0. The timer is presented to the original so
// that its "every 2nd frame" test gives PAL's answer, then advanced as usual.
HLE(fn_800B41E0) {
    if (ver::pal()) return hm::callWithoutFlag(c, "fn_800B41E0");
    if (!hm::palActive()) return callOriginal(c, "fn_800B41E0");
    static constexpr u8 kPattern[8] = {1, 1, 0, 1, 1, 0, 1, 0};
    const u32 self = c.r[3];
    const float tf = rdf(self + 0x11CC);
    const u32 t = tf >= 0 ? u32(tf) : 0;
    const u32 shown = kPattern[t & 7] ? (t & ~1u) : (t | 1u);
    wrf(self + 0x11CC, float(shown));
    callOriginal(c, "fn_800B41E0");
    if (rdf(self + 0x11CC) == float(shown) + 1.0f) {
        wrf(self + 0x11CC, float(t) + 1.0f);
        if (kPattern[t & 7] != ((t & 1) == 0)) HM_FIRST("fn_800B41E0 damaging-floor pattern");
    } else if (shown != 0 && rdf(self + 0x11CC) == float(shown)) {
        wrf(self + 0x11CC, tf);  // timer untouched by the original: put the real value back
    }
}

// Chauncey's ball spawner: balls 0 and 1 of each volley start at z = +700 in the PAL hidden
// mansion instead of -700 (ball 2 keeps z = -500). The spawn position is set again on the ball.
HLE(fn_8009F1DC) {
    if (ver::pal()) return hm::callWithoutFlag(c, "fn_8009F1DC");
    if (!hm::palActive()) return callOriginal(c, "fn_8009F1DC");
    const u32 self = c.r[3];
    const u32 before = rd32(self + 0x1F4);
    callOriginal(c, "fn_8009F1DC");
    if (before == 0 || rd32(self + 0x1F4) != before - 1) return;  // nothing spawned
    const u32 idx = (5 - before) % 3;
    if (idx == 2) return;
    const u32 ball = rd32(self + 0x21C + idx * 4);
    if (!inMem1(ball)) return;
    static const u32 setPos = guestSymbol("fn_80067CB0");
    guestCallF(c, setPos, ball, rdf(ball + 0x44), rdf(ball + 0x48), 700.0);
    HM_FIRST("fn_8009F1DC Chauncey ball spawn z = +700");
}

// KtEnMoku2 (Chauncey's rocking horses) charge: in the PAL hidden mansion the horse weaves
// side to side (x velocity 100*sin(20 deg * t), t = the global timer at 0x804D8BF8), faces
// its direction of travel, and its hit shapes against Luigi are twice as large.
HLE(fn_800A184C) {
    if (ver::pal()) return hm::callWithoutFlag(c, "fn_800A184C");
    if (!hm::palActive()) return callOriginal(c, "fn_800A184C");
    const u32 actor = rd32(c.r[3] + 4);
    const u32 model = inMem1(actor) ? rd32(actor + 0x808) : 0;
    if (!inMem1(model)) return callOriginal(c, "fn_800A184C");

    // Weave: same sine table and angle units as the game (0x10000 = 360 degrees).
    const float t = rdf(0x804D8BF8);
    const s32 ang = s32(65536.0f * (20.0f * t) / 360.0f);
    const float s = rdf(0x80218238 + ((u32(ang) >> 2) & 0x3FFC));
    const float vx = 100.0f * s;
    wrf(model + 0x6C, vx);
    static const u32 atan2Fn = guestSymbol("fn_8000965C");
    const u32 yaw = guestCallF(c, atan2Fn, 0, rdf(model + 0x74), vx);
    wr16(model + 0x88, u16(yaw));
    wr16(model + 0x8A, u16(yaw));

    // Hit shapes x2 for the duration of the call.
    static const u32 shapeFn = guestSymbol("fn_800AC2B4");
    const u32 desc = guestCall1(c, shapeFn, model);
    u32 base = 0, n = 0;
    if (inMem1(desc)) {
        const u32 type = rd32(desc);
        if (type == 1) base = rd32(desc + 4), n = 2;       // two scalars (PAL scales words 0-1)
        else if (type == 2) base = rd32(desc + 8), n = 6;  // two vectors
    }
    u32 saved[6] = {};
    if (inMem1(base)) {
        for (u32 i = 0; i < n; i++) {
            saved[i] = rd32(base + 4 * i);
            wrf(base + 4 * i, rdf(base + 4 * i) * 2.0f);
        }
    } else {
        n = 0;
    }
    HM_FIRST("fn_800A184C rocking horse weave + hit shapes x2");
    callOriginal(c, "fn_800A184C");
    for (u32 i = 0; i < n; i++) wr32(base + 4 * i, saved[i]);
}

// Bowser (King Boo) bomb volleys. PAL adds a byte to the boss strategy (+0x1EE) that flips each
// time all three bombs are gone (hidden mansion only), and a new bomb state: while the byte is
// set, bombs after the first in a volley use it instead of the normal "thrown" state. The new
// state is the thrown state with timers 100/30 instead of 300/90 (PAL's normal thrown state also
// sets the second timer, 90). Host-side: the flip per boss, and which bombs were thrown fast.
namespace {
std::unordered_map<u32, bool> sBowserAlt;
std::unordered_set<u32> sFastBombs;
}  // namespace

HLE(fn_800A47BC) {  // Bowser strategy init
    const u32 self = c.r[3];
    callOriginal(c, "fn_800A47BC");
    sBowserAlt.erase(self);
}

HLE(fn_800A441C) {  // Bowser chase state: switches to state 0x12 when all three bombs are gone
    if (ver::pal()) return hm::callWithoutFlag(c, "fn_800A441C");
    const u32 self = c.r[3];
    const u32 before = rd16(self + 0xC);
    callOriginal(c, "fn_800A441C");
    if (hm::palActive() && before != 0x12 && rd16(self + 0xC) == 0x12) {
        sBowserAlt[self] = !sBowserAlt[self];
        HM_FIRST("fn_800A441C Bowser volley alternation");
    }
}

HLE(fn_800A5BA0) {  // Bowser throws the next bomb of the volley
    const u32 self = c.r[3];
    const u32 before = rd32(self + 0x22C);
    callOriginal(c, "fn_800A5BA0");
    const u32 after = rd32(self + 0x22C);
    if (!hm::palActive() || after + 1 != before || after == 0 || !sBowserAlt[self]) return;
    const u32 bomb = rd32(self + 0x1AC + (3 - after) * 4);
    if (inMem1(bomb)) sFastBombs.insert(bomb);
}

HLE(fn_800A926C) {  // bomb: enter "thrown" state
    const u32 self = c.r[3];
    callOriginal(c, "fn_800A926C");
    if (!hm::palActive()) {
        sFastBombs.erase(self);
        // The European normal mansion's rules (docs/VERSION_DIFFERENCES.md 3): its thrown-bomb
        // state sets +0x1D4 = 90 too (the USA game leaves it).
        if (hm::rules(hm::rulesVariant(), hm::hiddenActive()).bowserBomb90) {
            wrf(self + 0x1D4, 90.0f);
            RULE_FIRST("fn_800A926C Bowser bomb +0x1D4 = 90 (PAL rules)");
        }
        return;
    }
    if (sFastBombs.erase(self)) {
        wrf(self + 0x1D0, 100.0f);
        wrf(self + 0x1D4, 30.0f);
        HM_FIRST("fn_800A926C Bowser fast bomb");
    } else {
        wrf(self + 0x1D4, 90.0f);
    }
}

// ---- USA rules on the European game (hidden_mansion.h) -----------------------------------
// The remaining European functions whose only hidden-mansion code is PAL-specific: the GBH
// map mirror (fn_80029A74 sets the marker sign, fn_80029DD4/fn_8002A01C flip the map),
// the enemy strategy (0x8034BB48 states), Chauncey's rocking horses (scale 3.6, weave,
// fn_8008907C argument) and the JAudio stereo-swap flag.
#define HM_HIDE(name) \
    HLE(name) { hm::callWithoutFlag(c, #name); }
HM_HIDE(fn_8006CFBC)
HM_HIDE(fn_8006D0DC)
HM_HIDE(fn_8006D4B8)
HM_HIDE(fn_8006E414)
HM_HIDE(fn_800A09D0)
HM_HIDE(fn_800A1BCC)
// The GBH map mirror (fn_80029A74, fn_80029DD4, fn_8002A01C) and the stereo-swap flag
// (fn_8018865C, fn_80189054) are mirror code: they run with the flag reading hm::mirrorOn()
// (callMirror; mirror_world.cpp has their hooks, with the USA game's equivalent).

// Functions that read the flag in both releases (USA hidden-mansion behaviour): they see the
// real value even when reached from inside a hidden call. Damage x2 (fn_800B9680), the GBH
// screen (fn_8003DF88), fn_800DBE8C (x0.5); the heart and vacuum hooks do the same.
#define HM_SHOW(name) \
    HLE(name) { hm::callWithFlag(c, #name); }
// Luigi takes damage: x2 while the hidden-mansion flag is set (USA and PAL). The Japanese Hidden
// Mansion rules on those games: no doubling (the flag reads 0 for the call).
HLE(fn_800B9680) {
    if (hm::hiddenActive() && !hm::rules(hm::rulesVariant(), true).doubleDamage) {
        RULE_FIRST("fn_800B9680 no damage x2 (%s Hidden Mansion rules)", hm::variantName(hm::rulesVariant()));
        return hm::callWithFlagAs(c, "fn_800B9680", false);
    }
    hm::callWithFlag(c, "fn_800B9680");
}
HM_SHOW(fn_8003DF88)
HM_SHOW(fn_800DBE8C)

// ---- Mirror ------------------------------------------------------------------------------
// PAL mirrors the hidden mansion by redrawing the EFB copy of the 3D scene (the full-screen
// quad in US fn_8005F8B8 / PAL fn_80062388) with its S texture coordinates flipped; the HUD,
// drawn afterwards, is not mirrored. The fix-ups that keep the game consistent with the
// flipped picture (stick X, projected HUD positions, the iris wipe centre) follow below.

namespace {
// The quad's vertices are written to the GX pipe as f32 x, f32 y, u32 colour, u16 s, u16 t:
// the first 16-bit write after a 32-bit write is S (u16, 15 fractional bits: 0x8000 = 1.0).
int sLastFifoSize = 0;
u32 flipQuadS(u32 v, int size) {
    if (size == 2 && sLastFifoSize == 4) v = (v <= 0x8000) ? 0x8000 - v : v;
    sLastFifoSize = size;
    return v;
}
}  // namespace

HLE(fn_8005F8B8) {
    if (mirror::deferSceneCopy()) return;  // 2-player: after the last view (mirror_world.h)
    if (ver::pal()) return hm::callMirror(c, "fn_8005F8B8");
    if (!hm::mirrorActive()) return callOriginal(c, "fn_8005F8B8");
    HM_FIRST("fn_8005F8B8 mirrored screen copy");
    sLastFifoSize = 0;
    hw::fifoFilter = &flipQuadS;
    callOriginal(c, "fn_8005F8B8");
    hw::fifoFilter = nullptr;
}

namespace {
// Luigi's input readers see the main and C stick X negated (PAL negates the values it reads
// from the pad at +0x44 / +0x54). Done by negating them in the pad for the call's duration.
// Player 2's pad (2-player mode, coop.cpp) is an HLE allocation above MEM1, so the test is the
// whole guest RAM window, not MEM1.
std::unordered_set<u32> sNegatedPads;
bool guestRam(u32 a) { return a >= 0x80000000u && a < 0x84000000u; }
struct ScopedStickFlip {
    u32 pad = 0;
    explicit ScopedStickFlip(u32 p) {
        if (!hm::mirrorActive() || !guestRam(p) || !sNegatedPads.insert(p).second) return;
        pad = p;
        wrf(pad + 0x44, -rdf(pad + 0x44));
        wrf(pad + 0x54, -rdf(pad + 0x54));
    }
    ~ScopedStickFlip() {
        if (!pad) return;
        wrf(pad + 0x44, -rdf(pad + 0x44));
        wrf(pad + 0x54, -rdf(pad + 0x54));
        sNegatedPads.erase(pad);
    }
};
}  // namespace

HLE(fn_8007F9B8) {  // Luigi movement input (r4 -> {pad, ...})
    if (ver::pal()) return hm::callMirror(c, "fn_8007F9B8");
    ScopedStickFlip flip(rd32(c.r[4]));
    callOriginal(c, "fn_8007F9B8");
}
HLE(fn_800807EC) {  // pad axis reader (r3 = pad)
    if (ver::pal()) return hm::callMirror(c, "fn_800807EC");
    ScopedStickFlip flip(c.r[3]);
    callOriginal(c, "fn_800807EC");
}
HLE(fn_800809CC) {  // pad stick-pair reader (r4 = pad)
    if (ver::pal()) return hm::callMirror(c, "fn_800809CC");
    ScopedStickFlip flip(c.r[4]);
    callOriginal(c, "fn_800809CC");
}

namespace {
bool sInHud = false;    // inside fn_80041214 (HUD markers placed at projected positions)
bool sInWipe = false;   // inside fn_80055C54 (iris wipe)
}  // namespace

HLE(fn_80041214) {
    ws::AnchorScope centered(0);  // drawn at Luigi's projected position: not edge-anchored
    if (ver::pal()) return hm::callMirror(c, "fn_80041214");
    sInHud = hm::mirrorActive();
    callOriginal(c, "fn_80041214");
    sInHud = false;
}

// World -> screen projection (r5/r6 -> s16 x/y). Within the HUD function, x = 640 - x.
HLE(fn_80009148) {
    const u32 px = c.r[5];
    callOriginal(c, "fn_80009148");
    if (sInHud && inMem1(px)) wr16(px, u16(s16(640 - s16(rd16(px)))));
}

HLE(fn_80055C54) {
    ws::WipeScope wipe;  // widescreen: the mask covers the whole width (widescreen.h)
    if (ver::pal()) return hm::callMirror(c, "fn_80055C54");
    sInWipe = hm::mirrorActive();
    callOriginal(c, "fn_80055C54");
    sInWipe = false;
}

// Event <CHOICE> list box draw (both releases). Its left edge is a constant (.sdata halfword
// 0x804D0558 = 368, or +4 = 41 in the alternative layout selected by 0x803CF4D0) and it grows
// to the right with the widest entry (0x804D851C, set when the list opens), so the lab's
// "The Hidden Mansion (USA)" / "(PAL)" entries would run off a 640-wide screen. With the lab
// menu installed, a box that would cross the right edge is moved left for the draw.
HLE(fn_80043104) {
    if (!hm::labChoiceAvailable()) return callOriginal(c, "fn_80043104");
    const u32 kPos = VA(0x804D0558);  // {x, y} normal, {x, y} alternative
    const s32 width = s32(rd32(VA(0x804D851C)));
    constexpr s32 kFrame = 44;    // box frame + text inset beyond the widest entry
    constexpr s32 kRight = 624;   // right edge like the original list (~620 on a 640-wide screen)
    const u32 saved[2] = {rd16(kPos), rd16(kPos + 4)};
    bool moved = false;
    for (int i = 0; i < 2; i++) {
        const s32 x = s16(saved[i]);
        if (width > 0 && width < 600 && x + width + kFrame > kRight) {
            wr16(kPos + 4 * u32(i), u16(s16(std::max(8, kRight - kFrame - width))));
            moved = true;
        }
    }
    static bool sLogged = false;
    if (moved && !sLogged) {
        sLogged = true;
        log("[hm] lab menu: choice box width %d moved left to stay on screen (x %d -> %d)", width, s16(saved[0]),
            s16(rd16(kPos)));
    }
    callOriginal(c, "fn_80043104");
    if (moved) {
        wr16(kPos, saved[0]);
        wr16(kPos + 4, saved[1]);
    }
}

// JUTRect::set(l, t, r, b): the wipe's circle rectangle is centred on x = 312; PAL uses 328.
HLE(set__7JUTRectFiiii) {
    if (sInWipe && s32(c.r[4]) + s32(c.r[6]) == 2 * 0x138) {
        c.r[4] += 16;
        c.r[6] += 16;
    }
    callOriginal(c, "set__7JUTRectFiiii");
}

}  // namespace recomp
