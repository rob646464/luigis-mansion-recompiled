// The European Hidden Mansion's mirror outside the screen copy: the Game Boy Horror map and the
// stereo-swap flag, plus the 2-player screen copy (mirror_world.h). The screen copy, the stick
// and the projected HUD positions are in hidden_mansion_hooks.cpp. All of it follows
// hm::mirrorOn(): the European Hidden Mansion, or "Mirrored Mansion" (docs/ENHANCEMENTS.md).
#include "recomp/mirror_world.h"

#include "recomp/coop.h"
#include "recomp/guest_util.h"
#include "recomp/hidden_mansion.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/skip.h"
#include "recomp/version.h"


namespace recomp {
namespace mirror {
namespace {

using gu::inMem1;

// lbl_804CFF90: number of views the frame loop fn_8000BCE8 draws (fn_8000ADFC).
#define kViewCount VA(0x804CFF90)

bool usGame() { return ver::usa(); }

// ---- USA game: the European GBH map mirror (docs/HIDDEN_MANSION.md "Mirror") ----------------
// European code, with the USA names of its functions:
//   fn_80029A74 (map init)     sign variable (0x804C5A10) = flag ? -1 : +1
//   fn_80029DD4, fn_8002A01C   map draw: fn_80015BDC(view x Scale(-1, 1, 1), colour) instead of
//                              fn_80015BDC(view, colour) (view = lbl_803A3820 + 0x18C), then a new
//                              byte (0x804C5901) = 1 until the end of the draw, which makes the
//                              cull-mode setter fn_8001D894 take its mirrored row too. (The USA
//                              lbl_804D8230 selects the same row but is also read by the model
//                              node draws fn_80015844 etc., so it is not set here: a port-side
//                              flag drives the fn_8001D894 hook instead.)
//   fn_8002A5A0                map focus = (sign * x, y, z) of the icon at [lbl_804D833C], also
//                              passed (scaled) to the floor lookup fn_80017ADC
//   fn_8002A74C                map camera reset: x = 0.1 * [lbl_803CCD38 + 0x400] * sign
//   fn_8002AE74                Luigi's map effect (fn_80164F5C, id 0x62) at (sign * x, y, z)
//                              (USA game: instruction patch 60 + the fn_80164F58 stub hook)
// On the USA game the sign is hm::mirrorActive() at the time of use (the European one is fixed
// when the map is set up; both agree except right after mirror world is switched).
constexpr u32 kMapView = 0x803A3820 + 0x18C;   // the map's view matrix (Mtx 3x4)
constexpr u32 kCullMirror = 0x804D8230;        // byte: fn_8001D894 takes the mirrored cull row
constexpr u32 kMapIcon = 0x804D833C;           // -> the icon object Luigi's map focus follows
constexpr u32 kPatchMapEffect = 60;           // hooks.txt patch: bl fn_80164F58 at 0x8002B034
constexpr u32 kMapCameraX = 0x803CCD38 + 0x400;  // f32 read by the camera reset in fn_8002A74C

bool sMapDraw = false;       // inside fn_80029DD4 / fn_8002A01C, mirrored
bool sMatrixPending = false; // fn_80015BDC got the mirrored matrix: mirrored culling after it
bool sMapCull = false;       // European 0x804C5901: fn_8001D894 takes its mirrored row
u32 sScratch = 0;            // guest memory for the mirrored matrix / effect position

u32 scratch() {
    if (!sScratch) sScratch = hleAlloc(0x40);
    return sScratch;
}

void flipSign(u32 a) { wr32(a, rd32(a) ^ 0x80000000u); }

void mapDraw(Ctx& c, const char* name) {
    if (ver::pal()) return hm::callMirror(c, name);
    if (!hm::mirrorActive()) return callOriginal(c, name);
    static bool sLogged = false;
    if (!sLogged) {
        sLogged = true;
        log("[mirror] Game Boy Horror map drawn mirrored (%s)", name);
    }
    sMapDraw = true;
    callOriginal(c, name);
    sMapDraw = false;
    sMatrixPending = false;
    sMapCull = false;
}

// Runs a function with the f32 at `a` negated (restored afterwards unless it was rewritten).
void callNegated(Ctx& c, const char* name, u32 a) {
    flipSign(a);
    const u32 neg = rd32(a);
    callOriginal(c, name);
    if (rd32(a) == neg) flipSign(a);
}

// ---- 2-player screen copy ------------------------------------------------------------------
bool sDeferred = false;
bool sRunning = false;

// European stereo-swap flag (JAudio, set by fn_8018865C / fn_80189054 through fn_8019F8E0) and
// map sign variable: both European-only data.
constexpr u32 kPalJacSwap = 0x804C6A21;
constexpr u32 kPalMapSign = 0x804C5A10;

}  // namespace

void mapViewMatrix(Ctx& c) {
    if (!sMapDraw || c.r[3] != kMapView || !ver::usa()) return;
    const u32 dst = scratch();
    if (!dst) return;
    for (u32 i = 0; i < 12; i++) {
        const u32 v = rd32(kMapView + 4 * i);
        wr32(dst + 4 * i, (i % 4 == 0) ? v ^ 0x80000000u : v);  // column 0 (x) negated: view x Scale(-1,1,1)
    }
    c.r[3] = dst;
    sMatrixPending = true;
}

void afterMapViewMatrix() {
    if (!sMatrixPending) return;
    sMatrixPending = false;
    sMapCull = true;
}

bool deferSceneCopy() {
    if (sRunning || !hm::mirrorOn() || !coop::active() || coop::gbhView()) return false;
    if (rd32(kViewCount) < 2) return false;
    static bool sLogged = false;
    if (!sLogged) {
        sLogged = true;
        log("[mirror] 2-player views: the mirrored scene copy is made after the last view");
    }
    sDeferred = true;
    return true;
}

void beforeViewHud(Ctx& c, u32 view) {
    if (!sDeferred || view + 1 < rd32(kViewCount)) return;
    sDeferred = false;
    sRunning = true;
    gu::call(c, GU_SYM("fn_8005F8B8"));
    sRunning = false;
    // The display state resets the frame loop makes after the copy (fn_8000BCE8), again.
    gu::call(c, GU_SYM("fn_80009B80__Fv"));
    gu::call(c, GU_SYM("LMDefaultOrthoView__Fv"));
    gu::call(c, GU_SYM("fn_8005DF84"));
}

bool swapSplitHalves() { return coop::fixes() && hm::mirrorOn(); }

void onFrame() {
    // USA game: Luigi's map effect call goes to the fn_80164F58 hook while mirrored.
    if (ver::usa()) setPatch(kPatchMapEffect, hm::mirrorActive());
    static bool sLast = false;
    const bool on = hm::mirrorOn();
    if (on == sLast) return;
    sLast = on;
    log("[mirror] %s at guest frame %u (%s)", on ? "on" : "off", skip::guestFrames(),
        hm::mirrorWorld() ? "mirror world" : on ? "European Hidden Mansion" : "-");
    if (!ver::pal()) return;
    // The European game sets these when a scene starts (fn_8018865C / fn_80189054) and when the
    // map is set up (fn_80029A74), through hm::callMirror. Switched in between: set them now.
    wr8(kPalJacSwap, on ? 1 : 0);
    wr32(kPalMapSign, on ? u32(-1) : 1u);
    log("[mirror] European game: stereo swap and map sign %s", on ? "mirrored" : "normal");
}

}  // namespace mirror

// ---- Hooks ----------------------------------------------------------------------------------
// European only (tools/recomp/hooks.txt `pal`): mirror code that reads the hidden-mansion flag.
HLE(fn_80029A74) { hm::callMirror(c, "fn_80029A74"); }
HLE(fn_8018865C) { hm::callMirror(c, "fn_8018865C"); }
HLE(fn_80189054) { hm::callMirror(c, "fn_80189054"); }

// Both games (`us pal`): the map draws.
HLE(fn_80029DD4) { mirror::mapDraw(c, "fn_80029DD4"); }
HLE(fn_8002A01C) { mirror::mapDraw(c, "fn_8002A01C"); }

// fn_8001D894(model, packet): GXSetCullMode from the packet's cull index, the mirrored row when
// lbl_804D8230 is set. Inside the mirrored map draw (USA game) it reads as set for this call only,
// like the European game's extra byte.
HLE(fn_8001D894) {
    if (!mirror::sMapCull) return callOriginal(c, "fn_8001D894");
    const u8 saved = rd8(mirror::kCullMirror);
    wr8(mirror::kCullMirror, 1);
    callOriginal(c, "fn_8001D894");
    wr8(mirror::kCullMirror, saved);
}

// USA only (`us`): the map focus, camera reset and Luigi's map effect.
HLE(fn_8002A5A0) {
    const u32 icon = rd32(mirror::kMapIcon);
    if (!mirror::usGame() || !hm::mirrorActive() || !gu::inMem1(icon)) return callOriginal(c, "fn_8002A5A0");
    mirror::callNegated(c, "fn_8002A5A0", icon);
}
HLE(fn_8002A74C) {
    if (!mirror::usGame() || !hm::mirrorActive()) return callOriginal(c, "fn_8002A74C");
    mirror::callNegated(c, "fn_8002A74C", mirror::kMapCameraX);
}
// Luigi's map effect: fn_80164F5C(0x62, icon position, r5) at 0x8002B034 in fn_8002AE74. While
// mirrored (patch 60, mirror::onFrame) that call goes to the empty stub fn_80164F58 instead, and
// this hook spawns the effect at (-x, y, z), like the European code. Any other call of the stub
// returns at once, as the stub does. (A hook on fn_80164F5C itself, even passing straight
// through, changed the vacuum dust with the mirror off.)
HLE(fn_80164F58) {
    if (c.lr != 0x8002B038u || mirror::usGame() == false) return;
    if (const u32 dst = mirror::scratch(); dst && gu::inMem1(c.r[4])) {
        const u32 src = c.r[4];
        wr32(dst + 0x30, rd32(src) ^ 0x80000000u);
        wr32(dst + 0x34, rd32(src + 4));
        wr32(dst + 0x38, rd32(src + 8));
        c.r[4] = dst + 0x30;
    }
    c.call(0x80164F5Cu);
}

}  // namespace recomp
