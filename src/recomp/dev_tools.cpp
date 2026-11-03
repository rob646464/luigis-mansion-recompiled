// Developer tools: leftover debug displays, map warps and the info overlay (model viewer:
// model_viewer.cpp). Addresses are US GLME01 (VA() maps them to the European game's, see
// recomp/version.h); how each was found: docs/UNUSED_CONTENT.md.
#include "recomp/dev_tools.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/model_viewer.h"
#include "recomp/version.h"
#include "recomp/room_swap.h"
#include "recomp/coop.h"
#include "recomp/ui_scale.h"
#include "recomp/unused2.h"
#include "recomp/silly_world.h"

#include <imgui.h>
#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp {
void unusedModelsMapSetup();  // unused_models.cpp: unused model variants chosen per map
void testStateLuigi(u32 actor);  // test_state.cpp: LM_TEST_STATE presets, from Luigi's update
}

namespace recomp::dev {
namespace {

// --- game state ------------------------------------------------------------------------
#define kCurSceneId VA(0x804D80A0)       // sCurSceneId: the scene the manager is running
#define kReqSceneId VA(0x803A3AE4)       // lbl_803A3AE0+4: the scene the main loop asks for
#define kNextMapNo VA(0x804D80A4)        // map loaded by the next game-scene setup
#define kSceneChanging VA(0x804D8088)    // set by scene-change requests, cleared after setup
#define kCurMapNo VA(0x804D80C8)         // sCurMapNo
#define kAppearPoint VA(0x804D872C)      // sGlobalLuigiAppearPoint (SETLUIGI)
#define kRoomState VA(0x803A3CA0)        // +0xC: current room number in the top byte
#define kEventCursor (VA(0x803D2D40) + 0xC)  // event interpreter +0x0C: script cursor, 0 = idle

// Compiled-in debug switches that nothing in the retail game sets (docs/UNUSED_CONTENT.md).
#define kParticleReportFlag VA(0x804D8E04)  // byte; fn_80160138 prints emitter/particle counts
#define kDepthBlurFlag VA(0x804D86B0)       // word; fn_8005E9EC depth-of-field style blur pass
#define kHudTestFlag VA(0x804D854A)         // byte; fn_800433B4 HUD sprite test
#define kFontTestFlag VA(0x804D8488)        // u16; fn_8003EBB0 font test (PAL 0x804C5B58)
// USA only: the European font test (table lbl_80309AF8) already prints Latin lines.
constexpr u32 kFontTestLine11 = 0x80304C18;      // font-test string table lbl_80304BEC[11]
constexpr u32 kFontTestLine11Orig = 0x80304BC0;  // katakana line (retail)
constexpr u32 kFontTestLatin = 0x80304C1C;       // "alphabet 123" (what the PAL code points it at)

const MapInfo kMaps[] = {
    {0, "Test map (Iwamoto test_00-03)", false, true, "toad, butler, dancer, Boos, Mario test actors"},
    {1, "E. Gadd's lab", true, true, nullptr},
    {2, "Mansion", true, true, nullptr},
    {3, "Training room (Poltergust practice)", true, true, nullptr},
    {4, "Ghost Portrificationizer", true, true, nullptr},
    {5, "Unused: statue hall (h_03)", false, true, "4 rooms, Luigi only"},
    {6, "Portrait gallery", true, true, nullptr},
    {7, "Unused: statue hall, long version (h_05)", false, true, "map 5 with corridors 2000 units longer"},
    {8, "Unused: lantern hallway (h_06)", false, true, "E. Gadd test placement; old JMP tables converted"},
    {9, "Roof (King Boo / Bowser)", true, true, nullptr},
    {10, "Chauncey's room", true, true, nullptr},
    {11, "Balcony (Boolossus)", true, true, nullptr},
    {12, "Portrificationizer (after Bowser / game over)", true, true, "map 4's room, own setup"},
    {13, "Graveyard (Bogmire)", true, true, nullptr},
};

// map2 jmp/characterinfo "luige" entries (appear_point -> room_no); names as the game shows them on arrival.
const AppearPoint kAppear[] = {
    {0, 2, "Foyer (new game / default)"},
    {6, 2, "Foyer, by the front door"},
    {2, 16, "Graveyard (outside the 1F west wing)"},
    {9, 70, "Secret Altar (basement)"},
    {1, 24, "Nursery (2F)"},
    {7, 36, "Sealed Room (2F)"},
    {4, 56, "Clockwork Room (3F)"},
    {5, 59, "Balcony (3F)"},
    {8, 48, "3F west hallway"},
    {3, 60, "Roof"},
};

std::atomic<int> sWarpMap{-1};
std::atomic<int> sWarpAppear{0};
u32 sGameScene = 0xFFFFFFFF;  // scene id the game scene setup ran under
u32 sLuigi = 0;                // Luigi's actor, seen by his update this frame
u32 sLuigiFrames = 0;          // consecutive frames Luigi's update ran since the last map setup
u32 sSetupFrames = 0;          // frames since the last map setup

// Entry events of a warp's destination (see fn_8002B6D4 below). A warp sets sArrivalPending;
// the next map setup turns it into a visit whose window lasts kEntryWindow frames. Event triggers whose condition holds in that window are recorded and
// kept from starting for the rest of the visit (until the next map setup).
constexpr u32 kEntryWindow = 150;
bool sArrivalPending = false;
bool sVisit = false;
std::vector<u32> sSuppressed;  // trigger records (0x58 bytes each, array lbl_804D8370)

// Values sampled on the guest thread for the host-side overlays.
struct Status {
    bool inGame = false;
    int map = -1, room = -1;
    u32 scene = 0;
    float pos[3] = {};
};
Status sStatus;

// The game's debug-print queue (JUTReport, fn_801D71B0 -> fn_801D7114). Retail never creates
// the print object; the hook collects the text instead and the overlay draws it.
struct ReportLine {
    int x, y;
    std::string text;
};
std::vector<ReportLine> sReportBuild, sReportShown;

bool sInfoOverlay = std::getenv("LM_DEV_INFO") != nullptr;  // F3; LM_DEV_INFO=1 starts with it on

int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}

}  // namespace

const MapInfo* maps(int* count) {
    *count = int(std::size(kMaps));
    return kMaps;
}
const AppearPoint* mansionAppearPoints(int* count) {
    *count = int(std::size(kAppear));
    return kAppear;
}
void requestWarp(int map, int appearPoint) {
    rooms::cancel();
    sWarpAppear = appearPoint;
    sWarpMap = map;
}
bool inGameScene() { return sStatus.inGame; }
int currentMap() { return sStatus.map; }
// "Debug Mode" Action Replay code (403A2B84 001325CB): Luigi's debug-flag halfword +0x1324 gets
// low byte 0xCB every frame. Bits as the game reads them: 0x08 debug text (fn_800B172C pass 6),
// 0x40 scan boxes (pass 3), 0x10 (fn_800AE770), 0x20 (fn_800B1BD4, toggled by fn_800B2F98),
// 0x04 (fn_800B80C4); Luigi's actor initialises it to 0x14 (fn_800AD5AC). The value the game
// had is put back when the option is turned off.
void noteLuigi(unsigned guestActor) {
    sLuigi = guestActor;
    testStateLuigi(guestActor);
    static u32 sActor = 0;
    static u8 sSaved = 0;
    const bool on = port::settings().enhancements.dbgLuigiFlags || envInt("LM_DEV_LUIGI_FLAGS", 0) != 0;
    if (on) {
        if (sActor != guestActor) {
            sActor = guestActor;
            sSaved = u8(rd8(guestActor + 0x1325));
        }
        hostp(guestActor + 0x1325)[0] = 0xCB;
    } else if (sActor) {
        if (sActor == guestActor) hostp(guestActor + 0x1325)[0] = sSaved;
        sActor = 0;
    }
}

// ---- hooks (guest thread) ------------------------------------------------------------

// Game-scene setup for sCurMapNo. LM_START_MAP=<n> (dev aid, e.g. for the boss maps 9/10/11)
// loads map n instead of the mansion the first time map 2 loads; LM_START_APPEAR=<p> picks the
// mansion appear point.
HLE(fn_8000C188) {
    recomp::unusedModelsMapSetup();
    static const int sMap = envInt("LM_START_MAP", -1);
    static const int sAppear = envInt("LM_START_APPEAR", 0);
    static bool sDone = false;
    if (!sDone && rd32(kCurMapNo) == 2 && (sMap >= 0 || sAppear > 0)) {
        sDone = true;
        if (sMap >= 0) {
            log("[dev] LM_START_MAP: loading map %d instead of the mansion", sMap);
            wr32(kCurMapNo, u32(sMap));
        }
        if (sAppear > 0) wr32(kAppearPoint, u32(sAppear));
    }
    sGameScene = rd32(kReqSceneId);
    sLuigiFrames = 0;
    sSetupFrames = 0;
    sSuppressed.clear();
    sVisit = sArrivalPending;  // reached by a warp: its entry events are held back
    sArrivalPending = false;
    rooms::onMapSetup();
    coop::onMapSetup(c);  // unused 2-player mode: latched per map (coop.cpp)
    unused2::onMapSetup(c);  // second unused-content batch (unused2.cpp)
    callOriginal(c, "fn_8000C188");
}

// Event trigger conditions (trigger record): fn_8002B7BC asks this every frame for each
// jmp/eventinfo trigger that is idle; 1 starts the trigger's event (eventNN.txt). It checks
// the trigger's flags and then its EventIf condition callback (+0x10): 0 always, 2..6 Luigi
// within EventArea in the trigger's room, 1 (fn_8002C364) the same plus A pressed facing it.
// After a warp, automatic triggers (everything but A-to-talk) that would fire on arrival are
// entry cutscenes (E. Gadd's lab and Portrificationizer talks, boss intros, King Boo at the
// Secret Altar, the training room explanation): they are held back for that visit.
HLE(fn_8002B6D4) {
    const u32 trig = c.r[3];
    callOriginal(c, "fn_8002B6D4");
    if (!sVisit || !port::settings().enhancements.dbgWarpSkipEvents) return;
    bool held = false;
    for (u32 t : sSuppressed) held |= t == trig;
    if (!held && c.r[3] == 1 && sSetupFrames < kEntryWindow && rd32(trig + 0x10) != VA(0x8002C364)) {
        sSuppressed.push_back(trig);
        log("[dev] warp: entry event %d held back (map %d)", s32(s16(rd16(trig + 0x38))), s32(rd32(kCurMapNo)));
        held = true;
    }
    if (held) c.r[3] = 0;
}

// Gallery screen setup (game mode 6), called by the <GALLERY> event tag (event 79) with the
// trigger's event parameter: 1..23 a portrait plaque in the gallery (map 6), 100 the area-clear
// screen ("Now you've caught: (Area n) ..."), drawn by fn_8018721C. Test aid
// LM_GALLERY_PARAM=<n> replaces the parameter, so any plaque opens screen n (e.g. 100).
HLE(fn_8018712C) {
    static const int sParam = envInt("LM_GALLERY_PARAM", -1);
    if (sParam >= 0) {
        log("[dev] LM_GALLERY_PARAM: gallery screen %d instead of %d", sParam, s32(c.r[3]));
        c.r[3] = u32(sParam);
    }
    callOriginal(c, "fn_8018712C");
}

// Scene manager, once per main-loop iteration. A queued warp is applied exactly like the
// event command <WARP>(n): next map = n and the current scene id cleared, so the manager
// tears the game scene down and sets it up again for the new map on this call. Only while
// the scene is settled: two seconds after the map setup, Luigi running for a second where
// there is one (tearing a map down while it is still being set up crashes), and no event script.
HLE(fn_8000B4A0) {
    const int map = sWarpMap.load();
    if (map >= 0) {
        const u32 scene = rd32(kCurSceneId);
        if (scene == sGameScene && rd32(kReqSceneId) == scene && rd32(kSceneChanging) == 0 && sSetupFrames >= 60 &&
            (sLuigiFrames >= 30 || sSetupFrames >= 300) && rd32(kEventCursor) == 0) {
            sWarpMap = -1;
            sArrivalPending = true;
            log("[dev] warp to map %d (appear point %d)", map, sWarpAppear.load());
            wr32(kAppearPoint, u32(sWarpAppear.load()));
            wr32(kNextMapNo, u32(map));
            wr32(kCurSceneId, 0);
        }
    }
    callOriginal(c, "fn_8000B4A0");
}

// JUTReport's queue insert: (print object, x, y, flag, text, length). Collected for the overlay.
HLE(fn_801D7114) {
    if (c.r[8] > 0 && c.r[7]) sReportBuild.push_back({s32(c.r[4]), s32(c.r[5]), std::string(gstr(c.r[7]), c.r[8])});
}

// Luigi's draw-pass callback (self, pass). The Action Replay codes NOP three branches in it;
// the same effect is produced by presenting the flag the branch tests for the duration of the
// call (and, for the axes, by doing the skipped draw after the original):
//   0x800B17CC beq (pass 3, flag bit 0x40)  -> Game Boy Horror scan boxes (fn_8007BDA4)
//   0x800B17E4 beq (pass 6, flag bit 0x08)  -> debug text: dust, aura, water, heart (fn_8003EF40)
//   0x800B1794 ble (pass 2, +0x1328 > 0)    -> XYZ axes on joint +0x1328 (fn_80094170, size 150)
HLE(fn_800B172C) {
    const auto& e = port::settings().enhancements;
    static const int sEnvInfo = envInt("LM_DEV_INFO_DISPLAY", 0), sEnvScan = envInt("LM_DEV_SCAN_BOXES", 0),
                     sEnvAxes = envInt("LM_DEV_AXES", 0);
    const u32 self = c.r[3], pass = c.r[4];
    u16 force = 0;
    if (pass == 6 && (e.dbgInfoDisplay || sEnvInfo)) force = 0x08;
    if (pass == 3 && (e.dbgScanBoxes || sEnvScan)) force = 0x40;
    const u16 flags = u16(rd16(self + 0x1324));
    if (force) wr16(self + 0x1324, u16(flags | force));
    const bool axes = pass == 2 && (e.dbgAxes || sEnvAxes) && s32(rd32(self + 0x1328)) <= 0;
    callOriginal(c, "fn_800B172C");
    if (force) wr16(self + 0x1324, u16((rd16(self + 0x1324) & ~force) | (flags & force)));
    if (axes) {
        // As the patched code: concat(actor matrix +8, joint matrix [+0x1328 = 0]) then fn_80094170.
        const u32 joints = rd32(rd32(self + 4) + 0x14);
        constexpr u32 kMtx = 0x81810400;  // dev scratch (model_viewer.cpp uses 0x81810000-0x810300)
        if (gu::inMem1(joints)) {
            gu::call(c, GU_SYM("PSMTXConcat"), {self + 8, joints, kMtx});
            gu::call(c, GU_SYM("fn_80094170"), {kMtx}, {150.0});
        }
    }
}

// HUD test (lbl_804D854A, fn_800433B4): the Action Replay code rewrites `addi r16, r16, 0xA90`
// at 0x80043910, the base of the 19 sprites the test's second loop draws with fn_8003BEEC.
// 0xA90 icons (as shipped), -0x300 numbers, 0xFA0 element meter. Reproduced by moving the
// sprite pointer of the calls made from that loop (return address 0x80043930).
HLE(fn_8003BEEC) {
    static const int sEnvHud = envInt("LM_DEV_HUD_TEST", -1);
    const int mode = sEnvHud >= 0 ? sEnvHud : port::settings().enhancements.dbgHudTest;
    if (c.lr == VA(0x80043930) && (mode == 2 || mode == 3)) c.r[3] = c.r[3] - 0xA90 + (mode == 2 ? u32(-0x300) : 0xFA0u);
    callOriginal(c, "fn_8003BEEC");
}

// Font test (fn_8003EBB0, flag lbl_804D8488). In US it prints under whatever projection the HUD
// drawn just before it left behind, so nothing shows; PAL (fn_80040234) added a call to
// LMDefaultOrthoView at its start. The same is done here while the test is on.
HLE(fn_8003EBB0) {
    if (rd16(kFontTestFlag)) gu::call(c, GU_SYM("LMDefaultOrthoView__Fv"));
    callOriginal(c, "fn_8003EBB0");
}

// End of the main loop's frame: copy to the XFB and flip. Debug switches are applied, the
// overlays' state sampled and the model viewer drawn here (into this frame's EFB).
HLE(LMChangeFrameBuffer__Fv) {
    const auto& e = port::settings().enhancements;
    static const int sEnvReport = envInt("LM_DEV_PARTICLE_REPORT", -1);
    static const int sEnvBlur = envInt("LM_DEV_DEPTH_BLUR", -1);
    hostp(kParticleReportFlag)[0] = (sEnvReport >= 0 ? sEnvReport != 0 : e.dbgParticleReport) ? 1 : 0;
    wr32(kDepthBlurFlag, (sEnvBlur >= 0 ? sEnvBlur != 0 : e.dbgDepthBlur) ? 1 : 0);
    static const int sEnvHud = envInt("LM_DEV_HUD_TEST", -1);
    hostp(kHudTestFlag)[0] = (sEnvHud >= 0 ? sEnvHud : e.dbgHudTest) != 0 ? 1 : 0;
    static const int sEnvFont = envInt("LM_DEV_FONT_TEST", -1);
    const int font = sEnvFont >= 0 ? sEnvFont : (e.dbgFontTest ? (e.dbgFontTestLatin ? 2 : 1) : 0);
    wr16(kFontTestFlag, font ? 1 : 0);
    if (ver::usa()) wr32(kFontTestLine11, font == 2 ? kFontTestLatin : kFontTestLine11Orig);

    // Test aid: LM_DEV_WARPS=<map>[:<appear>]@<frame>,... queues warps at given frame counts.
    static std::vector<std::array<int, 3>> sEnvWarps = [] {
        std::vector<std::array<int, 3>> v;
        if (const char* w = std::getenv("LM_DEV_WARPS"))
            for (const char* p = w; *p;) {
                int map = 0, appear = 0, frame = 0;
                if (std::sscanf(p, "%d:%d@%d", &map, &appear, &frame) == 3 || std::sscanf(p, "%d@%d", &map, &frame) == 2)
                    v.push_back({frame, map, appear});
                const char* comma = std::strchr(p, ',');
                if (!comma) break;
                p = comma + 1;
            }
        return v;
    }();
    static u32 sFrames = 0;
    ++sFrames;
    for (const auto& w : sEnvWarps)
        if (u32(w[0]) == sFrames) requestWarp(w[1], w[2]);
    // Test aid: LM_DEV_ROOM=<leftover room index>@<frame>,... starts leftover-room visits.
    static const char* sEnvRoom = std::getenv("LM_DEV_ROOM");
    for (const char* p = sEnvRoom; p && *p;) {
        int idx = 0, frame = 0;
        if (std::sscanf(p, "%d@%d", &idx, &frame) == 2 && u32(frame) == sFrames) rooms::visit(idx);
        const char* comma = std::strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }

    Status s;
    s.scene = rd32(kCurSceneId);
    s.inGame = s.scene == sGameScene && rd32(kSceneChanging) == 0;
    if (s.inGame) {
        s.map = s32(rd32(kCurMapNo));
        s.room = s32(rd32(kRoomState + 0xC) >> 24);
        const u32 luigi = sLuigi;
        if (gu::inMem1(luigi))
            for (int i = 0; i < 3; i++) s.pos[i] = gu::rdf(luigi + 0x44 + 4 * i);
    }
    sStatus = s;
    rooms::onFrame(c, s.inGame, sLuigi != 0, s.map, s.room);
    // Test aid: LM_DEV_STATUS_LOG=<n> logs the warp-relevant state every n frames (scene, map,
    // room, Luigi's position, event script cursor, HUD-off and Luigi-stopped flags).
    static const int sStatusLog = envInt("LM_DEV_STATUS_LOG", 0);
    if (sStatusLog > 0 && sFrames % u32(sStatusLog) == 0) {
        const u32 luigi = sLuigi;
        const u32 lflags = gu::inMem1(luigi) ? rd32(luigi + 0x40) : 0;
        log("[dev] f%u scene %u ingame %d map %d room %d pos %.0f %.0f %.0f event %08X hudoff %u luigiflags %08X", sFrames,
            s.scene, s.inGame ? 1 : 0, s32(rd32(kCurMapNo)), s32(rd32(kRoomState + 0xC) >> 24), s.pos[0], s.pos[1],
            s.pos[2], rd32(kEventCursor), rd32(kSceneChanging), lflags);
    }

    sLuigiFrames = sLuigi ? sLuigiFrames + 1 : 0;
    ++sSetupFrames;
    sLuigi = 0;
    sReportShown.swap(sReportBuild);
    sReportBuild.clear();

    // Test aid: LM_DEV_VIEWER_TOGGLE=<frame>,<frame>,... opens/closes the model viewer (as F4
    // does) at those frames of this counter (open during play, close, reopen after a warp...).
    static const std::vector<u32> sViewerToggles = [] {
        std::vector<u32> v;
        for (const char* p = std::getenv("LM_DEV_VIEWER_TOGGLE"); p && *p;) {
            v.push_back(u32(std::strtoul(p, nullptr, 10)));
            const char* comma = std::strchr(p, ',');
            if (!comma) break;
            p = comma + 1;
        }
        return v;
    }();
    for (u32 f : sViewerToggles)
        if (f == sFrames) {
            viewer::setOpen(!viewer::isOpen());
            log("[dev] f%u model viewer %s (LM_DEV_VIEWER_TOGGLE)", sFrames, viewer::isOpen() ? "opened" : "closed");
        }

    unused2::onFrame(c);
    sillyworld::onFrame(c);
    viewer::guestFrame(c);
    callOriginal(c, "LMChangeFrameBuffer__Fv");
}

// ---- host side ------------------------------------------------------------------------

bool capturesInput() { return viewer::isOpen(); }

bool handleKey(const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return false;
    if (ev.key.key == SDLK_F3) {
        sInfoOverlay = !sInfoOverlay;
        return true;
    }
    if (ev.key.key == SDLK_F4) {
        viewer::setOpen(!viewer::isOpen());
        return true;
    }
    return false;
}

void drawOverlays() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!sReportShown.empty()) {
        // JUTReport coordinates are in the game's 640x480 screen.
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float sx = vp->Size.x / 640.0f, sy = vp->Size.y / 480.0f;
        for (const auto& l : sReportShown) {
            const ImVec2 p(vp->Pos.x + float(l.x) * sx, vp->Pos.y + float(l.y) * sy - ImGui::GetFontSize());
            dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 255), l.text.c_str());
            dl->AddText(p, IM_COL32(255, 255, 255, 255), l.text.c_str());
        }
    }
    if (sInfoOverlay) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 8 * recomp::ui::scale(), vp->Pos.y + 8 * recomp::ui::scale()),
                                ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##devinfo", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        const Status s = sStatus;
        if (s.inGame) {
            int n = 0;
            const MapInfo* m = maps(&n);
            ImGui::Text("map %d  %s", s.map, s.map >= 0 && s.map < n ? m[s.map].name : "?");
            ImGui::Text("room %d", s.room);
            ImGui::Text("luigi %.0f %.0f %.0f", s.pos[0], s.pos[1], s.pos[2]);
        } else {
            ImGui::Text("scene %u: transition, event or menu (warps wait)", s.scene);
        }
        ImGui::End();
    }
    viewer::window();
    unused2::drawOverlays();
}

namespace {

// Tooltip of the option just drawn (also while it is greyed out), as everywhere else in the menu.
void helpMarker(const char* text) { ImGui::SetItemTooltip("%s", text); }

// One table of maps with "Go" buttons; `unusedOnly` limits it to the maps normal play never loads.
void mapTable(const char* id, bool unusedOnly) {
    const bool inGame = sStatus.inGame;
    if (!inGame) ImGui::TextDisabled("Warps are available while playing (load a save or start a new game).");
    ImGui::BeginDisabled(!inGame);
    int n = 0;
    const MapInfo* m = maps(&n);
    // Button, then the map name with an "unused" tag; the note is the row's tooltip (a third column
    // for it cut the longer names and notes off at the menu's default width).
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##go", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##map", ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < n; i++) {
            if (unusedOnly && m[i].used) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[32];
            std::snprintf(label, sizeof(label), "Go##%s%d", id, m[i].no);
            ImGui::BeginDisabled(!m[i].loads);
            if (recomp::menu::SmallButton(label)) requestWarp(m[i].no, 0);
            ImGui::EndDisabled();
            if (!m[i].loads) ImGui::SetItemTooltip("This map does not load.");
            ImGui::TableNextColumn();
            ImGui::Text("%2d  %s", m[i].no, m[i].name);
            if (m[i].note) ImGui::SetItemTooltip("%s", m[i].note);
            if (!m[i].used) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "unused");
                if (m[i].note) ImGui::SetItemTooltip("%s", m[i].note);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    if (sWarpMap.load() >= 0) ImGui::TextDisabled("Warp queued (applied when no transition, event or menu is active)...");
}

}  // namespace

void unusedSection() {
    auto& e = port::settings().enhancements;
    if (recomp::menu::Checkbox("Unused depth blur pass", &e.dbgDepthBlur)) port::saveSettings();
    helpMarker("A screen pass in the game-scene draw (fn_8005E9EC) behind a flag nothing sets\n"
               "(0x804D86B0): the frame is copied at quarter size and blended back by depth,\n"
               "blurring everything away from the camera.");
    ImGui::SeparatorText("Unused areas");
    mapTable("unusedmaps", true);

    ImGui::SeparatorText("Unused rooms");
    ImGui::TextDisabled("Leftover room models. Go warps to the retail room and shows the old model\n"
                        "for that visit only.");
    // Its hooks (Luigi's start position for the visited room) are written against the USA code.
    const bool roomsHere = ver::usa();
    if (!roomsHere) ImGui::TextDisabled("(%s)", port::availNote(ver::pal() ? port::Avail::NotYet : port::Avail::NotOnJpn));
    ImGui::BeginDisabled(!sStatus.inGame || !roomsHere);
    int nr = 0;
    const rooms::LeftoverRoom* lr = rooms::list(&nr);
    if (ImGui::BeginTable("unusedrooms", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        for (int i = 0; i < nr; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[32];
            std::snprintf(label, sizeof(label), "Go##room%d", i);
            if (recomp::menu::SmallButton(label)) rooms::visit(i);
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", lr[i].name, rooms::active() == i ? "  (showing)" : "");
            ImGui::SetItemTooltip("%s\nmap %d%s", lr[i].note, lr[i].map, lr[i].room >= 0 ? ", room swap" : "");
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
}

void debugSection() {
    auto& e = port::settings().enhancements;
    ImGui::TextDisabled("Debug features left in the retail game, plus port-side viewers. All off by default.");
    ImGui::SeparatorText("Overlays and viewers");
    if (recomp::menu::Checkbox("Particle manager report (JUTReport)", &e.dbgParticleReport)) port::saveSettings();
    helpMarker("Emitter / particle / child counts and per-group usage printed by the game's particle\n"
               "manager with JUTReport (flag 0x804D8E04; retail never sets it nor creates the print queue).");
    recomp::menu::Checkbox("Info overlay (F3)", &sInfoOverlay);
    helpMarker("Port-side: current map, room and Luigi's position.");
    // "###" keeps one ID while the label changes (the click lands on the same button).
    if (recomp::menu::Button(viewer::isOpen() ? "Close model viewer (F4)###modelviewer" : "Model viewer (F4)###modelviewer"))
        viewer::setOpen(!viewer::isOpen());
    helpMarker("Port-side viewer for every model on the disc (and whole maps), drawn with the game's own\n"
               "model code. F4 opens and closes it in game.");

    ImGui::SeparatorText("Debug modes (Action Replay codes)");
    bool dirty = false;
    dirty |= recomp::menu::Checkbox("Debug mode (Luigi debug flags)", &e.dbgLuigiFlags);
    helpMarker("AR 403A2B84 001325CB: Luigi's debug flags (+0x1324) low byte = 0xCB every frame.\n"
               "Turns on the debug text and the scan boxes together and changes some of Luigi's\n"
               "drawing (his model uses a slightly different shading).");
    dirty |= recomp::menu::Checkbox("Debug info display", &e.dbgInfoDisplay);
    helpMarker("Japanese text at the top left: dust (mg), aura / ghosts captured, water, heart.\n"
               "AR: NOP 0x800B17E4 (originally on D-pad Left + A).");
    dirty |= recomp::menu::Checkbox("Game Boy Horror scan boxes", &e.dbgScanBoxes);
    helpMarker("Frames around what the Game Boy Horror can scan. AR: NOP 0x800B17CC.");
    dirty |= recomp::menu::Checkbox("XYZ axes at Luigi", &e.dbgAxes);
    helpMarker("Axis vectors at Luigi's position and facing. AR: NOP 0x800B1794.");
    static const char* kHud[] = {"Off", "Icons", "Numbers", "Element meter"};
    dirty |= ImGui::Combo("HUD test", &e.dbgHudTest, kHud, 4);
    helpMarker("The HUD sprite test (flag 0x804D854A) and its three sprite sets.\n"
               "AR: 0x80043910 addi r16, r16, 0xA90 / -0x300 / 0xFA0.");
    dirty |= recomp::menu::Checkbox("Font test", &e.dbgFontTest);
    helpMarker("Twelve lines of kanji / kana / symbols printed with the game font (flag 0x804D8488;\n"
               "PAL code 0A4B8590 0201 / 024C5B58 0001).");
    ImGui::BeginDisabled(!e.dbgFontTest || !ver::usa());
    ImGui::SameLine();
    dirty |= recomp::menu::Checkbox("Latin last line (PAL fix)", &e.dbgFontTestLatin);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", !ver::usa() ? "Only on the USA version (the European font test already prints Latin lines)."
                                           : "The font test's last line prints \"alphabet 123\" in Latin letters, as the\n"
                                             "European version does. Needs \"Font test\".");
    if (dirty) port::saveSettings();

    ImGui::SeparatorText("Visit area");
    if (recomp::menu::Checkbox("Skip entry cutscenes", &e.dbgWarpSkipEvents)) port::saveSettings();
    helpMarker("A warp does not start the cutscene or event the destination plays on arrival\n"
               "(E. Gadd in the lab and the Portrificationizer room, the training explanation,\n"
               "boss intros, King Boo at the Secret Altar), so Luigi can walk around freely.\n"
               "Events started later by walking somewhere, talking (A) or story flags play as usual.\n"
               "Normal play is unchanged.");
    mapTable("allmaps", false);
    recomp::menu::openTreesForCheck();
    if (ImGui::TreeNode("Mansion entry points")) {
        ImGui::BeginDisabled(!sStatus.inGame);
        int na = 0;
        const AppearPoint* a = mansionAppearPoints(&na);
        for (int i = 0; i < na; i++) {
            char label[64];
            std::snprintf(label, sizeof(label), "Go##ap%d", a[i].point);
            if (recomp::menu::SmallButton(label)) requestWarp(2, a[i].point);
            ImGui::SameLine();
            ImGui::Text("%s  (room %d, point %d)", a[i].where, a[i].room, a[i].point);
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
}

}  // namespace recomp::dev
