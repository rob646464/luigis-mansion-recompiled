#pragma once
// PAL Hidden Mansion rules on the US (GLME01) recompiled build.
// Background, addresses and the full list of PAL behaviour changes: docs/HIDDEN_MANSION.md.
//
// Wiring:
//   hle_dvd.cpp            serves overlay files (overlayEntry/isOverlayEntry/overlayRead).
//   frame.cpp              onFrame() once per presented frame.
//   hidden_mansion_hooks   tuning hooks (tools/recomp/hooks.txt).
//   audio output           mirrorStereo() on each mixed buffer (once audio is emulated).
#include "recomp/ctx.h"

#include <cstddef>
#include <optional>
#include <string>

namespace recomp::hm {

// Which release's Hidden Mansion rules apply. This is separate from the game's own
// hidden-mansion flag: the PAL rules are active only while the flag is set AND the variant is
// Pal, and switching back to Us fully restores US behaviour (overlay off, literals restored,
// hooks pass straight through to the original).
//
// Selection (docs/HIDDEN_MANSION.md, *E. Gadd's lab menu*): with both releases' data, picking
// "The Hidden Mansion" in E. Gadd's lab asks "(USA)" or "(PAL)" every visit; the event script
// records the answer in game flag 8 (the European release's own hidden-mansion save flag, set
// for PAL, cleared for USA and again on entering the lab). Without the lab menu (one disc, or
// an overlay built before it) the variant is F1 > Enhancements > Hidden Mansion version
// (settings hiddenMansionPal). --hidden-mansion-variant pal|us (alias --hidden-mansion; env
// LM_HIDDEN_MANSION_VARIANT) overrides both, for testing.
enum class Variant { Us = 0, Pal = 1, Jp = 2 };
Variant variant();
void setVariant(Variant v);  // the F1 setting (used without the lab menu)
const char* variantName(Variant v);  // "USA" / "PAL" / "Japan"
Variant baseVariant();               // the running game's own release

constexpr unsigned kLabFlagPal = 8;
bool labChoiceAvailable();           // both variants installed and the overlay has the lab menu
std::optional<Variant> labChoice();  // with the lab menu: the variant flag 8 records

// ---- Per-version mansions (docs/VERSION_DIFFERENCES.md "Lab menu design") ----------------
// With two or more discs, E. Gadd's lab asks which release's mansion "The Mansion" and "The
// Hidden Mansion" take you to (only installed releases, the base's own first). The answer is
// two persistent game flags set by the lab script: (60, 76) = (0,0) the base's own, (1,0) USA,
// (0,1) Japan, (1,1) PAL; flag 8 keeps meaning "the PAL Hidden Mansion" (its scripts read it).
// Rules of a release = its gameplay data (map archives from setup's versions overlay) + the code
// differences in rules() below; text, scripts and sound stay the base disc's.
constexpr unsigned kVersionFlagA = 60, kVersionFlagB = 76;
bool versionMenuAvailable();      // the versions overlay with the lab menu is installed
std::optional<Variant> versionField();  // the lab's answer, nullopt = the base's own (0,0)
Variant mansionVariant();         // whose normal mansion applies (outside the Hidden Mansion)
Variant rulesVariant();           // whose rules apply now (hidden: variant(), else mansionVariant())
bool variantInstalled(Variant v); // that release's disc is installed (its data was built)
// Hidden Mansion variants offered on this base (the lab menu's list; PAL on the Japanese base: not yet).
bool hiddenVariantOffered(Variant v);
// The gameplay-code differences of one release (docs/VERSION_DIFFERENCES.md sections 1-3).
struct Rules {
    int heartThreshold;   // small heart heals 20 from this treasure counter (status +0x6C8)
    bool doubleDamage;    // Luigi's damage x2 (hidden only)
    u32 rankA;            // money for the A rank
    float chaunceyDist;   // Chauncey stops approaching at this distance
    bool bowserBomb90;    // Bowser's thrown bombs: +0x1D4 = 90
};
Rules rules(Variant v, bool hidden);

// LM_HM_OVERLAY points at the overlay built by tools/hidden_mansion_data.py (default:
// orig/hidden_mansion, searched upwards from the working directory). LM_HM_FORCE=1 keeps the
// game's hidden-mansion flag set, for testing.

// Per presented frame: LM_HM_FORCE, flag-change logging, constant patches.
void onFrame();

// The game's own hidden-mansion ("ura") flag. US: sbss 0x804D80C0 (r13+0x160), set by the
// URALUIGI event command, cleared by OMOTELUIGI and on new game. PAL keeps the same variable
// (0x804C5784) and only adds a deferred "set at next scene" request (0x804C5780).
bool hiddenActive();
bool palActive();       // USA game with the PAL rules: variant() == Pal && hiddenActive()
bool usaOnPalActive();  // European game with the USA rules: variant() == Us && hiddenActive()
bool palRulesActive();  // the PAL rules apply (either game): variant() == Pal && hiddenActive()

// ---- USA rules on the European game ---------------------------------------------------
// The European game reads its hidden-mansion flag in 36 functions; most of them only for its
// PAL-specific changes (mirror, darker rooms, Chauncey/Boolossus/Bowser, enemy timers, the
// stereo swap). Their hooks call the original with the flag reading 0 for the duration
// (callWithoutFlag), which gives the USA behaviour. The few functions where the USA game
// reads the flag too (damage x2, hearts, vacuum, the GBH screen, ...) run with the real value
// even when reached from inside such a call (callWithFlag). Outside usaOnPalActive() both are
// plain callOriginal. Flag readers and their reachability: docs/SETUP.md.
void callWithoutFlag(Ctx& c, const char* name);
void callWithFlag(Ctx& c, const char* name);
// The original with the flag reading `shown` for the duration (another release's rules where
// the running game reads the flag for one difference, e.g. no damage x2 for the Japanese rules).
void callWithFlagAs(Ctx& c, const char* name, bool shown);

// ---- Disc overlay ---------------------------------------------------------------------
// Host file that should be served instead of a guest disc path, if any. Map archives are
// redirected only while palActive(); patched event archives whenever variant() == Pal (their
// PAL changes are inside CHECKFLAG(8) "ura" branches, so the normal mansion is unaffected).
std::optional<std::string> redirect(const char* guestPath);

// Virtual FST entry numbers for redirected files, so entry-number based opens keep working.
s32 overlayEntry(const char* guestPath);  // -1 when the path is not redirected
bool isOverlayEntry(s32 entry);
u32 overlayLength(s32 entry);
s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset);

// ---- Runtime constant patches ---------------------------------------------------------
// Some PAL changes multiply a literal that, in the US build, lives in .sdata2 and is used by
// exactly one function. Those are applied by rewriting the literal in guest memory while
// palActive() and restoring it otherwise. Cheap; call once per frame.
void syncConstants();

// ---- Mirror (PAL draws the hidden mansion left/right mirrored) -----------------------------
// Mirrored Mansion (F1 > Enhancements > Gameplay, docs/ENHANCEMENTS.md "Mirror world"): the same
// mirror in every mode (normal mansion, either Hidden Mansion, either game). On while the
// option is on (setting gameplay.mirror_world, LM_MIRROR=1, LM_GAMEPLAY=mirror=1) and the main
// game scene is current (title / file select / Options never run the mirror code anyway).
bool mirrorWorld();
// The picture is mirrored now (either game): the European Hidden Mansion (PAL rules + the
// game's flag; LM_HM_MIRROR=0 turns that part off on the USA game) or mirror world.
bool mirrorOn();
// USA game: the mirror hooks apply (mirrorOn() on the USA game; false on the European one,
// whose own code mirrors, driven through callMirror).
bool mirrorActive();
// European game: calls one of its mirror functions (the ones that read the hidden-mansion
// flag only for the mirror, docs/HIDDEN_MANSION.md) with the flag reading mirrorOn() for the
// duration. Replaces callWithoutFlag for them (USA rules on the European game: flag hidden).
void callMirror(Ctx& c, const char* name);
// Negate the X row of a perspective projection (orthographic 2D/HUD is left alone).
void mirrorProjection(float m[4][4], bool perspective);
// Mirroring flips triangle winding: swap GX_CULL_FRONT and GX_CULL_BACK.
u32 mirrorCullMode(u32 gxCullMode);
// Screen-space X of something the game projected itself (PAL: x = 640 - x).
float mirrorScreenX(float x, float width = 640.0f);
// PAL swaps the left/right mix buffers in Jac_VframeWork; here: swap interleaved L/R.
void mirrorStereo(s16* interleavedStereo, std::size_t frames);

}  // namespace recomp::hm
