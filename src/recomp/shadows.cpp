// Shadow-map enhancements (docs/RENDERING.md, "Shadows").
//
// The game's real-time shadows (flashlight, room light, lightning) come from one shadow map:
// fn_8005D984 turns the main camera (lbl_803A3820) into the light's camera (perspective from
// the light fovY +0xA0, aspect +0x94, near +0xA4, far +0xA8, eye +0x24, target +0x30, or
// orthographic), the casters are drawn into the EFB, fn_8005D8CC copies the depth as Z16, and
// every receiver projects it with the light matrices (+0xB0 / +0xE0 of the shadow object) and
// compares depths in two TEV stages (fn_8005DD50). Everything the pass and the receivers use is
// built by the game from those camera fields, so changing them for the setup call keeps the
// map, its projection and the light's culling consistent by construction.
//
// Widescreen ("Widescreen shadows"): the light's frustum only covers what the 4:3 picture shows;
// the light's aspect is multiplied by the widescreen ratio for the setup call.
//
// Any distance ("Shadows at any distance"): outside the flashlight, the gameplay shadow light is
// a light that follows Luigi (fn_80029050/fn_80028CF0: placed relative to Luigi from the room's
// light direction, fov/near/far from the light table, e.g. fov 17 near 3000 far 5000 for a high
// distant light, or fov 31 near 619 far 2322 near the camera), so only a small region around
// Luigi gets shadows; in big rooms the shadows of everything farther away drop out as he walks.
// The light's position and direction are kept; its fov, aspect and near/far are widened so the
// frustum contains both its original frustum and the player's view (sampled from 1/4 to 4x the
// camera-to-target distance), clamped to the current room's bounds so the map stays as tight as
// possible. Test aids: LM_SHADOW_LOG=1, LM_ROOMS_LIT=1 (docs/RENDERING.md).
//
// Resolution ("Shadow map at render resolution"): the receivers sample the Z16 copy at the
// internal resolution (Aurora's copy texture) instead of the console's 640x480 bytes.
#include "port/settings.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/version.h"
#include "recomp/widescreen.h"

#include <aurora/efb_ram.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iterator>

namespace recomp {
namespace {

// Camera fields (lbl_803A3820).
constexpr u32 kCamViewEye = 0x00, kCamViewTarget = 0x0C;         // vec3: the player's view
constexpr u32 kCamUp = 0x18, kCamEye = 0x24, kCamTarget = 0x30;  // vec3: light setup (MTXLookAt)
constexpr u32 kCamViewFov = 0x90;                                // f32 degrees: the player's view
constexpr u32 kCamAspect = 0x94;                                 // f32 (MTXPerspective)
constexpr u32 kCamLightFov = 0xA0, kCamLightNear = 0xA4, kCamLightFar = 0xA8;

constexpr float kDegToRad = 3.14159265f / 180.0f;

bool sShadowOffscreen = false;  // the caster pass renders offscreen (enhanced "Shadow map size")

struct V3 {
    float x, y, z;
};
V3 rdv(u32 a) { return {rdf32(a), rdf32(a + 4), rdf32(a + 8)}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(V3 a) { return std::sqrt(dot(a, a)); }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) {
    const float l = len(a);
    return l > 1e-6f ? mul(a, 1.0f / l) : V3{0, 0, -1};
}

// Right/up axes of a look direction (MTXLookAt convention, up hint u).
void basis(V3 d, V3 upHint, V3& right, V3& up) {
    right = cross(d, upHint);
    if (len(right) < 1e-4f) right = cross(d, V3{0, 0, 1});
    right = norm(right);
    up = cross(right, d);
}

// The 8 corners of a symmetric perspective frustum.
void frustumCorners(V3 eye, V3 dir, V3 upHint, float tanY, float aspect, float zNear, float zFar, V3* out) {
    V3 r, u;
    basis(dir, upHint, r, u);
    int n = 0;
    for (const float z : {zNear, zFar}) {
        const float h = z * tanY, w = h * aspect;
        for (const float sx : {-1.0f, 1.0f})
            for (const float sy : {-1.0f, 1.0f})
                out[n++] = add(add(eye, mul(dir, z)), add(mul(r, sx * w), mul(u, sy * h)));
    }
}

struct Fit {
    float fov, aspect, zNear, zFar;
};

constexpr float kViewNear = 0.25f, kViewDepth = 4.0f, kMinNear = 10.0f, kMaxHalfAngle = 75.0f;

// Light frustum (same eye) containing the light's own frustum and the player's view region.
bool fitFrustum(u32 cam, float lightAspect, float viewAspect, Fit& fit) {
    const V3 L = rdv(cam + kCamEye), T = rdv(cam + kCamTarget), up = rdv(cam + kCamUp);
    const V3 D = norm(sub(T, L));
    const float lightTanY = std::tan(rdf32(cam + kCamLightFov) * 0.5f * kDegToRad);
    const float n = rdf32(cam + kCamLightNear), f = rdf32(cam + kCamLightFar);
    const V3 E = rdv(cam + kCamViewEye), C = rdv(cam + kCamViewTarget);
    const float dist = len(sub(C, E)), viewFov = rdf32(cam + kCamViewFov) * kDegToRad;
    if (!(dist > 1.0f) || !(viewFov > 0.0f && viewFov < 3.0f) || !(lightTanY > 0.0f) || !(f > n * 1.01f))
        return false;

    // The light's own frustum, and the view's frustum sampled at several depths (the room light
    // can sit inside the visible region, e.g. beyond Luigi facing the camera; then only the part
    // in front of it can be covered, as wide as kMaxHalfAngle allows).
    constexpr float kDepths[] = {kViewNear, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, kViewDepth};
    V3 pts[8 + 4 * std::size(kDepths)];
    frustumCorners(L, D, up, lightTanY, lightAspect, n, f, pts);
    const V3 viewDir = norm(sub(C, E));
    for (size_t k = 0; k < std::size(kDepths); k += 2) {
        V3 corners[8];
        const float z0 = kDepths[k] * dist, z1 = kDepths[k + 1] * dist;
        frustumCorners(E, viewDir, V3{0, 1, 0}, std::tan(viewFov * 0.5f), viewAspect, z0, z1, corners);
        for (int i = 0; i < 8; i++) pts[8 + 4 * k + i] = corners[i];
    }
    // Only what can receive a shadow in Luigi's room matters: the view samples are clamped to the
    // current room's bounds (room table lbl_804D8170, 0x30 per room: min xyz +0x8, max xyz +0x14,
    // ints), which keeps the widened frustum (and so the shadow map's texel size) as small as
    // possible. Rooms seen through doorways keep the game's own coverage.
    const u32 rooms = rd32(VA(0x804D8170));
    const u32 roomNo = rd32(VA(0x803A3CA0) + 0xC) >> 24;
    if ((rooms >> 28) == 8 && roomNo < 0x80) {
        const u32 b = rooms + roomNo * 0x30;
        const float lo[3] = {float(s32(rd32(b + 0x8))), float(s32(rd32(b + 0xC))), float(s32(rd32(b + 0x10)))};
        const float hi[3] = {float(s32(rd32(b + 0x14))), float(s32(rd32(b + 0x18))), float(s32(rd32(b + 0x1C)))};
        if (lo[0] < hi[0] && lo[1] < hi[1] && lo[2] < hi[2] && hi[0] - lo[0] < 20000.0f && hi[2] - lo[2] < 20000.0f) {
            constexpr float kMargin = 50.0f;
            for (size_t i = 8; i < std::size(pts); i++) {
                pts[i].x = std::clamp(pts[i].x, lo[0] - kMargin, hi[0] + kMargin);
                pts[i].y = std::clamp(pts[i].y, lo[1] - kMargin, hi[1] + kMargin);
                pts[i].z = std::clamp(pts[i].z, lo[2] - kMargin, hi[2] + kMargin);
            }
        }
    }
    const float zClip = std::max(kMinNear, n * 0.25f);

    // The light keeps its direction (and so the look of the shadows); only the frustum widens.
    const V3 D2 = D;
    V3 r, u;
    basis(D2, up, r, u);
    float zMin = 1e30f, zMax = 0.0f, tx = 0.0f, ty = 0.0f;
    for (const V3& p : pts) {
        const V3 v = sub(p, L);
        const float z = dot(v, D2);
        if (z < zClip) continue;  // behind (or right at) the light: a perspective map cannot cover it
        zMin = std::min(zMin, z);
        zMax = std::max(zMax, z);
        tx = std::max(tx, std::fabs(dot(v, r)) / z);
        ty = std::max(ty, std::fabs(dot(v, u)) / z);
    }
    const float maxTan = std::tan(kMaxHalfAngle * kDegToRad);
    if (ty < 1e-3f || tx < 1e-3f) return false;
    // Snapped to steps (6% in tan, 10% in depth) and rounded up: a fit that follows the camera
    // continuously changes the map's texel size and depth scale every frame, which makes shadow
    // edges and the depth compare shimmer. With steps the map only changes when the fit crosses
    // a step (the light itself still moves with Luigi, as in the original).
    const auto snapUp = [](float v, float step) { return std::pow(step, std::ceil(std::log(v) / std::log(step))); };
    tx = std::min(snapUp(tx * 1.02f, 1.06f), maxTan);
    ty = std::min(snapUp(ty * 1.02f, 1.06f), maxTan);
    fit.fov = 2.0f * std::atan(ty) / kDegToRad;
    fit.aspect = std::clamp(tx / ty, 0.1f, 10.0f);
    const float nearFit = zMin * 0.9f > 1.0f ? 1.0f / snapUp(1.0f / (zMin * 0.9f), 1.1f) : zMin * 0.9f;
    fit.zNear = std::min(n, std::max({nearFit, kMinNear, n * 0.25f}));  // keep Z16 precision
    fit.zFar = std::max(f, snapUp(zMax * 1.05f, 1.1f));
    return true;
}

bool sCasterSnap = false;
// LM_SHADOW_SNAP=0 (test aid): blend the shadow pass and the receivers' shadow matrices as before.
// LM_SHADOW_SNAP: 3 (default) consistent blend: the caster pass factored by the light's camera and
// the receivers' shadow matrices rebuilt from the same blended light (interp::noteShadowLight);
// 1 snap (shadows at 30 Hz); 0 blend each matrix on its own (inconsistent); 2 linear caster pass.
// shadowSnap() runs per receiver batch (fn_8005DCA0 / fn_8005DD50, per object that receives a
// shadow), so the switch is read into sShadowInterp once.
const int sShadowInterp = [] { const char* e = std::getenv("LM_SHADOW_SNAP"); return e ? std::atoi(e) : 3; }();
int shadowInterpMode() { return sShadowInterp; }
bool shadowSnap() { return sShadowInterp == 1; }

// Set by fn_800293A8, the gameplay shadow light's parameter setup, which fn_8005EF84 calls right
// before the pass (cutscene shadow passes, fn_80114144, use their own keyframed lights).
bool sGameplayLight = false;
bool sFlashlightLight = false;

// Test aids: LM_NO_SHADOW=1 draws the frame without the game's shadows (strength 0, for isolating
// them by difference), LM_ROOMS_LIT=1 makes rooms behave as lit, LM_SHADOW_LOG=<every> logs each
// shadow setup (every 30 when set to 1).
const bool sNoShadow = std::getenv("LM_NO_SHADOW") != nullptr;
const bool sRoomsLit = std::getenv("LM_ROOMS_LIT") != nullptr;
const bool sShadowLog = std::getenv("LM_SHADOW_LOG") != nullptr;
const int sShadowLogEvery =
    [] { const char* e = std::getenv("LM_SHADOW_LOG"); return e ? std::max(1, std::atoi(e) == 1 ? 30 : 1) : 30; }();

}  // namespace

// Test aid LM_ROOMS_LIT=1: rooms behave as lit (lights on, as after clearing them) for the shadow
// light choice. fn_80029050 picks the flashlight as the shadow light when the room light object
// (lbl_803A3CA0 +0x24) has its dark flag +0xAC set and mode +0x94 == 5; the flag is cleared for
// the call, so the room shadow light (the one that follows Luigi) is used.
HLE(fn_80029050) {
    const u32 obj = sRoomsLit ? rd32(VA(0x803A3CA0) + 0x24) : 0;
    if ((obj >> 28) != 8) return callOriginal(c, "fn_80029050");
    const u32 saved = rd32(obj + 0xAC);
    wr32(obj + 0xAC, 0);
    callOriginal(c, "fn_80029050");
    wr32(obj + 0xAC, saved);
}

// Gameplay shadow light parameters (fov/near/far): the flashlight's (lbl_804D82E9 or
// lbl_804D8304 set: fn_800985A0) or the light table's (room light following Luigi, lightning).
HLE(fn_800293A8) {
    sFlashlightLight = rd8(VA(0x804D82E9)) == 1 || rd32(VA(0x804D8304)) == 1;
    callOriginal(c, "fn_800293A8");
    sGameplayLight = true;
}

// Shadow pass setup (shadow object): runs only while a shadow pass is requested (+0xA0).
HLE(fn_8005D984) {
    const auto& r = port::settings().rendering;
    aurora_efb_ram_set_scaled_depth_reads(r.shadowRenderResolution);
    const u32 cam = VA(0x803A3820);
    const bool active = rd32(c.r[3] + 0xA0) != 0;
    const bool perspective = rd32(c.r[3] + 0xA4) == 1;
    // Only the gameplay room/lightning light: the flashlight's frustum is its beam (shadows only
    // show where it shines), cutscene lights are authored per shot.
    const bool gameplay = sGameplayLight && !sFlashlightLight;

    // Enhanced lighting: when the game switches its single shadow light (flashlight <-> room
    // light / lightning / a cutscene light), the new light's shadows fade in over about six
    // game frames instead of popping in (the shadow strength, K3 alpha in lbl_804D07E0 +4, is
    // read by the receivers' setup fn_8005DD50 after this pass; the game sets it again every
    // frame). Plain renderer (master switch off): untouched.
    if (active) {
        static int sKind = -1;
        static float sFade = 1.0f;
        static V3 sEye{};
        const int kind = sFlashlightLight ? 1 : sGameplayLight ? 2 : 3;
        const V3 eye = rdv(cam + kCamEye);
        const V3 de = sub(eye, sEye);
        const bool jumped = dot(de, de) > 1500.0f * 1500.0f;  // a different light of the same kind
        if (kind != sKind || jumped) sFade = sKind < 0 ? 1.0f : 0.0f;
        sKind = kind;
        sEye = eye;
        if (enh::active() && sFade < 1.0f) {
            const u32 k3 = VA(0x804D07E0) + 4;
            const u32 a = rd32(k3) & 0xFFu;
            wr32(k3, (rd32(k3) & ~0xFFu) | u32(float(a) * sFade * sFade * (3.0f - 2.0f * sFade) + 0.5f));
        }
        sFade = std::min(1.0f, sFade + 1.0f / 6.0f);
    }
    sGameplayLight = false;
    if (sNoShadow && active) wr32(VA(0x804D07E0) + 4, rd32(VA(0x804D07E0) + 4) & ~0xFFu);

    const float aspect = rdf32(cam + kCamAspect);
    const float wideAspect = r.shadowWidescreen && ws::active() ? aspect / ws::factor() : aspect;
    const V3 target = rdv(cam + kCamTarget);
    const float fov = rdf32(cam + kCamLightFov), zNear = rdf32(cam + kCamLightNear), zFar = rdf32(cam + kCamLightFar);
    Fit fit{};
    const bool refit = active && perspective && gameplay && r.shadowAnyDistance &&
                       fitFrustum(cam, wideAspect, aspect / ws::factor(), fit);

    static u32 sCalls = 0;
    if (sShadowLog && active && (sCalls++ % sShadowLogEvery) == 0) {
        const V3 eye = rdv(cam + kCamEye);
        log("[shadow] k3a %u %s eye %.0f %.0f %.0f at %.0f %.0f %.0f fov %.1f asp %.2f near %.0f far %.0f%s",
            rd32(VA(0x804D07E0) + 4) & 0xFF, gameplay ? "game" : "other", eye.x, eye.y, eye.z, target.x, target.y, target.z, fov, wideAspect, zNear,
            zFar, refit ? "" : " (kept)");
        const V3 ve = rdv(cam + kCamViewEye), vt = rdv(cam + kCamViewTarget);
        log("[shadow]   view eye %.0f %.0f %.0f at %.0f %.0f %.0f", ve.x, ve.y, ve.z, vt.x, vt.y, vt.z);
        if (refit)
            log("[shadow]   refit fov %.1f asp %.2f near %.0f far %.0f", fit.fov, fit.aspect, fit.zNear, fit.zFar);
    }

    if (active) enh::setCasterPass(true);
    if (active) interp::setPass(1);
    // Frame interpolation: the shadow map and the light projection it is rendered with are
    // replayed exactly as captured (not blended), like the receivers' shadow texture matrices
    // (fn_8005DCA0 / fn_8005DD50 below): blending the caster pass (rigid slerp of the light-view
    // matrices, element-wise projection) and the receivers' projection matrices (row-wise) gave
    // two different in-between light positions, so the shadows slid back and forth at
    // 60/120 fps while the flashlight moved.
    if (active && (shadowInterpMode() == 1 || shadowInterpMode() == 2) && !sCasterSnap) {
        if (shadowSnap()) interp::pushNoBlend();
        else interp::pushLinearPosMtx();
        sCasterSnap = true;
    }
    // Enhanced lighting "Shadow map size": the caster pass renders into an offscreen target of
    // that size instead of the EFB; the Z16 copy (fn_8005D8CC) then comes from it at that size.
    // Needs "Shadow map at render resolution" (receivers sample Aurora's copy texture).
    if (active && !sShadowOffscreen && r.shadowRenderResolution) {
        if (const u32 w = enh::shadowMapWidth()) {
            GXCreateFrameBuffer(w, w * 3 / 4);
            sShadowOffscreen = true;
            static bool sLogged = false;
            if (!sLogged) log("[shadow] caster pass offscreen at %ux%u", w, w * 3 / 4), sLogged = true;
        }
    }
    ws::setShadowLightSetup(true);
    if (refit) {
        wrf32(cam + kCamLightFov, fit.fov);
        wrf32(cam + kCamAspect, fit.aspect);
        wrf32(cam + kCamLightNear, fit.zNear);
        wrf32(cam + kCamLightFar, fit.zFar);
    } else {
        wrf32(cam + kCamAspect, wideAspect);
    }
    callOriginal(c, "fn_8005D984");
    if (active && shadowInterpMode() == 3) {
        // Frame interpolation: the light pass's camera and the receivers' K matrices of this frame.
        float lv[12], kp[12], kr[12];
        for (int k = 0; k < 12; k++) {
            lv[k] = rdf32(cam + VOFF(0x18C, 0x198) + 4 * k);
            kp[k] = rdf32(VA(0x803CFC0C) + 0xB0 + 4 * k);
            kr[k] = rdf32(VA(0x803CFC0C) + 0xE0 + 4 * k);
        }
        interp::noteShadowLight(lv, kp, kr);
    }
    wrf32(cam + kCamLightFov, fov);
    wrf32(cam + kCamAspect, aspect);
    wrf32(cam + kCamLightNear, zNear);
    wrf32(cam + kCamLightFar, zFar);
    ws::setShadowLightSetup(false);
}

// The shadow map's Z16 copy (with clear) ends the caster pass.
HLE(fn_8005D8CC) {
    callOriginal(c, "fn_8005D8CC");
    if (sShadowOffscreen) {
        GXRestoreFrameBuffer();
        sShadowOffscreen = false;
    }
    enh::setCasterPass(false);
    interp::setPass(0);
    if (sCasterSnap) {
        if (shadowSnap()) interp::popNoBlend();
        else interp::popLinearPosMtx();
        sCasterSnap = false;
    }
}

// Receivers' shadow texgen: room geometry (fn_8005DCA0: TEXMTX0 = depth ramp x model,
// TEXMTX1 = light projection x model) and actors (fn_8005DD50: post-transform matrices
// PT18/PT19 = ramp / light projection x inverse view). Snapped like the caster pass.
HLE(fn_8005DCA0) {
    const int mode = shadowInterpMode();
    if (mode == 1) interp::pushNoBlend();
    if (mode == 3) interp::pushShadowReceivers();
    callOriginal(c, "fn_8005DCA0");
    if (mode == 1) interp::popNoBlend();
    if (mode == 3) interp::popShadowReceivers();
}
HLE(fn_8005DD50) {
    const int mode = shadowInterpMode();
    if (mode == 1) interp::pushNoBlend();
    if (mode == 3) interp::pushShadowReceivers();
    callOriginal(c, "fn_8005DD50");
    if (mode == 1) interp::popNoBlend();
    if (mode == 3) interp::popShadowReceivers();
}

// Frame interpolation camera factoring (interp.h): the game camera's GX view matrix.
u32 interp::viewMatrixSource() { return VA(0x803A3820) + VOFF(0x18C, 0x198); }

}  // namespace recomp
