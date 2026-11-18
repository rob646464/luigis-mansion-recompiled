// Frame interpolation (interp.h, docs/INTERPOLATION.md): tapes, pairing, blended replay.
#include "recomp/interp.h"

#include "recomp/efb_copy.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/pacing.h"
#include "recomp/sched.h"

#include <aurora/present_timing.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
std::string guestSymbolize(u32 addr);  // runtime.cpp
}

namespace recomp::interp {

// LM_INTERP_DRAWS=<frame> (with LM_FRAME_DUMP numbering): log every draw and noted value of
// that game frame's tape, with the caller chain and what each value was paired with.
// LM_INTERP_DRAWS=<first>[,<last>].
static u32 sDrawLogLast = 0;
static const u32 sDrawLogFrame = [] {
    const char* v = std::getenv("LM_INTERP_DRAWS");
    if (!v) return ~0u;
    char* end = nullptr;
    const u32 first = u32(std::strtoul(v, &end, 10));
    sDrawLogLast = *end == ',' ? u32(std::strtoul(end + 1, nullptr, 10)) : first;
    return first;
}();
static bool drawLogFrame(u32 f) { return sDrawLogFrame != ~0u && f >= sDrawLogFrame && f <= sDrawLogLast; }
bool drawLogActive() { return capturing() && drawLogFrame(copyDispCount() + 1); }
void drawLogCaller(const std::string& chain) {
    if (drawLogActive()) log("[draws] caller %s", chain.c_str());
}
void detail::drawLog(const char* fmt, ...) {
    if (!drawLogActive()) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log("[draws] %s", buf);
}

u64 detail::mix(u64 h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

namespace {

using detail::mix;
using Clock = std::chrono::steady_clock;
constexpr double kRetrace = 1001.0 / 60000.0;  // seconds per VI field (NTSC)
constexpr u32 kCaptureSize = 64u << 20;        // one guest frame's commands
constexpr u32 kKinds = u32(Kind::Count);

// The centre of the vertices each position matrix slot moves in one draw (model space).
struct SlotCentres {
    u32 bundle;
    u16 mask;
    f32 c[10][3];
};

// A value in the tape that moves between frames.
struct Patch {
    u32 offset;   // first float, in tape bytes
    u16 count;    // floats (Verts: vertices)
    Kind kind;
    // Identity (tagged interpolation): the tag scope path it was noted in, the GX call site and
    // its occurrence there. 0: untagged, never paired.
    u64 key = 0;
    u32 source = 0, slot = 0;
    u16 stride = 0, posOffset = 0;  // Verts
    u32 bundle = ~0u;  // the draw it belongs to (index into Tape::bundleIds)
    u16 view = 0xFFFF;  // index into Tape::views: the camera it was noted under
    u8 role = 0;        // 1: a shadow receiver's texture matrix (noteShadowLight), 2: 2D (orthographic)
    TagCat cat = TagCat::None;  // category of the innermost object scope (policy)
    bool inScope = false;       // noted inside any tag scope (a view at least): untagged values outside are left as captured
    bool ownCamera = false;     // noted in a SubView (its own camera: blended as drawn)
    u32 site = 0;       // guest call site (logs)
    u64 group = 0;      // TagFrame::group: values of one model instance switch together
    u16 clrOffset = 0xFFFF;  // Verts: a direct colour in each vertex (blended unless it jumps)
    u8 clrBytes = 0;
    // 2D blocks (role 2): position format (0x100 | type | frac << 3) and direct texture coordinates.
    u8 uvBytes = 0;
    u16 posFmt = 0;
    u16 uvOffset = 0xFFFF;
    bool sky = false;  // noted inside the sky dome's draw (pushSky)
    u16 tgroup = 0;  // text box group (pushTextGroup): index + 1 into Tape::textGroups, 0 none
    s32 mtxPatch = -1;  // 2D blocks: the position matrix patch current at its GXBegin (-1: none noted)
    s32 projPatch = -1;  // 2D blocks: the projection patch its vertices were written under (-1: none noted)
    bool tumble = false;  // noted in a tagTumbler scope: model matrices blend the short way
    bool tracked = false;  // noted in a tagTracked scope: a CPU-built block turning back is a bounce
    bool stream = false;   // an element stream's drop (tagStreamItem / tagTumbler; LM_SPRAY_LOG)
    s32 backStep = -1;    // tagBackStep: index into Tape::backSteps (world step per game frame), -1 none
};

// A text box drawn in one game frame (pushTextGroup): its patches [first, end) are blended as one
// layout (replayTextGroups; docs/INTERPOLATION.md "Text boxes at 120").
struct TextGroupRec {
    u32 id = 0;
    u32 first = 0, end = 0;
    bool oneLayout = false;
};
// A viewport (XF 0x101A, 6 floats) or scissor box (BP 0x20 / 0x21) written in a text group.
struct TextView {
    u32 at = 0;     // tape offset of the write
    u8 scissor = 0;
    u16 group = 0;  // index + 1 into Tape::textGroups
    float f = 1, c = 0;  // layout x -> pixel x
};

using M34 = std::array<f32, 12>;
struct ShadowLight {
    bool valid = false;
    M34 lv{}, kp{}, kr{};
};

// --- per-tape host state (recycled) ---------------------------------------------
// pairTapes gives each tape one array per kind it follows across game frames (the world* fields and
// vertStep below), one entry per patch. Two tapes cannot share a block: replayPair reads a's entries
// while it writes b's, and the queue can replay an older pair than the newest (sQueue). A block comes
// back to this pool when its tape is destroyed. Seven arrays per tape per game frame were 18 malloc +
// 39 free in endTape.
struct FreeArray {
    void* data = nullptr;
    u32 capacity = 0;  // elements
    u32 element = 0;   // bytes per element (4: atomic<f32>, 1: atomic<u8>)
};
constexpr u64 kFreeArrayBytes = 32u << 20;  // the live tapes' worth, not one huge frame's
std::vector<FreeArray> sFreeArrays;
u64 sFreeArrayBytes = 0;
std::mutex sFreeArraysM;

template <class T>
std::atomic<T>* takeArray(size_t want, u32& capacity) {
    capacity = u32(want);
    if (want == 0) return nullptr;
    {
        std::lock_guard lk(sFreeArraysM);
        size_t best = sFreeArrays.size();
        for (size_t i = 0; i < sFreeArrays.size(); i++)
            if (sFreeArrays[i].element == sizeof(T) && sFreeArrays[i].capacity >= want &&
                (best == sFreeArrays.size() || sFreeArrays[i].capacity < sFreeArrays[best].capacity))
                best = i;
        if (best != sFreeArrays.size()) {
            auto* p = static_cast<std::atomic<T>*>(sFreeArrays[best].data);
            capacity = sFreeArrays[best].capacity;
            sFreeArrayBytes -= u64(capacity) * sizeof(T);
            sFreeArrays[best] = sFreeArrays.back();
            sFreeArrays.pop_back();
            return p;
        }
    }
    return new std::atomic<T>[want];
}

template <class T>
void freeArray(std::atomic<T>* p, u32 capacity) {
    if (!p) return;
    const u64 bytes = u64(capacity) * sizeof(T);
    std::lock_guard lk(sFreeArraysM);
    // Drop the smallest block of this kind first: a block too small for the room the game draws can
    // never serve again, and the pool has to keep room for the ones that can.
    while (sFreeArrayBytes + bytes > kFreeArrayBytes) {
        size_t smallest = sFreeArrays.size();
        for (size_t i = 0; i < sFreeArrays.size(); i++)
            if (sFreeArrays[i].element == sizeof(T) &&
                (smallest == sFreeArrays.size() || sFreeArrays[i].capacity < sFreeArrays[smallest].capacity))
                smallest = i;
        if (smallest == sFreeArrays.size()) break;
        sFreeArrayBytes -= u64(sFreeArrays[smallest].capacity) * sizeof(T);
        delete[] static_cast<std::atomic<T>*>(sFreeArrays[smallest].data);
        sFreeArrays[smallest] = sFreeArrays.back();
        sFreeArrays.pop_back();
    }
    if (sFreeArrayBytes + bytes > kFreeArrayBytes) {
        delete[] p;
        return;
    }
    sFreeArrays.push_back({p, capacity, u32(sizeof(T))});
    sFreeArrayBytes += bytes;
}

// A tape's per-patch array: taken from the pool above, handed back when the tape is destroyed.
template <class T>
struct PooledDelete {
    u32 capacity = 0;
    void operator()(std::atomic<T>* p) const { freeArray(p, capacity); }
};
template <class T>
using Pooled = std::unique_ptr<std::atomic<T>[], PooledDelete<T>>;
template <class T>
void setPooled(Pooled<T>& slot, u32 n) {
    u32 capacity = 0;
    std::atomic<T>* p = takeArray<T>(n, capacity);
    slot = Pooled<T>(p, PooledDelete<T>{capacity});
}

struct Tape {
    std::vector<u8> bytes;
    // (tape offset, guest lr) of every GX call in it: names the caller of a malformed command
    // (Aurora 0235, describeStreamPos).
    std::shared_ptr<const std::vector<std::pair<u32, u32>>> sites;
    std::vector<Patch> patches;
    std::vector<u64> bundleIds;  // per draw: what is drawn (display list, vertex layout)
    std::vector<u64> bundleKey;  // per draw: its tag scope instance (0: untagged)
    std::vector<std::array<u32, 2>> bundleRange;  // per draw: its tape bytes (~0u: unknown)
    std::vector<u8> bundlePass;                    // per draw: the pass (0 main, 1 shadow casters)
    // Per draw: the centre of the bounding box of its indexed positions (model space; x NaN: unknown).
    std::vector<std::array<f32, 3>> bundleCentre;
    // Draws whose vertices pick their matrix (direct PNMTXIDX): the centre per slot (by bundle, ascending).
    std::vector<SlotCentres> slotCentres;
    // Per draw (judder detector only): which noted position matrix each PNMTX slot held (-1: unknown).
    std::vector<std::array<s32, 10>> bundleSlots;
    std::vector<M34> views;    // camera view matrices the values were noted under
    std::vector<u64> viewKeys;   // per view: its identity (structural scope path + order)
    ShadowLight shadow;
    std::vector<std::vector<u8>> arrays;  // vertex-array copies the tape's array loads point at
    std::vector<detail::ArrayRec> arrayRecs;  // tagged position arrays among them (CPU-deformed geometry)
    std::vector<s32> arrayPartner;            // per arrayRec: the same identity's record in pairedWith, or -1
    u64 arrayBytes = 0;
    std::vector<std::shared_ptr<const std::vector<u8>>> textures;  // texture/TLUT copies its loads point at
    u64 textureBytes = 0;  // newly copied for this tape
    mutable std::atomic<bool> replayed{false};  // host: replayed at least once
    u32 retrace = 0;
    Clock::time_point done;
    // Pairing with the tape captured before this one (`pairedWith`, by identical keys).
    const Tape* pairedWith = nullptr;
    u64 seq = 0;  // capture order (consecutive tapes differ by 1); replay state slots
    std::vector<s32> partner;  // per patch: index into pairedWith->patches, or -1
    // Per patch: 1 when it is a position matrix of a model instance (tagGroup) with two or more of them in
    // this tape (an articulated or skinned model; set at pairing). The spinner centre rule is for rigid
    // things only.
    std::vector<u8> multiMember;
    // Per patch (host, set by the replays from pairedWith): how far the model matrix moved in world
    // space since pairedWith (-1: not known). A pair that keeps moving fast is motion, not a jump.
    Pooled<float> worldStep;
    // Per patch (host, likewise): how far its world rotation turned since pairedWith, in degrees (-1:
    // not known), about which axis, and for how many more game frames it counts as tumbling (noteTurn).
    Pooled<float> worldTurn;
    Pooled<float> worldAxis;  // x, y, z per patch
    Pooled<u8> worldTumble;
    Pooled<u8> worldErratic;  // game frames left of erratic-step evidence (instance pass)
    // Per patch (host, set by each replay from pairedWith): 1 when its model instance has a matrix
    // that cannot blend, so the whole instance is shown as b (the detector judges it as a switch).
    Pooled<u8> groupSnap;
    // Per CPU-built block (host, set by the replays from pairedWith): how its centre moved since
    // pairedWith (x, y, z; NaN: not known). A block that turns back is another particle in that slot.
    Pooled<float> vertStep;
    // Per texture matrix (guest, at pairing; texWrapPairs): each row's offset step since pairedWith
    // as blended (NaN: unknown), the whole textures a wrap skipped per row, and the decision
    // (0 the old rules, 1 a scroll blended by its step less texShift, 2 placed anew: b).
    std::vector<std::array<f32, 3>> texStep;
    std::vector<std::array<f32, 3>> texShift;
    std::vector<u8> texMode;
    std::vector<s32> viewToA;  // per view: the view of pairedWith with the same key, or -1
    u16 mainView = 0xFFFF;     // the main camera (most 3D model matrices of the main pass)
    bool cut = false;          // the main camera jumped since pairedWith (or appeared): shown as is
    float camMove = 0, camTurn = 0;  // the main camera's eye move / turn (degrees) since pairedWith
    u32 paired = 0, unpaired = 0, untagged = 0;
    std::vector<std::pair<u32, u32>> lights;  // extra-light commands: offset of the first light, count
    std::vector<u64> lightKeys;               // per extra light (flattened, command order): identity, 0 none
    std::vector<TextGroupRec> textGroups;  // text boxes drawn in this game frame
    std::vector<TextView> textViews;       // their viewports and scissor boxes
    std::vector<std::array<f32, 3>> backSteps;  // tagBackStep: world steps of things just born (Patch::backStep)
    // Host (replays only): per patch the block of pairedWith it shows the same glyph/picture as,
    // in a text group (sequence alignment, computed at the first replay of the pair; -1 none).
    mutable std::vector<s32> textMatch;
    mutable std::vector<u8> textKind;  // per patch: 1 the same place in its box, 2 moved with the text layer, 3 on its own
    mutable std::vector<std::array<float, 2>> textD;  // per patch: its panel's text layer offset since pairedWith (layout)
    mutable std::shared_ptr<void> textCache;  // host: the text groups' blocks and panels, built at the first replay of the pair
    u32 dumpOffset = ~0u;  // LM_INTERP_TEST: tape offset of the LM_FRAME_DUMP copy's target pointer
    u32 dumpFrame = 0;
    u32 gameFrame = 0;  // GXCopyDisp count + 1 when captured (LM_FRAME_DUMP / LM_INTERP_DRAWS numbering; logs)
    // LM_TRACK_LOG: Luigi, Poltergust and nozzle actor places at the end of the game frame (detail::trackPoints).
    std::array<f32, 9> track{};
    bool trackOk = false;
    u32 guestUs = 0;  // guest-thread time spent closing the tape (array copies' bookkeeping, pairing)
    // Breakdown of guestUs: array bookkeeping, tape copy, pairing, -, cut check.
    u32 phaseUs[5] = {};
};

u32 usSince(std::chrono::steady_clock::time_point& t) {
    const auto now = std::chrono::steady_clock::now();
    const u32 us = u32(std::chrono::duration_cast<std::chrono::microseconds>(now - t).count());
    t = now;
    return us;
}

// A key -> value table whose entries live for one game frame (occurrence counters, pairTapes' key
// indexes): an entry carries the generation it was written in, so a new tape bumps the generation
// instead of clearing and rehashing (those clears took 96 + 60 + 43 guest-thread samples). Only
// tables probed by key, never iterated, are stamped; stale keys go once the table holds a few tapes.
template <class V>
class GenTable {
public:
    struct Found {
        V& val;
        bool fresh;  // just created: V{}
    };
    Found entry(u64 key) {
        auto [it, added] = rows.try_emplace(key);
        Entry& e = it->second;
        if (!added && e.gen == gen) return {e.val, false};
        e.gen = gen;
        e.val = V{};
        return {e.val, true};
    }
    const V* find(u64 key) const {
        const auto it = rows.find(key);
        return it != rows.end() && it->second.gen == gen ? &it->second.val : nullptr;
    }
    void reserve(size_t n) { rows.reserve(n); }
    // The next game frame: everything written so far reads as absent.
    void nextFrame(size_t cap) {
        if (rows.size() > cap) rows.clear();
        if (++gen == 0) {  // wrapped: 0 is the generation of a never-written entry
            rows.clear();
            gen = 1;
        }
    }

private:
    struct Entry {
        u32 gen = 0;
        V val{};
    };
    std::unordered_map<u64, Entry> rows;
    u32 gen = 1;
};

// --- guest side (guest thread only) ---
std::vector<u8> sScratch;
u64 sSeq = 0;  // tapes captured so far
bool sCapturing = false;
std::vector<std::pair<u32, u32>> sTapeSites;  // the capture's GX call sites (noteCallSite)
// Recent replays: where each put its tape in Aurora's FIFO stream (describeStreamPos).
struct ReplayRec {
    u64 start = 0;
    u32 size = 0;
    u64 seq = 0;
    u32 gameFrame = 0;
    std::shared_ptr<const std::vector<std::pair<u32, u32>>> sites;
};
std::mutex sReplayRecM;
std::array<ReplayRec, 16> sReplayRecs;
u32 sReplayRecNext = 0;
bool sSuspended = false;  // the game is recording its own display list
u32 sBase = 0;            // start of the current capture segment in sScratch
std::vector<Patch> sPatches;
u32 sUngrouped = 0;  // patches from here on wait for their draw
std::vector<u64> sBundleIds;
std::vector<u64> sBundleKey;
std::vector<std::array<u32, 2>> sBundleRange;
std::vector<u8> sBundlePass;
std::vector<std::array<f32, 3>> sBundleCentre;
std::vector<SlotCentres> sSlotCentres;  // (by bundle, ascending)
s32 sOpenDraw = -1;  // immediate-mode draw whose end is not known yet
std::vector<M34> sViews;
std::vector<u64> sViewKeys;
ShadowLight sShadow;
int sShadowRecv = 0;
u32 sPass = 0;
bool sOrtho = false;
std::vector<TextGroupRec> sTextGroups;
int sTextDepth = 0;
std::vector<TextView> sTextViews;
std::vector<std::array<f32, 3>> sBackSteps;  // tagBackStep of the tape being captured
u32 sTextViewJoin = 0;  // pushTextViewJoin's group id (0 none)
int sTextViewJoinDepth = 0;
u16 sTextCur = 0;   // the open text group (index + 1), 0 none
u16 sVertsText = 0;  // the text group of the immediate-mode block begun at GXBegin
u32 sCurMtxSlot = 0;  // GXSetCurrentMtx / 3
s32 sLastPosMtx[10];  // per position matrix slot: the patch that loaded it in this tape (-1 none)
s32 sVertsMtx = -1;   // the current position matrix patch at the block's GXBegin
s32 sLastProj = -1;   // the last projection noted in this tape

// Judder detector (LM_JUDDER_LOG, see probeJudge): the noted position matrix each PNMTX slot holds.
FILE* const sProbeOut = [] {
    const char* p = std::getenv("LM_JUDDER_LOG");
    return p && *p ? std::fopen(p, "w") : nullptr;
}();
const float sProbeThresh = [] {
    const char* v = std::getenv("LM_JUDDER_PX");
    return v ? float(std::atof(v)) : 1.5f;
}();
// Path check (LM_TRACK_LOG=<file>[,<radius>[,<px>]], needs LM_JUDDER_LOG for the draw probe; see
// trackJudge): every drawn vertex near Luigi (radius, default 300 units) against the straight line
// between its two game-frame places, whatever the blending policy.
FILE* const sTrackOut = [] {
    const char* p = std::getenv("LM_TRACK_LOG");
    if (!p || !*p) return (FILE*)nullptr;
    std::string s(p);
    return std::fopen(s.substr(0, s.find(',')).c_str(), "w");
}();
const float sTrackRadius = [] {
    const char* p = std::getenv("LM_TRACK_LOG");
    const char* c = p ? std::strchr(p, ',') : nullptr;
    return c ? float(std::atof(c + 1)) : 300.0f;
}();
const float sTrackPx = [] {
    const char* p = std::getenv("LM_TRACK_LOG");
    const char* c = p ? std::strchr(p, ',') : nullptr;
    c = c ? std::strchr(c + 1, ',') : nullptr;
    return c ? float(std::atof(c + 1)) : 3.0f;
}();
std::mutex sTrackFileM;
void trackLine(const char* fmt, ...) {
    if (!sTrackOut) return;
    std::lock_guard lk(sTrackFileM);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(sTrackOut, fmt, ap);
    va_end(ap);
}
std::mutex sProbeFileM;
void probeLine(const char* fmt, ...) {
    std::lock_guard lk(sProbeFileM);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(sProbeOut, fmt, ap);
    va_end(ap);
}
// Identity check (guest, at pairing): a tagged model matrix followed over three game frames by its
// identity must move smoothly in the world; a jump there and back in the middle frame means the
// identity paired different things (a wrong tag).
struct IdentityTotals {
    u64 checked = 0, flagged = 0;
    u64 vertsChecked = 0, vertsFlagged = 0;  // CPU-built vertex blocks (their centroid)
} sIdTotals;
std::array<s32, 10> sSlotPatch;
std::vector<std::array<s32, 10>> sBundleSlots;

// --- tags (guest thread) ---
struct TagFrame {
    u64 inst;      // identity of this scope instance
    TagCat cat;
    bool object;   // an object scope is open here or above (values inside are tagged)
    u64 structural;  // identity of the structural (View) scopes only: the camera's identity
    bool ownCamera = false;  // inside a SubView: drawn with a camera other than the game camera
    u64 group = 0;  // the model instance it belongs to (tagGroup: a J3D model's skinned meshes), 0: none
    bool tumble = false;  // tagTumbler, inherited
    bool tracked = false;  // tagTracked, inherited
    bool stream = false;   // tagStreamItem / tagTumbler, inherited
    s32 backStep = -1;    // tagBackStep (index into sBackSteps), inherited
};
constexpr int kMaxTagDepth = 64;
TagFrame sTagStack[kMaxTagDepth];
int sTagDepth = 0;
GenTable<u32> sTagOcc;   // per tape: scope base identity -> occurrences so far
GenTable<u32> sValOcc;   // per tape: (scope, kind, site...) -> values noted so far
std::map<u32, u32> sGen;  // tagged object address -> generation (ordered: freed blocks are ranges)
u32 sGenEpoch = 0;        // added to every generation; bumped when the table is reset
std::vector<SiteTag> sSiteTags;
u32 sCallLr = 0;        // guest call site of the GX call being handled
// Call-site tag of the GX call being handled (a loop inside one game function: one item each).
bool sVirtActive = false;
TagFrame sVirt{};
u64 sVirtBase = 0;      // the item of the previous matching call (same item: same instance)
u64 sTagLogFrame = ~0ull;  // LM_TAG_LOG: the dump frame being logged

u64 rootInst() { return mix(0x7A6500000ull ^ u64(sPass) << 8); }
const TagFrame* topTag() {
    if (sVirtActive) return &sVirt;
    if (sTagDepth > 0 && sTagDepth <= kMaxTagDepth) return &sTagStack[sTagDepth - 1];
    return nullptr;
}
u32 generationOf(u32 obj) {
    if (!obj) return 0;
    if (sGen.size() > (1u << 18)) {  // bounded (only tagged addresses); a reset starts a new epoch
        sGen.clear();
        sGenEpoch += 0x10000;
    }
    return sGen.try_emplace(obj, 0u).first->second + sGenEpoch;
}
// A scope's identity under `parent`.
u64 scopeBase(u64 parent, TagCat cat, u32 obj, u32 type, u32 sub) {
    return mix(parent ^ mix(u64(cat) << 56 ^ u64(obj) << 20 ^ u64(type) * 0x9E3779B97F4A7C15ull) ^
               mix(u64(sub) << 32 ^ generationOf(obj)));
}
u32 occurrence(GenTable<u32>& m, u64 k) {
    if (!sCapturing) return 0;
    return m.entry(k).val++;
}

// The camera the game is drawing with now (see viewMatrixSource), as an index into sViews. A
// view's identity is the structural scope path it is used in (pass, view, sub-view) and the
// order of distinct cameras in that path.
u16 sampleView() {
    const u32 src = viewMatrixSource();
    if (!src) return 0xFFFF;
    M34 m;
    for (int i = 0; i < 12; i++) m[i] = std::bit_cast<f32>(rd32(src + 4 * i));
    const TagFrame* t = topTag();
    const u64 path = mix((t ? t->structural : rootInst()) ^ u64(sPass) << 48 ^ 0xCA3E7Aull);
    for (size_t i = sViews.size(); i-- > 0 && i + 16 >= sViews.size();)
        if (sViews[i] == m && sViewKeys[i] >> 8 == path >> 8) return u16(i);
    if (sViews.size() >= 0xFFF0) return 0xFFFF;
    sViews.push_back(m);
    sViewKeys.push_back((path & ~0xFFull) | (occurrence(sValOcc, path) & 0xFF));
    return u16(sViews.size() - 1);
}
std::vector<std::pair<u32, u32>> sLights;  // extra-light commands of the tape being captured
std::vector<u64> sLightKeys;               // per extra light (in command order): its identity
bool sOverflowLogged = false;
u32 sDumpAt = ~0u, sDumpFrame = 0;
const void* sDumpPtr = nullptr;
std::shared_ptr<Tape> sLastTape;  // the previous tape, for pairing
std::shared_ptr<Tape> sLastTape2;  // and the one before it (judder detector's identity check)

// LM_INTERP_TEST=<steps> (with LM_FRAME_DUMP): deterministic interpolation test. The guest
// waits after each frame N while the host renders, with guest memory as it is at the end of
// N: tape N as is (fN_fresh, the 30 Hz frame), then frames N-2 -> N-1 at alpha s/steps
// (fN-1_s<s>; s = steps is tape N-1 replayed one game frame late, as the real replay is).
const int sTestSteps = [] {
    const char* v = std::getenv("LM_INTERP_TEST");
    return v ? std::max(1, std::atoi(v)) : 0;
}();
std::mutex sTestM;
std::condition_variable sTestCv;
bool sTestPending = false;  // the guest waits for the host to finish its tape
std::atomic<int64_t> sTestHostSeen{0};  // steady clock of the host's last test replay

// --- shared ---
std::mutex sM;
std::shared_ptr<const Tape> sNewest, sPrevious;
// Tapes the FIFO may still be reading (their vertex-array copies) after being replaced.
std::deque<std::shared_ptr<const Tape>> sRetired;
// Every closed tape in capture order (the newest 8): the host picks the pair its presentation
// clock falls between (Pacer), which is not always the newest two.
std::deque<std::shared_ptr<const Tape>> sQueue;

// --- host side ---
std::vector<u8> sReplay;
std::vector<s32> sViewToA;  // per replay: view of b -> view of a (by key)
u32 sPresented = 0, sTapes = 0, sBlended = 0, sSnapped = 0, sCuts = 0, sCamOnly = 0;
Clock::time_point sStatsStart = Clock::now();
u64 sTapeBytes = 0, sArrayBytes = 0, sTexBytes = 0, sPairedSum = 0, sUnpairedSum = 0, sUntaggedSum = 0, sGuestUs = 0,
    sReplayUs = 0;
u64 sPhaseUs[5] = {};
// LM_INTERP_TRACE=1: per present, alpha, time since the last present and what
// could not be blended (by kind).
const bool sTrace = std::getenv("LM_INTERP_TRACE") != nullptr;
// LM_INTERP_JUDDER=<file>: pacing log (interp.h hostMark, tools/interp/judder.py).
FILE* const sJudder = [] {
    const char* p = std::getenv("LM_INTERP_JUDDER");
    return p ? std::fopen(p, "w") : nullptr;
}();
std::mutex sJudderM;
const Clock::time_point sJudderT0 = Clock::now();
double judderUs(Clock::time_point t) { return std::chrono::duration<double, std::micro>(t - sJudderT0).count(); }
void judderLine(const char* fmt, ...) {
    if (!sJudder) return;
    std::lock_guard lk(sJudderM);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(sJudder, fmt, ap);
    va_end(ap);
}
u32 sCrossCamera = 0;
u64 sArraysBlended = 0;  // CPU-deformed position arrays blended (stats window)
u64 sProjShared = 0;  // unpaired projections given the blended values of an identical one (per stats window)
u64 sXlPaired = 0, sXlNew = 0, sXlJumped = 0;  // extra lights per stats window: blended / without partner / identity jumped  // per replay: values whose partner was drawn under another camera (trace)
u32 sSnapKinds[kKinds];
bool sLogSnaps = false;  // LM_INTERP_TEST + trace: log what one test replay snapped
u32 sLoggedSnaps[kKinds];

// LM_TAG_LOG=<file>,<first>[,<last>] (frame numbers as LM_FRAME_DUMP): every value noted in those
// game frames with its key, category, call site, scope depth and, for untagged 3D values, the
// guest call chain (to find draw code that still needs a tag scope).
FILE* sTagLog = nullptr;
u32 sTagLogFirst = ~0u, sTagLogLast = 0;
const bool sTagLogInit = [] {
    const char* v = std::getenv("LM_TAG_LOG");
    if (!v) return false;
    char path[512] = {};
    u32 a = 0, b = 0;
    const int n = std::sscanf(v, "%511[^,],%u,%u", path, &a, &b);
    if (n < 2) return false;
    sTagLog = std::fopen(path, "w");
    sTagLogFirst = a;
    sTagLogLast = n >= 3 ? b : a;
    return sTagLog != nullptr;
}();
std::string sTagChain;  // guest call chain of the current GX call (only while logging)
const bool sTagLogChains = std::getenv("LM_TAG_LOG_CHAINS") != nullptr;  // LM_TAG_LOG: call chains of tagged values too

f32 loadBE(const u8* p) { return std::bit_cast<f32>(u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]); }
void storeBE(u8* p, f32 f) {
    const u32 v = std::bit_cast<u32>(f);
    p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v);
}
f32 val(const Tape& t, const Patch& p, int i) { return loadBE(t.bytes.data() + p.offset + 4 * i); }
// Verts: coordinate c of vertex v.
f32 vtx(const Tape& t, const Patch& p, u32 v, int c) {
    return loadBE(t.bytes.data() + p.offset + v * p.stride + p.posOffset + 4 * c);
}

// --- blending ---------------------------------------------------------------------------

bool rigidView(const f32* v);

bool compatible(const Patch& a, const Patch& b) {
    return a.kind == b.kind && a.count == b.count && a.stride == b.stride && a.posOffset == b.posOffset;
}

// A 3x3 (cols 3) or 3x4 (cols 4, row-major) matrix's 3x3 part as M = Q U: a rotation Q (as
// a quaternion) times an upper-triangular scale/shear U (Gram-Schmidt on the columns; a
// mirrored matrix gets a negative U[2][2]). Model-view and normal matrices blend well this
// way, including the sheared ones a non-uniformly scaled parent produces.
struct Rigid {
    float q[4];     // x, y, z, w
    float u[3][3];  // upper triangular
};

bool decompose(const f32* m, int cols, Rigid& out) {
    float c[3][3];  // columns
    for (int j = 0; j < 3; j++)
        for (int r = 0; r < 3; r++) c[j][r] = m[cols * r + j];
    auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    float e[3][3];  // orthonormal columns
    std::memset(out.u, 0, sizeof(out.u));
    for (int j = 0; j < 3; j++) {
        float v[3] = {c[j][0], c[j][1], c[j][2]};
        for (int i = 0; i < j; i++) {
            const float d = dot(e[i], c[j]);
            out.u[i][j] = d;
            for (int r = 0; r < 3; r++) v[r] -= d * e[i][r];
        }
        const float len = std::sqrt(dot(v, v));
        if (!(len > 1e-7f) || !std::isfinite(len)) return false;
        out.u[j][j] = len;
        for (int r = 0; r < 3; r++) e[j][r] = v[r] / len;
    }
    const float cr[3] = {e[0][1] * e[1][2] - e[0][2] * e[1][1], e[0][2] * e[1][0] - e[0][0] * e[1][2],
                         e[0][0] * e[1][1] - e[0][1] * e[1][0]};
    if (dot(cr, e[2]) < 0) {  // mirrored: keep Q a rotation
        for (int r = 0; r < 3; r++) e[2][r] = -e[2][r];
        out.u[2][2] = -out.u[2][2];
    }
    // Rotation matrix R[r][j] = e[j][r] to quaternion.
    const float m00 = e[0][0], m01 = e[1][0], m02 = e[2][0], m10 = e[0][1], m11 = e[1][1], m12 = e[2][1], m20 = e[0][2],
                m21 = e[1][2], m22 = e[2][2];
    const float tr = m00 + m11 + m22;
    float* q = out.q;
    if (tr > 0) {
        const float k = 0.5f / std::sqrt(tr + 1.0f);
        q[3] = 0.25f / k, q[0] = (m21 - m12) * k, q[1] = (m02 - m20) * k, q[2] = (m10 - m01) * k;
    } else if (m00 > m11 && m00 > m22) {
        const float k = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        q[3] = (m21 - m12) / k, q[0] = 0.25f * k, q[1] = (m01 + m10) / k, q[2] = (m02 + m20) / k;
    } else if (m11 > m22) {
        const float k = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        q[3] = (m02 - m20) / k, q[0] = (m01 + m10) / k, q[1] = 0.25f * k, q[2] = (m12 + m21) / k;
    } else {
        const float k = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        q[3] = (m10 - m01) / k, q[0] = (m02 + m20) / k, q[1] = (m12 + m21) / k, q[2] = 0.25f * k;
    }
    return true;
}

// The translation of a blended 3x4 matrix as a screw motion (docs/INTERPOLATION.md "The hose
// nozzle"). Blending the rotation by slerp and the translation linearly turns the part about its
// matrix ORIGIN, so its in-betweens depend on where that origin happens to be. For a skinned or
// articulated model the origin of a joint's draw matrix is the model's origin carried along by the
// joint (vertices are stored in model space), often 100-1000 units from the vertices it moves: a
// hose segment or hand turning 45 degrees about its joint swung its origin by 50-200 units, and the
// linear origin path pulled the vertices off their own path by about their whole step (the
// Poltergust hose and nozzle "jumping" mid-interval), worse the farther the origin. The motion from
// a to b (ignoring the change of scale/shear) is a rotation by theta about an axis n through some
// point c plus a slide along n; turning the part by t x theta about that same axis (and sliding t of
// the way) moves every point along an arc about the part's real pivot, the same whatever origin
// the matrix has, and a point on the pivot does not move at all. The 3x3 part is unchanged (slerp).
// qa, qb: the two rotations as blendRigid has them (qb on qa's side); a, b, out: 3x4 row-major.
// LM_SCREW_BLEND=0: the old linear origin (A/B).
const bool sScrewBlend = [] {
    const char* v = std::getenv("LM_SCREW_BLEND");
    return !(v && *v == '0');
}();
void screwTranslation(const float* qa, const float* qb, const f32* a, const f32* b, float t, f32* out) {
    // Relative rotation r = qb * conj(qa) (w >= 0: the short way, as the slerp).
    const float x = qb[0], y = qb[1], z = qb[2], w = qb[3];
    const float cx = -qa[0], cy = -qa[1], cz = -qa[2], cw = qa[3];
    float r[4] = {w * cx + x * cw + y * cz - z * cy, w * cy - x * cz + y * cw + z * cx, w * cz + x * cy - y * cx + z * cw,
                  w * cw - x * cx - y * cy - z * cz};
    if (r[3] < 0)
        for (float& v : r) v = -v;
    const float s = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    const float theta = 2.0f * std::atan2(s, r[3]);
    if (!(theta > 0.01f) || !(theta < 3.1f)) return;  // nearly no turn (linear is the limit), or a half turn (axis unsure)
    const float n[3] = {r[0] / s, r[1] / s, r[2] / s};
    // Rotation matrix of a quaternion (unit), applied to v.
    const auto rot = [](const float* q, const float* v, float* o) {
        const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
        const float R[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
                               {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
                               {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
        for (int i = 0; i < 3; i++) o[i] = R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2];
    };
    const float ta[3] = {a[3], a[7], a[11]}, tb[3] = {b[3], b[7], b[11]};
    // d = tb - Rd ta: the motion is x -> Rd x + d.
    float rta[3], d[3];
    rot(r, ta, rta);
    for (int i = 0; i < 3; i++) d[i] = tb[i] - rta[i];
    const float along = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    float dp[3];
    for (int i = 0; i < 3; i++) dp[i] = d[i] - along * n[i];
    // The axis point c: (I - Rd) c = dp, c = dp / 2 + cot(theta / 2) / 2 (n x dp).
    const float k = 0.5f / std::tan(0.5f * theta);
    const float nx[3] = {n[1] * dp[2] - n[2] * dp[1], n[2] * dp[0] - n[0] * dp[2], n[0] * dp[1] - n[1] * dp[0]};
    float c[3], v[3], o[3];
    for (int i = 0; i < 3; i++) c[i] = 0.5f * dp[i] + k * nx[i], v[i] = ta[i] - c[i];
    const float h = 0.5f * t * theta;
    const float qt[4] = {n[0] * std::sin(h), n[1] * std::sin(h), n[2] * std::sin(h), std::cos(h)};
    rot(qt, v, o);
    float res[3];
    for (int i = 0; i < 3; i++) {
        res[i] = o[i] + c[i] + t * along * n[i];
        if (!std::isfinite(res[i])) return;
    }
    for (int i = 0; i < 3; i++) out[4 * i + 3] = res[i];
}

// Blends two matrices by rotation (slerp), scale/shear and translation, so a turning
// object keeps its shape at in-between frames (an entrywise blend shrinks it).
bool blendRigid(const f32* a, const f32* b, int cols, float t, f32* out) {
    Rigid ra, rb;
    if (!decompose(a, cols, ra) || !decompose(b, cols, rb) || (ra.u[2][2] < 0) != (rb.u[2][2] < 0)) return false;
    float d = ra.q[0] * rb.q[0] + ra.q[1] * rb.q[1] + ra.q[2] * rb.q[2] + ra.q[3] * rb.q[3];
    float qb[4] = {rb.q[0], rb.q[1], rb.q[2], rb.q[3]};
    if (d < 0) {
        d = -d;
        for (float& v : qb) v = -v;
    }
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        const float th = std::acos(std::min(d, 1.0f)), sn = std::sin(th);
        wa = std::sin((1.0f - t) * th) / sn;
        wb = std::sin(t * th) / sn;
    }
    float q[4];
    float len = 0;
    for (int k = 0; k < 4; k++) q[k] = wa * ra.q[k] + wb * qb[k], len += q[k] * q[k];
    len = std::sqrt(len);
    for (float& v : q) v /= len;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    float U[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) U[i][j] = ra.u[i][j] + (rb.u[i][j] - ra.u[i][j]) * t;
    for (int r = 0; r < 3; r++)
        for (int j = 0; j < 3; j++) out[cols * r + j] = R[r][0] * U[0][j] + R[r][1] * U[1][j] + R[r][2] * U[2][j];
    if (cols == 4) {
        for (int r = 0; r < 3; r++) out[4 * r + 3] = a[4 * r + 3] + (b[4 * r + 3] - a[4 * r + 3]) * t;
        if (sScrewBlend) screwTranslation(ra.q, qb, a, b, t, out);
    }
    return true;
}

// A 2x4 texture matrix that turns texture coordinates in their plane (a spinning
// particle sprite): [c -s 0 tx; s c 0 ty] times a uniform scale.
struct Spin {
    float angle, scale;
};
bool spin2D(const f32* m, Spin& out) {
    const float sc = std::sqrt(m[0] * m[0] + m[4] * m[4]);
    if (!(sc > 1e-6f) || std::fabs(m[2]) > 1e-4f || std::fabs(m[6]) > 1e-4f) return false;
    if (std::fabs(m[0] - m[5]) > 0.02f * sc || std::fabs(m[1] + m[4]) > 0.02f * sc) return false;
    out = {std::atan2(m[4], m[0]), sc};
    return true;
}
float angleDelta(float from, float to) {
    float d = std::remainder(to - from, 6.2831853f);
    return d;
}

// Blends two spinning texture matrices by angle and scale about their common pivot (the
// sprite's centre), so the texture turns instead of shrinking through an entrywise blend.
bool blendSpin(const f32* a, const f32* b, float t, f32* out) {
    Spin sa, sb;
    if (!spin2D(a, sa) || !spin2D(b, sb)) return false;
    const float d = angleDelta(sa.angle, sb.angle);
    if (std::fabs(d) > 2.6f || sb.scale > 2.0f * sa.scale || sa.scale > 2.0f * sb.scale) return false;
    // Pivot c with t = c - M c, i.e. (I - M) c = t; from whichever matrix turns more.
    auto pivot = [](const f32* m, float& cx, float& cy) {
        const float a00 = 1 - m[0], a01 = -m[1], a10 = -m[4], a11 = 1 - m[5];
        const float det = a00 * a11 - a01 * a10;
        if (std::fabs(det) < 1e-3f) return false;
        cx = (m[3] * a11 - a01 * m[7]) / det;
        cy = (a00 * m[7] - a10 * m[3]) / det;
        return true;
    };
    float cx, cy, cx2, cy2;
    const bool pa = pivot(a, cx, cy), pb = pivot(b, cx2, cy2);
    const float ang = sa.angle + d * t, sc = sa.scale + (sb.scale - sa.scale) * t;
    const float c = std::cos(ang) * sc, s = std::sin(ang) * sc;
    out[0] = c, out[1] = -s, out[2] = 0, out[4] = s, out[5] = c, out[6] = 0;
    if (pa && pb && std::fabs(cx - cx2) < 0.05f && std::fabs(cy - cy2) < 0.05f) {
        out[3] = cx - (c * cx - s * cy);
        out[7] = cy - (s * cx + c * cy);
    } else {
        out[3] = a[3] + (b[3] - a[3]) * t;
        out[7] = a[7] + (b[7] - a[7]) * t;
    }
    return true;
}

bool blendable(Kind kind, const f32* a, const f32* b, int n, float prevTurn = -1.0f);

// Camera position of a rigid view matrix (eye = -R^T t) and the view direction (row 2).
void eyeOf(const M34& v, float e[3]) {
    for (int c = 0; c < 3; c++) e[c] = -(v[c] * v[3] + v[4 + c] * v[7] + v[8 + c] * v[11]);
}
// A camera cut: the eye jumps by more than 250 units or the camera turns by more than 30 degrees
// within one game frame.
bool cameraJump(const M34& va, const M34& vb, float* moved = nullptr, float* turned = nullptr) {
    float ea[3], eb[3], dot = 0, de = 0;
    eyeOf(va, ea);
    eyeOf(vb, eb);
    for (int c = 0; c < 3; c++) dot += va[8 + c] * vb[8 + c], de += (ea[c] - eb[c]) * (ea[c] - eb[c]);
    if (moved) *moved = std::sqrt(de);
    if (turned) *turned = std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.2958f;
    return !(std::sqrt(de) <= 250.0f) || !(dot >= 0.866f);
}

// Texture scrolls, whatever their scale (b.texMode / texShift). A texture matrix whose
// scale/rotation part stays the same is a scroll; each row's offset step is compared with the step
// it took the game frame before (texStep: the step as blended, NaN unknown):
// - about the same step plus a whole number k of textures: the scroll wrapped (texture coordinates
//   repeat), so it blends by the step less k (mode 1, texShift = k);
// - anything else beyond half a texture: the game placed the texture anew (J3D materials in the
//   mansion jump their offset by 2-13 textures every ~2 s), shown as b (mode 2).
// Before, only matrices with every entry within +-4 were taken as wraps (by the short way modulo 1,
// still the rule without history, `blendTexWrap`); larger ones (a texture repeated more than 4
// times: the boss skies' layers, also caught here now, and those J3D materials) were blended
// entrywise and swept through whole textures within one game frame. Projection-style matrices
// (flashlight, shadows) change their rotation part as the camera turns and are left alone.
// LM_TEXWRAP_LOG=<file>: every scroll pair with a step over half a texture, and the decision.
void texWrapPairs(const Tape& a, Tape& b) {
    static FILE* sLog = [] {
        const char* p = std::getenv("LM_TEXWRAP_LOG");
        return p && *p ? std::fopen(p, "w") : nullptr;
    }();
    const f32 kNan = std::nanf("");
    b.texStep.assign(b.patches.size(), {kNan, kNan, kNan});
    b.texShift.assign(b.patches.size(), {0, 0, 0});
    b.texMode.assign(b.patches.size(), 0);
    const bool aSteps = a.texStep.size() == a.patches.size();
    for (u32 i = 0; i < b.patches.size(); i++) {
        const Patch& p = b.patches[i];
        const s32 j = b.partner[i];
        // (Texture matrix ids start at GX_TEXMTX0 = 30; below are position matrices noted as TexMtx
        // for element-wise blending, sLinearPos.)
        if (j < 0 || p.kind != Kind::TexMtx || p.role != 0 || (p.count != 8 && p.count != 12) || p.slot < 30) continue;
        const Patch& q = a.patches[j];
        bool steady = true, jumped = false;
        float big = 0, d[3] = {};
        for (int k = 0; k < p.count; k++) {
            const float x = val(a, q, k), y = val(b, p, k);
            if (!(std::isfinite(x) && std::isfinite(y))) {
                steady = false;
                break;
            }
            big = std::max({big, std::fabs(x), std::fabs(y)});
            if ((k & 3) == 3) {
                d[k >> 2] = y - x;
                if (std::fabs(d[k >> 2]) > 0.5f) jumped = true;
            } else if (std::fabs(x - y) > 0.02f * std::max({1.0f, std::fabs(x), std::fabs(y)})) {
                steady = false;
            }
        }
        if (!steady) continue;
        const int rows = p.count / 4;
        float prev[3] = {kNan, kNan, kNan}, shift[3] = {0, 0, 0};
        bool known = aSteps, placed = false;
        for (int r = 0; r < rows && known; r++) {
            prev[r] = a.texStep[j][r];
            known = std::isfinite(prev[r]);
        }
        if (known) {
            for (int r = 0; r < rows; r++) {
                const float k = std::round(d[r] - prev[r]);
                if (k == 0.0f) continue;  // within half a texture of the last step: moving on
                if (std::fabs(k) <= 16.0f && std::fabs(d[r] - k - prev[r]) <= 0.05f + 0.25f * std::fabs(prev[r])) shift[r] = k;
                else placed = true;
            }
            b.texMode[i] = placed ? 2 : 1;
        } else if (jumped && big > 4.0f && !p.sky) {
            b.texMode[i] = 2;  // no history: a jump through several textures is not blended
        }
        for (int r = 0; r < rows; r++) {
            b.texShift[i][r] = shift[r];
            if (b.texMode[i] == 1) b.texStep[i][r] = d[r] - shift[r];
            else if (b.texMode[i] == 0) b.texStep[i][r] = jumped && (big <= 4.0f || p.sky) ? d[r] - std::round(d[r]) : d[r];
        }
        if (sLog && jumped)
            std::fprintf(sLog, "%llu f%u site %08X key %016llX cat %u n %u sky %d big %.2f d %.3f %.3f %.3f prev %.4f %.4f %.4f shift %.0f %.0f %.0f mode %d\n",
                         (unsigned long long)b.seq, b.gameFrame, p.site, (unsigned long long)p.key, u32(p.cat), u32(p.count),
                         int(p.sky), big, d[0], d[1], rows > 2 ? d[2] : 0.0f, prev[0], prev[1], prev[2], shift[0], shift[1],
                         shift[2], int(b.texMode[i]));
        if (sLog && jumped && std::getenv("LM_TEXWRAP_VALUES")) {
            std::fprintf(sLog, "  a #%d:", j);
            for (int k = 0; k < p.count; k++) std::fprintf(sLog, " %.3f", val(a, q, k));
            std::fprintf(sLog, "\n  b #%u:", i);
            for (int k = 0; k < p.count; k++) std::fprintf(sLog, " %.3f", val(b, p, k));
            std::fprintf(sLog, "\n");
        }
    }
    if (sLog) std::fflush(sLog);
}

// Pairs tape b with its predecessor a strictly by identity: a value of b pairs with the value of a
// that has the same key (same scope path, call site and occurrence) and the same shape. Nothing
// else is compared: no nearest match, no ordering, no layout grouping. Cameras pair the same way.
void pairTapes(const Tape& a, Tape& b) {
    auto tp = Clock::now();
    b.pairedWith = &a;
    const u32 nPatches = u32(b.patches.size());
    b.partner.assign(nPatches, -1);
    setPooled(b.worldStep, nPatches);
    setPooled(b.worldTurn, nPatches);
    setPooled(b.worldAxis, 3 * nPatches);
    setPooled(b.worldTumble, nPatches);
    setPooled(b.worldErratic, nPatches);
    setPooled(b.groupSnap, nPatches);
    setPooled(b.vertStep, 3 * nPatches);
    const float kUnknownStep = std::nanf("");  // once: nanf parses its tag on every call
    for (u32 i = 0; i < 3 * nPatches; i++) b.vertStep[i].store(kUnknownStep, std::memory_order_relaxed);
    for (u32 i = 0; i < nPatches; i++) {
        b.worldStep[i].store(-1.0f, std::memory_order_relaxed);
        b.worldTurn[i].store(-1.0f, std::memory_order_relaxed);
        b.worldTumble[i].store(0, std::memory_order_relaxed);
        b.worldErratic[i].store(0, std::memory_order_relaxed);
        for (int c = 0; c < 3; c++) b.worldAxis[3 * i + c].store(0.0f, std::memory_order_relaxed);
        b.groupSnap[i].store(0, std::memory_order_relaxed);
    }
    // The pairing tables below outlive the tape: a key seen only in an older game frame reads as
    // absent (GenTable), so none of them is cleared and rehashed per game frame.
    static GenTable<s32> byKey;
    byKey.nextFrame(4 * a.patches.size() + 4096);
    byKey.reserve(a.patches.size() * 2);
    for (u32 i = 0; i < a.patches.size(); i++)
        if (const u64 k = a.patches[i].key) {
            auto [v, fresh] = byKey.entry(k);
            v = fresh ? s32(i) : -2;  // the same identity twice: ambiguous, never paired
        }
    static GenTable<u8> seenB;
    seenB.nextFrame(4 * b.patches.size() + 4096);
    b.paired = b.unpaired = b.untagged = 0;
    for (u32 i = 0; i < b.patches.size(); i++) {
        const Patch& p = b.patches[i];
        if (!p.key) {
            b.untagged++;
            continue;
        }
        if (!seenB.entry(p.key).fresh) {  // ambiguous in b too
            b.unpaired++;
            continue;
        }
        const s32* j = byKey.find(p.key);
        if (!j || *j < 0 || !compatible(a.patches[*j], p)) {
            b.unpaired++;
            continue;
        }
        b.partner[i] = *j;
        b.paired++;
    }
    texWrapPairs(a, b);
    {
        static GenTable<u32> perGroup;
        perGroup.nextFrame(4 * b.patches.size() + 4096);
        for (const Patch& p : b.patches)
            if (p.group && p.kind == Kind::PosMtx && p.count == 12 && p.role == 0) perGroup.entry(p.group).val++;
        b.multiMember.assign(b.patches.size(), 0);
        for (u32 i = 0; i < b.patches.size(); i++) {
            const Patch& p = b.patches[i];
            if (!p.group || p.tumble || p.kind != Kind::PosMtx || p.count != 12 || p.role != 0) continue;
            const u32* members = perGroup.find(p.group);
            if (members && *members >= 2) b.multiMember[i] = 1;
        }
    }
    // Cameras by key.
    b.viewToA.assign(b.views.size(), -1);
    {
        static GenTable<s32> va;
        va.nextFrame(4 * a.viewKeys.size() + 4096);
        for (u32 i = 0; i < a.viewKeys.size(); i++) {
            auto [v, fresh] = va.entry(a.viewKeys[i]);
            v = fresh ? s32(i) : -2;
        }
        for (u32 i = 0; i < b.viewKeys.size(); i++)
            if (const s32* v = va.find(b.viewKeys[i]); v && *v >= 0) b.viewToA[i] = *v;
    }
    // Tagged position arrays (CPU-deformed geometry): the same identity, size, stride and format.
    b.arrayPartner.assign(b.arrayRecs.size(), -1);
    {
        static GenTable<s32> ra;
        ra.nextFrame(4 * a.arrayRecs.size() + 4096);
        for (u32 i = 0; i < a.arrayRecs.size(); i++) {
            auto [v, fresh] = ra.entry(a.arrayRecs[i].key);
            v = fresh ? s32(i) : -2;
        }
        for (u32 i = 0; i < b.arrayRecs.size(); i++) {
            const detail::ArrayRec& r = b.arrayRecs[i];
            const s32* v = ra.find(r.key);
            if (!v || *v < 0) continue;
            const detail::ArrayRec& q = a.arrayRecs[*v];
            if (q.stride != r.stride || q.cnt != r.cnt || q.type != r.type || q.frac != r.frac ||
                a.arrays[q.snap].size() != b.arrays[r.snap].size())
                continue;
            b.arrayPartner[i] = *v;
        }
        // LM_ARRAY_LOG=<file> (test aid): every tagged position array of each tape, its partner and how
        // far its positions moved (max over coordinates).
        static FILE* sArrLog = [] {
            const char* p = std::getenv("LM_ARRAY_LOG");
            return p && *p ? std::fopen(p, "w") : nullptr;
        }();
        if (sArrLog) {
            for (u32 i = 0; i < b.arrayRecs.size(); i++) {
                const detail::ArrayRec& r = b.arrayRecs[i];
                const s32 j = b.arrayPartner[i];
                float jump = -1;
                static constexpr u32 kSz[] = {1, 1, 2, 2, 4};
                const u32 cs = r.type <= GX_F32 ? kSz[r.type] : 0;
                if (j >= 0 && cs && r.stride >= 3 * cs && a.arrays[a.arrayRecs[j].snap].size() == b.arrays[r.snap].size()) {
                    const std::vector<u8>& da = a.arrays[a.arrayRecs[j].snap];
                    const std::vector<u8>& db = b.arrays[r.snap];
                    const float sc = r.type == GX_F32 ? 1.0f : float(1u << r.frac);
                    const auto rd = [&](const u8* p) -> float {
                        u32 v = 0;
                        for (u32 k = 0; k < cs; k++) v = v << 8 | p[k];
                        switch (r.type) {
                        case GX_F32: return std::bit_cast<f32>(v);
                        case GX_S16: return float(s16(v)) / sc;
                        case GX_U16: return float(u16(v)) / sc;
                        case GX_S8: return float(s8(v)) / sc;
                        default: return float(u8(v)) / sc;
                        }
                    };
                    jump = 0;
                    for (u32 o = 0; o + 3 * cs <= db.size(); o += r.stride)
                        for (u32 c = 0; c < 3; c++) jump = std::max(jump, std::fabs(rd(da.data() + o + c * cs) - rd(db.data() + o + c * cs)));
                }
                std::fprintf(sArrLog, "%llu %u key %016llX bytes %u stride %u type %u partner %d jump %.2f\n", (unsigned long long)b.seq, i,
                             (unsigned long long)r.key, u32(b.arrays[r.snap].size()), r.stride, r.type, j, jump);
            }
            std::fprintf(sArrLog, "%llu end %u records\n", (unsigned long long)b.seq, u32(b.arrayRecs.size()));
        }
    }
    b.phaseUs[2] = usSince(tp);
    // The main camera: the view most 3D model matrices of the main pass were drawn under.
    {
        static std::vector<u32> n;
        n.assign(b.views.size(), 0);
        for (const Patch& p : b.patches)
            if (p.kind == Kind::PosMtx && p.role == 0 && p.view < n.size() &&
                (p.bundle >= b.bundlePass.size() || b.bundlePass[p.bundle] == 0))
                n[p.view]++;
        u32 best = 0;
        for (u32 v = 0; v < n.size(); v++)
            if (n[v] > best) best = n[v], b.mainView = u16(v);
        if (best < 8) b.mainView = 0xFFFF;
    }
    // A camera cut (or a new scene): the main camera jumped, or it has no counterpart in a.
    b.cut = false;
    if (b.mainView != 0xFFFF) {
        const s32 qa = b.viewToA[b.mainView];
        float moved = 0, turned = 0;
        // b's main camera paired with a camera that was not a's main one: the view structure changed
        // (a camera appeared in the path and the main one moved to another index; a teleport into the
        // basement paired it with a minor view and blended across rooms, sweep A).
        // A cut when b's main camera is also far from a's main camera (the main view index can change
        // between game frames without the camera changing).
        bool otherMain = false;
        if (qa >= 0 && a.mainView < a.views.size() && qa != s32(a.mainView)) {
            float m2 = 0, t2 = 0;
            cameraJump(a.views[a.mainView], b.views[b.mainView], &m2, &t2);
            otherMain = m2 > 250.0f || t2 > 30.0f;
        }
        // The view structure changed under the main camera (a view appeared or vanished and the main
        // view moved to another index): the other views' order-based keys paired the wrong cameras
        // and the world went through a wrong in-between camera for one interval (the chimney drop
        // into the Sealed Room, tape 819: views 5 -> 4, main 3 -> 2; the new-game foyer transition;
        // sweep B). Shown as captured.
        const bool restructured = a.views.size() != b.views.size() && a.mainView != 0xFFFF && b.mainView != a.mainView;
        if (qa < 0 || otherMain || restructured) b.cut = true;
        else if (rigidView(a.views[qa].data()) && rigidView(b.views[b.mainView].data())) {
            cameraJump(a.views[qa], b.views[b.mainView], &moved, &turned);
            b.camMove = moved, b.camTurn = turned;
            // A cut is a discontinuity: far beyond what the camera did the frame before. A fast camera
            // (a boss fight's, Bogmire's graveyard) moves or turns a lot every frame and is not a cut:
            // treating it as one showed whole game frames without interpolation (the background
            // hitched).
            // (Over 1500 units in one game frame is always a cut: a teleport whose camera first moved a
            // few hundred units passed the 3x test.)
            b.cut = (moved > 250.0f && moved > 3.0f * a.camMove + 60.0f) || moved > 1500.0f ||
                    (turned > 30.0f && turned > 3.0f * a.camTurn + 8.0f);
        }
        if (sTrace && (a.views.size() != b.views.size() || qa != s32(a.mainView)))
            log("[interp] tape %llu: views %zu -> %zu, main %d -> %u (paired with %d), moved %.0f turned %.0f, cut %d",
                (unsigned long long)b.seq, a.views.size(), b.views.size(), a.mainView == 0xFFFF ? -1 : int(a.mainView),
                b.mainView, qa, moved, turned, int(b.cut));
        if (sTrace && b.cut)
            log("[interp] tape %llu: camera cut (%s; eye moved %.0f, turned %.0f degrees)", (unsigned long long)b.seq,
                qa < 0 ? "no main camera before" : "jump", moved, turned);
    }
    b.phaseUs[4] = usSince(tp);
}

// Whether a and b are close enough to blend; a jump (teleport, texture scroll wrapping,
// a spawn reusing a slot) shows b as is.
// A model matrix pair in WORLD space (wa, wb: the matrices with their cameras taken out): blendable
// unless it turns by more than ~150 degrees, changes scale 16x or jumps by more than half its
// distance from the camera (`dist`; at least 200 units). In view space (blendable) a fast camera
// move alone made static things nearby "jump" (the opening's crane shots: snapped every frame).
// A scrolling texture whose offset wrapped (1.0 back to 0.0, or a scroll faster than half the texture
// per game frame): texture coordinates repeat, so the offset is blended by the short way modulo 1
// (it used to be shown unblended for the whole game frame: animated skies and water stepped). Only
// texture-coordinate matrices (entries within +-4) whose scale/rotation part barely changed.
// `sky`: the sky dome's texture SRT animations (fn_8002FA34). Their layers repeat the texture 2-10
// times across the dome (scale entries 6, -8, -10 on vrball_B), outside the +-4 bound, and their
// offsets wrap by exactly 1 about every 60 game frames: blended entrywise, a layer swept back
// through the whole texture within one game frame (the boss arena skies' jumps); any matrix of the
// sky is a texture-coordinate matrix, so only its scale/rotation part must barely change.
bool blendTexWrap(const f32* a, const f32* b, int n, float t, f32* out, bool sky = false) {
    if (n != 8 && n != 12) return false;
    bool wrapped = false;
    for (int i = 0; i < n; i++) {
        if (!(std::isfinite(a[i]) && std::isfinite(b[i]))) return false;
        if (!sky && !(std::fabs(a[i]) <= 4.0f && std::fabs(b[i]) <= 4.0f)) return false;
        const bool trans = (i & 3) == 3;
        if (!trans && std::fabs(a[i] - b[i]) > 0.35f) return false;
        if (trans && std::fabs(a[i] - b[i]) > 0.5f) wrapped = true;
    }
    if (!wrapped) return false;
    for (int i = 0; i < n; i++) {
        float d = b[i] - a[i];
        if ((i & 3) == 3) d -= std::round(d);
        out[i] = b[i] - d * (1.0f - t);  // exact b at t = 1
    }
    return true;
}

// A matrix whose 3x3 part is nearly flat (squashed to almost no volume: its columns nearly
// coplanar): its rotation and mirroring are meaningless (the Gram-Schmidt frame of a squashed axis
// flips from one game frame to the next). Chauncey's squash-and-stretch bones.
bool flatMatrix(const f32* m) {
    float len[3];
    for (int j = 0; j < 3; j++) len[j] = std::sqrt(m[j] * m[j] + m[4 + j] * m[4 + j] + m[8 + j] * m[8 + j]);
    const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    const float vol = len[0] * len[1] * len[2];
    return vol > 0 && std::isfinite(det) && std::fabs(det) < 0.1f * vol;
}

// A pair that turns mostly about one of its own axes by more than 75 degrees in one game frame (a
// twist, while that axis itself turns by less than 35 degrees): the Poltergust hose's segments are
// rebuilt from the hose direction every frame and their roll about the tube changes by up to ~150
// degrees from one game frame to the next, so blending spun every rib around the hose (and the
// slerp pulled the tube off its line): the hose looked jumpy at 120 and right at 30. The twist is
// taken from b at once (a is turned about that axis by it) and only the rest of the turn blends;
// a ring looks the same either way, and a real spin that fast cannot be followed at 30 Hz anyway.
// `wa` is changed in place; returns whether it was.
bool invertAffine(const f32* m, f32* out);

bool untwist(f32* wa, const f32* wb, int cols = 4) {
    // Not with the screw blend (blendRigid): the twist was taken about the matrix's local axis
    // through its ORIGIN, which for the hose's joint matrices (origin at the model's origin, 20-160
    // units away) threw the hose rings 15-145 units off their place for a whole interval at every
    // fast roll (LM_TRACK_LOG: hose and nozzle tip jumping while the game frames barely moved them).
    // The screw blend turns a twisting part about its own axis, and "the slerp pulled the tube off
    // its line" that this rule was made for was the linear origin path the screw blend replaced.
    if (sScrewBlend) return false;
    // The turn from a to b in a's local frame: Ma^-1 Mb (3x3 parts).
    f32 a34[12] = {}, ia[12];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) a34[4 * r + c] = wa[cols * r + c];
    if (!invertAffine(a34, ia)) return false;
    f32 rel[9];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) rel[3 * r + c] = ia[4 * r] * wb[c] + ia[4 * r + 1] * wb[cols + c] + ia[4 * r + 2] * wb[2 * cols + c];
    Rigid q;
    if (!decompose(rel, 3, q) || q.u[2][2] < 0) return false;
    float w = q.q[3], v[3] = {q.q[0], q.q[1], q.q[2]};
    if (w < 0) w = -w, v[0] = -v[0], v[1] = -v[1], v[2] = -v[2];
    const float total = 2.0f * std::acos(std::min(w, 1.0f)) * 57.2958f;
    if (total < 75.0f) return false;
    int k = 0;
    for (int i = 1; i < 3; i++)
        if (std::fabs(v[i]) > std::fabs(v[k])) k = i;
    const float phi = 2.0f * std::atan2(v[k], w);  // the twist about local axis k (radians)
    if (std::fabs(phi) * 57.2958f < 75.0f) return false;
    // The swing left after the twist: q * twist^-1; its angle from the scalar part.
    const float hc = std::cos(0.5f * phi), hs = std::sin(0.5f * phi);
    const float swingW = w * hc + v[k] * hs;  // scalar part of q * conj(twist)
    if (2.0f * std::acos(std::min(std::fabs(swingW), 1.0f)) * 57.2958f > 35.0f) return false;
    // a turned about its own axis k by phi: Ma * T (T rotates the local frame about e_k). Only when it
    // is scaled alike across that axis (a twist mixes those two axes: a flattened part would stretch).
    const int i1 = (k + 1) % 3, i2 = (k + 2) % 3;
    float l1 = 0, l2 = 0;
    for (int r = 0; r < 3; r++) l1 += wa[cols * r + i1] * wa[cols * r + i1], l2 += wa[cols * r + i2] * wa[cols * r + i2];
    l1 = std::sqrt(l1), l2 = std::sqrt(l2);
    if (!(std::fabs(l1 - l2) <= 0.1f * std::max(l1, l2))) return false;
    const float c = std::cos(phi), s = std::sin(phi);
    for (int r = 0; r < 3; r++) {
        const float x = wa[cols * r + i1], y = wa[cols * r + i2];
        wa[cols * r + i1] = c * x + s * y;
        wa[cols * r + i2] = -s * x + c * y;
    }
    return true;
}

// `prevStep`: how far the same identity moved over the game frame before (-1: unknown); a pair that
// moves at most about as far again is fast motion (Chauncey's swoops, a boss charging), not a jump.
// prevTurn (noteTurn) above this: the identity spins or tumbles, so a turn over ~150 degrees in one
// game frame blends the short way instead of switching.
constexpr float kSpinnerTurn = 60.0f;

const bool sSkewFlipSwitch = std::getenv("LM_SKEW_FLIP_SWITCH") != nullptr;  // test aid: sheared mirror flips switch (old)
// A weighted (envelope) matrix whose columns are far from orthogonal and whose handedness flips
// between game frames passed through a singular blend of its joints (Chauncey's squash-and-stretch
// body at the start of a spin: determinant -4.9 -> +8.9 with 40-60 degree "turns"). Its rotation and
// mirroring mean nothing: like a flat one, it blends entrywise (each skinned vertex in a straight
// line) when its axes' lengths and place allow, with no flip or twist resolution (b turned 180
// degrees about an axis is other geometry when sheared: the in-betweens converged to it and the part
// snapped at the game frame). Shown as b, the whole body held for one interval at every spin start
// (sweep C). Orthogonal (rigid) mirrors still switch.
bool shearedMatrix(const f32* m, int cols = 4) {
    float c[3][3], l[3];
    for (int k = 0; k < 3; k++) {
        for (int r = 0; r < 3; r++) c[k][r] = m[cols * r + k];
        l[k] = std::sqrt(c[k][0] * c[k][0] + c[k][1] * c[k][1] + c[k][2] * c[k][2]);
    }
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (std::fabs(c[i][0] * c[j][0] + c[i][1] * c[j][1] + c[i][2] * c[j][2]) > 0.25f * l[i] * l[j]) return true;
    return false;
}
float det3(const f32* m, int cols = 4) {
    const f32 *r0 = m, *r1 = m + cols, *r2 = m + 2 * cols;
    return r0[0] * (r1[1] * r2[2] - r1[2] * r2[1]) - r0[1] * (r1[0] * r2[2] - r1[2] * r2[0]) + r0[2] * (r1[0] * r2[1] - r1[1] * r2[0]);
}
bool skewFlip(const f32* wa, const f32* wb, int cols = 4) {
    return !sSkewFlipSwitch && (det3(wa, cols) < 0) != (det3(wb, cols) < 0) && (shearedMatrix(wa, cols) || shearedMatrix(wb, cols));
}
// `anyScale`: no limit on scale changes (an element stream's drop, tumbler: splashes grow from almost
// nothing within a game frame, 5-8x along one axis; a 16x limit on a flat drop switched them).
// How far a matrix's geometry moved: its draw's centre (model space, `centre`) carried by both
// matrices, or (unknown centre) its origin. A joint matrix of a skinned or articulated model has
// its origin at the model's origin carried along by the joint, often hundreds of units from the
// vertices it moves: the Poltergust's suction swirl (a J3D model of ~10 joints spinning 60-150
// degrees per game frame about the nozzle) moved its origins by 300-1100 units while the swirl
// itself moved 50-350, and its members were switched one by one as "jumps" (torn for an interval).
// LM_SCREW_BLEND=0: origins (old).
float placeStep(const f32* wa, const f32* wb, const f32* centre) {
    if (!centre || !sScrewBlend)
        return std::hypot(wa[3] - wb[3], wa[7] - wb[7], wa[11] - wb[11]);
    f32 pa[3], pb[3];
    for (int r = 0; r < 3; r++) {
        pa[r] = wa[4 * r] * centre[0] + wa[4 * r + 1] * centre[1] + wa[4 * r + 2] * centre[2] + wa[4 * r + 3];
        pb[r] = wb[4 * r] * centre[0] + wb[4 * r + 1] * centre[1] + wb[4 * r + 2] * centre[2] + wb[4 * r + 3];
    }
    const float d = std::hypot(pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2]);
    return std::isfinite(d) ? d : std::hypot(wa[3] - wb[3], wa[7] - wb[7], wa[11] - wb[11]);
}
// Where the geometry of position matrix patch p is (model space; nullptr: unknown): the centre of
// the vertices of its draw that use its slot, else of the whole draw.
const f32* drawCentre(const Tape& t, const Patch& p) {
    if (p.kind == Kind::PosMtx && p.slot % 3 == 0 && p.slot / 3 < 10 && !t.slotCentres.empty()) {
        const auto it = std::lower_bound(t.slotCentres.begin(), t.slotCentres.end(), p.bundle,
                                         [](const SlotCentres& s, u32 b) { return s.bundle < b; });
        if (it != t.slotCentres.end() && it->bundle == p.bundle && (it->mask & (1u << (p.slot / 3)))) return it->c[p.slot / 3];
    }
    return p.bundle < t.bundleCentre.size() && std::isfinite(t.bundleCentre[p.bundle][0]) ? t.bundleCentre[p.bundle].data() : nullptr;
}
// Distance from the camera of view-space matrix v's geometry (centre as placeStep).
float viewDist(const f32* v, const f32* centre) {
    if (centre && sScrewBlend) {
        f32 p[3];
        for (int r = 0; r < 3; r++) p[r] = v[4 * r] * centre[0] + v[4 * r + 1] * centre[1] + v[4 * r + 2] * centre[2] + v[4 * r + 3];
        const float d = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (std::isfinite(d)) return d;
    }
    return std::sqrt(v[3] * v[3] + v[7] * v[7] + v[11] * v[11]);
}
bool blendableWorld(const f32* wa, const f32* wb, float dist, float prevStep = -1.0f, float prevTurn = -1.0f, bool anyScale = false,
                    const f32* centre = nullptr) {
    Rigid ra, rb;
    if (!decompose(wa, 4, ra) || !decompose(wb, 4, rb)) {
        for (int i = 0; i < 12; i++)
            if (std::fabs(wa[i] - wb[i]) > ((i & 3) == 3 ? 400.0f : anyScale ? 1e30f : 0.35f)) return false;
        return true;
    }
    // (A tumbler whose random orientation came out mirrored in one game frame only: entrywise, like a
    // flat matrix; ceiling splashes switched.)
    if (flatMatrix(wa) || flatMatrix(wb) || skewFlip(wa, wb) || (anyScale && (ra.u[2][2] < 0) != (rb.u[2][2] < 0))) {
        // Squashed flat: only its axes' lengths (appearing/vanishing by scale) and place count.
        for (int k = 0; k < 3; k++) {
            const float sa = std::sqrt(wa[k] * wa[k] + wa[4 + k] * wa[4 + k] + wa[8 + k] * wa[8 + k]);
            const float sb = std::sqrt(wb[k] * wb[k] + wb[4 + k] * wb[4 + k] + wb[8 + k] * wb[8 + k]);
            if (!anyScale && std::max(sa, sb) > 0.05f && (sb > 16.0f * sa + 1e-4f || sa > 16.0f * sb + 1e-4f)) return false;
        }
    } else {
        if ((ra.u[2][2] < 0) != (rb.u[2][2] < 0)) return false;
        if (!(prevTurn > kSpinnerTurn) &&
            std::fabs(ra.q[0] * rb.q[0] + ra.q[1] * rb.q[1] + ra.q[2] * rb.q[2] + ra.q[3] * rb.q[3]) < 0.26f)
            return false;
        for (int k = 0; k < 3; k++) {
            const float sa = std::fabs(ra.u[k][k]), sb = std::fabs(rb.u[k][k]);
            if (!anyScale && std::max(sa, sb) > 0.05f && (sb > 16.0f * sa + 1e-4f || sa > 16.0f * sb + 1e-4f)) return false;
        }
    }
    const float limit = std::max({200.0f, 0.5f * dist, prevStep >= 0 ? 2.0f * prevStep + 60.0f : 0.0f});
    return placeStep(wa, wb, centre) <= limit;
}

// Orientation ambiguity: a matrix built from direction vectors (a hose segment's frame, look-at
// style) can flip its roll by 180 degrees about one of its own axes from one game frame to the next
// while the thing itself barely moves. When a pair turns by more than ~150 degrees and b turned by 180
// degrees about one of its local axes is within 45 degrees of a, b is replaced by that equivalent
// (columns i, j negated) for the in-betweens; the game frame itself still shows b.
float turnDegrees(const f32* a, const f32* b, int cols) {
    Rigid ra, rb;
    if (!decompose(a, cols, ra) || !decompose(b, cols, rb)) return 0.0f;
    const float d = std::fabs(ra.q[0] * rb.q[0] + ra.q[1] * rb.q[1] + ra.q[2] * rb.q[2] + ra.q[3] * rb.q[3]);
    return 2.0f * std::acos(std::min(d, 1.0f)) * 57.2958f;
}
bool resolveFlip(const f32* a, f32* b, int cols) {
    if (turnDegrees(a, b, cols) <= 150.0f) return false;
    for (int k = 0; k < 3; k++) {
        f32 c[12];
        std::copy(b, b + 3 * cols, c);
        for (int r = 0; r < 3; r++)
            for (int j = 0; j < 3; j++)
                if (j != k) c[cols * r + j] = -c[cols * r + j];
        if (turnDegrees(a, c, cols) < 45.0f) {
            std::copy(c, c + 3 * cols, b);
            return true;
        }
    }
    return false;
}
// Records how patch i of b turned since its partner j in a (world matrices wa -> wb, before flip or
// twist resolution: angle, axis) and returns the prevTurn argument of blendableWorld / blendable:
// above kSpinnerTurn when the identity spins (it turned by over 60 degrees in the game frame before:
// Chauncey, rolling balls, 150-175 degrees per game frame about one axis, blend the short way, which
// is their direction under 180) or tumbles (two turns in a row over 20 degrees about axes more than
// 45 degrees apart; the evidence holds for 8 game frames, since a tumbler's turn is sometimes small).
// The element streams' drops (fn_8011FB40's items, one shared S3D model) take a random orientation
// every game frame (35-180 degrees about any axis): the turns over 150 were shown at the game frame
// while the rest blended, and the water spray shimmered at 30 Hz (user report; LM_TWIST_LOG). Luigi
// turns about one axis by 20-25 degrees per game frame: his instant turns stay switches.
float noteTurn(const Tape* a, s32 j, const Tape& b, u32 i, const f32* wa, const f32* wb, int cols) {
    Rigid ra, rb;
    if (!decompose(wa, cols, ra) || !decompose(wb, cols, rb)) return -1.0f;
    // r = qb * conj(qa): the turn in world space.
    const float* x = rb.q;
    const float y[4] = {-ra.q[0], -ra.q[1], -ra.q[2], ra.q[3]};
    float r[4] = {x[3] * y[0] + x[0] * y[3] + x[1] * y[2] - x[2] * y[1], x[3] * y[1] - x[0] * y[2] + x[1] * y[3] + x[2] * y[0],
                  x[3] * y[2] + x[0] * y[1] - x[1] * y[0] + x[2] * y[3], x[3] * y[3] - x[0] * y[0] - x[1] * y[1] - x[2] * y[2]};
    if (r[3] < 0)
        for (float& v : r) v = -v;
    const float deg = 2.0f * std::acos(std::min(r[3], 1.0f)) * 57.2958f;
    const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    const float axis[3] = {len > 1e-6f ? r[0] / len : 0.0f, len > 1e-6f ? r[1] / len : 0.0f, len > 1e-6f ? r[2] / len : 0.0f};
    if (!b.worldTurn || !b.worldAxis || !b.worldTumble) return -1.0f;
    for (int c = 0; c < 3; c++) b.worldAxis[3 * i + c].store(axis[c], std::memory_order_relaxed);
    b.worldTurn[i].store(deg, std::memory_order_relaxed);
    if (!a || j < 0 || !a->worldTurn || !a->worldAxis || !a->worldTumble) return -1.0f;
    const float prev = a->worldTurn[j].load(std::memory_order_relaxed);
    const u8 prevTumble = a->worldTumble[j].load(std::memory_order_relaxed);
    u8 tumble = prevTumble > 0 ? u8(prevTumble - 1) : 0;
    bool axisEvidence = false;
    if (prev > 20.0f && deg > 20.0f && len > 1e-6f) {
        float d = 0;
        for (int c = 0; c < 3; c++) d += axis[c] * a->worldAxis[3 * j + c].load(std::memory_order_relaxed);
        axisEvidence = std::fabs(d) < 0.7071f;
    }
    // A turn over 150 degrees is evidence too, for the game frames after it (a drop whose random
    // turns alternate small and large never showed two large ones in a row: sweep A, room 5's stream);
    // it is itself a switch unless the identity already counted as a tumbler.
    if (axisEvidence || deg > 150.0f) tumble = 8;
    b.worldTumble[i].store(tumble, std::memory_order_relaxed);
    if (prev > kSpinnerTurn) return prev;
    return prevTumble > 0 || axisEvidence ? 1000.0f : -1.0f;
}
// A tumbler (tagTumbler: the element streams' drops, a random orientation every game frame) always
// counts as one: its turns over ~150 degrees blend the short way from its first game frame on.
float turnPolicy(const Patch& p, float prevTurn) { return p.tumble ? std::max(prevTurn, 1000.0f) : prevTurn; }
// A tumbler whose random orientation came out mirrored in one of the two game frames: blended
// entrywise (each vertex in a straight line), like a sheared flip (ceiling splashes).
bool tumblerMirror(const Patch& p, const f32* wa, const f32* wb) { return p.tumble && (det3(wa) < 0) != (det3(wb) < 0); }
u32 sFlips = 0;  // pairs blended through an equivalent flipped orientation (stats)
u32 sFlatBlends = 0;  // squashed-flat model matrices blended entrywise in world space (stats)
u32 sUntwists = 0;    // pairs whose fast twist was taken from b (stats)
u32 sGroupSnaps = 0;  // model instances switched as a whole (stats)
u32 sColorsBlended = 0, sColorsSwitched = 0;  // colour registers (stats)
std::vector<s32> sBundleToA;  // replayPair: per draw of b, the draw of a it pairs with (-1 none, -2 ambiguous)
u32 sReversals = 0;   // CPU-built blocks shown as captured because they turned back (another particle)
u32 sSprayPresent = 0;  // LM_SPRAY_LOG: the present being replayed
u32 sBornMoving = 0;  // values without partner shown moving along their step into place (tagBackStep; stats)
u32 sGroupForced = 0; // model instances blended linearly as a whole (stats)
u64 sOwnSkinned = 0, sOwnPivoted = 0;  // own-camera scenes: skinned instances blended entrywise / rigid matrices blended about their draw (stats)
// LM_GROUP_LOG: members of a model instance without a say this replay (1 outlier, 2 erratic now, 3 held
// erratic), per patch of b, and how the per-matrix path then treated them (X lines).
std::vector<u8> sExcluded;
bool sGroupLogNow = false;
struct ExclStat {
    u32 n = 0, snapped = 0, flipped = 0, untwisted = 0, turn90 = 0, policy = 0;
};
std::unordered_map<u64, ExclStat> sExclStats;
std::unordered_map<u64, std::pair<u32, float>> sPivotStats;  // LM_GROUP_LOG: per instance, members moved by the spinner centre rule, largest shift
// Set by replayPair around blendFactored for the matrices of a model instance blended linearly
// (groupSnap 2): entrywise in world space, no flip or twist resolution.
bool sLinearBlend = false;

bool blendable(Kind kind, const f32* a, const f32* b, int n, float prevTurn) {
    switch (kind) {
    case Kind::PosMtx:
    case Kind::NrmMtx: {
        // Rotation by up to ~150 degrees per game frame (Luigi turns around within a frame or
        // two; a swinging limb, or
        // the camera turning, which also swings far objects around in view space); a
        // translation jump larger than the distance (teleport, cut, respawn) snaps.
        const int cols = kind == Kind::PosMtx ? 4 : 3;
        Rigid ra, rb;
        if (!decompose(a, cols, ra) || !decompose(b, cols, rb)) {  // not a rotation and scale: entrywise
            for (int i = 0; i < n; i++)
                if (std::fabs(a[i] - b[i]) > ((cols == 4 && (i & 3) == 3) ? 400.0f : 0.35f)) return false;
            return true;
        }
        if ((ra.u[2][2] < 0) != (rb.u[2][2] < 0)) return false;
        if (!(prevTurn > kSpinnerTurn) &&
            std::fabs(ra.q[0] * rb.q[0] + ra.q[1] * rb.q[1] + ra.q[2] * rb.q[2] + ra.q[3] * rb.q[3]) < 0.26f)
            return false;  // cos(angle / 2) < 0.26: more than ~150 degrees, no telling which way
        for (int k = 0; k < 3; k++) {
            const float sa = std::fabs(ra.u[k][k]), sb = std::fabs(rb.u[k][k]);
            // Scales blend freely (particles grow, eyelids blink to nothing); a wrong pair would
            // also have to agree in rotation and position.
            if (std::max(sa, sb) > 0.05f && (sb > 16.0f * sa + 1e-4f || sa > 16.0f * sb + 1e-4f)) return false;
        }
        if (cols == 4) {
            float da = 0, db = 0, dd = 0;
            for (int r = 0; r < 3; r++) {
                da += a[4 * r + 3] * a[4 * r + 3];
                db += b[4 * r + 3] * b[4 * r + 3];
                dd += (a[4 * r + 3] - b[4 * r + 3]) * (a[4 * r + 3] - b[4 * r + 3]);
            }
            if (std::sqrt(dd) > std::max(200.0f, 0.5f * std::sqrt(std::max(da, db)))) return false;
        }
        return true;
    }
    case Kind::TexMtx: {
        // Some effects alternate a projection-style matrix with a zero matrix between
        // frames: never blend toward or away from zero. Scrolling coordinates wrap.
        bool za = true, zb = true;
        float big = 0;
        for (int i = 0; i < n; i++) za &= a[i] == 0.0f, zb &= b[i] == 0.0f, big = std::max(big, std::fabs(b[i]));
        if (za || zb) return za && zb;
        if (n == 8) {  // a spinning sprite: up to ~150 degrees per game frame
            Spin sa, sb;
            if (spin2D(a, sa) && spin2D(b, sb))
                return std::fabs(angleDelta(sa.angle, sb.angle)) <= 2.6f && sb.scale <= 2.0f * sa.scale &&
                       sa.scale <= 2.0f * sb.scale;
        }
        if (big <= 4.0f) {
            // Texture-coordinate scale/scroll: a wrap makes the translation jump by about 1.
            // Otherwise (rows turning: environment/reflection mapping from normals) the rows are
            // compared like a rotation below.
            const int cols = n == 8 ? 4 : n / 3;
            if (cols == 4)
                for (int r = 0; r * 4 < n; r++)
                    if (std::fabs(a[4 * r + 3] - b[4 * r + 3]) > 0.5f) return false;
            if (cols != 4)
                for (int i = 0; i < n; i++)
                    if (std::fabs(a[i] - b[i]) > 0.5f) return false;
        }
        // Projection-style (projected flashlight, shadows; rows of a scaled view matrix):
        // follows the camera and the light. Translations pass through zero as things turn,
        // so only the rotation/scale part is compared, per row.
        const int cols = n == 8 ? 4 : n / 3;
        for (int r = 0; r * cols < n; r++) {
            float na = 0, nb = 0, dd = 0;
            for (int c = 0; c < 3 && c < cols; c++) {
                const float x = a[r * cols + c], y = b[r * cols + c];
                na += x * x, nb += y * y, dd += (x - y) * (x - y);
            }
            if (std::sqrt(dd) > 1.9f * std::sqrt(std::max(na, nb)) + 1e-6f) return false;  // ~145 degrees
        }
        return true;
    }
    case Kind::Projection:
        for (int i = 0; i < n; i++)
            if (std::fabs(a[i] - b[i]) > 0.25f * std::max({std::fabs(a[i]), std::fabs(b[i]), 1e-3f})) return false;
        return true;
    case Kind::Light: {  // position xyz, direction xyz (view space)
        // A jump larger than half its distance (at least 400 units) snaps: distant lights move by
        // hundreds of units per frame as the camera turns (from the SMS port).
        float da = 0, db = 0, dd = 0;
        for (int i = 0; i < 3; i++) da += a[i] * a[i], db += b[i] * b[i], dd += (a[i] - b[i]) * (a[i] - b[i]);
        if (!std::isfinite(dd)) return false;
        return std::sqrt(dd) <= std::max(400.0f, 0.5f * std::sqrt(std::max(da, db)));
    }
    default:
        return false;
    }
}

}  // namespace

// --- guest ---------------------------------------------------------------------------

namespace {
const char* const kCatName[] = {"none", "view", "world", "object", "particle", "hud", "effect", "subview"};
bool worldOrigin(const Tape& t, const Patch& p, f32 o[3]) {
    if (p.view >= t.views.size() || p.count != 12) return false;
    const M34& v = t.views[p.view];
    f32 d[3];
    for (int r = 0; r < 3; r++) d[r] = val(t, p, 4 * r + 3) - v[4 * r + 3];
    for (int r = 0; r < 3; r++) o[r] = v[r] * d[0] + v[4 + r] * d[1] + v[8 + r] * d[2];
    return std::isfinite(o[0] + o[1] + o[2]);
}
void checkIdentity(const Tape& t0, const Tape& t1, const Tape& t2) {
    u32 flagged = 0, checked = 0;
    for (u32 i = 0; i < t2.patches.size(); i++) {
        const Patch& p2 = t2.patches[i];
        if (p2.kind != Kind::PosMtx || p2.role != 0 || p2.cat == TagCat::World || p2.cat == TagCat::None) continue;
        if (p2.bundle < t2.bundlePass.size() && t2.bundlePass[p2.bundle] != 0) continue;
        const s32 j = t2.partner[i];
        if (j < 0) continue;
        const s32 k = t1.partner.empty() ? -1 : t1.partner[j];
        if (k < 0) continue;
        const Patch &p1 = t1.patches[j], &p0 = t0.patches[k];
        // Loaded from the same memory all three frames (a joint's own matrix): the same thing, any
        // back-and-forth is the animation's own (Luigi's trembling hands).
        f32 o0[3], o1[3], o2[3];
        if (!worldOrigin(t0, p0, o0) || !worldOrigin(t1, p1, o1) || !worldOrigin(t2, p2, o2)) continue;
        checked++;
        if (p2.source && p0.source == p2.source && p1.source == p2.source) continue;
        float jerk = 0, s01 = 0, s12 = 0;
        for (int c = 0; c < 3; c++) {
            jerk += (o0[c] - 2 * o1[c] + o2[c]) * (o0[c] - 2 * o1[c] + o2[c]);
            s01 += (o1[c] - o0[c]) * (o1[c] - o0[c]);
            s12 += (o2[c] - o1[c]) * (o2[c] - o1[c]);
        }
        jerk = std::sqrt(jerk), s01 = std::sqrt(s01), s12 = std::sqrt(s12);
        // A wrong pair jumps there and back: both steps large and nearly opposite (jerk ~ their sum).
        if (!(jerk > 30.0f && jerk > 1.5f * std::max(s01, s12))) continue;
        flagged++;
        if (flagged <= 20)
            probeLine("P %llu %s jerk %.1f steps %.1f %.1f key %016llX site %08X world %.0f %.0f %.0f\n", (unsigned long long)t2.seq,
                      kCatName[u32(p2.cat)], jerk, s01, s12, (unsigned long long)p2.key, p2.site, o2[0], o2[1], o2[2]);
    }
    sIdTotals.checked += checked;
    sIdTotals.flagged += flagged;
    // CPU-built geometry (hose quads, particles' quads, effect strips): a block followed by its
    // identity over three frames; its centroid must not jump there and back (blocks paired with
    // another part of the mesh). The detector's draw probe only judges matrix draws.
    u32 vChecked = 0, vFlagged = 0;
    const auto centroid = [](const Tape& t, const Patch& p, f32 c[3]) {
        c[0] = c[1] = c[2] = 0;
        for (u32 v = 0; v < p.count; v++)
            for (int k = 0; k < 3; k++) c[k] += vtx(t, p, v, k);
        for (int k = 0; k < 3; k++) c[k] /= float(std::max<u32>(p.count, 1));
        return std::isfinite(c[0] + c[1] + c[2]);
    };
    for (u32 i = 0; i < t2.patches.size(); i++) {
        const Patch& p2 = t2.patches[i];
        if (p2.kind != Kind::Verts || p2.role == 2) continue;  // (2D blocks: colours only)
        const s32 j = t2.partner[i];
        if (j < 0) continue;
        const s32 k = t1.partner.empty() ? -1 : t1.partner[j];
        if (k < 0) continue;
        f32 c0[3], c1[3], c2[3];
        if (!centroid(t0, t0.patches[k], c0) || !centroid(t1, t1.patches[j], c1) || !centroid(t2, p2, c2)) continue;
        vChecked++;
        float jerk = 0, s01 = 0, s12 = 0;
        for (int c = 0; c < 3; c++) {
            jerk += (c0[c] - 2 * c1[c] + c2[c]) * (c0[c] - 2 * c1[c] + c2[c]);
            s01 += (c1[c] - c0[c]) * (c1[c] - c0[c]);
            s12 += (c2[c] - c1[c]) * (c2[c] - c1[c]);
        }
        jerk = std::sqrt(jerk), s01 = std::sqrt(s01), s12 = std::sqrt(s12);
        if (!(jerk > 20.0f && jerk > 1.5f * std::max(s01, s12))) continue;
        if (vFlagged++ < 20)
            probeLine("P %llu verts %s jerk %.1f steps %.1f %.1f key %016llX site %08X n %u at %.0f %.0f %.0f\n", (unsigned long long)t2.seq,
                      kCatName[u32(p2.cat)], jerk, s01, s12, (unsigned long long)p2.key, p2.site, u32(p2.count), c2[0], c2[1], c2[2]);
    }
    sIdTotals.vertsChecked += vChecked;
    sIdTotals.vertsFlagged += vFlagged;
}
}  // namespace


bool capturing() { return sCapturing && !sSuspended; }

bool g_captureOpen = false;
u32 g_captureGen = 1;
thread_local u32 t_captureGen = 0;

void joinCaptureSlow() {
    AuroraCaptureJoin(1);
    t_captureGen = g_captureGen;
    static int sLogged = 0;
    if (sLogged++ < 8) log("[interp] guest thread %08X joins the frame capture (GX calls outside the drawing thread)", sched::currentGuest());
}

void noteCallSite(u32 lr) {
    if (capturing()) sTapeSites.emplace_back(mark(), lr);
}

std::string describeStreamPos(u64 streamPos) {
    ReplayRec rec;
    {
        std::lock_guard lk(sReplayRecM);
        for (const ReplayRec& r : sReplayRecs)
            if (r.size && streamPos >= r.start && streamPos < r.start + r.size) rec = r;
    }
    if (!rec.size) return {};
    const u32 off = u32(streamPos - rec.start);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "in the replay of game frame %u (tape %llu) at offset %u of %u", rec.gameFrame,
                  static_cast<unsigned long long>(rec.seq), off, rec.size);
    std::string s = buf;
    if (rec.sites) {
        const auto& v = *rec.sites;
        auto it = std::upper_bound(v.begin(), v.end(), std::pair<u32, u32>{off, ~0u});
        s += "; GX calls before it (bytes before the bad one): ";
        for (int i = 0; i < 4 && it != v.begin(); i++) {
            --it;
            std::snprintf(buf, sizeof(buf), "%s-%u B: ", i ? "; " : "", off - it->first);
            s += buf + guestSymbolize(it->second);
        }
    }
    return s;
}

void beginTape() {
    if (sScratch.empty()) sScratch.resize(kCaptureSize);
    sBase = 0;
    const size_t lastKeys = sLastTape ? sLastTape->patches.size() : 0;
    // endTape moves the staging vectors into the tape, which leaves them with no capacity: without a
    // hint every game frame grows them from nothing again (10+ reallocations for a few thousand
    // patches). The previous tape's sizes are the best guess.
    if (sLastTape) {
        const Tape& last = *sLastTape;
        sPatches.reserve(lastKeys);
        sBundleIds.reserve(last.bundleIds.size());
        sBundleKey.reserve(last.bundleKey.size());
        sBundleRange.reserve(last.bundleRange.size());
        sBundlePass.reserve(last.bundlePass.size());
        sBundleCentre.reserve(last.bundleCentre.size());
        sSlotCentres.reserve(last.slotCentres.size());
        sBundleSlots.reserve(last.bundleSlots.size());
        sViews.reserve(last.views.size());
        sViewKeys.reserve(last.viewKeys.size());
        sLights.reserve(last.lights.size());
        sLightKeys.reserve(last.lightKeys.size());
        sTextGroups.reserve(last.textGroups.size());
        sTextViews.reserve(last.textViews.size());
        sBackSteps.reserve(last.backSteps.size());
        if (last.sites) sTapeSites.reserve(last.sites->size());
    }
    sPatches.clear();
    sBundleIds.clear();
    sBundleKey.clear();
    sBundleRange.clear();
    sBundlePass.clear();
    sOpenDraw = -1;
    sViews.clear();
    sViewKeys.clear();
    sShadow = {};
    sLights.clear();
    sLightKeys.clear();
    sUngrouped = 0;
    sTextGroups.clear();
    sTextViews.clear();
    sBackSteps.clear();
    sTextCur = 0;
    sTextDepth = 0;
    std::fill(std::begin(sLastPosMtx), std::end(sLastPosMtx), -1);
    sLastProj = -1;
    // A new tape: every key belongs to another game frame, so the counters of the tape just captured
    // read as absent (GenTable).
    sTagOcc.nextFrame(4 * lastKeys + 4096);
    sValOcc.nextFrame(4 * lastKeys + 4096);
    sVirtBase = 0;
    sSlotPatch.fill(-1);
    sBundleSlots.clear();
    sTapeSites.clear();
    // The thread that begins the capture owns it. When it is not one already sharing the current
    // capture, a new group starts in its own recording; the other guest threads join it at their
    // next GX call (joinCapture).
    if (t_captureGen != g_captureGen) {
        g_captureGen++;
        AuroraCaptureJoin(0);
        t_captureGen = g_captureGen;
    }
    AuroraCaptureBegin(sScratch.data(), kCaptureSize);
    sCapturing = true;
    g_captureOpen = true;
    sSuspended = false;
    detail::vtxBeginTape();
    sTagLogFrame = sTagLog && copyDispCount() + 1 >= sTagLogFirst && copyDispCount() + 1 <= sTagLogLast ? copyDispCount() + 1 : ~0ull;
}

void endTape(u32 retraceCount) {
    if (!sCapturing) return;
    const auto t0 = Clock::now();
    const u32 size = sBase + AuroraCaptureEnd();
    std::vector<std::vector<u8>> arrays;
    std::vector<detail::ArrayRec> arrayRecs;
    u64 arrayBytes = 0;
    detail::vtxEndTape(sScratch.data(), size, arrays, arrayBytes, arrayRecs);
    std::vector<std::shared_ptr<const std::vector<u8>>> textures;
    u64 textureBytes = 0;
    detail::texEndTape(textures, textureBytes);
    detail::drawEnd(mark());
    detail::noteDraw(0);
    sCapturing = false;
    g_captureOpen = false;
    if (sTagLog && sTagLogFrame != ~0ull) std::fflush(sTagLog);
    auto tp = t0;
    const u32 vtxUs = usSince(tp);
    if (size + 4096 >= kCaptureSize) {  // the capture ran out of room: drop this frame
        if (!sOverflowLogged) log("[interp] frame larger than %u MiB; dropped", kCaptureSize >> 20);
        sOverflowLogged = true;
        return;
    }
    auto tape = std::make_shared<Tape>();
    tape->seq = ++sSeq;
    tape->gameFrame = copyDispCount() + 1;
    tape->bytes.assign(sScratch.begin(), sScratch.begin() + size);
    tape->sites = std::make_shared<const std::vector<std::pair<u32, u32>>>(std::move(sTapeSites));
    sTapeSites = {};
    tape->patches = std::move(sPatches);
    sPatches = {};
    tape->bundleIds = std::move(sBundleIds);
    sBundleIds = {};
    tape->bundleKey = std::move(sBundleKey);
    sBundleKey = {};
    tape->bundleRange = std::move(sBundleRange);
    sBundleRange = {};
    tape->bundlePass = std::move(sBundlePass);
    sBundlePass = {};
    tape->bundleCentre = std::move(sBundleCentre);
    sBundleCentre = {};
    tape->slotCentres = std::move(sSlotCentres);
    sSlotCentres = {};
    tape->bundleSlots = std::move(sBundleSlots);
    sBundleSlots = {};
    tape->views = std::move(sViews);
    sViews = {};
    tape->viewKeys = std::move(sViewKeys);
    sViewKeys = {};
    tape->shadow = sShadow;
    tape->lights = std::move(sLights);
    sLights = {};
    tape->lightKeys = std::move(sLightKeys);
    sLightKeys = {};
    for (TextGroupRec& g : sTextGroups) g.end = std::min<u32>(g.end, u32(tape->patches.size()));
    tape->textGroups = std::move(sTextGroups);
    sTextGroups = {};
    tape->textViews = std::move(sTextViews);
    sTextViews = {};
    tape->backSteps = std::move(sBackSteps);
    sBackSteps = {};
    tape->arrays = std::move(arrays);
    tape->arrayRecs = std::move(arrayRecs);
    tape->arrayBytes = arrayBytes;
    tape->textures = std::move(textures);
    tape->textureBytes = textureBytes;
    tape->retrace = retraceCount;
    if (sTrackOut) {
        tape->trackOk = detail::trackPoints(tape->track.data());
        const auto& t = tape->track;
        trackLine("HT %llu f%u %d %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n", (unsigned long long)tape->seq, tape->gameFrame,
                  int(tape->trackOk), t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8]);
    }
    tape->phaseUs[0] = vtxUs;
    tape->phaseUs[1] = usSince(tp);
    if (sDumpAt != ~0u) {
        const u64 ptr = u64(reinterpret_cast<uintptr_t>(sDumpPtr));
        for (u32 i = sDumpAt; i + 8 <= std::min(size, sDumpAt + 64); i++) {
            u64 v = 0;
            for (int k = 0; k < 8; k++) v = v << 8 | tape->bytes[i + k];
            if (v == ptr) {
                tape->dumpOffset = i;
                tape->dumpFrame = sDumpFrame;
                break;
            }
        }
        sDumpAt = ~0u;
    }
    if (sLastTape) pairTapes(*sLastTape, *tape);
    if (sProbeOut && sLastTape && sLastTape2 && sLastTape->pairedWith == sLastTape2.get() && tape->pairedWith == sLastTape.get()) {
        auto tc = Clock::now();
        checkIdentity(*sLastTape2, *sLastTape, *tape);
        tape->phaseUs[3] = usSince(tc);
    }
    if (sTagLog && sLastTape && tape->dumpFrame >= sTagLogFirst && tape->dumpFrame <= sTagLogLast) {
        u32 kinds[kKinds][3] = {};  // paired, unpaired, untagged
        for (u32 i = 0; i < tape->patches.size(); i++) {
            const Patch& p = tape->patches[i];
            kinds[u32(p.kind)][!p.key ? 2 : tape->partner[i] >= 0 ? 0 : 1]++;
        }
        std::fprintf(sTagLog, "== f%u tape %llu: paired/unpaired/untagged pos %u/%u/%u nrm %u/%u/%u tex %u/%u/%u proj %u/%u/%u "
                              "light %u/%u/%u verts %u/%u/%u; views %zu main %d cut %d\n",
                     tape->dumpFrame, (unsigned long long)tape->seq, kinds[0][0], kinds[0][1], kinds[0][2], kinds[1][0], kinds[1][1],
                     kinds[1][2], kinds[2][0], kinds[2][1], kinds[2][2], kinds[3][0], kinds[3][1], kinds[3][2], kinds[4][0],
                     kinds[4][1], kinds[4][2], kinds[5][0], kinds[5][1], kinds[5][2], tape->views.size(), int(tape->mainView),
                     int(tape->cut));
        for (u32 v = 0; v < tape->views.size(); v++)
            std::fprintf(sTagLog, "   view %u key %016llX -> a %d\n", v, (unsigned long long)tape->viewKeys[v], tape->viewToA[v]);
        std::fflush(sTagLog);
    }
    tape->done = Clock::now();
    tape->guestUs = u32(std::chrono::duration_cast<std::chrono::microseconds>(tape->done - t0).count());
    judderLine("T %llu %u %.1f %u %u %u\n", (unsigned long long)tape->seq, retraceCount, judderUs(tape->done), tape->guestUs,
               tape->cut ? 1u : 0u, copyDispCount());
    sLastTape2 = sLastTape;
    sLastTape = tape;
    {
        std::lock_guard lk(sM);
        if (sPrevious) sRetired.push_back(std::move(sPrevious));
        while (sRetired.size() > 6) sRetired.pop_front();
        sPrevious = std::move(sNewest);
        sNewest = tape;
        sQueue.push_back(tape);
        while (sQueue.size() > 8) {
            sRetired.push_back(std::move(sQueue.front()));
            sQueue.pop_front();
        }
    }
    if (sTestSteps) {
        // Wait for the host to render this tape's test items, but only while the host is replaying
        // tapes: when it is not (back in lockstep for a 60 Hz scene, a shader hold), waiting here
        // stalled both sides for 20 s per tape (a run looked frozen for two minutes).
        std::unique_lock lk(sTestM);
        sTestPending = true;
        const auto until = Clock::now() + std::chrono::seconds(20);
        while (sTestPending && Clock::now() < until) {
            sTestCv.wait_for(lk, std::chrono::milliseconds(50), [] { return !sTestPending; });
            if (Clock::now() - std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(sTestHostSeen.load())) >
                std::chrono::milliseconds(500))
                break;
        }
        sTestPending = false;
    }
}

void noteDump(u32 at, const void* target, u32 frameNo) {
    if (at == ~0u || !capturing()) return;
    sDumpAt = at;
    sDumpPtr = target;
    sDumpFrame = frameNo;
}

u8* detail::captureBytes() { return sScratch.data(); }

u32 mark() { return capturing() ? sBase + AuroraCapturePos() : ~0u; }

void noteExtraLights(u32 at, u32 count) {
    if (at == ~0u || !capturing() || count == 0) return;
    // GX_AURORA (0x50), u16 GX_AURORA_SET_EXTRA_LIGHTS, u8 count, then the lights.
    const u32 end = at + 4 + count * GX_AURORA_EXTRA_LIGHT_WORDS * 4;
    if (end > sBase + AuroraCapturePos() || sScratch[at] != 0x50 || sScratch[at + 3] != count) return;
    sLights.emplace_back(at + 4, count);
    // Identity of each light: the providing code's id (ghost actor, emitter, fixed source) and, for
    // an id that is a guest object's address, that object's generation: a light of a new ghost or
    // emitter at a reused address is another light.
    for (u32 i = 0; i < count; i++) {
        const u8* l = sScratch.data() + at + 4 + i * GX_AURORA_EXTRA_LIGHT_WORDS * 4;
        const u32 id = u32(l[36]) << 24 | u32(l[37]) << 16 | u32(l[38]) << 8 | l[39];
        u32 obj = id;
        if (!(obj >= 0x80000000u && obj < 0x81800000u)) obj = id ^ 0x57000000u;  // world lights: emitter
        const u32 gen = obj >= 0x80000000u && obj < 0x81800000u ? generationOf(obj) : 0;
        sLightKeys.push_back(id ? mix(u64(id) << 20 ^ gen) : 0);
    }
}

namespace {
int sNoBlend = 0;
int sFrameLock = 0;  // text boxes: nothing noted (matrices, colours, 2D vertex blocks, position arrays)
int sLinearPos = 0;
}  // namespace
void pushNoBlend() { sNoBlend++; }
void popNoBlend() { sNoBlend = sNoBlend > 0 ? sNoBlend - 1 : 0; }
void pushFrameLock() { sFrameLock++; }
void setCurrentMtx(u32 id) { sCurMtxSlot = id / 3; }
void pushTextGroup(u32 id, bool oneLayout) {
    if (sTextDepth++ > 0 || !capturing() || sFrameLock > 0 || sTextGroups.size() >= 0xFFF0) return;
    sTextGroups.push_back({id, u32(sPatches.size()), ~0u, oneLayout});
    sTextCur = u16(sTextGroups.size());
}
void pushTextGroupJoin(u32 id) {
    if (sTextDepth++ > 0 || !capturing() || sFrameLock > 0) return;
    for (u32 g = u32(sTextGroups.size()); g-- > 0;)
        if (sTextGroups[g].id == id) {
            sTextCur = u16(g + 1);
            return;
        }
}
void noteTextView(u32 at, bool scissor, float f, float c) {
    if (at == ~0u || !capturing() || sFrameLock > 0) return;
    u16 g = sTextCur;
    if (!g && sTextViewJoin)
        for (u32 k = u32(sTextGroups.size()); k-- > 0;)
            if (sTextGroups[k].id == sTextViewJoin) {
                g = u16(k + 1);
                break;
            }
    if (!g) return;
    const u32 end = sBase + AuroraCapturePos();
    // Viewport: XF write 0x10, header 0x0005101A, 6 floats. Scissor: BP 0x61 0x20 (top, left),
    // 0x61 0x21 (bottom, right).
    if (!scissor && !(at + 29 <= end && sScratch[at] == 0x10 && sScratch[at + 3] == 0x10 && sScratch[at + 4] == 0x1A)) return;
    if (scissor && !(at + 10 <= end && sScratch[at] == 0x61 && sScratch[at + 1] == 0x20 && sScratch[at + 5] == 0x61 &&
                     sScratch[at + 6] == 0x21))
        return;
    sTextViews.push_back({at, u8(scissor), g, f, c});
}
void noteTextViewport(u32 at, float f, float c) { noteTextView(at, false, f, c); }
void noteTextScissor(u32 at, float f, float c) { noteTextView(at, true, f, c); }
void pushTextViewJoin(u32 id) {
    if (sTextViewJoinDepth++ == 0) sTextViewJoin = id;
}
void popTextViewJoin() {
    if (sTextViewJoinDepth > 0 && --sTextViewJoinDepth == 0) sTextViewJoin = 0;
}
void popTextGroup() {
    if (sTextDepth <= 0) return;
    if (--sTextDepth > 0 || !sTextCur) return;
    TextGroupRec& r = sTextGroups[sTextCur - 1];
    r.end = r.end == ~0u ? u32(sPatches.size()) : std::max<u32>(r.end, u32(sPatches.size()));
    sTextCur = 0;
}
void popFrameLock() { sFrameLock = sFrameLock > 0 ? sFrameLock - 1 : 0; }
bool tagLogOn() { return sTagLogFrame != ~0ull && capturing(); }
void tagLogMark(const char* text) {
    if (sTagLogFrame != ~0ull && capturing())
        std::fprintf(sTagLog, "f%llu #%u mark %s\n", (unsigned long long)sTagLogFrame, u32(sPatches.size()), text);
}
namespace {
std::vector<size_t> sSkyStart;
}  // namespace
void pushSky() { sSkyStart.push_back(sPatches.size()); }
void popSky() {
    if (sSkyStart.empty()) return;
    for (size_t i = sSkyStart.back(); i < sPatches.size(); i++) sPatches[i].sky = true;
    sSkyStart.pop_back();
}
void pushLinearPosMtx() { sLinearPos++; }
void popLinearPosMtx() { sLinearPos = sLinearPos > 0 ? sLinearPos - 1 : 0; }

// --- tags ---

void pushTag(TagCat cat, u32 object, u32 type, u32 sub) {
    if (sTagDepth >= kMaxTagDepth) {
        sTagDepth++;
        return;
    }
    TagFrame f{};
    const TagFrame* parent = sTagDepth > 0 ? &sTagStack[sTagDepth - 1] : nullptr;
    f.cat = cat;
    if (sCapturing) {
        const u64 base = scopeBase(parent ? parent->inst : rootInst(), cat, object, type, sub);
        f.inst = mix(base ^ u64(occurrence(sTagOcc, base)) << 44);
        const bool structural = cat == TagCat::View || cat == TagCat::SubView;
        f.object = !structural || (parent && parent->object);
        f.ownCamera = cat == TagCat::SubView || (parent && parent->ownCamera);
        // The camera's identity follows the structural scopes only (a view, a sub-view or pass),
        // not the objects drawn in it.
        const u64 sp = parent ? parent->structural : rootInst();
        f.structural = structural ? mix(sp ^ mix(u64(object) << 8 ^ sub ^ u64(cat) << 40 ^ 0x51E3ull << 48)) : sp;
        f.group = parent ? parent->group : 0;  // set by tagGroup (J3D models), inherited
        f.tumble = parent && parent->tumble;
        f.tracked = parent && parent->tracked;
        f.stream = parent && parent->stream;
        f.backStep = parent ? parent->backStep : -1;
    }
    sTagStack[sTagDepth++] = f;
}

void popTag() {
    if (sTagDepth > 0) sTagDepth--;
}

void tagGeneration(u32 addr) {
    if (const auto it = sGen.find(addr); it != sGen.end()) it->second++;
}

void tagFreed(u32 addr, u32 size) {
    if (sGen.empty() || size == 0) return;
    for (auto it = sGen.lower_bound(addr); it != sGen.end() && it->first - addr < size; ++it) it->second++;
}

void addSiteTag(const SiteTag& t) { sSiteTags.push_back(t); }

void tagGroup(u32 id) {
    if (sCapturing && sTagDepth > 0 && sTagDepth <= kMaxTagDepth) sTagStack[sTagDepth - 1].group = mix(u64(id) ^ 0x6A0Full << 48);
}
void tagTumbler() {
    if (sCapturing && sTagDepth > 0 && sTagDepth <= kMaxTagDepth) sTagStack[sTagDepth - 1].tumble = sTagStack[sTagDepth - 1].stream = true;
}
void tagStreamItem() {
    if (sCapturing && sTagDepth > 0 && sTagDepth <= kMaxTagDepth) sTagStack[sTagDepth - 1].stream = true;
}
void tagTracked() {
    if (sCapturing && sTagDepth > 0 && sTagDepth <= kMaxTagDepth) sTagStack[sTagDepth - 1].tracked = true;
}
void tagBackStep(const float step[3]) {
    if (!sCapturing || sTagDepth <= 0 || sTagDepth > kMaxTagDepth || sBackSteps.size() >= (1u << 20)) return;
    if (!(std::isfinite(step[0]) && std::isfinite(step[1]) && std::isfinite(step[2]))) return;
    sTagStack[sTagDepth - 1].backStep = s32(sBackSteps.size());
    sBackSteps.push_back({step[0], step[1], step[2]});
}
void tagGroupIfNone(u32 id) {
    if (sCapturing && sTagDepth > 0 && sTagDepth <= kMaxTagDepth && !sTagStack[sTagDepth - 1].group) tagGroup(id);
}

void gxCall(const Ctx& c) {
    sCallLr = c.lr;
    sVirtActive = false;
    if (!sCapturing) return;
    for (const SiteTag& s : sSiteTags) {
        if (c.lr < s.lo || c.lr >= s.hi) continue;
        const u32 obj = s.objReg < 32 ? c.r[s.objReg] : 0, type = s.typeReg < 32 ? c.r[s.typeReg] : 0;
        const TagFrame* parent = sTagDepth > 0 && sTagDepth <= kMaxTagDepth ? &sTagStack[sTagDepth - 1] : nullptr;
        u32 birth = 0;
        if (s.ageOffset >= 0 && obj >= 0x80000000u && obj < 0x81800000u)
            birth = u32(sSeq + 1) - u32(s.ageStep) * rd16(obj + u32(s.ageOffset));  // constant over its life
        const u64 base = scopeBase(parent ? parent->inst : rootInst(), s.cat, obj, type, s.lo ^ birth * 0x9E3779B1u);
        if (base != sVirtBase) {  // the next item of the loop
            sVirtBase = base;
            // LM_TAG_PEEK=<n words>: dump each loop item's memory (finding a particle's age field).
            static const int sPeek = std::getenv("LM_TAG_PEEK") ? std::atoi(std::getenv("LM_TAG_PEEK")) : 0;
            if (sPeek > 0 && obj >= 0x80000000u && obj < 0x81800000u) {
                std::string line;
                char buf[16];
                for (int w = 0; w < sPeek; w++) {
                    std::snprintf(buf, sizeof(buf), " %08X", rd32(obj + 4 * w));
                    line += buf;
                }
                log("[peek] f%u site %08X obj %08X type %08X%s", copyDispCount(), s.lo, obj, type, line.c_str());
            }
            sVirt.inst = mix(base ^ u64(occurrence(sTagOcc, base)) << 44);
        }
        sVirt.cat = s.cat;
        sVirt.object = true;
        sVirt.structural = parent ? parent->structural : rootInst();
        sVirt.ownCamera = parent && parent->ownCamera;
        sVirt.group = parent ? parent->group : 0;
        sVirt.tumble = parent && parent->tumble;
        sVirt.tracked = parent && parent->tracked;
        sVirt.stream = parent && parent->stream;
        sVirt.backStep = parent ? parent->backStep : -1;
        sVirtActive = true;
        break;
    }
    if (sTagLogFrame != ~0ull) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08X", c.lr);
        sTagChain = buf;
        u32 sp = c.r[1];
        for (int i = 0; i < 10; i++) {
            sp = rd32(sp);
            if (sp < 0x80000000u || sp >= 0x81800000u) break;
            std::snprintf(buf, sizeof(buf), "<%08X", rd32(sp + 4));
            sTagChain += buf;
        }
    }
}

// The key of a value of `kind` noted now: its scope, its call site, its occurrence there.
// Untagged (no object scope) values get 0, except lights (a light object is its own tag: the
// game's light structure + slot) and projections (the camera's).
u64 valueKey(Kind kind, u32 source, u32 slot, TagCat& cat) {
    const TagFrame* t = topTag();
    cat = t && t->object ? t->cat : TagCat::None;
    const bool tagged = t && t->object;
    if (!tagged && kind != Kind::Light && kind != Kind::Projection) return 0;
    // Untagged lights/projections: the structural path (camera) only.
    const u64 scope = tagged ? t->inst : (t ? t->structural : rootInst());
    u64 k = mix(scope ^ u64(kind) << 56 ^ u64(sCallLr) << 16 ^ u64(sPass) << 8);
    if (kind == Kind::Light) k = mix(k ^ u64(source) << 24 ^ slot);
    return mix(k ^ u64(occurrence(sValOcc, k) + 1) << 40);
}

void note(u32 at, Kind kind, u32 source, u32 slot, int nFloats, int first) {
    if (kind == Kind::PosMtx && slot % 3 == 0 && slot / 3 < 10) sLastPosMtx[slot / 3] = -1;  // (set below when noted)
    if (at == ~0u || !capturing() || sNoBlend + sFrameLock > 0) return;
    if (sLinearPos > 0 && kind == Kind::PosMtx) kind = Kind::TexMtx;  // element-wise, own key
    // Aurora writes XF loads as 0x10, a u32 header, then the values.
    if (at + 5 + 4 * u32(first + nFloats) > sBase + AuroraCapturePos() || sScratch[at] != 0x10) return;
    Patch p{at + 5 + 4 * u32(first), u16(nFloats), kind};
    p.source = source;
    p.slot = slot;
    p.site = sCallLr;
    p.key = valueKey(kind, source, slot, p.cat);
    if (const TagFrame* g = topTag(); g && p.key) p.group = g->group, p.tumble = g->tumble, p.backStep = g->backStep, p.stream = g->stream;
    p.inScope = sTagDepth > 0 || sVirtActive;
    if (kind == Kind::TexMtx && sShadowRecv > 0) p.role = 1;
    else if (sOrtho && kind != Kind::Projection) p.role = 2;
    if (sProbeOut && kind == Kind::PosMtx && nFloats == 12 && slot % 3 == 0 && slot / 3 < 10)
        sSlotPatch[slot / 3] = s32(sPatches.size());
    p.tgroup = sTextCur;
    if (kind == Kind::PosMtx && slot % 3 == 0 && slot / 3 < 10) sLastPosMtx[slot / 3] = s32(sPatches.size());
    if (kind == Kind::Projection) sLastProj = s32(sPatches.size());
    // (Inside a SubView the game camera is not the one these values were built with: not factored.)
    const TagFrame* tf = topTag();
    const bool ownCamera = tf && tf->ownCamera;
    p.ownCamera = ownCamera;
    if (!ownCamera && p.role != 2 && (kind == Kind::PosMtx || kind == Kind::NrmMtx || kind == Kind::Light || p.role == 1))
        p.view = sampleView();
    if (sTagLogFrame != ~0ull)
        std::fprintf(sTagLog, "f%llu #%u %s %s key %016llX site %08X slot %u src %08X depth %d view %d t %.1f %.1f %.1f%s%s\n",
                     (unsigned long long)sTagLogFrame, u32(sPatches.size()),
                     kind == Kind::PosMtx ? "pos" : kind == Kind::NrmMtx ? "nrm" : kind == Kind::TexMtx ? "tex"
                     : kind == Kind::Projection ? "proj" : kind == Kind::Light ? "light" : "?",
                     kCatName[u32(p.cat)], (unsigned long long)p.key, sCallLr, slot, source, sTagDepth, int(p.view),
                     nFloats == 12 ? loadBE(&sScratch[p.offset + 12]) : 0.0f, nFloats == 12 ? loadBE(&sScratch[p.offset + 28]) : 0.0f,
                     nFloats == 12 ? loadBE(&sScratch[p.offset + 44]) : 0.0f, p.key && !sTagLogChains ? "" : " chain ",
                     p.key && !sTagLogChains ? "" : sTagChain.c_str());
    sPatches.push_back(p);
}

// --- colours ---
// A colour patch is one 32-bit word of the tape: a BP write (register byte + 24-bit value: TEV
// colour/konst register halves 0xE0-0xE7, fog colour 0xF2) or an XF word (channel ambient/material
// colours 0x100A-0x100D, a light's colour 0x600 + 16 n + 3, RGBA8). Patch::stride holds the format
// (kColorBp, kColorFog, kColorRgba8) so only the same register pairs.
constexpr u16 kColorBp = 1, kColorFog = 2, kColorRgba8 = 3;

// `light`: index + 1 of the Light value (GXLoadLightObjImm) this colour word belongs to (0: none).
void noteColor(u32 off, u16 format, u32 reg, u32 light = 0) {
    Patch p{off, 1, Kind::Color};
    p.stride = format;
    p.source = reg;
    p.site = sCallLr;
    if (light && sPatches[light - 1].key) {
        // A light's colour is part of that light (the light object + slot, its own identity even
        // outside any object scope): Bogmire's arena lightning fades the scene light's colour from
        // white to black over a second, loaded untagged per view; keyed by scope it was key 0 and
        // stepped at 30 Hz (sweep C).
        p.key = mix(sPatches[light - 1].key ^ 0xC01015C01015ull ^ u64(reg) << 20);
        p.cat = sPatches[light - 1].cat;
        p.slot = light;
    } else {
        p.key = valueKey(Kind::Color, reg, 0, p.cat);
        if (!p.key && sOrtho) {
            // An untagged 2D colour (a full-screen fade overlay, a fill box): the structural scope, call
            // site, register and occurrence; it blends only with a draw of the same kind (replayPair).
            const TagFrame* t = topTag();
            const u64 k = mix((t ? t->structural : rootInst()) ^ u64(Kind::Color) << 56 ^ u64(sCallLr) << 16 ^ u64(reg) ^ 0x2DC0ull << 40);
            p.key = mix(k ^ u64(occurrence(sValOcc, k) + 1) << 40);
            p.slot = 0;
            p.posOffset = 1;  // (marks an untagged 2D colour)
        }
    }
    if (sOrtho) p.role = 2;  // 2D layout: as captured
    p.inScope = sTagDepth > 0 || sVirtActive;
    p.tgroup = sTextCur;
    sPatches.push_back(p);
}

void noteColors(u32 at, u32 lightObj) {
    if (at == ~0u || !capturing() || sNoBlend + sFrameLock > 0) return;
    const u32 end = sBase + AuroraCapturePos();
    u32 light = 0;  // the Light value just noted for lightObj (index + 1)
    if (lightObj && !sPatches.empty() && sPatches.back().kind == Kind::Light && sPatches.back().source == lightObj)
        light = u32(sPatches.size());
    for (u32 i = at; i < end;) {
        const u8 op = sScratch[i];
        if (op == 0x61 && i + 5 <= end) {  // BP write
            const u8 reg = sScratch[i + 1];
            if (reg >= 0xE0 && reg <= 0xE7) noteColor(i + 1, kColorBp, reg);
            else if (reg == 0xF2) noteColor(i + 1, kColorFog, reg);
            i += 5;
        } else if (op == 0x10 && i + 5 <= end) {  // XF write: header (count - 1) << 16 | address
            const u32 h = u32(sScratch[i + 1]) << 24 | u32(sScratch[i + 2]) << 16 | u32(sScratch[i + 3]) << 8 | sScratch[i + 4];
            const u32 n = (h >> 16) + 1, addr = h & 0xFFFF;
            if (i + 5 + 4 * n > end) break;
            for (u32 k = 0; k < n; k++) {
                const u32 a = addr + k;
                if ((a >= 0x100A && a <= 0x100D) || (a >= 0x600 && a < 0x680 && (a & 0xF) == 3))
                    noteColor(i + 5 + 4 * k, kColorRgba8, a, a >= 0x600 && a < 0x680 ? light : 0);
            }
            i += 5 + 4 * n;
        } else if (op == 0x08 && i + 6 <= end) {  // CP write
            i += 6;
        } else if (op == 0x00) {  // NOP
            i += 1;
        } else {
            break;  // anything else: stop (only the writes the call itself made are expected)
        }
    }
}

namespace detail {
void noteDrawColor(u32 at, u32 reg, u8 type) {
    if (at == ~0u || !capturing() || sNoBlend + sFrameLock > 0 || at + 4 > sBase + AuroraCapturePos()) return;
    noteColor(at, type == 3 ? kColorRgba8 : type == 2 ? kColorFog : kColorBp, reg);
}
}  // namespace detail

// Blends colour word `a` toward `b` (big-endian bytes in the tape); false when it jumped (a blink,
// a flash, on/off: more than 96 of 255 in a channel) or changed type: then b stays.
bool blendColorWord(u16 format, const u8* a, const u8* b, float t, u8* out) {
    const auto be = [](const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; };
    const u32 va = be(a), vb = be(b);
    if (va == vb) return true;
    u32 o = vb;
    if (format == kColorBp) {
        if ((va >> 24) != (vb >> 24) || ((va >> 23) & 1) != ((vb >> 23) & 1)) return false;  // other register / colour vs konst
        const bool konst = (vb >> 23) & 1;
        // Two fields per write: bits 0-10 and 12-22 (s11 for colour registers, u8 for konst).
        int fa[2], fb[2];
        for (int f = 0; f < 2; f++) {
            const u32 sh = f ? 12 : 0;
            int x = int((va >> sh) & 0x7FF), y = int((vb >> sh) & 0x7FF);
            if (!konst) {
                if (x & 0x400) x -= 0x800;
                if (y & 0x400) y -= 0x800;
            }
            fa[f] = x, fb[f] = y;
            if (std::abs(x - y) > 96) return false;
        }
        o = vb & ~((0x7FFu) | (0x7FFu << 12));
        for (int f = 0; f < 2; f++) {
            const int v = int(std::lround(fa[f] + (fb[f] - fa[f]) * t));
            o |= (u32(v) & 0x7FF) << (f ? 12 : 0);
        }
    } else {
        // RGBA8 (XF) or fog RGB (BP 0xF2: the low 24 bits).
        const int nc = format == kColorFog ? 3 : 4;
        if (format == kColorFog && (va >> 24) != (vb >> 24)) return false;
        for (int c = 0; c < nc; c++) {
            const int x = int((va >> (8 * c)) & 0xFF), y = int((vb >> (8 * c)) & 0xFF);
            if (std::abs(x - y) > 96) return false;
        }
        for (int c = 0; c < nc; c++) {
            const int x = int((va >> (8 * c)) & 0xFF), y = int((vb >> (8 * c)) & 0xFF);
            const u32 v = u32(std::lround(x + (y - x) * t)) & 0xFF;
            o = (o & ~(0xFFu << (8 * c))) | v << (8 * c);
        }
    }
    out[0] = u8(o >> 24), out[1] = u8(o >> 16), out[2] = u8(o >> 8), out[3] = u8(o);
    return true;
}

namespace {
// --- 2D pictures whose layout animates ------------------------------------------------------------
// A 2D quad (direct positions and colours, role 2) paired by identity (structural scope, call site,
// occurrence, vertex layout, the texture it shows) whose positions changed between the two game
// frames: a message window opening or closing (fn_80044138's window picture, 480x120, grows from a
// thin bar to its full height over 3-4 game frames; at 30 Hz its edges stepped 60 px per game frame
// at 1080p while everything behind it moved smoothly), a panel sliding in. Its positions blend when
// it is the same picture: every vertex's texture coordinates identical in both frames (a meter that
// crops its texture, or one glyph paired with another, is not) and at least 48 units across in both
// frames (text glyphs are smaller: letters appear and lines scroll at the game frame, as on the
// console), every vertex moving by at most 96 units (a relayout jumps). Positions are s16 in the
// game's 640x480 layout: the in-betweens move in whole layout units (4.5 px at 2160 lines).
u32 s2DBlended = 0, s2DKept = 0;  // stats
FILE* s2DLog = [] {
    const char* v = std::getenv("LM_2D_LOG");  // per blended picture: tape, alpha, key, bounds a / b / shown
    return v && *v ? std::fopen(v, "w") : nullptr;
}();
const bool s2DOff = [] {
    const char* v = std::getenv("LM_INTERP_2D_LAYOUT");  // 0: 2D positions always as captured
    return v && *v == '0';
}();

f32 load2D(const u8* p, u32 type, u32 frac) {
    f32 v;
    switch (type) {
    case GX_U8: v = f32(p[0]); break;
    case GX_S8: v = f32(s8(p[0])); break;
    case GX_U16: v = f32(u16(p[0] << 8 | p[1])); break;
    case GX_S16: v = f32(s16(p[0] << 8 | p[1])); break;
    default: return loadBE(p);
    }
    return std::ldexp(v, -int(frac));
}
void store2D(u8* p, u32 type, u32 frac, f32 v) {
    if (type == GX_F32) return storeBE(p, v);
    const long r = std::lround(std::ldexp(v, int(frac)));
    switch (type) {
    case GX_U8: p[0] = u8(std::clamp(r, 0L, 255L)); break;
    case GX_S8: p[0] = u8(s8(std::clamp(r, -128L, 127L))); break;
    case GX_U16: {
        const u16 x = u16(std::clamp(r, 0L, 65535L));
        p[0] = u8(x >> 8), p[1] = u8(x);
        break;
    }
    default: {
        const u16 x = u16(s16(std::clamp(r, -32768L, 32767L)));
        p[0] = u8(x >> 8), p[1] = u8(x);
        break;
    }
    }
}
}  // namespace

// Blends the positions of 2D block p (tape b) toward its partner q (tape a) into sReplay; false
// when it is not the same picture moving (then it stays as captured).
bool blend2DLayout(const Tape& a, const Patch& q, s32 j, const Tape& b, const Patch& p, u32 i, float alpha) {
    if (s2DOff || !(p.posFmt & 0x100) || q.posFmt != p.posFmt || p.count != q.count || p.count < 3 || !p.uvBytes ||
        q.uvBytes != p.uvBytes || q.uvOffset != p.uvOffset || q.posOffset != p.posOffset)
        return false;
    const u32 type = p.posFmt & 7, frac = (p.posFmt >> 3) & 31;
    const u32 csize = type == GX_F32 ? 4 : type >= GX_U16 ? 2 : 1;
    const u32 comps = std::min<u32>(p.source / csize, 3);
    if (comps < 2) return false;
    float lo[2][2] = {{1e30f, 1e30f}, {1e30f, 1e30f}}, hi[2][2] = {{-1e30f, -1e30f}, {-1e30f, -1e30f}};
    float move = 0;
    for (u32 v = 0; v < p.count; v++) {
        const u8* va = a.bytes.data() + q.offset + v * q.stride;
        const u8* vb = b.bytes.data() + p.offset + v * p.stride;
        if (std::memcmp(va + q.uvOffset, vb + p.uvOffset, p.uvBytes) != 0) {
            s2DKept++;
            return false;
        }
        for (u32 c = 0; c < comps; c++) {
            const f32 x = load2D(va + q.posOffset + c * csize, type, frac), y = load2D(vb + p.posOffset + c * csize, type, frac);
            if (!std::isfinite(x) || !std::isfinite(y)) return false;
            move = std::max(move, std::fabs(x - y));
            if (c < 2) {
                lo[0][c] = std::min(lo[0][c], x), hi[0][c] = std::max(hi[0][c], x);
                lo[1][c] = std::min(lo[1][c], y), hi[1][c] = std::max(hi[1][c], y);
            }
        }
    }
    const float extA = std::max(hi[0][0] - lo[0][0], hi[0][1] - lo[0][1]), extB = std::max(hi[1][0] - lo[1][0], hi[1][1] - lo[1][1]);
    if (!(extA >= 48.0f && extB >= 48.0f && move <= 96.0f)) {
        s2DKept++;
        return false;
    }
    // A picture that changes size animates (opening, closing, pulsing). One that only moves
    // blends when it moved the same way in the game frame before (sliding in): a single jump to
    // another place (a menu cursor, a relayout) stays a jump, as on the console.
    const float d[2] = {(lo[1][0] + hi[1][0] - lo[0][0] - hi[0][0]) * 0.5f, (lo[1][1] + hi[1][1] - lo[0][1] - hi[0][1]) * 0.5f};
    if (b.vertStep) b.vertStep[3 * i].store(d[0], std::memory_order_relaxed), b.vertStep[3 * i + 1].store(d[1], std::memory_order_relaxed);
    const bool sized = std::fabs((hi[1][0] - lo[1][0]) - (hi[0][0] - lo[0][0])) > 0.5f || std::fabs((hi[1][1] - lo[1][1]) - (hi[0][1] - lo[0][1])) > 0.5f;
    if (!sized) {
        const float pd[2] = {a.vertStep ? a.vertStep[3 * j].load(std::memory_order_relaxed) : std::nanf(""),
                             a.vertStep ? a.vertStep[3 * j + 1].load(std::memory_order_relaxed) : std::nanf("")};
        const float pl = std::hypot(pd[0], pd[1]), dl = std::hypot(d[0], d[1]);
        if (!(std::isfinite(pl) && pl > 0.5f && pd[0] * d[0] + pd[1] * d[1] > 0.5f * pl * dl)) {
            s2DKept++;
            return false;
        }
    }
    for (u32 v = 0; v < p.count; v++) {
        const u8* va = a.bytes.data() + q.offset + v * q.stride + q.posOffset;
        const u8* vb = b.bytes.data() + p.offset + v * p.stride + p.posOffset;
        u8* out = sReplay.data() + p.offset + v * p.stride + p.posOffset;
        for (u32 c = 0; c < comps; c++) {
            const f32 x = load2D(va + c * csize, type, frac), y = load2D(vb + c * csize, type, frac);
            store2D(out + c * csize, type, frac, x + (y - x) * alpha);
        }
    }
    if (s2DLog) {
        float lo2[2] = {1e30f, 1e30f}, hi2[2] = {-1e30f, -1e30f};
        for (u32 v = 0; v < p.count; v++)
            for (u32 c = 0; c < 2; c++) {
                const f32 o = load2D(sReplay.data() + p.offset + v * p.stride + p.posOffset + c * csize, type, frac);
                lo2[c] = std::min(lo2[c], o), hi2[c] = std::max(hi2[c], o);
            }
        std::fprintf(s2DLog, "%llu %.3f %016llX a %.1f %.1f %.1f %.1f b %.1f %.1f %.1f %.1f shown %.1f %.1f %.1f %.1f site %08X f%u\n",
                     (unsigned long long)b.seq, alpha, (unsigned long long)p.key, lo[0][0], lo[0][1], hi[0][0], hi[0][1], lo[1][0],
                     lo[1][1], hi[1][0], hi[1][1], lo2[0], lo2[1], hi2[0], hi2[1], p.site, b.gameFrame);
    }
    s2DBlended++;
    return true;
}

// --- text boxes as one layout (docs/INTERPOLATION.md "Text boxes at 120") -------------------------
// Everything a text box draws in one game frame (pushTextGroup: fn_80044138's message windows, the
// room name) is replayed as one 2D layout instead of value by value:
//  - the box picture (the group's first textured block at least 48 units across: fn_8003B054's
//    480x120 window) gives the layout transform M: while it opens, closes or slides, its rectangle
//    is blended and M maps b's rectangle onto the blended one; M is applied to every 2D position
//    matrix loaded inside the group, so the icon, text, arrow and choices move and scale with it;
//  - the other blocks (glyphs: one quad per character from JUTResFont::drawChar_scale, the speaker
//    icon, the "more" arrow) are matched to the previous game frame's by sequence alignment of what
//    they show (texture, texture coordinates, vertex count): J2DPrint draws a page's text from its
//    start every frame, so the text so far is the same sequence with new letters at its end;
//  - text that scrolls (a new line pushes the page up over several game frames: the matched glyphs
//    move by one common offset D) is drawn D x (1 - alpha) behind b's place, letters new in b
//    included: the whole text layer moves smoothly and keeps its own layout;
//  - a letter new in b fades in: its vertex alpha is scaled by the present's alpha (0 at the game
//    frame that adds it, its full value at the next one); letters already shown stay solid; a page
//    change or a box that closes switches at the game frame (old text never blends into new text);
//  - colours of matched blocks blend (unless a channel jumps by more than 96).
// LM_TEXTBOX_LOG=<file>: per replay and group the box rectangles, M, D and the block counts;
// LM_TEXTBOX_LOG_BLOCKS=1 adds every block (matched block, local position shown, alpha shown).
namespace {
FILE* const sTextLog = [] {
    const char* v = std::getenv("LM_TEXTBOX_LOG");
    return v && *v ? std::fopen(v, "w") : nullptr;
}();
const bool sTextLogBlocks = std::getenv("LM_TEXTBOX_LOG_BLOCKS") && *std::getenv("LM_TEXTBOX_LOG_BLOCKS") == '1';
u32 sTextGroupsBlended = 0, sTextFaded = 0, sTextScrolled = 0, sTextBoxMoved = 0;

// The layout x/y of a block drawn with a 2D position matrix (rows x and y of the 3x4 matrix in its
// current slot at its GXBegin; identity when none was loaded in this game frame).
struct Affine2D {
    f32 m[6] = {1, 0, 0, 0, 1, 0};
    void apply(const float in[2], float out[2]) const {
        const float x = in[0], y = in[1];
        out[0] = m[0] * x + m[1] * y + m[2], out[1] = m[3] * x + m[4] * y + m[5];
    }
    bool invert(const float in[2], float out[2], bool linear = false) const {
        const float det = m[0] * m[4] - m[1] * m[3];
        if (std::fabs(det) < 1e-8f) return false;
        const float x = in[0] - (linear ? 0 : m[2]), y = in[1] - (linear ? 0 : m[5]);
        out[0] = (m[4] * x - m[1] * y) / det, out[1] = (m[0] * y - m[3] * x) / det;
        return true;
    }
};

struct TextBlock {
    u32 i;               // patch index
    float lo[2], hi[2];  // bounds of its positions (local coordinates)
    float at[2];         // its top-left in layout coordinates, relative to its panel's box
    u64 pic;             // what it shows: texture/layout, vertex count, texture coordinates
    u64 sig;             // pic and its size (glyphs and icons: one character cell, one picture)
    bool box;            // a box picture: starts a panel
    u32 panel;           // index into the group's panels (0: blocks before the first box)
    Affine2D A;          // its position matrix
    u64 key = 0;         // what it is drawn with (texture, vertex layout)
    float rect[2][2];    // its layout rectangle
};
struct TextPanel {
    s32 box = -1;            // block index of its (first) box picture (-1: none)
    float rect[2][2] = {};   // the box's layout rectangle (lo, hi): the union of its pieces
    u64 key = 0;             // what the box is drawn with (texture, vertex layout)
};

bool is2DBlock(const Patch& p) { return p.kind == Kind::Verts && p.role == 2 && (p.posFmt & 0x100) && p.count >= 1; }

u32 posComps(const Patch& p, u32& csize) {
    const u32 type = p.posFmt & 7;
    csize = type == GX_F32 ? 4 : type >= GX_U16 ? 2 : 1;
    return std::min<u32>(p.source / csize, 3);
}

Affine2D blockMatrix(const Tape& t, const Patch& p) {
    Affine2D a;
    if (p.mtxPatch >= 0 && u32(p.mtxPatch) < t.patches.size()) {
        const Patch& q = t.patches[p.mtxPatch];
        if (q.kind == Kind::PosMtx && q.count == 12) {
            a.m[0] = val(t, q, 0), a.m[1] = val(t, q, 1), a.m[2] = val(t, q, 3);
            a.m[3] = val(t, q, 4), a.m[4] = val(t, q, 5), a.m[5] = val(t, q, 7);
        }
    }
    return a;
}

void layoutRect(const Affine2D& m, const float blo[2], const float bhi[2], float lo[2], float hi[2]) {
    lo[0] = lo[1] = 1e30f, hi[0] = hi[1] = -1e30f;
    for (int c = 0; c < 4; c++) {
        const float in[2] = {c & 1 ? bhi[0] : blo[0], c & 2 ? bhi[1] : blo[1]};
        float o[2];
        m.apply(in, o);
        lo[0] = std::min(lo[0], o[0]), hi[0] = std::max(hi[0], o[0]);
        lo[1] = std::min(lo[1], o[1]), hi[1] = std::max(hi[1], o[1]);
    }
}

bool textBlock(const Tape& t, u32 i, TextBlock& out) {
    const Patch& p = t.patches[i];
    u32 csize;
    const u32 comps = posComps(p, csize);
    if (comps < 2 || p.offset + u32(p.count) * p.stride > t.bytes.size()) return false;
    const u32 type = p.posFmt & 7, frac = (p.posFmt >> 3) & 31;
    out.i = i;
    out.lo[0] = out.lo[1] = 1e30f, out.hi[0] = out.hi[1] = -1e30f;
    u64 h = mix(u64(p.count) << 32 ^ (p.bundle < t.bundleIds.size() ? t.bundleIds[p.bundle] : 0) ^ 0x7E47ull);
    for (u32 v = 0; v < p.count; v++) {
        const u8* vp = t.bytes.data() + p.offset + v * p.stride;
        for (u32 c = 0; c < 2; c++) {
            const f32 x = load2D(vp + p.posOffset + c * csize, type, frac);
            if (!std::isfinite(x)) return false;
            out.lo[c] = std::min(out.lo[c], x), out.hi[c] = std::max(out.hi[c], x);
        }
        for (u32 k = 0; k < p.uvBytes; k++) h = mix(h ^ u64(vp[p.uvOffset + k]) << (8 * (k & 7)) ^ u64(v) << 60);
    }
    out.pic = h;
    out.sig = mix(h ^ u64(std::lround((out.hi[0] - out.lo[0]) * 8)) << 20 ^ u64(std::lround((out.hi[1] - out.lo[1]) * 8)));
    out.box = p.uvBytes && p.count == 4 && std::max(out.hi[0] - out.lo[0], out.hi[1] - out.lo[1]) >= 48.0f;
    out.A = blockMatrix(t, p);
    out.panel = 0;
    out.at[0] = out.at[1] = 0;
    return true;
}

// The 2D blocks of group g (index) of tape t, split into panels: every box picture (a textured quad
// at least 48 units across: the message window, a piece of the choice box, a piece of the Game Boy
// Horror's frame) starts a panel; the pieces drawn right after it with the same texture that touch
// it (the choice box is nine pieces, opening by moving and stretching them) extend its box, and the
// blocks drawn after them (its text, icon, arrow) belong to it.
void groupBlocks(const Tape& t, u32 g, std::vector<TextBlock>& blocks, std::vector<TextPanel>& panels) {
    blocks.clear();
    panels.assign(1, TextPanel{});
    const TextGroupRec& r = t.textGroups[g];
    bool open = false;  // the current panel's box can still grow (only its pieces drawn since)
    for (u32 i = r.first; i < r.end && i < t.patches.size(); i++) {
        const Patch& p = t.patches[i];
        if (p.tgroup != g + 1 || !is2DBlock(p)) continue;
        TextBlock tb;
        if (!textBlock(t, i, tb)) continue;
        const u64 key = mix((p.bundle < t.bundleIds.size() ? t.bundleIds[p.bundle] : 0) ^ u64(p.site) << 32);
        float lo[2], hi[2];
        layoutRect(tb.A, tb.lo, tb.hi, lo, hi);
        tb.key = key;
        std::memcpy(tb.rect[0], lo, sizeof(lo));
        std::memcpy(tb.rect[1], hi, sizeof(hi));
        TextPanel& cur = panels.back();
        if (r.oneLayout) {
            // One picture: everything moves as its first box picture does (its other pieces come
            // and go: the call screen's frame appears a game frame after the bezel).
            if (tb.box) {
                if (panels.size() == 1) {
                    TextPanel pn;
                    pn.box = s32(blocks.size());
                    pn.key = tb.pic;  // (another first picture: no blend)
                    std::memcpy(pn.rect[0], lo, sizeof(lo));
                    std::memcpy(pn.rect[1], hi, sizeof(hi));
                    panels.push_back(pn);
                    for (TextBlock& prev : blocks) prev.panel = 1;
                } else {
                    tb.box = true;
                }
            }
            tb.panel = u32(panels.size() - 1);
            blocks.push_back(tb);
            continue;
        }
        // A piece touches or overlaps the box (and is drawn by the same code with the same texture:
        // the speaker icon inside the window is content).
        const auto touches = [](const float r[2][2], const float l[2], const float h[2]) {
            return l[0] <= r[1][0] + 1 && h[0] >= r[0][0] - 1 && l[1] <= r[1][1] + 1 && h[1] >= r[0][1] - 1;
        };
        if (open && p.uvBytes && key == cur.key && touches(cur.rect, lo, hi)) {
            tb.box = true;  // another piece of the box
            for (int c = 0; c < 2; c++) cur.rect[0][c] = std::min(cur.rect[0][c], lo[c]), cur.rect[1][c] = std::max(cur.rect[1][c], hi[c]);
        } else if (tb.box) {
            TextPanel pn;
            pn.box = s32(blocks.size());
            pn.key = key;
            std::memcpy(pn.rect[0], lo, sizeof(lo));
            std::memcpy(pn.rect[1], hi, sizeof(hi));
            // Pieces of this box drawn just before it (the choice box's top-left corner, smaller
            // than a box picture) belong to it too.
            u32 first = u32(blocks.size());
            while (first > 0) {
                TextBlock& prev = blocks[first - 1];
                if (prev.box || prev.key != key || !t.patches[prev.i].uvBytes || !touches(pn.rect, prev.rect[0], prev.rect[1])) break;
                for (int c = 0; c < 2; c++)
                    pn.rect[0][c] = std::min(pn.rect[0][c], prev.rect[0][c]), pn.rect[1][c] = std::max(pn.rect[1][c], prev.rect[1][c]);
                first--;
            }
            panels.push_back(pn);
            for (u32 k = first; k < blocks.size(); k++) {
                blocks[k].box = true;
                blocks[k].panel = u32(panels.size() - 1);
            }
            if (first < blocks.size()) panels.back().box = s32(first);
            open = true;
        } else {
            open = false;
        }
        tb.panel = u32(panels.size() - 1);
        blocks.push_back(tb);
    }
    for (TextBlock& tb : blocks) {
        float lay[2];
        tb.A.apply(tb.lo, lay);
        tb.at[0] = lay[0] - panels[tb.panel].rect[0][0], tb.at[1] = lay[1] - panels[tb.panel].rect[0][1];
    }
}

// Panels of b paired with panels of a: panel 0 with panel 0, then in order by what the box is drawn
// with.
void matchPanels(const std::vector<TextPanel>& pa, const std::vector<TextPanel>& pb, std::vector<s32>& out) {
    out.assign(pb.size(), -1);
    out[0] = 0;
    u32 next = 1;
    for (u32 k = 1; k < pb.size(); k++)
        for (u32 q = next; q < pa.size(); q++)
            if (pa[q].key == pb[k].key) {
                out[k] = s32(q);
                next = q + 1;
                break;
            }
}

// Which block of a's panel each block of b's panel shows again (b.textMatch / textKind / textD).
// Blocks pair by what they show (texture, texture coordinates, size) and where they are in their box:
//  1. at the same place (letters already shown, the speaker icon);
//  2. moved by D, the most common offset between a block of b and a block of a showing the same
//     (at least 3 blocks: the page scrolling up by a line over a few game frames);
//  3. on its own by at most 16 units, when the picture is unique in both panels and most of the
//     text paired (the same page): e.g. the "more" arrow bobbing.
// Everything else is new in b (a page change: the old text is gone, the new one fades in).
void matchText(const std::vector<TextBlock>& ba, const std::vector<TextPanel>& pa, u32 qa, const Tape& b,
               const std::vector<TextBlock>& bb, const std::vector<TextPanel>& pb, u32 qb) {
    // The box's pieces, in order (their colours blend; their places follow the panel's layout).
    {
        std::vector<u32> xa, xb;
        for (u32 k = 0; k < ba.size(); k++)
            if (ba[k].panel == qa && ba[k].box) xa.push_back(k);
        for (u32 k = 0; k < bb.size(); k++)
            if (bb[k].panel == qb && bb[k].box) xb.push_back(k);
        for (size_t k = 0; k < xb.size() && k < xa.size(); k++)
            if (ba[xa[k]].pic == bb[xb[k]].pic || xa.size() == xb.size()) {
                b.textMatch[bb[xb[k]].i] = s32(ba[xa[k]].i);
                b.textKind[bb[xb[k]].i] = 1;
            }
    }
    const auto key = [](const TextBlock& t, float dx, float dy) {
        return mix(t.sig ^ u64(u32(std::lround((t.at[0] - dx) * 16))) << 32 ^ u64(u32(std::lround((t.at[1] - dy) * 16))));
    };
    std::unordered_multimap<u64, u32> where;  // a's blocks by what and where
    std::unordered_map<u64, u32> countA, countB;
    std::vector<u32> ia, ib;
    for (u32 k = 0; k < ba.size(); k++)
        if (ba[k].panel == qa && !ba[k].box) {
            ia.push_back(k);
            where.emplace(key(ba[k], 0, 0), k);
            countA[ba[k].sig]++;
        }
    for (u32 k = 0; k < bb.size(); k++)
        if (bb[k].panel == qb && !bb[k].box) ib.push_back(k), countB[bb[k].sig]++;
    std::unordered_set<u32> used;
    u32 paired = 0;
    const auto take = [&](u32 kb, float dx, float dy, u8 kind) {
        auto [lo, hi] = where.equal_range(key(bb[kb], dx, dy));
        for (auto it = lo; it != hi; ++it) {
            if (used.count(it->second) || ba[it->second].sig != bb[kb].sig) continue;
            used.insert(it->second);
            b.textMatch[bb[kb].i] = s32(ba[it->second].i);
            b.textKind[bb[kb].i] = kind;
            paired++;
            return true;
        }
        return false;
    };
    std::vector<u32> rest;
    for (const u32 kb : ib)
        if (!take(kb, 0, 0, 1)) rest.push_back(kb);
    // The text layer's offset: the most common offset to an unused block of a showing the same.
    std::map<std::pair<s32, s32>, u32> hist;
    if (rest.size() * ia.size() <= (4u << 20))
        for (const u32 kb : rest)
            for (const u32 k : ia) {
                if (used.count(k) || ba[k].sig != bb[kb].sig) continue;
                const float dx = bb[kb].at[0] - ba[k].at[0], dy = bb[kb].at[1] - ba[k].at[1];
                if (std::hypot(dx, dy) <= 64.0f) hist[{s32(std::lround(dx * 16)), s32(std::lround(dy * 16))}]++;
            }
    u32 best = 0;
    float D[2] = {0, 0};
    for (const auto& [d, n] : hist)
        if (n > best) best = n, D[0] = d.first / 16.0f, D[1] = d.second / 16.0f;
    // A scroll keeps most of a's text (J2DPrint redraws the page from its start: a's lines 2..n are
    // b's lines 1..n-1), so the offset must explain a good share of a's text not already in place.
    // A page change whose first letters also occur on a's second line ("So" of "Sounds": 4 blocks
    // of a 118-block page) slid the new page's letters up from there (sweep A3; real scrolls match
    // 50-60 of 110-128 blocks).
    const u32 unusedA = u32(ia.size() > used.size() ? ia.size() - used.size() : 0);
    if (best < 3 || 4 * best < unusedA) best = 0, D[0] = D[1] = 0;
    std::vector<u32> rest2;
    for (const u32 kb : rest)
        if (best < 3 || !take(kb, D[0], D[1], 2)) rest2.push_back(kb);
    for (const u32 kb : ib) b.textD[bb[kb].i] = {D[0], D[1]};
    // On their own: unique pictures of the same page.
    if (2 * paired >= ib.size())
        for (const u32 kb : rest2) {
            if (countB[bb[kb].sig] != 1 || countA[bb[kb].sig] != 1) continue;
            for (const u32 k : ia) {
                if (used.count(k) || ba[k].sig != bb[kb].sig) continue;
                if (std::hypot(bb[kb].at[0] - ba[k].at[0], bb[kb].at[1] - ba[k].at[1]) <= 16.0f) {
                    used.insert(k);
                    b.textMatch[bb[kb].i] = s32(ba[k].i);
                    b.textKind[bb[kb].i] = 3;
                }
                break;
            }
        }
}

void readPos(const Tape& t, const Patch& p, u32 v, float xy[2]) {
    u32 csize;
    posComps(p, csize);
    const u8* o = t.bytes.data() + p.offset + v * p.stride + p.posOffset;
    for (u32 c = 0; c < 2; c++) xy[c] = load2D(o + c * csize, p.posFmt & 7, (p.posFmt >> 3) & 31);
}
void readShown(const Patch& p, u32 v, float xy[2]) {
    u32 csize;
    posComps(p, csize);
    const u8* o = sReplay.data() + p.offset + v * p.stride + p.posOffset;
    for (u32 c = 0; c < 2; c++) xy[c] = load2D(o + c * csize, p.posFmt & 7, (p.posFmt >> 3) & 31);
}
void writePos(const Patch& p, u32 v, const float xy[2]) {
    u32 csize;
    posComps(p, csize);
    u8* o = sReplay.data() + p.offset + v * p.stride + p.posOffset;
    for (u32 c = 0; c < 2; c++) store2D(o + c * csize, p.posFmt & 7, (p.posFmt >> 3) & 31, xy[c]);
}

// Colours of block p (b) blended toward q (a) unless a channel jumps; false: left as b's.
bool blendBlockColours(const Tape& a, const Patch& q, const Tape& b, const Patch& p, float alpha) {
    if (!p.clrBytes || q.clrBytes != p.clrBytes || q.clrOffset != p.clrOffset || q.count != p.count) return false;
    for (u32 v = 0; v < p.count; v++)
        for (u32 c = 0; c < p.clrBytes; c++)
            if (std::abs(int(a.bytes[q.offset + v * q.stride + q.clrOffset + c]) - int(b.bytes[p.offset + v * p.stride + p.clrOffset + c])) > 96)
                return false;
    for (u32 v = 0; v < p.count; v++)
        for (u32 c = 0; c < p.clrBytes; c++) {
            const int x = a.bytes[q.offset + v * q.stride + q.clrOffset + c], y = b.bytes[p.offset + v * p.stride + p.clrOffset + c];
            sReplay[p.offset + v * p.stride + p.clrOffset + c] = u8(std::lround(x + (y - x) * alpha));
        }
    return true;
}

// x' = s x + t per axis.
struct Layout2D {
    float sx = 1, tx = 0, sy = 1, ty = 0;
    bool moved = false;
};
}  // namespace

void replayTextGroups(const Tape& a, const Tape& b, float alpha) {
    if (b.textGroups.empty()) return;
    // The group of a each group of b is (same drawing function, same order among its groups).
    const auto partnerGroup = [&](u32 g) -> s32 {
        u32 ord = 0;
        for (u32 k = 0; k < g; k++) ord += b.textGroups[k].id == b.textGroups[g].id;
        for (u32 h = 0, n = 0; h < a.textGroups.size(); h++)
            if (a.textGroups[h].id == b.textGroups[g].id && n++ == ord) return s32(h);
        return -1;
    };
    // Built once per pair (a tape is replayed several times, always with the same pairedWith).
    struct GroupCache {
        s32 ga = -1;
        std::vector<TextBlock> ba, bb;
        std::vector<TextPanel> pa, pb;
        std::vector<s32> panelToA;
    };
    struct Cache {
        std::vector<GroupCache> groups;
        // per position matrix / projection patch: the text groups (index + 1, 0 none) whose 2D blocks use it
        std::unordered_map<s32, std::vector<u16>> mtxUsers, projUsers;
    };
    if (!b.textCache) {
        auto cache = std::make_shared<Cache>();
        b.textMatch.assign(b.patches.size(), -1);
        b.textKind.assign(b.patches.size(), 0);
        b.textD.assign(b.patches.size(), {0.0f, 0.0f});
        cache->groups.resize(b.textGroups.size());
        for (u32 g = 0; g < b.textGroups.size(); g++) {
            GroupCache& c = cache->groups[g];
            c.ga = partnerGroup(g);
            groupBlocks(b, g, c.bb, c.pb);
            if (c.ga >= 0) {
                groupBlocks(a, u32(c.ga), c.ba, c.pa);
                matchPanels(c.pa, c.pb, c.panelToA);
                for (u32 k = 0; k < c.pb.size(); k++)
                    if (c.panelToA[k] >= 0) matchText(c.ba, c.pa, u32(c.panelToA[k]), b, c.bb, c.pb, k);
            } else {
                c.panelToA.assign(c.pb.size(), -1);
            }
        }
        for (const Patch& p : b.patches) {
            if (!is2DBlock(p)) continue;
            for (auto [map, key] : {std::pair{&cache->mtxUsers, p.mtxPatch}, std::pair{&cache->projUsers, p.projPatch}}) {
                auto& v = (*map)[key];
                if (std::find(v.begin(), v.end(), p.tgroup) == v.end()) v.push_back(p.tgroup);
            }
        }
        b.textCache = cache;
    }
    const Cache& cache = *static_cast<const Cache*>(b.textCache.get());
    static std::vector<Layout2D> lay;
    for (u32 g = 0; g < b.textGroups.size(); g++) {
        const TextGroupRec& rg = b.textGroups[g];
        const GroupCache& gc = cache.groups[g];
        const s32 ga = gc.ga;
        const std::vector<TextBlock>& ba = gc.ba;
        const std::vector<TextBlock>& bb = gc.bb;
        const std::vector<TextPanel>& pa = gc.pa;
        const std::vector<TextPanel>& pb = gc.pb;
        const std::vector<s32>& panelToA = gc.panelToA;
        // Each box: its rectangle in a and b (layout coordinates) and the transform mapping b's
        // rectangle onto the one shown.
        lay.assign(pb.size(), Layout2D{});
        static std::vector<std::array<float, 4>> rects;  // per panel: the blended rectangle (lo x, lo y, hi x, hi y)
        rects.assign(pb.size(), {});
        u32 moved = 0;
        for (u32 k = 1; k < pb.size(); k++) {
            const s32 q = panelToA[k];
            if (q < 0) continue;
            const float(&rb)[2][2] = pb[k].rect;
            const float(&ra)[2][2] = pa[q].rect;
            const u32 ib = bb[pb[k].box].i, ia = ba[pa[q].box].i;
            const float d[2] = {(rb[0][0] + rb[1][0] - ra[0][0] - ra[1][0]) * 0.5f, (rb[0][1] + rb[1][1] - ra[0][1] - ra[1][1]) * 0.5f};
            if (b.vertStep) b.vertStep[3 * ib].store(d[0], std::memory_order_relaxed), b.vertStep[3 * ib + 1].store(d[1], std::memory_order_relaxed);
            float move = 0;
            for (int c = 0; c < 2; c++) move = std::max({move, std::fabs(ra[0][c] - rb[0][c]), std::fabs(ra[1][c] - rb[1][c])});
            const bool sized = std::fabs((rb[1][0] - rb[0][0]) - (ra[1][0] - ra[0][0])) > 0.5f ||
                               std::fabs((rb[1][1] - rb[0][1]) - (ra[1][1] - ra[0][1])) > 0.5f;
            bool blend = move > 0.01f && move <= 160.0f;
            if (blend && !sized && move > 32.0f) {
                // Only moved, far: a slide (it moved the same way in the game frame before) blends,
                // a single jump stays a jump. (Small moves blend: the Game Boy Horror rising by
                // 1-2 units per game frame as E. Gadd's call starts.)
                const float pd[2] = {a.vertStep ? a.vertStep[3 * ia].load(std::memory_order_relaxed) : std::nanf(""),
                                     a.vertStep ? a.vertStep[3 * ia + 1].load(std::memory_order_relaxed) : std::nanf("")};
                const float pl = std::hypot(pd[0], pd[1]), dl = std::hypot(d[0], d[1]);
                blend = std::isfinite(pl) && pl > 0.5f && pd[0] * d[0] + pd[1] * d[1] > 0.5f * pl * dl;
            }
            if (!blend) continue;
            for (int e = 0; e < 2; e++)
                for (int c = 0; c < 2; c++) rects[k][2 * e + c] = ra[e][c] + (rb[e][c] - ra[e][c]) * alpha;
            lay[k].moved = true;
            moved++;
        }
        const auto setLayout = [&](u32 k) {
            Layout2D& L = lay[k];
            const float(&rb)[2][2] = pb[k].rect;
            const auto& rs = rects[k];
            const float wb = rb[1][0] - rb[0][0], hb = rb[1][1] - rb[0][1];
            L.sx = wb > 0.5f ? (rs[2] - rs[0]) / wb : 1.0f;
            L.sy = hb > 0.5f ? (rs[3] - rs[1]) / hb : 1.0f;
            L.tx = rs[0] - L.sx * rb[0][0];
            L.ty = rs[1] - L.sy * rb[0][1];
        };
        for (u32 k = 1; k < pb.size(); k++)
            if (lay[k].moved) setLayout(k);
        // How a panel's transform reaches its blocks: through the position matrix loaded inside the
        // group or the orthographic projection they are drawn with, when every block drawn with it
        // moves alike (exact for any vertex format: the Game Boy Horror's s16 pieces rise by 1 unit
        // per game frame), else on each block's vertices (whole units for s16 positions).
        const auto sameLayout = [](const Layout2D& x, const Layout2D& y) {
            return x.moved == y.moved && (!x.moved || (std::fabs(x.sx - y.sx) < 1e-5f && std::fabs(x.sy - y.sy) < 1e-5f &&
                                                       std::fabs(x.tx - y.tx) < 1e-3f && std::fabs(x.ty - y.ty) < 1e-3f));
        };
        std::unordered_map<s32, s32> mtxOwner, projOwner;  // patch -> panel whose transform it takes (-2: none)
        for (u32 k = 0; k < bb.size(); k++) {
            const Patch& p = b.patches[bb[k].i];
            for (auto [map, key] : {std::pair{&mtxOwner, p.mtxPatch}, std::pair{&projOwner, p.projPatch}}) {
                auto it = map->find(key);
                if (it == map->end()) (*map)[key] = s32(bb[k].panel);
                else if (it->second >= 0 && !sameLayout(lay[it->second], lay[bb[k].panel])) it->second = -2;
            }
        }
        // (and nothing outside the group drawn with them)
        for (auto [owners, users] : {std::pair{&mtxOwner, &cache.mtxUsers}, std::pair{&projOwner, &cache.projUsers}})
            for (auto& [key, who] : *owners)
                if (auto it = users->find(key); it != users->end() && (it->second.size() > 1 || it->second[0] != g + 1)) who = -2;
        // 0: matrix, 1: projection, 2: vertices
        const auto route = [&](const Patch& p) {
            const s32 mp = p.mtxPatch, pp = p.projPatch;
            if (mp >= s32(rg.first) && u32(mp) < b.patches.size() && b.patches[mp].tgroup == g + 1 && b.patches[mp].kind == Kind::PosMtx &&
                b.patches[mp].count == 12 && mtxOwner.count(mp) && mtxOwner.at(mp) >= 0)
                return 0;
            if (pp >= 0 && u32(pp) < b.patches.size() && b.patches[pp].kind == Kind::Projection && b.patches[pp].count == 6 &&
                b.bytes[b.patches[pp].offset + 27] == GX_ORTHOGRAPHIC && projOwner.count(pp) && projOwner.at(pp) >= 0)
                return 1;
            return 2;
        };
        const auto viaMatrix = [&](const Patch& p) { return route(p) != 2; };
        // Pictures with whole-unit positions (s16) moved on their vertices can only land on whole
        // units: their panel then takes the rounded rectangle, so everything in it moves by the same
        // whole units (exactly together when it only moves; while it opens or closes, pieces inside
        // the box can still be half a unit apart).
        for (u32 k = 1; k < pb.size(); k++) {
            if (!lay[k].moved) continue;
            bool intVerts = false;
            for (const TextBlock& tb : bb) {
                const Patch& bp = b.patches[tb.i];
                intVerts |= tb.panel == k && !viaMatrix(bp) && (bp.posFmt & 7) != GX_F32 && ((bp.posFmt >> 3) & 31) == 0;
            }
            if (!intVerts) continue;
            for (float& v : rects[k]) v = std::round(v);
            setLayout(k);
        }
        sTextBoxMoved += moved;
        if (moved) {
            std::unordered_set<s32> done;
            for (const TextBlock& tb : bb) {
                const Patch& bp = b.patches[tb.i];
                const Layout2D& L = lay[tb.panel];
                const int r = route(bp);
                if (!L.moved || r == 2) continue;
                const s32 at = r == 0 ? bp.mtxPatch : bp.projPatch;
                if (!done.insert(at).second) continue;
                const Patch& p = b.patches[at];
                if (r == 0) {
                    f32 m[12];
                    for (int k = 0; k < 12; k++) m[k] = val(b, p, k);
                    for (int k = 0; k < 4; k++) m[k] *= L.sx, m[4 + k] *= L.sy;
                    m[3] += L.tx, m[7] += L.ty;
                    for (int k = 0; k < 8; k++) storeBE(sReplay.data() + p.offset + 4 * k, m[k]);
                } else {
                    // Orthographic: clip x = p0 x + p1, clip y = p2 y + p3.
                    const f32 p0 = val(b, p, 0), p1 = val(b, p, 1), p2 = val(b, p, 2), p3 = val(b, p, 3);
                    storeBE(sReplay.data() + p.offset, p0 * L.sx);
                    storeBE(sReplay.data() + p.offset + 4, p1 + p0 * L.tx);
                    storeBE(sReplay.data() + p.offset + 8, p2 * L.sy);
                    storeBE(sReplay.data() + p.offset + 12, p3 + p2 * L.ty);
                }
            }
        }
        // Viewports and scissor boxes: the panel of the last block drawn before them.
        if (moved)
            for (const TextView& v : b.textViews) {
                if (v.group != g + 1) continue;
                u32 panel = 0;
                for (const TextBlock& tb : bb)
                    if (b.patches[tb.i].offset < v.at) panel = tb.panel;
                const Layout2D& L = lay[panel];
                if (!L.moved) continue;
                const float px = v.f * L.tx + v.c * (1 - L.sx);
                // A full-frame viewport or scissor box (the GBH's status page resets them to
                // 640x480 before drawing) is not a sub-view of the panel: the blocks drawn under it
                // are moved by M already, and moving it as well put them 2x the panel's offset off
                // (the green page jumped back up to 4 units at each game frame while the bezel slid;
                // sweep A3). Only boxes smaller than the frame (the call portrait) move with it.
                {
                    const u8* o = b.bytes.data() + v.at;
                    if (!v.scissor && std::fabs(loadBE(o + 5 + 4)) >= 239.0f) continue;
                    if (v.scissor) {
                        const auto fld = [](const u8* w, int sh) { return int(((u32(w[2]) << 16 | u32(w[3]) << 8 | w[4]) >> sh) & 0x7FF); };
                        if (fld(o + 5, 0) - fld(o, 0) + 1 >= 470) continue;
                    }
                }
                if (!v.scissor) {
                    u8* o = sReplay.data() + v.at + 5;
                    const float vsx = loadBE(o), vsy = loadBE(o + 4), vox = loadBE(o + 12), voy = loadBE(o + 16);
                    storeBE(o, vsx * L.sx);
                    storeBE(o + 4, vsy * L.sy);
                    storeBE(o + 12, 340.0f + L.sx * (vox - 340.0f) + px);
                    storeBE(o + 16, 340.0f + L.sy * (voy - 340.0f) + L.ty);
                    if (sTextLog && sTextLogBlocks)
                        std::fprintf(sTextLog, "  V viewport origin %.2f %.2f -> %.2f %.2f\n", vox, voy, loadBE(o + 12), loadBE(o + 16));
                    continue;
                }
                u8* o = sReplay.data() + v.at;
                const auto field = [](const u8* w, int sh) { return int(((u32(w[2]) << 16 | u32(w[3]) << 8 | w[4]) >> sh) & 0x7FF); };
                const int top = field(o, 0) - 342, left = field(o, 12) - 342, bottom = field(o + 5, 0) - 342 + 1,
                          right = field(o + 5, 12) - 342 + 1;
                const int nt = int(std::lround(L.sy * top + L.ty)), nb = int(std::lround(L.sy * bottom + L.ty));
                const int nl = int(std::lround(L.sx * left + px)), nr = int(std::lround(L.sx * right + px));
                const auto put = [](u8* w, int lo, int hi) {
                    const u32 val = (u32(std::clamp(lo + 342, 0, 0x7FF)) | u32(std::clamp(hi + 342, 0, 0x7FF)) << 12) & 0xFFFFFF;
                    w[2] = u8(val >> 16), w[3] = u8(val >> 8), w[4] = u8(val);
                };
                put(o, nt, nl);
                put(o + 5, nb - 1, nr - 1);
                if (sTextLog && sTextLogBlocks)
                    std::fprintf(sTextLog, "  V scissor %d %d %d %d -> %d %d %d %d\n", left, top, right, bottom, nl, nt, nr, nb);
            }
        u32 faded = 0, indiv = 0, fresh = 0, matched = 0;
        bool scroll = false;
        for (u32 k = 0; k < bb.size(); k++) {
            const TextBlock& tb = bb[k];
            const Patch& p = b.patches[tb.i];
            const s32 j = b.textMatch[tb.i];
            const u8 kind = b.textKind[tb.i];
            const Layout2D& L = lay[tb.panel];
            float layer[2] = {0, 0};  // where this block is drawn from b's place (layout units)
            bool own = false;         // positions blended on its own (the bobbing arrow)
            if (j >= 0) {
                const Patch& q = a.patches[j];
                matched += !tb.box;
                if (kind == 2) {
                    layer[0] = -(1 - alpha) * b.textD[tb.i][0], layer[1] = -(1 - alpha) * b.textD[tb.i][1];
                    scroll = true;
                } else if (kind == 3 && q.count == p.count) {
                    own = true;
                    for (u32 v = 0; v < p.count; v++) {
                        float xa[2], xb[2], o[2];
                        readPos(a, q, v, xa);
                        readPos(b, p, v, xb);
                        for (int c = 0; c < 2; c++) o[c] = xa[c] + (xb[c] - xa[c]) * alpha;
                        writePos(p, v, o);
                    }
                    indiv++;
                }
                blendBlockColours(a, q, b, p, alpha);
            } else if (!tb.box) {
                // New in b: part of its panel's (scrolling) text layer; a letter fades in over this
                // game frame.
                fresh++;
                layer[0] = -(1 - alpha) * b.textD[tb.i][0], layer[1] = -(1 - alpha) * b.textD[tb.i][1];
                if (!rg.oneLayout && p.clrBytes == 4 && std::max(tb.hi[0] - tb.lo[0], tb.hi[1] - tb.lo[1]) < 48.0f) {
                    for (u32 v = 0; v < p.count; v++) {
                        u8& al = sReplay[p.offset + v * p.stride + p.clrOffset + 3];
                        al = u8(std::lround(al * std::clamp(alpha, 0.0f, 1.0f)));
                    }
                    faded++;
                }
            }
            const bool vm = viaMatrix(p);
            if (layer[0] != 0 || layer[1] != 0 || (L.moved && !vm)) {
                float dl[2] = {0, 0};
                if (layer[0] != 0 || layer[1] != 0) tb.A.invert(layer, dl, true);  // layout -> local units
                for (u32 v = 0; v < p.count; v++) {
                    float l[2], w[2];
                    readShown(p, v, l);
                    if (!own) l[0] += dl[0], l[1] += dl[1];
                    if (L.moved && !vm) {
                        tb.A.apply(l, w);
                        w[0] = L.sx * w[0] + L.tx, w[1] = L.sy * w[1] + L.ty;
                        tb.A.invert(w, l);
                    }
                    writePos(p, v, l);
                }
            }
            if (sTextLog && sTextLogBlocks) {
                float s0[2], w0[2];
                readShown(p, 0, s0);
                tb.A.apply(s0, w0);
                if (L.moved && vm) w0[0] = L.sx * w0[0] + L.tx, w0[1] = L.sy * w0[1] + L.ty;
                std::fprintf(sTextLog, "  B %u %s%s k%u m %d p %u b %.2f %.2f %.2f %.2f shown %.2f %.2f layout %.2f %.2f al %d mtx %d site %08X i %u alb %d route %d\n", k,
                             tb.box ? "box" : "glyph", own ? " own" : "", u32(kind), j, tb.panel, tb.lo[0], tb.lo[1], tb.hi[0], tb.hi[1], s0[0],
                             s0[1], w0[0], w0[1], p.clrBytes == 4 ? int(sReplay[p.offset + p.clrOffset + 3]) : -1, p.mtxPatch, p.site,
                             tb.i, p.clrBytes == 4 ? int(b.bytes[p.offset + p.clrOffset + 3]) : -1, route(p));
            }
        }
        sTextFaded += faded;
        if (scroll) sTextScrolled++;
        sTextGroupsBlended++;
        if (sTextLog) {
            std::fprintf(sTextLog, "P %llu %.3f f%u gf %u g %08X%s blocks %zu matched %u new %u faded %u own %u panels %zu", (unsigned long long)b.seq,
                         alpha, b.dumpFrame, b.gameFrame, rg.id, ga < 0 ? " (new)" : "", bb.size(), matched, fresh, faded, indiv, pb.size());
            for (u32 k = 1; k < pb.size(); k++) {
                const Layout2D& L = lay[k];
                const float(&r)[2][2] = pb[k].rect;
                std::fprintf(sTextLog, " | box %.2f %.2f %.2f %.2f shown %.2f %.2f %.2f %.2f a %d", r[0][0], r[0][1], r[1][0], r[1][1],
                             L.sx * r[0][0] + L.tx, L.sy * r[0][1] + L.ty, L.sx * r[1][0] + L.tx, L.sy * r[1][1] + L.ty, panelToA[k]);
            }
            std::fprintf(sTextLog, "\n");
        }
    }
}

void noteShadowLight(const float lightView[12], const float kProj[12], const float kRamp[12]) {
    if (!capturing()) return;
    sShadow.valid = true;
    std::copy(lightView, lightView + 12, sShadow.lv.begin());
    std::copy(kProj, kProj + 12, sShadow.kp.begin());
    std::copy(kRamp, kRamp + 12, sShadow.kr.begin());
}
void pushShadowReceivers() { sShadowRecv++; }
void setOrthographic(bool ortho) { sOrtho = ortho; }
void popShadowReceivers() { sShadowRecv = sShadowRecv > 0 ? sShadowRecv - 1 : 0; }

void setPass(u32 pass) { sPass = pass; }

void detail::noteDraw(u64 id) {
    id = mix(id ^ u64(sPass) << 60);  // the same model drawn in another pass is another draw
    const u32 bundle = u32(sBundleIds.size());
    sBundleIds.push_back(id);
    const TagFrame* t = topTag();
    sBundleKey.push_back(t && t->object ? t->inst : 0);
    sBundleRange.push_back({~0u, ~0u});
    sBundlePass.push_back(u8(sPass));
    sBundleCentre.push_back({std::nanf(""), 0.0f, 0.0f});
    if (sProbeOut) sBundleSlots.push_back(sSlotPatch);
    sOpenDraw = -1;
    for (u32 i = sUngrouped; i < sPatches.size(); i++) {
        Patch& p = sPatches[i];
        if (p.kind == Kind::Verts) continue;  // already belongs to its own draw
        p.bundle = bundle;
    }
    sUngrouped = u32(sPatches.size());
}

void detail::noteDrawCentre(const float c[3]) {
    if (!sBundleCentre.empty()) sBundleCentre.back() = {c[0], c[1], c[2]};
}
void detail::noteDrawSlotCentres(const float c[10][3], u16 mask) {
    if (sBundleCentre.empty()) return;
    SlotCentres sc;
    sc.bundle = u32(sBundleCentre.size() - 1);
    sc.mask = mask;
    for (int s = 0; s < 10; s++)
        for (int k = 0; k < 3; k++) sc.c[s][k] = c[s][k];
    sSlotCentres.push_back(sc);
}

u64 detail::arrayKey() {
    if (!capturing() || sFrameLock > 0) return 0;
    TagCat cat;
    const u64 k = valueKey(Kind::Verts, 0x41525241u, 0, cat);
    // LM_TAG_LOG: every position array bound, with its scope's key (0: untagged, never blended)
    // and the guest call chain (how the shower curtain's untagged array was found).
    if (sTagLogFrame != ~0ull)
        std::fprintf(sTagLog, "f%llu array %s key %016llX site %08X depth %d chain %s\n", (unsigned long long)sTagLogFrame,
                     kCatName[u32(cat)], (unsigned long long)k, sCallLr, sTagDepth, sTagChain.c_str());
    return k ^ 0xA77A1ull;  // (a pseudo light-like source: own key space)
}

namespace {
u64 sVertsKey = 0;
s32 sVertsBack = -1;  // tagBackStep of the scope the block was begun in
bool sVertsTracked = false;  // tagTracked of that scope
bool sVertsLocked = false;  // begun inside a text box (pushFrameLock): its vertices are not noted
TagCat sVertsCat = TagCat::None;
u32 sVertsSite = 0;
std::string sVertsChain;
}  // namespace
void detail::vertsBegin(bool ortho) {
    if (!capturing()) return;
    // The block is noted when its vertices are complete, at the next GX call: possibly after the
    // text box's scope closed (the window's last quad blended while the rest of it was locked).
    sVertsLocked = sFrameLock > 0;
    sVertsText = sTextCur;
    sVertsMtx = sCurMtxSlot < 10 ? sLastPosMtx[sCurMtxSlot] : -1;
    if (sVertsLocked) return;
    sVertsSite = sCallLr;
    sVertsKey = valueKey(Kind::Verts, 0, 0, sVertsCat);
    {
        const TagFrame* t = topTag();
        sVertsBack = sVertsKey && t ? t->backStep : -1;
        sVertsTracked = sVertsKey && t && t->tracked;
    }
    if (!sVertsKey && ortho) {
        // An untagged 2D quad (a fade, a full-screen picture): the structural scope, call site and
        // occurrence; its colours blend only while its positions are identical (replayPair).
        const TagFrame* t = topTag();
        const u64 k = mix((t ? t->structural : rootInst()) ^ u64(Kind::Verts) << 56 ^ u64(sCallLr) << 16 ^ 0x2D2Dull);
        sVertsKey = mix(k ^ u64(occurrence(sValOcc, k) + 1) << 40);
    }
    if (sTagLogFrame != ~0ull) sVertsChain = sTagChain;
}

s32 detail::currentProjection() { return sLastProj; }

void detail::noteVertices(u32 at, u32 count, u32 stride, u32 posOffset, u64 id, u32 clrOffset, u32 clrBytes, u32 posBytes2D,
                          u32 posFmt2D, u32 uvOffset, u32 uvBytes, s32 projPatch) {
    const bool locked = sVertsLocked;
    sVertsLocked = false;
    if (locked) sVertsKey = 0;
    if (!capturing() || locked || count == 0 || count > 0xFFFF) return;
    Patch p{at, u16(count), Kind::Verts};
    p.stride = u16(stride);
    p.posOffset = u16(posOffset);
    if (posBytes2D) {
        p.role = 2, p.source = posBytes2D;  // 2D: colours, and positions of animated pictures (replayPair)
        p.posFmt = u16(posFmt2D);
        if (uvBytes && uvOffset + uvBytes <= stride) p.uvOffset = u16(uvOffset), p.uvBytes = u8(uvBytes);
    }
    if (clrBytes && clrOffset + clrBytes <= stride) p.clrOffset = u16(clrOffset), p.clrBytes = u8(clrBytes);
    // The key was taken at GXBegin (vertsBegin): the scope may have closed by the time the draw's
    // vertices are complete (noted at the next GX call).
    p.site = sVertsSite;
    p.cat = sVertsCat;
    p.key = sVertsKey ? mix(sVertsKey ^ id) : 0;  // the same layout (vertex count is checked when pairing)
    p.backStep = p.key && !posBytes2D ? sVertsBack : -1;
    p.tracked = p.key && sVertsTracked;
    sVertsTracked = false;
    sVertsKey = 0;
    sVertsBack = -1;
    if (sTagLogFrame != ~0ull && p.role == 2 && (p.posFmt & 0x100) && at + count * stride <= sBase + AuroraCapturePos()) {
        const u32 type = p.posFmt & 7, frac = (p.posFmt >> 3) & 31;
        const u32 csize = type == GX_F32 ? 4 : type >= GX_U16 ? 2 : 1;
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (u32 v = 0; v < count; v++)
            for (u32 c = 0; c < 2; c++) {
                const f32 x = load2D(sScratch.data() + at + v * stride + posOffset + c * csize, type, frac);
                lo[c] = std::min(lo[c], x), hi[c] = std::max(hi[c], x);
            }
        u32 uv = 0;
        if (p.uvBytes >= 4) {
            const u8* q = sScratch.data() + at + p.uvOffset;
            uv = u32(q[0]) << 24 | u32(q[1]) << 16 | u32(q[2]) << 8 | q[3];
        }
        std::fprintf(sTagLog, "f%llu #%u 2d bounds %.2f %.2f %.2f %.2f uv %08X fmt %X n %u\n", (unsigned long long)sTagLogFrame,
                     u32(sPatches.size()), lo[0], lo[1], hi[0], hi[1], uv, p.posFmt, count);
    }
    if (sTagLogFrame != ~0ull)
        std::fprintf(sTagLog, "f%llu #%u verts %s key %016llX site %08X slot %u src %08X depth %d view -1 role %u clr %u@%u %08X%s%s\n",
                     (unsigned long long)sTagLogFrame, u32(sPatches.size()), kCatName[u32(p.cat)], (unsigned long long)p.key,
                     p.site, count, stride, sTagDepth, p.role, p.clrBytes, p.clrOffset,
                     p.clrBytes && at + p.clrOffset + 4 <= sBase + AuroraCapturePos()
                         ? u32(sScratch[at + p.clrOffset]) << 24 | u32(sScratch[at + p.clrOffset + 1]) << 16 |
                               u32(sScratch[at + p.clrOffset + 2]) << 8 | sScratch[at + p.clrOffset + 3]
                         : 0u,
                     p.key ? "" : " chain ", p.key ? "" : sVertsChain.c_str());
    detail::drawLog("  #%u verts %u stride %u pos@%u", u32(sPatches.size()), count, stride, posOffset);
    // It belongs to the draw just begun (vtxBegin noted it), not to the next one.
    if (sBundleIds.empty()) {
        sBundleIds.push_back(id);
        sBundleKey.push_back(0);
        sBundleRange.push_back({~0u, ~0u});
        sBundlePass.push_back(u8(sPass));
        sBundleCentre.push_back({std::nanf(""), 0.0f, 0.0f});
        if (sProbeOut) sBundleSlots.push_back(sSlotPatch);
    }
    p.bundle = u32(sBundleIds.size() - 1);
    // Noted at the next GX call, possibly after the text group closed: it still belongs to it.
    p.tgroup = sVertsText;
    p.mtxPatch = sVertsMtx;
    p.projPatch = projPatch;
    sVertsText = 0;
    if (p.tgroup && p.tgroup <= sTextGroups.size() && sTextGroups[p.tgroup - 1].end != ~0u &&
        sTextGroups[p.tgroup - 1].end <= sPatches.size())
        sTextGroups[p.tgroup - 1].end = u32(sPatches.size()) + 1;
    const bool tail = sUngrouped == sPatches.size();
    sPatches.push_back(p);
    if (tail) sUngrouped = u32(sPatches.size());
}

void detail::drawRange(u32 start, u32 end) {
    if (!capturing() || sBundleRange.empty() || start == ~0u || end == ~0u) return;
    sBundleRange.back() = {start, end};
    sOpenDraw = -1;
}
void detail::drawStart(u32 start) {
    if (!capturing() || sBundleRange.empty() || start == ~0u) return;
    sBundleRange.back() = {start, ~0u};
    sOpenDraw = s32(sBundleRange.size() - 1);
}
void detail::drawEnd(u32 end) {
    if (sOpenDraw < 0 || end == ~0u || u32(sOpenDraw) >= sBundleRange.size()) return;
    sBundleRange[sOpenDraw][1] = end;
    sOpenDraw = -1;
}
void vtxEnd() { detail::drawEnd(mark()); }
// A display list or the stream loaded XF matrix memory directly (indexed or raw loads): the
// detector's slot table no longer knows those slots.
void detail::xfMatrixWritten(u32 addr, u32 words) {
    if (!sProbeOut || !capturing()) return;
    for (u32 s = 0; s < 10; s++)
        if (addr < 12 * s + 12 && addr + words > 12 * s) sSlotPatch[s] = -1;
}


void suspendForDisplayList() {
    if (!sCapturing) return;
    sBase += AuroraCaptureEnd();  // padded with NOPs; the stream stays valid
    sSuspended = true;
}

void resumeAfterDisplayList() {
    if (!sCapturing) return;
    AuroraCaptureBegin(sScratch.data() + sBase, kCaptureSize - sBase);
    sSuspended = false;
}

// --- host ----------------------------------------------------------------------------

namespace {

u64 sEndSeq[4] = {};  // per replay state slot: the tape whose end state it holds (host side)

// --- camera factoring (3x4 row-major matrices as the affine 4x4 [M; 0 0 0 1]) ---
// out = a x b (a may be any 3x4, e.g. a projective texture matrix's rows).
void compose(const f32* a, const f32* b, f32* out) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            out[4 * r + c] = a[4 * r] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c] + (c == 3 ? a[4 * r + 3] : 0.0f);
}
// Inverse of the 3x3 part (adjugate); false if singular.
bool invert33(const f32* m, int cols, f32 out[9]) {
    const f32 a = m[0], b = m[1], c = m[2], d = m[cols], e = m[cols + 1], f = m[cols + 2], g = m[2 * cols],
              h = m[2 * cols + 1], i = m[2 * cols + 2];
    const f32 A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const f32 det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-12f) || !std::isfinite(det)) return false;
    const f32 k = 1.0f / det;
    out[0] = A * k, out[1] = -(b * i - c * h) * k, out[2] = (b * f - c * e) * k;
    out[3] = B * k, out[4] = (a * i - c * g) * k, out[5] = -(a * f - c * d) * k;
    out[6] = C * k, out[7] = -(a * h - b * g) * k, out[8] = (a * e - b * d) * k;
    return true;
}
// out = K^-1 x T for affine-like 3x4 K (solves T = K x W for W).
bool solveAffine(const f32* k, const f32* t, f32* out) {
    f32 inv[9];
    if (!invert33(k, 4, inv)) return false;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            f32 s = 0;
            for (int j = 0; j < 3; j++) s += inv[3 * r + j] * (t[4 * j + c] - (c == 3 ? k[4 * j + 3] : 0.0f));
            out[4 * r + c] = s;
        }
    return true;
}
bool invertAffine(const f32* m, f32* out) {
    static const f32 kId[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    return solveAffine(m, kId, out);
}
// A camera view matrix: rotation (orthonormal rows) and translation.
bool rigidView(const f32* v) {
    for (int r = 0; r < 3; r++)
        for (int s = r; s < 3; s++) {
            const f32 d = v[4 * r] * v[4 * s] + v[4 * r + 1] * v[4 * s + 1] + v[4 * r + 2] * v[4 * s + 2];
            if (!(std::fabs(d - (r == s ? 1.0f : 0.0f)) < 2e-3f)) return false;
        }
    return true;
}
// Blended camera for a pair of views (cached per replay: a tape has few views).
struct ViewPair {
    u32 key;
    bool ok;
    bool cut;  // the camera jumped (another light took over the shadow pass, a cut): show b as is
    M34 va, vb, vt;  // views and blended view
    M34 ia, ib, it;  // inverses
};
std::deque<ViewPair> sViewPairs;  // (a deque: pointers handed out stay valid as pairs are added)
float diff12(const f32* x, const f32* y);
std::vector<u64> sLightsBlended;  // (draw << 8 | light index) of the lights blended in this replay
const ViewPair* viewPair(const Tape& a, u16 qa, const Tape& b, u16 qb, float alpha) {
    if (qa >= a.views.size() || qb >= b.views.size()) return nullptr;
    const u32 key = u32(qa) << 16 | qb;
    for (const ViewPair& vp : sViewPairs)
        if (vp.key == key) return vp.ok || vp.cut ? &vp : nullptr;
    ViewPair vp{key, false, false};
    vp.va = a.views[qa], vp.vb = b.views[qb];
    {
        float ea[3], eb[3], dot = 0, de = 0;
        for (int c = 0; c < 3; c++) {
            ea[c] = -(vp.va[c] * vp.va[3] + vp.va[4 + c] * vp.va[7] + vp.va[8 + c] * vp.va[11]);
            eb[c] = -(vp.vb[c] * vp.vb[3] + vp.vb[4 + c] * vp.vb[7] + vp.vb[8 + c] * vp.vb[11]);
            dot += vp.va[8 + c] * vp.vb[8 + c];
            de += (ea[c] - eb[c]) * (ea[c] - eb[c]);
        }
        vp.cut = std::sqrt(de) > 250.0f || dot < 0.866f;
        // The main camera: its cut decision is the tape's (continuity, pairTapes).
        if (qb == b.mainView && b.viewToA.size() > qb && b.viewToA[qb] == s32(qa)) vp.cut = b.cut;
        // Another view (its own key) seen through the very camera of b's main view, paired with a view
        // of a that is another camera: it moves with the main camera (a teleport into the basement
        // inserted a view, the room's draws paired with a's main camera by order and were shown as
        // captured while the main camera glided 420 units: 1000 px apart, sweep A).
        else if (vp.cut && qb != b.mainView && b.mainView < b.views.size() && b.viewToA.size() > b.mainView &&
                 b.viewToA[b.mainView] >= 0 && diff12(b.views[qb].data(), b.views[b.mainView].data()) < 1e-3f)
            return viewPair(a, u16(b.viewToA[b.mainView]), b, b.mainView, alpha);
        if (sLogSnaps)
            log("[interp]   view pair a%u b%u: eye moved %.0f, turned %.1f deg%s", qa, qb, std::sqrt(de),
                std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.2958f, vp.cut ? " CUT" : "");
    }
    // The in-between camera: rotation slerped, eye moved in a straight line (blending the view
    // matrix's translation would move the eye along an arc while the camera turns).
    vp.ok = rigidView(vp.va.data()) && rigidView(vp.vb.data()) && blendRigid(vp.va.data(), vp.vb.data(), 4, alpha, vp.vt.data()) &&
            invertAffine(vp.va.data(), vp.ia.data()) && invertAffine(vp.vb.data(), vp.ib.data());
    if (vp.ok) {
        f32 ea[3], eb[3], e[3];
        eyeOf(vp.va, ea);
        eyeOf(vp.vb, eb);
        for (int c = 0; c < 3; c++) e[c] = ea[c] + (eb[c] - ea[c]) * alpha;
        for (int r = 0; r < 3; r++) vp.vt[4 * r + 3] = -(vp.vt[4 * r] * e[0] + vp.vt[4 * r + 1] * e[1] + vp.vt[4 * r + 2] * e[2]);
        vp.ok = invertAffine(vp.vt.data(), vp.it.data());
    }
    sViewPairs.push_back(vp);
    return sViewPairs.back().ok || sViewPairs.back().cut ? &sViewPairs.back() : nullptr;
}
// The blended light of a pair of tapes with shadow maps: LV(t) and P(t) for both K matrices.
struct LightPair {
    bool ok = false;
    M34 lvt, kpt, krt;  // K(t) = P(t) x LV(t)
};
LightPair sLightPair;
const bool sLightLegacy = std::getenv("LM_LIGHT_ARC") != nullptr;  // test aid: the old translation blend
void makeLightPair(const Tape& a, const Tape& b, float alpha) {
    sLightPair = {};
    if (!a.shadow.valid || !b.shadow.valid) return;
    {  // another light took over (flashlight <-> room light, lightning): no blend
        float ea[3], eb[3], dot = 0, de = 0;
        const M34 &la = a.shadow.lv, &lb = b.shadow.lv;
        for (int c = 0; c < 3; c++) {
            ea[c] = -(la[c] * la[3] + la[4 + c] * la[7] + la[8 + c] * la[11]);
            eb[c] = -(lb[c] * lb[3] + lb[4 + c] * lb[7] + lb[8 + c] * lb[11]);
            dot += la[8 + c] * lb[8 + c];
            de += (ea[c] - eb[c]) * (ea[c] - eb[c]);
        }
        if (std::sqrt(de) > 250.0f || dot < 0.866f) return;
    }
    M34 ia, ib;
    if (!rigidView(a.shadow.lv.data()) || !rigidView(b.shadow.lv.data()) || !invertAffine(a.shadow.lv.data(), ia.data()) ||
        !invertAffine(b.shadow.lv.data(), ib.data()) ||
        !blendRigid(a.shadow.lv.data(), b.shadow.lv.data(), 4, alpha, sLightPair.lvt.data()))
        return;
    // The eye moves in a straight line, as for every camera (viewPair): the caster pass is drawn
    // through the light's camera pair, so the receivers' light must be that same in-between camera.
    // Blending the view matrix's translation instead moved the eye along an arc while the light
    // turned: the follow light (moving 35-150 units and turning every game frame after "shadows at
    // any distance") then looked up a shadow map drawn from another place mid-interval, and lit walls
    // shadowed themselves in the in-betweens (sweep B: a candle glow in the 2F east hallway dipped
    // at every game frame).
    if (!sLightLegacy) {
        f32 ea[3], eb[3], e[3];
        eyeOf(a.shadow.lv, ea);
        eyeOf(b.shadow.lv, eb);
        M34& t = sLightPair.lvt;
        for (int c = 0; c < 3; c++) e[c] = ea[c] + (eb[c] - ea[c]) * alpha;
        for (int r = 0; r < 3; r++) t[4 * r + 3] = -(t[4 * r] * e[0] + t[4 * r + 1] * e[1] + t[4 * r + 2] * e[2]);
    }
    for (int which = 0; which < 2; which++) {
        const M34& ka = which ? a.shadow.kr : a.shadow.kp;
        const M34& kb = which ? b.shadow.kr : b.shadow.kp;
        M34 pa, pb, pt;
        compose(ka.data(), ia.data(), pa.data());
        compose(kb.data(), ib.data(), pb.data());
        // The depth ramp follows b's near/far ("shadows at any distance" refits them every frame;
        // the ramp is not linear in them): the caster pass writes depth with b's near/far too
        // (its projection's depth terms are taken from b), so caster and receivers agree.
        for (int k = 0; k < 12; k++) pt[k] = which ? pb[k] : pa[k] + (pb[k] - pa[k]) * alpha;
        compose(pt.data(), sLightPair.lvt.data(), (which ? sLightPair.krt : sLightPair.kpt).data());
    }
    sLightPair.ok = true;
}
float diff12(const f32* x, const f32* y) {
    float d = 0;
    for (int k = 0; k < 12; k++) d += std::fabs(x[k] - y[k]) / (1.0f + std::fabs(y[k]));
    return d;
}
// A shadow receiver's texture matrix rebuilt from the blended light (see noteShadowLight).
// Per draw of a tape, the G of its shadow receivers' matrices T = K x G. G is the receiver's
// world matrix (TEXMTX on model-space positions) or the inverse view the game built the actors'
// post-transform matrices with (PT18/PT19, on view-space positions); the game uses the same G with
// the projection K and the depth-ramp K, but the ramp's K is singular (its s and t rows are both
// depth), so G is solved from the projection matrix of the same draw.
std::unordered_map<u32, M34> sRecvA, sRecvB;
void receiverGs(const Tape& t, std::unordered_map<u32, M34>& out) {
    out.clear();
    if (!t.shadow.valid) return;
    for (const Patch& p : t.patches) {
        const u32 key = p.bundle * 2 + (p.slot >= 64 ? 1 : 0);  // post-transform and TEXMTX receivers apart
        if (p.role != 1 || p.count != 12 || out.count(key)) continue;
        f32 v[12];
        for (int k = 0; k < 12; k++) v[k] = val(t, p, k);
        M34 g;
        if (!solveAffine(t.shadow.kp.data(), v, g.data())) continue;
        // The projection's G has orthogonal columns (a world matrix or an inverse view).
        float skew = 0;
        for (int i = 0; i < 3; i++)
            for (int j = i + 1; j < 3; j++) skew += std::fabs(g[i] * g[j] + g[4 + i] * g[4 + j] + g[8 + i] * g[8 + j]);
        if (skew < 0.05f) out.emplace(key, g);
    }
}
bool blendShadowReceiver(const Tape& a, const Patch& q, const f32* ta, const Tape& b, const Patch& p, const f32* tb,
                         float alpha, f32* out) {
    if (!sLightPair.ok || p.count != 12) return false;
    const auto ga = sRecvA.find(q.bundle * 2 + (q.slot >= 64 ? 1 : 0)), gb = sRecvB.find(p.bundle * 2 + (p.slot >= 64 ? 1 : 0));
    if (ga == sRecvA.end() || gb == sRecvB.end()) return false;
    // Which K: the one that rebuilds b's value from its G.
    M34 xp, xr;
    compose(b.shadow.kp.data(), gb->second.data(), xp.data());
    compose(b.shadow.kr.data(), gb->second.data(), xr.data());
    const bool ramp = diff12(xr.data(), tb) < diff12(xp.data(), tb);
    M34 gt;
    if (!blendRigid(ga->second.data(), gb->second.data(), 4, alpha, gt.data()))
        for (int k = 0; k < 12; k++) gt[k] = ga->second[k] + (gb->second[k] - ga->second[k]) * alpha;
    compose((ramp ? sLightPair.krt : sLightPair.kpt).data(), gt.data(), out);
    (void)ta;
    return true;
}
// A shadow receiver's matrix without a partner (set up outside any tag scope): rebuilt from the
// blended light with b's G; for post-transform receivers (G = the inverse camera the game built them
// with) the in-between camera's inverse, so the shadow lookup follows the blended camera and light.
bool shadowReceiverFromB(const Tape& b, const Patch& p, const f32* tb, const ViewPair* mvp, f32* out) {
    if (!sLightPair.ok || p.count != 12) return false;
    const auto gb = sRecvB.find(p.bundle * 2 + (p.slot >= 64 ? 1 : 0));
    if (gb == sRecvB.end()) return false;
    M34 xp, xr;
    compose(b.shadow.kp.data(), gb->second.data(), xp.data());
    compose(b.shadow.kr.data(), gb->second.data(), xr.data());
    const bool ramp = diff12(xr.data(), tb) < diff12(xp.data(), tb);
    const bool postTransform = p.slot >= 64 && mvp && mvp->ok && !mvp->cut && diff12(gb->second.data(), mvp->ib.data()) < 0.05f;
    compose((ramp ? sLightPair.krt : sLightPair.kpt).data(), postTransform ? mvp->it.data() : gb->second.data(), out);
    return true;
}
bool reexpress(const ViewPair& vp, bool fromA, const Patch& p, const f32* v, f32* out);
// view x world values blended as blended view x blended world.
bool blendFactored(const Tape& a, const Patch& q, const f32* va, const Tape& b, const Patch& p, const f32* vb, float alpha,
                   f32* out) {
    if (p.view == 0xFFFF || q.view == 0xFFFF) return false;
    // The partner was drawn under another camera than the one b's camera corresponds to (by
    // majority): blending across the two cameras put the value somewhere in between (Luigi drawn
    // small and far off for a game frame when the camera jumped while the scenery was shown as b).
    // b's value is placed under the blended camera of b's camera pair instead, like the rest of
    // its scene (or as captured on a cut).
    if (p.view < sViewToA.size() && sViewToA[p.view] >= 0 && u16(sViewToA[p.view]) != q.view) {
        const ViewPair* mp = viewPair(a, u16(sViewToA[p.view]), b, p.view, alpha);
        if (!mp || mp->cut || !reexpress(*mp, false, p, vb, out)) std::copy(vb, vb + p.count, out);
        sCrossCamera++;
        return true;
    }
    const ViewPair* vp = viewPair(a, q.view, b, p.view, alpha);
    if (!vp) return false;
    if (vp->cut) {  // drawn from a camera that jumped: as captured
        std::copy(vb, vb + p.count, out);
        return true;
    }
    if (p.kind == Kind::PosMtx && p.count == 12) {
        M34 wa, wb, wt;
        compose(vp->ia.data(), va, wa.data());
        compose(vp->ib.data(), vb, wb.data());
        const bool skew = skewFlip(wa.data(), wb.data()) || tumblerMirror(p, wa.data(), wb.data());
        if (!sLinearBlend && !skew && resolveFlip(wa.data(), wb.data(), 4)) sFlips++;
        if (!sLinearBlend && !skew && untwist(wa.data(), wb.data())) sUntwists++;
        // LM_TWIST_LOG=<file> (test aid): model matrices that turn by over 30 degrees in one game
        // frame, with how far each local axis turned (a turn about a barely moving axis is a twist).
        static FILE* sTwistLog = [] {
            const char* f = std::getenv("LM_TWIST_LOG");
            return f && *f ? std::fopen(f, "w") : nullptr;
        }();
        if (sTwistLog && alpha < 0.3f) {
            float ax[3];
            float total = 0;
            Rigid ra, rb;
            if (decompose(wa.data(), 4, ra) && decompose(wb.data(), 4, rb)) {
                const float d = std::fabs(ra.q[0] * rb.q[0] + ra.q[1] * rb.q[1] + ra.q[2] * rb.q[2] + ra.q[3] * rb.q[3]);
                total = 2.0f * std::acos(std::min(d, 1.0f)) * 57.2958f;
            }
            for (int k = 0; k < 3; k++) {
                float ca[3], cb[3], la = 0, lb = 0, dt = 0;
                for (int r = 0; r < 3; r++) ca[r] = wa[4 * r + k], cb[r] = wb[4 * r + k], la += ca[r] * ca[r], lb += cb[r] * cb[r], dt += ca[r] * cb[r];
                ax[k] = std::acos(std::clamp(dt / std::sqrt(std::max(la * lb, 1e-12f)), -1.0f, 1.0f)) * 57.2958f;
            }
            if (total > 30.0f)
                std::fprintf(sTwistLog, "%llu site %08X src %08X key %016llX turn %.0f axes %.0f %.0f %.0f move %.1f at %.0f %.0f %.0f\n",
                             (unsigned long long)b.seq, p.site, p.source, (unsigned long long)p.key, total, ax[0], ax[1], ax[2],
                             std::hypot(wa[3] - wb[3], wa[7] - wb[7], wa[11] - wb[11]), wb[3], wb[7], wb[11]);
        }
        if (sLinearBlend || skew || flatMatrix(wa.data()) || flatMatrix(wb.data()) || !blendRigid(wa.data(), wb.data(), 4, alpha, wt.data())) {
            // Squashed flat (or mirrored against its partner): entrywise in world space.
            for (int k = 0; k < 12; k++) wt[k] = wa[k] + (wb[k] - wa[k]) * alpha;
            sFlatBlends++;
        }
        // LM_MTX_DUMP=<lo>,<hi> (test aid): world matrices a, b and the blend for sources in [lo, hi).
        static const std::pair<u32, u32> sDump = [] {
            const char* v = std::getenv("LM_MTX_DUMP");
            u32 lo = 0, hi = 0;
            if (v) std::sscanf(v, "%x,%x", &lo, &hi);
            return std::pair<u32, u32>{lo, hi};
        }();
        if (p.source >= sDump.first && p.source < sDump.second) {
            const auto row = [](const char* tag, const f32* m) {
                log("[mtx]   %s %7.3f %7.3f %7.3f %8.1f | %7.3f %7.3f %7.3f %8.1f | %7.3f %7.3f %7.3f %8.1f", tag, m[0], m[1], m[2], m[3], m[4],
                    m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
            };
            log("[mtx] seq %llu alpha %.3f src %08X key %016llX", (unsigned long long)b.seq, alpha, p.source, (unsigned long long)p.key);
            row("A", wa.data());
            row("B", wb.data());
            row("T", wt.data());
        }
        compose(vp->vt.data(), wt.data(), out);
        return true;
    }
    // Rotation parts (rows of a rigid view): R^T x n (world), R(t) x n (view).
    const auto rotT = [](const M34& v, const f32* n, f32* o) {
        for (int r = 0; r < 3; r++) o[r] = v[r] * n[0] + v[4 + r] * n[1] + v[8 + r] * n[2];
    };
    const auto rot = [](const M34& v, const f32* n, f32* o) {
        for (int r = 0; r < 3; r++) o[r] = v[4 * r] * n[0] + v[4 * r + 1] * n[1] + v[4 * r + 2] * n[2];
    };
    if (p.kind == Kind::NrmMtx && p.count == 9) {
        f32 na[9], nb[9], nt[9];
        for (int c = 0; c < 3; c++) {  // columns: n_world = R^T n_view
            const f32 ca[3] = {va[c], va[3 + c], va[6 + c]}, cb[3] = {vb[c], vb[3 + c], vb[6 + c]};
            f32 wa[3], wb[3];
            rotT(vp->va, ca, wa);
            rotT(vp->vb, cb, wb);
            for (int r = 0; r < 3; r++) na[3 * r + c] = wa[r], nb[3 * r + c] = wb[r];
        }
        if (sLinearBlend || skewFlip(na, nb, 3)) {
            for (int k = 0; k < 9; k++) nt[k] = na[k] + (nb[k] - na[k]) * alpha;
        } else {
            resolveFlip(na, nb, 3);
            untwist(na, nb, 3);
            if (!blendRigid(na, nb, 3, alpha, nt)) return false;
        }
        for (int c = 0; c < 3; c++) {
            const f32 cw[3] = {nt[c], nt[3 + c], nt[6 + c]};
            f32 cv[3];
            rot(vp->vt, cw, cv);
            for (int r = 0; r < 3; r++) out[3 * r + c] = cv[r];
        }
        return true;
    }
    if (p.kind == Kind::Light && p.count == 6) {
        f32 pa[3], pb[3], da[3], db[3], ra[3], rb[3];
        for (int k = 0; k < 3; k++) ra[k] = va[k] - vp->va[4 * k + 3], rb[k] = vb[k] - vp->vb[4 * k + 3];
        rotT(vp->va, ra, pa);
        rotT(vp->vb, rb, pb);
        rotT(vp->va, va + 3, da);
        rotT(vp->vb, vb + 3, db);
        f32 pw[3], dw[3];
        for (int k = 0; k < 3; k++) pw[k] = pa[k] + (pb[k] - pa[k]) * alpha, dw[k] = da[k] + (db[k] - da[k]) * alpha;
        // The direction turns like the model it belongs to (slerp: the flashlight's light stays
        // on the cone's axis, which is blended by rotation); its length is set by the caller.
        {
            const float la = std::sqrt(da[0] * da[0] + da[1] * da[1] + da[2] * da[2]);
            const float lb = std::sqrt(db[0] * db[0] + db[1] * db[1] + db[2] * db[2]);
            if (la > 1e-6f && lb > 1e-6f && std::isfinite(la + lb)) {
                const float c = std::clamp((da[0] * db[0] + da[1] * db[1] + da[2] * db[2]) / (la * lb), -1.0f, 1.0f);
                const float th = std::acos(c);
                if (th > 1e-3f && th < 3.1f) {
                    const float sn = std::sin(th), wa = std::sin((1 - alpha) * th) / sn, wb = std::sin(alpha * th) / sn;
                    const float len = la + (lb - la) * alpha;
                    for (int k = 0; k < 3; k++) dw[k] = (wa * da[k] / la + wb * db[k] / lb) * len;
                }
            }
        }
        rot(vp->vt, pw, out);
        for (int k = 0; k < 3; k++) out[k] += vp->vt[4 * k + 3];
        rot(vp->vt, dw, out + 3);
        return true;
    }
    return false;
}

// --- spawn/despawn fades ---------------------------------------------------------------
// A value without partner, re-expressed under the blended camera: its world placement (from
// the frame it belongs to) seen from the in-between camera, so it moves with the scene
// instead of with the camera of its own frame.
bool reexpress(const ViewPair& vp, bool fromA, const Patch& p, const f32* v, f32* out) {
    if (!vp.ok || vp.cut) return false;
    const M34& V = fromA ? vp.va : vp.vb;
    const M34& inv = fromA ? vp.ia : vp.ib;
    const auto rotT = [&](const f32* n, f32* o) {
        for (int r = 0; r < 3; r++) o[r] = V[r] * n[0] + V[4 + r] * n[1] + V[8 + r] * n[2];
    };
    const auto rot = [&](const f32* n, f32* o) {
        for (int r = 0; r < 3; r++) o[r] = vp.vt[4 * r] * n[0] + vp.vt[4 * r + 1] * n[1] + vp.vt[4 * r + 2] * n[2];
    };
    if (p.kind == Kind::PosMtx && p.count == 12) {
        M34 w;
        compose(inv.data(), v, w.data());
        compose(vp.vt.data(), w.data(), out);
        return true;
    }
    if (p.kind == Kind::NrmMtx && p.count == 9) {
        for (int c = 0; c < 3; c++) {
            const f32 n[3] = {v[c], v[3 + c], v[6 + c]};
            f32 w[3], o[3];
            rotT(n, w);
            rot(w, o);
            for (int r = 0; r < 3; r++) out[3 * r + c] = o[r];
        }
        return true;
    }
    if (p.kind == Kind::Light && p.count == 6) {
        f32 r[3], w[3];
        for (int k = 0; k < 3; k++) r[k] = v[k] - V[4 * k + 3];
        rotT(r, w);
        rot(w, out);
        for (int k = 0; k < 3; k++) out[k] += vp.vt[4 * k + 3];
        rotT(v + 3, w);
        rot(w, out + 3);
        return true;
    }
    return false;
}

// --- judder detector (LM_JUDDER_LOG=<file>; docs/INTERPOLATION.md "Tagged interpolation") -------
// Aurora reports every draw it processes with the position matrix (and its slot), projection and
// first vertex it is drawn with (patch 0218, AuroraSetDrawProbe). Each in-between replay is checked
// draw by draw against the ideal: for a tagged draw paired by identity, its first vertex's world
// position blended linearly between the two game frames (the matrices that slot held at the draw
// in a and in b, at capture), seen through the ideal in-between camera (rotation slerped, eye moved
// linearly); for everything shown from the newest frame (untagged, unpaired, static world), b's
// world position under that camera. Off by more than a threshold (pixels of a 2160-line picture,
// LM_JUDDER_PX, default 1.5) is logged with the cause; a summary every 5 s goes to the game log.
// Works live (every present) and in LM_INTERP_TEST runs.
struct ProbeRec {
    u32 tag;
    u8 prim, mtxIdx;
    u16 n;
    bool hasPos;
    f32 mtx[12], proj[16], pos[3];
    f32 ndcMin[2], ndcMax[2], viewport[4];
};
std::mutex sProbeM;
std::vector<ProbeRec> sProbeIn;
void probeCallback(const AuroraDrawProbe* p) {
    if ((p->tag & 0xFFFFF) == 0xFFFFF) return;  // not judged (between draws, or a replay not sampled)
    ProbeRec r;
    r.tag = p->tag, r.prim = p->prim, r.mtxIdx = p->mtxIdx, r.n = p->vtxCount, r.hasPos = p->hasPos != 0;
    std::memcpy(r.mtx, p->mtx, sizeof(r.mtx));
    std::memcpy(r.proj, p->proj, sizeof(r.proj));
    std::memcpy(r.pos, p->pos, sizeof(r.pos));
    std::memcpy(r.ndcMin, p->ndcMin, sizeof(r.ndcMin));
    std::memcpy(r.ndcMax, p->ndcMax, sizeof(r.ndcMax));
    std::memcpy(r.viewport, p->viewport, sizeof(r.viewport));
    std::lock_guard lk(sProbeM);
    // (Capped at 2^18 records, ~50 MB: unjudged draws piled up to 1M records and the doubling vector
    // failed to allocate under memory pressure, sweep A.)
    if (sProbeIn.size() < (1u << 18)) sProbeIn.push_back(r);
}
struct ProbeReplay {
    u32 id;
    std::shared_ptr<const Tape> a, b;
    float alpha;
    u32 present;
};
std::deque<ProbeReplay> sProbeIssued;  // under sProbeIssuedM (host pushes, the judge thread pops)
std::mutex sProbeIssuedM;
u32 sProbeNextId = 1;
bool sProbeFill = false;
// LM_JUDDER_EVERY=<n>: judge every n-th in-between replay (default 1 in LM_INTERP_TEST runs, else 3:
// the detector stays cheap enough to play with; judging runs on its own thread).
const u32 sProbeEvery = [] {
    const char* v = std::getenv("LM_JUDDER_EVERY");
    return v ? std::max(1u, u32(std::atoi(v))) : std::getenv("LM_INTERP_TEST") ? 1u : 3u;
}();
void probeProcess();
// After replayPair: the tapes of the replay just issued (kept alive until it is judged).
void probeFill(const std::shared_ptr<const Tape>& a, const std::shared_ptr<const Tape>& b) {
    if (!sProbeFill) return;
    sProbeFill = false;
    std::lock_guard lk(sProbeIssuedM);
    if (sProbeIssued.empty()) return;
    sProbeIssued.back().a = a;
    sProbeIssued.back().b = b;
}
struct ProbeTotals {
    u64 replays = 0, judged = 0, flagged = 0, cause[8] = {};
    float maxErr = 0;
    // What the player can see: draws of the full-frame view covering at least kVisiblePx2 on screen.
    u64 visFlagged = 0, visCause[8] = {}, hiddenFlagged = 0;
    float visMaxErr = 0;
} sProbeTotals, sProbeWindow;
constexpr float kVisiblePx2 = 64.0f;  // (a 2160-line picture)
// blend: a blended pair off its ideal (a defect); turn / switch / jump: pairs the game itself turned
// by more than ~150 degrees, scaled 16x or to zero / mirrored, or moved by more than half the camera
// distance in one game frame: shown as the game frame shows them (correct by policy, listed apart);
// camera: untagged/unpaired/world off the in-between camera (a defect); other: no a-side truth.
const char* const kCauseName[8] = {"blend", "turn", "camera", "not-noted", "other", "switch", "jump", "?"};


bool perspective(const f32* P) { return P[14] == -1.0f && P[15] == 0.0f; }
// Screen position in pixels of a 2160-line picture (x scaled by the projection's aspect).
bool project(const f32* P, const f32 v[3], f32 out[2]) {
    const f32 x = P[0] * v[0] + P[1] * v[1] + P[2] * v[2] + P[3];
    const f32 y = P[4] * v[0] + P[5] * v[1] + P[6] * v[2] + P[7];
    const f32 w = P[12] * v[0] + P[13] * v[1] + P[14] * v[2] + P[15];
    if (!(w > 1.0f) || !std::isfinite(x) || !std::isfinite(y)) return false;
    const f32 aspect = P[0] != 0.0f ? P[5] / P[0] : 1.0f;
    out[0] = x / w * 1080.0f * aspect;
    out[1] = y / w * 1080.0f;
    return std::fabs(out[0]) < 1.3f * 1080.0f * aspect && std::fabs(out[1]) < 1.3f * 1080.0f;
}
void xform34(const f32* m, const f32 v[3], f32 o[3]) {
    for (int r = 0; r < 3; r++) o[r] = m[4 * r] * v[0] + m[4 * r + 1] * v[1] + m[4 * r + 2] * v[2] + m[4 * r + 3];
}
// The ideal in-between camera: rotation slerped, eye moved linearly.
bool idealCamera(const M34& va, const M34& vb, float alpha, M34& out) {
    M34 ra = va, rb = vb;
    ra[3] = ra[7] = ra[11] = rb[3] = rb[7] = rb[11] = 0;
    if (!blendRigid(ra.data(), rb.data(), 4, alpha, out.data())) return false;
    f32 ea[3], eb[3], e[3];
    eyeOf(va, ea);
    eyeOf(vb, eb);
    for (int c = 0; c < 3; c++) e[c] = ea[c] + (eb[c] - ea[c]) * alpha;
    for (int r = 0; r < 3; r++) out[4 * r + 3] = -(out[4 * r] * e[0] + out[4 * r + 1] * e[1] + out[4 * r + 2] * e[2]);
    return true;
}

// replayPair: an in-between replay is tagged draw by draw (probe tag = replay id << 20 | draw).
void probeBeforeReplay(const Tape* a, const Tape& b, float alpha, std::vector<u8>& buf) {
    if (!sProbeOut) return;
    static const bool sInit = [] {
        AuroraSetDrawProbe(probeCallback);
        return true;
    }();
    (void)sInit;
    static const bool sThread = [] {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                probeProcess();
            }
        }).detach();
        return true;
    }();
    (void)sThread;
    const u32 id = sProbeNextId++ & 0xFFF;
    static u32 sCount = 0;
    if (!a || alpha >= 1.0f || ++sCount % sProbeEvery != 0) {
        GXSetProbeTagAurora(id << 20 | 0xFFFFF);  // not judged
        return;
    }
    {
        std::lock_guard lk(sProbeIssuedM);
        sProbeIssued.push_back({id, nullptr, nullptr, alpha, sProbeNextId});
    }
    sProbeFill = true;  // the tapes are filled in by the caller, which holds them (probeFill)
    static std::vector<std::pair<u32, u32>> marks;  // offset, tag
    marks.clear();
    for (u32 k = 0; k < b.bundleRange.size(); k++) {
        const auto& r = b.bundleRange[k];
        if (r[0] == ~0u || r[1] == ~0u || r[1] <= r[0] || r[1] > buf.size()) continue;
        marks.emplace_back(r[0], id << 20 | (k & 0xFFFFF));
        marks.emplace_back(r[1], id << 20 | 0xFFFFF);
    }
    std::stable_sort(marks.begin(), marks.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    static std::vector<u8> out;
    out.clear();
    out.reserve(buf.size() + marks.size() * 7);
    u32 pos = 0;
    for (const auto& [at, tag] : marks) {
        if (at < pos) continue;
        out.insert(out.end(), buf.begin() + pos, buf.begin() + at);
        pos = at;
        out.push_back(0x50);
        out.push_back(u8(GX_AURORA_PROBE_TAG >> 8));
        out.push_back(u8(GX_AURORA_PROBE_TAG));
        for (int s = 24; s >= 0; s -= 8) out.push_back(u8(tag >> s));
    }
    out.insert(out.end(), buf.begin() + pos, buf.end());
    buf.swap(out);
}

// Judges one in-between replay's draws.
void probeJudge(const ProbeReplay& pr, std::vector<ProbeRec>& recs) {
    const Tape& b = *pr.b;
    const Tape* a = pr.a.get();
    if (!a || b.pairedWith != a || b.cut) return;
    sProbeTotals.replays++;
    u32 judged = 0, flagged = 0;
    for (const ProbeRec& r : recs) {
        const u32 k = r.tag & 0xFFFFF;
        if (k >= b.bundleKey.size() || !r.hasPos || !perspective(r.proj) || r.mtxIdx >= 10) continue;
        if (k < b.bundlePass.size() && b.bundlePass[k] != 0) continue;  // shadow caster pass: not on screen
        if (k >= b.bundleSlots.size()) continue;
        const s32 pb = b.bundleSlots[k][r.mtxIdx];
        if (pb < 0 || u32(pb) >= b.patches.size()) continue;  // matrix not noted (stream/indexed loads)
        const Patch& P = b.patches[pb];
        if (P.role != 0 || P.view >= b.views.size() || b.viewToA[P.view] < 0) continue;
        const s32 qv = b.viewToA[P.view];
        M34 vt, ib, ia;
        if (!idealCamera(a->views[qv], b.views[P.view], pr.alpha, vt) || !invertAffine(b.views[P.view].data(), ib.data()) ||
            !invertAffine(a->views[qv].data(), ia.data()))
            continue;
        f32 m[12], v[3], wb[3], wt[3], ideal[2], actual[2];
        for (int e = 0; e < 12; e++) m[e] = val(b, P, e);
        xform34(m, r.pos, v);
        xform34(ib.data(), v, wb);
        // The partner draw in a and the matrix its slot held there.
        int cause = 4;
        float whyRot = 0, whyScale = 1, whyJump = 0;  // a snapped pair: its turn (degrees), scale ratio, world jump
        std::copy(wb, wb + 3, wt);
        const s32 j = b.partner.empty() ? -1 : b.partner[pb];
        const bool cameraOnly = P.cat == TagCat::World || !P.key || j < 0;
        if (!cameraOnly) {
            const Patch& Q = a->patches[j];
            M34 iq;
            if (Q.view < a->views.size() && invertAffine(a->views[Q.view].data(), iq.data())) {
                f32 ma[12], va[3], wa[3];
                for (int e = 0; e < 12; e++) ma[e] = val(*a, Q, e);
                xform34(ma, r.pos, va);
                xform34(iq.data(), va, wa);
                cause = 0;
                // The ideal motion of a rigid part: its world transform blended (rotation slerped
                // about its origin, origin moved linearly), so a turning part's vertices follow an arc.
                // A pair too far apart to blend (an instant turn) is judged against the straight line.
                M34 Wa, Wb, Wt;
                compose(iq.data(), ma, Wa.data());
                compose(ib.data(), m, Wb.data());
                const float prevTurn = turnPolicy(P, noteTurn(a, j, b, pb, Wa.data(), Wb.data(), 4));
                const bool skew = skewFlip(Wa.data(), Wb.data()) || tumblerMirror(P, Wa.data(), Wb.data());
                if (!skew) {
                    resolveFlip(Wa.data(), Wb.data(), 4);
                    untwist(Wa.data(), Wb.data());
                }
                const u8 groupPolicy = b.groupSnap ? b.groupSnap[pb].load(std::memory_order_relaxed) : 0;
                const bool steadyMember = sScrewBlend && groupPolicy == 0 && u32(pb) < b.multiMember.size() && b.multiMember[pb] &&
                                          b.worldErratic && b.worldErratic[pb].load(std::memory_order_relaxed) == 0 &&
                                          blendableWorld(Wa.data(), Wb.data(), 1e9f, -1.0f, prevTurn, P.tumble, drawCentre(b, P));
                if (groupPolicy != 2 && !steadyMember &&
                    !blendableWorld(Wa.data(), Wb.data(), viewDist(m, drawCentre(b, P)),
                                    a->worldStep ? a->worldStep[j].load(std::memory_order_relaxed) : -1.0f, prevTurn, P.tumble,
                                    drawCentre(b, P))) {
                    cause = 1;
                    Rigid ra, rb;
                    if (decompose(Wa.data(), 4, ra) && decompose(Wb.data(), 4, rb)) {
                        const float d = std::fabs(ra.q[0] * rb.q[0] + ra.q[1] * rb.q[1] + ra.q[2] * rb.q[2] + ra.q[3] * rb.q[3]);
                        whyRot = 2.0f * std::acos(std::min(d, 1.0f)) * 57.2958f;
                        for (int kk = 0; kk < 3; kk++) {
                            const float sa = std::fabs(ra.u[kk][kk]), sb = std::fabs(rb.u[kk][kk]);
                            whyScale = std::max(whyScale, std::max(sa, sb) / std::max(std::min(sa, sb), 1e-6f));
                        }
                    } else {
                        whyScale = -1;  // degenerate (a zero scale)
                    }
                    whyJump = std::hypot(Wa[3] - Wb[3], Wa[7] - Wb[7], Wa[11] - Wb[11]);
                    cause = whyScale < 0 || whyScale > 16.0f ? 5 : whyRot > 150.0f ? 1 : whyJump > 200.0f ? 6 : 5;
                }
                if (cause == 0 && groupPolicy == 1) cause = 5;  // its instance switched
                const bool flat = skew || flatMatrix(Wa.data()) || flatMatrix(Wb.data());  // entrywise, as the replay
                if (cause == 0 && !flat && groupPolicy != 2 && blendRigid(Wa.data(), Wb.data(), 4, pr.alpha, Wt.data())) {
                    // (A spinner / tumbler: its drawn centre moves in a straight line, as replayed.)
                    // (The ideal of a member of an articulated instance is its own rigid motion, also with
                    // LM_PIVOT_MEMBERS: so that test aid's replays show what the old rule did.)
                    const bool member = u32(pb) < b.multiMember.size() && b.multiMember[pb];
                    if (prevTurn > kSpinnerTurn && !member && P.bundle < b.bundleCentre.size() && std::isfinite(b.bundleCentre[P.bundle][0])) {
                        const f32* c = b.bundleCentre[P.bundle].data();
                        M34 ra, rb;
                        compose(iq.data(), ma, ra.data());
                        compose(ib.data(), m, rb.data());
                        f32 pa[3], pbw[3], have[3];
                        xform34(ra.data(), c, pa);
                        xform34(rb.data(), c, pbw);
                        xform34(Wt.data(), c, have);
                        for (int rr = 0; rr < 3; rr++) Wt[4 * rr + 3] += pa[rr] + (pbw[rr] - pa[rr]) * pr.alpha - have[rr];
                    }
                    xform34(Wt.data(), r.pos, wt);
                }
                else if (cause != 0) std::copy(wb, wb + 3, wt);  // shown at the game frame's place (policy)
                else
                    for (int c = 0; c < 3; c++) wt[c] = wa[c] + (wb[c] - wa[c]) * pr.alpha;
            }
        } else {
            cause = 2;
            // Born moving along its step (tagBackStep): its place is b's less (1 - alpha) steps.
            if (j < 0 && P.backStep >= 0 && u32(P.backStep) < b.backSteps.size())
                for (int c = 0; c < 3; c++) wt[c] -= (1.0f - pr.alpha) * b.backSteps[P.backStep][c];
        }
        f32 vtw[3], av[3];
        xform34(vt.data(), wt, vtw);
        if (!project(r.proj, vtw, ideal)) continue;
        xform34(r.mtx, r.pos, av);
        if (!project(r.proj, av, actual)) continue;
        judged++;
        const float err = std::hypot(actual[0] - ideal[0], actual[1] - ideal[1]);
        const bool policySwitch = cause == 1 || cause == 5 || cause == 6;
        if (!(err > sProbeThresh) && !policySwitch) continue;
        if (!policySwitch) flagged++;
        // Visibility: the draw's screen extent (clipped to the picture) in a full-frame viewport.
        float area = 0;
        if (r.ndcMax[0] >= r.ndcMin[0] && r.ndcMax[1] >= r.ndcMin[1]) {
            const float aspect = r.proj[0] != 0.0f ? r.proj[5] / r.proj[0] : 1.0f;
            const float w = (std::min(r.ndcMax[0], 1.0f) - std::max(r.ndcMin[0], -1.0f)) * 1080.0f * aspect;
            const float h = (std::min(r.ndcMax[1], 1.0f) - std::max(r.ndcMin[1], -1.0f)) * 1080.0f;
            area = w > 0 && h > 0 ? w * h : 0.0f;
        }
        const bool fullView = r.viewport[2] >= 600.0f && r.viewport[3] >= 440.0f;
        const bool visible = fullView && area >= kVisiblePx2;
        if (policySwitch) {  // counted, not a defect (and not logged line by line)
            sProbeTotals.cause[cause]++, sProbeWindow.cause[cause]++;
            if (visible) sProbeTotals.visCause[cause]++, sProbeWindow.visCause[cause]++;
            static u64 sLastKey = 0;  // one S line per matrix (a draw reports many vertices' records)
            if (P.key != sLastKey) {
                probeLine("S %u %llu %.3f %s cat %s key %016llX site %08X area %.0f %s why %.0f %.2f %.0f world %.0f %.0f %.0f src %08X\n", pr.present,
                          (unsigned long long)b.seq, pr.alpha, kCauseName[cause], kCatName[u32(P.cat)], (unsigned long long)P.key, P.site,
                          area, visible ? "visible" : fullView ? "tiny" : "subview", whyRot, whyScale, whyJump, wb[0], wb[1], wb[2], P.source);
                // LM_JUDDER_MATRICES=1: both game frames' world matrices of the pair (orientation analysis).
                static const bool sMats = std::getenv("LM_JUDDER_MATRICES") != nullptr;
                if (sMats && j >= 0) {
                    const Patch& Q = a->patches[j];
                    M34 ia2, ib2, A2, B2;
                    f32 ma[12], mb[12];
                    for (int e = 0; e < 12; e++) ma[e] = val(*a, Q, e), mb[e] = val(b, P, e);
                    if (Q.view < a->views.size() && invertAffine(a->views[Q.view].data(), ia2.data()) && invertAffine(b.views[P.view].data(), ib2.data())) {
                        compose(ia2.data(), ma, A2.data());
                        compose(ib2.data(), mb, B2.data());
                        probeLine("M A %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f\n", A2[0], A2[1], A2[2], A2[3], A2[4], A2[5],
                                  A2[6], A2[7], A2[8], A2[9], A2[10], A2[11]);
                        probeLine("M B %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f\n", B2[0], B2[1], B2[2], B2[3], B2[4], B2[5],
                                  B2[6], B2[7], B2[8], B2[9], B2[10], B2[11]);
                    }
                }
            }
            sLastKey = P.key;
            continue;
        }
        if (visible) {
            sProbeWindow.visFlagged++, sProbeTotals.visFlagged++;
            sProbeWindow.visCause[cause]++, sProbeTotals.visCause[cause]++;
            sProbeWindow.visMaxErr = std::max(sProbeWindow.visMaxErr, err);
        } else {
            sProbeWindow.hiddenFlagged++, sProbeTotals.hiddenFlagged++;
        }
        sProbeTotals.cause[cause]++;
        sProbeWindow.cause[cause]++;
        sProbeTotals.maxErr = std::max(sProbeTotals.maxErr, err);
        sProbeWindow.maxErr = std::max(sProbeWindow.maxErr, err);
        probeLine( "J %u %llu %.3f %s %.2f cat %s key %016llX site %08X slot %u draw %u act %.0f %.0f ideal %.0f %.0f world %.0f %.0f %.0f area %.0f %s why %.0f %.2f %.0f\n",
                     pr.present, (unsigned long long)b.seq, pr.alpha, kCauseName[cause], err, kCatName[u32(P.cat)],
                     (unsigned long long)P.key, P.site, r.mtxIdx, k, actual[0], actual[1], ideal[0], ideal[1], wb[0], wb[1], wb[2], area,
                     visible ? "visible" : fullView ? "tiny" : "subview", whyRot, whyScale, whyJump);
    }
    sProbeTotals.judged += judged;
    sProbeTotals.flagged += flagged;
    sProbeWindow.judged += judged;
    sProbeWindow.flagged += flagged;
    sProbeWindow.replays++;
    probeLine( "R %u %llu %.3f judged %u flagged %u\n", pr.present, (unsigned long long)b.seq, pr.alpha, judged, flagged);
}

// Path check (LM_TRACK_LOG; docs/INTERPOLATION.md "The hose nozzle"). Independent of the blending
// policies: every draw of the main pass whose first vertex lies within sTrackRadius of Luigi is
// followed in world space (the drawn view-space place through the inverse of the ideal in-between
// camera) and compared with the straight line between its two game-frame places (the matrices its
// slot held at capture in a and b). Anything that leaves that line by more than a rigid turn's arc
// allows (0.4 x the game-frame step + 1 unit) and sTrackPx pixels (2160-line picture) is a jump,
// switches by policy included: a part shown at b (or held at a) for an interval while the rest of
// the body blends is what the eye sees as a jump. H lines per flagged vertex, HR per replay.
struct TrackTotals {
    u64 replays = 0, judged = 0, flagged = 0, nozJudged = 0, nozFlagged = 0, unknown = 0, unpaired = 0;
    float maxPx = 0, nozMaxPx = 0;
} sTrackTotals, sTrackWindow;
void trackJudge(const ProbeReplay& pr, const std::vector<ProbeRec>& recs) {
    const Tape& b = *pr.b;
    const Tape* a = pr.a.get();
    if (!a || b.pairedWith != a || b.cut) return;
    const bool haveLuigi = a->trackOk && b.trackOk;  // (without him only own-camera scenes are judged)
    const float t = pr.alpha;
    f32 luigi[3], noz[3];
    for (int c = 0; c < 3; c++) {
        luigi[c] = a->track[c] + (b.track[c] - a->track[c]) * t;
        noz[c] = a->track[3 + c] + (b.track[3 + c] - a->track[3 + c]) * t;  // (+0x7A4: placed at the nozzle)
    }
    const bool haveNoz = std::isfinite(noz[0] + noz[1] + noz[2]);
    // The ideal in-between camera of each view pair (inverse), by b's view index.
    static std::unordered_map<u32, std::pair<M34, M34>> cams;  // vt, its inverse
    cams.clear();
    const auto camOf = [&](u16 v) -> const std::pair<M34, M34>* {
        if (v >= b.views.size() || v >= b.viewToA.size() || b.viewToA[v] < 0 || u32(b.viewToA[v]) >= a->views.size()) return nullptr;
        auto it = cams.find(v);
        if (it == cams.end()) {
            std::pair<M34, M34> c;
            if (!idealCamera(a->views[b.viewToA[v]], b.views[v], t, c.first) || !invertAffine(c.first.data(), c.second.data())) return nullptr;
            it = cams.emplace(v, c).first;
        }
        return &it->second;
    };
    static std::unordered_map<u32, u32> occ;  // per draw: records so far (one per primitive)
    occ.clear();
    struct Census {
        u32 n = 0, srcLo = ~0u, srcHi = 0;
        float sum[3] = {}, nozMin = 1e9f;
        std::unordered_set<u64> keys;
    };
    static std::unordered_map<u64, Census> census;
    static u64 sCensusSeq = 0;
    u32 judged = 0, flagged = 0, nozJ = 0, nozF = 0;
    float maxPx = 0;
    for (const ProbeRec& r : recs) {
        const u32 k = r.tag & 0xFFFFF;
        const u32 n = occ[k]++;
        if (k >= b.bundleKey.size() || !r.hasPos || !perspective(r.proj) || r.mtxIdx >= 10) continue;
        if (k < b.bundlePass.size() && b.bundlePass[k] != 0) continue;
        if (!(r.viewport[2] >= 600.0f && r.viewport[3] >= 440.0f)) continue;
        const s32 pb = k < b.bundleSlots.size() ? b.bundleSlots[k][r.mtxIdx] : -1;
        const Patch* P = pb >= 0 && u32(pb) < b.patches.size() ? &b.patches[pb] : nullptr;
        if (P && (P->role != 0 || P->kind != Kind::PosMtx || P->count != 12)) P = nullptr;
        // A scene with its own camera (events: fn_80114E18) is blended as drawn, in view space: judged
        // there (every vertex of it, Luigi's place being in another space).
        const bool own = P && P->ownCamera;
        static const std::pair<M34, M34> kIdentity{M34{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, M34{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
        const auto* cam = own ? &kIdentity : camOf(P ? P->view : b.mainView);
        if (!cam) continue;
        f32 e[3], aw[3];
        xform34(r.mtx, r.pos, e);
        xform34(cam->second.data(), e, aw);
        if (!own && (!haveLuigi || std::hypot(aw[0] - luigi[0], aw[1] - luigi[1], aw[2] - luigi[2]) > sTrackRadius)) continue;
        const bool nearNoz = !own && haveNoz && std::hypot(aw[0] - noz[0], aw[1] - noz[1], aw[2] - noz[2]) < 60.0f;
        const s32 j = P && !b.partner.empty() ? b.partner[pb] : -1;
        if (!P) {
            sTrackTotals.unknown++, sTrackWindow.unknown++;
            continue;
        }
        if (j < 0 || !P->key || P->cat == TagCat::World) {
            sTrackTotals.unpaired++, sTrackWindow.unpaired++;
            continue;
        }
        // The element streams' drops take a random orientation every game frame (tumblers): their
        // vertices have no straight path to follow (spray_check.py judges them by their centre).
        if (P->tumble || P->stream) continue;
        const Patch& Q = a->patches[j];
        M34 ia = kIdentity.first, ib = kIdentity.first;
        if (!own && (Q.view >= a->views.size() || P->view >= b.views.size() || !invertAffine(a->views[Q.view].data(), ia.data()) ||
                     !invertAffine(b.views[P->view].data(), ib.data())))
            continue;
        if (own && !Q.ownCamera) continue;
        f32 ma[12], mb[12], va[3], vb[3], wa[3], wb[3];
        for (int c = 0; c < 12; c++) ma[c] = val(*a, Q, c), mb[c] = val(b, *P, c);
        xform34(ma, r.pos, va);
        xform34(ia.data(), va, wa);
        xform34(mb, r.pos, vb);
        xform34(ib.data(), vb, wb);
        f32 line[3], d[3];
        float chord2 = 0, along = 0;
        for (int c = 0; c < 3; c++) {
            line[c] = wa[c] + (wb[c] - wa[c]) * t;
            d[c] = wb[c] - wa[c];
            chord2 += d[c] * d[c];
            along += (aw[c] - wa[c]) * d[c];
        }
        const float chord = std::sqrt(chord2);
        // (An own-camera model that moves by more than a tenth of its distance in one game frame was
        // placed anew or its camera cut: shown at b by design, like a cut of the game camera.)
        if (own && chord > std::max(20.0f, 0.1f * std::sqrt(wb[0] * wb[0] + wb[1] * wb[1] + wb[2] * wb[2]))) continue;
        const float errW = std::hypot(aw[0] - line[0], aw[1] - line[1], aw[2] - line[2]);
        // Group census (every 60 tapes): each model instance drawn near Luigi, its members' place;
        // every 600 tapes all vertices (HV lines: what each instance is, by its shape).
        if (b.seq % 600 == 0 && b.seq != sCensusSeq)
            trackLine("HV %llu grp %016llX src %08X %.1f %.1f %.1f\n", (unsigned long long)b.seq, (unsigned long long)P->group, P->source,
                      aw[0] - luigi[0], aw[1] - luigi[1], aw[2] - luigi[2]);
        if (b.seq % 60 == 0 && P->group) {
            auto& g = census[P->group];
            g.n++;
            g.srcLo = std::min(g.srcLo, P->source), g.srcHi = std::max(g.srcHi, P->source);
            for (int c = 0; c < 3; c++) g.sum[c] += aw[c] - luigi[c];
            if (haveNoz) g.nozMin = std::min(g.nozMin, std::hypot(aw[0] - noz[0], aw[1] - noz[1], aw[2] - noz[2]));
            g.keys.insert(P->key);
        }
        f32 lv[3], ps[2], pa[2];
        xform34(cam->first.data(), line, lv);
        if (!project(r.proj, lv, ps) || !project(r.proj, e, pa)) continue;
        const float errPx = std::hypot(ps[0] - pa[0], ps[1] - pa[1]);
        judged++;
        nozJ += nearNoz;
        // A rigid turn moves a vertex along an arc, off the line by up to ~0.4 of its step mid-interval
        // and little near the game frames; a part shown at b early (or held at a), or swinging out
        // farther than any turn would, is a jump.
        const float arc = 0.45f * std::min(1.0f, 4.0f * t * (1.0f - t)) + 0.03f;
        if (!(errW > 1.0f + arc * chord) || !(errPx > sTrackPx)) continue;
        flagged++;
        nozF += nearNoz;
        maxPx = std::max(maxPx, errPx);
        const u8 pol = b.groupSnap ? b.groupSnap[pb].load(std::memory_order_relaxed) : 0;
        const bool member = u32(pb) < b.multiMember.size() && b.multiMember[pb];
        M34 Wa, Wb;
        compose(ia.data(), ma, Wa.data());
        compose(ib.data(), mb, Wb.data());
        const float turn = turnDegrees(Wa.data(), Wb.data(), 4);
        const float ostep = std::hypot(Wa[3] - Wb[3], Wa[7] - Wb[7], Wa[11] - Wb[11]);
        trackLine("H %u %llu f%u %.3f px %.1f w %.1f chord %.1f frac %.2f turn %.0f ostep %.1f cen %d pol %u mm %d noz %d own %d key %016llX grp %016llX site %08X src %08X slot %u draw %u prim %u "
                  "at %.1f %.1f %.1f a %.1f %.1f %.1f b %.1f %.1f %.1f\n",
                  pr.present, (unsigned long long)b.seq, b.gameFrame, t, errPx, errW, chord, chord2 > 1e-6f ? along / chord2 : 0.0f, turn, ostep,
                  int(drawCentre(b, *P) != nullptr), pol, int(member), int(nearNoz), int(own), (unsigned long long)P->key, (unsigned long long)P->group, P->site, P->source, r.mtxIdx, k, n,
                  aw[0], aw[1], aw[2], wa[0], wa[1], wa[2], wb[0], wb[1], wb[2]);
    }
    if (b.seq % 60 == 0 && b.seq != sCensusSeq && !census.empty()) {
        sCensusSeq = b.seq;
        for (const auto& [g, c] : census)
            if (c.n >= 8)
                trackLine("HG %llu grp %016llX vertices %u matrices %zu src %08X-%08X off %.0f %.0f %.0f nozzle %.0f\n", (unsigned long long)b.seq,
                          (unsigned long long)g, c.n, c.keys.size(), c.srcLo, c.srcHi, c.sum[0] / c.n, c.sum[1] / c.n, c.sum[2] / c.n, c.nozMin);
    }
    census.clear();
    trackLine("HR %u %llu %.3f judged %u flagged %u noz %u %u max %.1f\n", pr.present, (unsigned long long)b.seq, t, judged, flagged, nozJ, nozF,
              maxPx);
    for (TrackTotals* tt : {&sTrackTotals, &sTrackWindow}) {
        tt->replays++, tt->judged += judged, tt->flagged += flagged, tt->nozJudged += nozJ, tt->nozFlagged += nozF;
        tt->maxPx = std::max(tt->maxPx, maxPx);
    }
}

void probeProcess() {
    if (!sProbeOut) return;
    std::vector<ProbeRec> in;
    {
        std::lock_guard lk(sProbeM);
        in.swap(sProbeIn);
    }
    // Records arrive when Aurora has processed a replay (a frame or more after it was issued): a
    // replay is complete once a later one has records (the FIFO is in order).
    static std::unordered_map<u32, std::vector<ProbeRec>> byReplay;
    for (const ProbeRec& r : in) byReplay[r.tag >> 20].push_back(r);
    for (;;) {
        ProbeReplay pr;
        {
            std::lock_guard lk(sProbeIssuedM);
            if (sProbeIssued.empty()) break;
            bool later = false;
            for (size_t k = 1; k < sProbeIssued.size() && !later; k++) later = byReplay.count(sProbeIssued[k].id) != 0;
            if (!later && sProbeIssued.size() < 32) break;
            pr = std::move(sProbeIssued.front());
            sProbeIssued.pop_front();
        }
        if (auto it = byReplay.find(pr.id); it != byReplay.end()) {
            if (pr.b) probeJudge(pr, it->second);
            if (pr.b && sTrackOut) trackJudge(pr, it->second);
            byReplay.erase(it);
        }
    }
    if (byReplay.size() > 256) byReplay.clear();
    static u32 n = 0;
    if (++n % 64 == 0) {
        std::fflush(sProbeOut);
        if (sTrackOut) {
            std::lock_guard lk(sTrackFileM);
            std::fflush(sTrackOut);
        }
    }
}

void auroraCmd(std::vector<u8>& out, u16 sub, u8 arg) {
    out.push_back(0x50);
    out.push_back(u8(sub >> 8));
    out.push_back(u8(sub));
    out.push_back(arg);
}

// A value shown from the newest frame (untagged, unpaired, static world geometry, or a pair
// that jumped): b's own placement in the world seen from the in-between camera, so it stays where
// it is in the world while the camera moves. 2D, shadow-receiver and values without a camera
// counterpart are left as captured.
bool placeUnderCamera(const Tape& a, const Tape& b, const Patch& p, float alpha) {
    if (p.role != 0 || p.count > 16 || p.view >= sViewToA.size() || sViewToA[p.view] < 0) return false;
    // Untagged values outside every scope (frame setup, J3D's default matrices) are not things in the
    // 3D view: as captured.
    if (!p.key && !p.inScope) return false;
    if (p.kind != Kind::PosMtx && p.kind != Kind::NrmMtx && p.kind != Kind::Light) return false;
    const ViewPair* vp = viewPair(a, u16(sViewToA[p.view]), b, p.view, alpha);
    if (!vp) return false;
    f32 v[16], out[16];
    for (int k = 0; k < p.count; k++) v[k] = val(b, p, k);
    if (!reexpress(*vp, false, p, v, out)) return false;
    for (int k = 0; k < p.count; k++) storeBE(sReplay.data() + p.offset + 4 * k, out[k]);
    return true;
}

// Model matrices drawn in a scene with its own camera (TagCat::SubView: event scenes such as the
// walk up to the mansion door after E. Gadd's lab, door close-ups, the call portrait). Their
// values are not camera-factored (that camera is not the game camera), so each one is blended as
// drawn, in that camera's space. A rigid blend there turns the matrix about its own origin, and
// the event scenes' models have their origin far from their geometry (the walk-up's Luigi: 4000-
// 5600 units, the demo scene's origin): a turn of a few degrees per game frame (the following
// camera, a swinging limb) moved the geometry off its path by several units mid-interval, by a
// different amount for every matrix of a skinned mesh, and the hair at the back of Luigi's head
// poked through his cap (docs/INTERPOLATION.md "Event scenes: models far from their origin").
// - A skinned instance (a member sheared or flat: an envelope's weighted sum of joints) is
//   blended entrywise as a whole: every vertex moves in a straight line, the skin stays together
//   (as the game camera's skinned instances are on a fast turn).
// - Any other matrix is blended rigidly about the centre of its draw (its geometry), not about
//   its origin: that centre moves in a straight line (as spinners do under the game camera).
// LM_OWNCAM_INSTANCES=0 (test aid): the old blend (each matrix rigidly about its origin).
const bool sOwnInstances = [] {
    const char* v = std::getenv("LM_OWNCAM_INSTANCES");
    return !v || std::atoi(v) != 0;
}();
// A skin envelope's matrix: sheared (a weighted sum of joints that point different ways) or flat.
bool skinMatrix(const f32* m) {
    Rigid r;
    if (!decompose(m, 4, r) || flatMatrix(m)) return true;
    const float diag = std::max({std::fabs(r.u[0][0]), std::fabs(r.u[1][1]), std::fabs(r.u[2][2]), 1e-6f});
    return std::max({std::fabs(r.u[0][1]), std::fabs(r.u[0][2]), std::fabs(r.u[1][2])}) > 0.08f * diag;
}

// Builds tape b with its values blended from a toward b (alpha 1 = b as captured) into
// sReplay and submits it. `dump` redirects the LM_FRAME_DUMP copy (LM_INTERP_TEST).
// Policies (docs/INTERPOLATION.md "Tagged interpolation"):
// - a value pairs only with the value of a that has the same identity key (pairTapes);
// - static world geometry (TagCat::World): only the camera moves it (b's world matrix under the
//   in-between camera); objects, particles, HUD: blended (rotation slerp, scale/shear, translation,
//   in world space under the blended camera);
// - colours and other material registers are never blended: always b's;
// - untagged or unpaired: b's values, placed under the in-between camera when in the 3D view;
// - appear/disappear: b's set of draws is drawn for the whole interval (switch at the game frame),
//   never faded;
// - a camera cut shows b as captured.
void replayPair(const Tape* a, const Tape& b, float alpha, void* dump) {
    sCrossCamera = 0;
    sReplay.assign(b.bytes.begin(), b.bytes.end());
    if (a && b.pairedWith != a) a = nullptr;  // not consecutive (a tape was dropped): no pairing
    if (a && b.cut) {
        sCuts++;
        a = nullptr;
    }
    if (a && alpha < 1.0f && sLogSnaps) {  // self-check: the rebuilt values at alpha 1 must equal b's
        sViewPairs.clear();
        sViewToA = b.viewToA;
        makeLightPair(*a, b, 1.0f);
        receiverGs(*a, sRecvA);
        receiverGs(b, sRecvB);
        u32 bad = 0, checked = 0;
        for (u32 i = 0; i < b.patches.size(); i++) {
            const s32 j = b.partner[i];
            if (j < 0) continue;
            const Patch& p = b.patches[i];
            const Patch& q = a->patches[j];
            if (p.kind == Kind::Verts || p.kind == Kind::Color || p.count > 16) continue;
            f32 va[16], vb[16], out[16];
            for (int k = 0; k < p.count; k++) va[k] = val(*a, q, k), vb[k] = val(b, p, k);
            bool ok;
            if (p.role == 1) ok = blendShadowReceiver(*a, q, va, b, p, vb, 1.0f, out);
            else ok = blendFactored(*a, q, va, b, p, vb, 1.0f, out);
            if (!ok) continue;
            checked++;
            float e = 0;
            for (int k = 0; k < p.count; k++) e = std::max(e, std::fabs(out[k] - vb[k]) / (1.0f + std::fabs(vb[k])));
            if (e > 5e-3f && bad++ < 6) {  // float noise: values near 0 under a far camera
                char buf[600];
                int n = std::snprintf(buf, sizeof(buf), "[interp]   self-check kind %u role %u slot %u err %.4f:", u32(p.kind), p.role, p.slot, e);
                for (int k = 0; k < p.count && n < 560; k++) n += std::snprintf(buf + n, sizeof(buf) - n, " %.2f/%.2f", out[k], vb[k]);
                log("%s", buf);
            }
        }
        log("[interp]   self-check: %u of %u rebuilt values differ at alpha 1", bad, checked);
    }
    if (a && alpha < 1.0f) {
        sViewPairs.clear();
        sLightsBlended.clear();
        sViewToA = b.viewToA;
        makeLightPair(*a, b, alpha);
        receiverGs(*a, sRecvA);
        receiverGs(b, sRecvB);
        // LM_INTERP_NOBLEND=<mask> (test aid): value kinds (bit = Kind) shown as captured.
        static const u32 sNoBlendKinds = [] {
            const char* v = std::getenv("LM_INTERP_NOBLEND");
            return v ? u32(std::strtoul(v, nullptr, 0)) : 0u;
        }();
        const auto snap = [&](const Patch& p, const f32* va, const f32* vb) {
            sSnapped++;
            sSnapKinds[u32(p.kind)]++;
            if (sLogSnaps && sLoggedSnaps[u32(p.kind)]++ < 12) {
                char buf[512];
                int n = std::snprintf(buf, sizeof(buf), "[interp]   snap kind %u cat %s site %08X slot %u:", u32(p.kind),
                                      kCatName[u32(p.cat)], p.site, p.slot);
                for (int k = 0; va && k < p.count && n < 480; k++) n += std::snprintf(buf + n, sizeof(buf) - n, " %.2f>%.2f", va[k], vb[k]);
                log("%s", buf);
            }
            placeUnderCamera(*a, b, p, alpha);
        };
        // A thing just born (no partner, or its slot re-placed) with a known step per game frame
        // (tagBackStep): shown moving along that step into its place, b - (1 - alpha) step, in world
        // space under the in-between camera, instead of standing at b's place for the whole interval
        // (the water spray's new points and drops stood still for one interval, then moved on).
        const auto backStep = [&](const Patch& p) -> bool {
            if (p.backStep < 0 || u32(p.backStep) >= b.backSteps.size()) return false;
            const auto& st = b.backSteps[p.backStep];
            const float k = 1.0f - alpha;
            if (p.kind == Kind::Verts && p.role == 0) {
                for (u32 v = 0; v < p.count; v++)
                    for (int c = 0; c < 3; c++)
                        storeBE(sReplay.data() + p.offset + v * p.stride + p.posOffset + 4 * c, vtx(b, p, v, c) - k * st[c]);
                sBornMoving++;
                return true;
            }
            if (p.kind != Kind::PosMtx || p.count != 12 || p.role != 0 || p.ownCamera || p.view >= sViewToA.size() || sViewToA[p.view] < 0)
                return false;
            const ViewPair* vp = viewPair(*a, u16(sViewToA[p.view]), b, p.view, alpha);
            if (!vp || !vp->ok || vp->cut) return false;
            f32 vb[12];
            M34 wb;
            for (int c = 0; c < 12; c++) vb[c] = val(b, p, c);
            compose(vp->ib.data(), vb, wb.data());
            for (int r = 0; r < 3; r++) wb[4 * r + 3] -= k * st[r];
            f32 out[12];
            compose(vp->vt.data(), wb.data(), out);
            for (int c = 0; c < 12; c++) storeBE(sReplay.data() + p.offset + 4 * c, out[c]);
            sBornMoving++;
            return true;
        };
        // A model instance (an S3D model: fn_80059C54 & co., or a J3D model: its packets' matrix buffer;
        // tagGroup) is handled as one. Skinned parts are drawn with weighted sums of its joints (sheared,
        // nearly flat where two joints differ a lot), so the skin only stays together if every matrix of
        // the instance is blended the same way:
        //  - a matrix that jumps (moves farther than blending allows: a respawn, a teleport) switches
        //    the whole instance (groupSnap 1);
        //  - when any matrix turns by more than 90 degrees in the game frame (or is squashed flat, or
        //    turns or scales too far to blend at all), every matrix of the instance is blended
        //    entrywise in world space (groupSnap 2): each skinned vertex then moves in a straight
        //    line from its place in a to its place in b, the same for all of them.
        // The Poltergust hose on the vacuum's on/off: its envelope matrices turn by 100-150 degrees in
        // one game frame. Blended one by one (slerp, flip and twist resolution chosen per matrix, a
        // few switched) neighbouring matrices took different paths and the skin between them
        // stretched into long straight sticks (user clips, 0a5e5f2: tape 160 a 0.05 / 0.34, tape 165
        // a 0.67); switching the whole hose (2f69188) detached it from Luigi's hand.
        {
            // Per instance of this tape: its members, the jump vote and the five policies (snap /
            // force / fast / skin / tear below). Kept across presents with a generation stamp like
            // GenTable: clearing per present reallocated every member list and freed five sets' nodes
            // (replayPair runs twice per game frame at 60 Hz). `live` is what this present touched.
            struct GroupRec {
                std::vector<u32> members;
                u32 voters = 0, jumped = 0;
                u32 gen = 0;
                u8 snap = 0, force = 0, fast = 0, skin = 0, tear = 0;
            };
            static std::unordered_map<u64, GroupRec> groups;
            static std::vector<u64> live;
            static u32 gen = 0;
            live.clear();
            // Group keys are per-tape identities (TagFrame::group): nothing recurs across tapes, so
            // the table is dropped once it holds a few tapes' worth instead of growing with them.
            if (++gen == 0 || groups.size() > (1u << 13)) {  // (0 is the generation of a never-written entry)
                groups.clear();
                gen = 1;
            }
            const auto groupRec = [](u64 g) -> GroupRec& {
                GroupRec& r = groups[g];
                if (r.gen != gen) {
                    r.gen = gen;
                    r.members.clear();
                    r.voters = r.jumped = 0;
                    r.snap = r.force = r.fast = r.skin = r.tear = 0;
                    live.push_back(g);
                }
                return r;
            };
            const auto findRec = [](u64 g) -> const GroupRec* {
                const auto it = groups.find(g);
                return it != groups.end() && it->second.gen == gen ? &it->second : nullptr;
            };
            // Same lookup without inserting: the loops below walk `live`, and groupRec() could rehash
            // the table under them.
            const auto findRecMut = [&findRec](u64 g) -> GroupRec* { return const_cast<GroupRec*>(findRec(g)); };
            // The members, their world matrices in a and b, and per instance the median place of its
            // members in b: a member far from all the others (more than 4x the instance's median
            // spread and 250 units) skins nothing visible of it and has no say (the nursery bears' one
            // erratic draw matrix lands 400-900 units from the bear's joints every game frame; its
            // previous step was not always large, sweep B).
            struct Member {
                u32 i;
                s32 j;
                M34 wa, wb;
                float dist;
                float step;         // placeStep(wa, wb) about `centre`: the instance median and the
                                    // member's own classification both need it
                const f32* centre;  // drawCentre(b, patches[i]): pure in (b, patch)
                f32 cb[3];  // where its geometry is in b (world; the slot's vertex centre, else the origin)
            };
            static std::vector<Member> members;
            members.clear();
            for (u32 i = 0; i < b.patches.size(); i++) {
                const Patch& p = b.patches[i];
                const s32 j = b.partner[i];
                if (!p.group || j < 0 || p.kind != Kind::PosMtx || p.count != 12 || p.role != 0 || p.ownCamera || p.cat == TagCat::World)
                    continue;
                if (p.bundle < b.bundlePass.size() && b.bundlePass[p.bundle] == 1 && sLightPair.ok) continue;  // caster pass: blends
                if (!(p.view < sViewToA.size() && sViewToA[p.view] == s32(a->patches[j].view))) continue;
                const ViewPair* vp = viewPair(*a, a->patches[j].view, b, p.view, alpha);
                if (!vp || !vp->ok || vp->cut) continue;
                f32 va[12], vb[12];
                for (int k = 0; k < 12; k++) va[k] = val(*a, a->patches[j], k), vb[k] = val(b, p, k);
                Member m{i, j};
                m.centre = drawCentre(b, p);
                compose(vp->ia.data(), va, m.wa.data());
                compose(vp->ib.data(), vb, m.wb.data());
                m.dist = viewDist(vb, m.centre);
                {
                    const f32* c = sScrewBlend ? m.centre : nullptr;
                    for (int r = 0; r < 3; r++)
                        m.cb[r] = c ? m.wb[4 * r] * c[0] + m.wb[4 * r + 1] * c[1] + m.wb[4 * r + 2] * c[2] + m.wb[4 * r + 3] : m.wb[4 * r + 3];
                }
                groupRec(p.group).members.push_back(u32(members.size()));
                members.push_back(m);
            }
            static std::vector<u8> outlier;
            outlier.assign(members.size(), 0);
            static std::vector<float> groupStep;  // per member: the median step of its instance's members
            groupStep.assign(members.size(), 1e9f);
            for (const u64 g : live) {
                const std::vector<u32>& idx = findRec(g)->members;  // live: no insert while iterating
                {
                    static std::vector<float> st;
                    st.clear();
                    for (const u32 m : idx) {
                        members[m].step = placeStep(members[m].wa.data(), members[m].wb.data(), members[m].centre);
                        st.push_back(members[m].step);
                    }
                    std::nth_element(st.begin(), st.begin() + st.size() / 2, st.end());
                    if (idx.size() >= 4)
                        for (const u32 m : idx) groupStep[m] = st[st.size() / 2];
                }
                if (idx.size() < 4) continue;
                static std::vector<float> xs[3], ds;
                f32 med[3];
                for (int c = 0; c < 3; c++) {
                    xs[c].clear();
                    for (const u32 m : idx) xs[c].push_back(members[m].cb[c]);
                    std::nth_element(xs[c].begin(), xs[c].begin() + xs[c].size() / 2, xs[c].end());
                    med[c] = xs[c][xs[c].size() / 2];
                }
                ds.clear();
                for (const u32 m : idx)
                    ds.push_back(std::hypot(members[m].cb[0] - med[0], members[m].cb[1] - med[1], members[m].cb[2] - med[2]));
                static std::vector<float> sorted;
                sorted.assign(ds.begin(), ds.end());
                std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
                const float spread = sorted[sorted.size() / 2];
                for (size_t k = 0; k < idx.size(); k++)
                    if (ds[k] > std::max(250.0f, 4.0f * spread)) outlier[idx[k]] = 1;
            }
            // A real jump of an instance (a respawn, a teleport) moves most of its members alike; one
            // of 49 jumping is a stray matrix and switches nothing (Luigi's J3D instance: one member's
            // first erratic step after a quiet spell switched all of Luigi, sweep B).
            // Test aid LM_GROUP_LOG=<file>: per tape (its first in-between replay), every model instance of 4
            // or more paired members: members, outliers, erratic (now / held), voters, jumped, fast, skin,
            // sheared flips, members without partner, the members' source range, and the policy taken
            // (0 per matrix, 1 switched, 2 entrywise); E lines name each erratic / outlier member.
            static FILE* sGroupLog = [] {
                const char* v = std::getenv("LM_GROUP_LOG");
                return v && *v ? std::fopen(v, "w") : nullptr;
            }();
            static u64 sGroupLogSeq = 0;
            const bool groupLog = sGroupLog && b.seq != sGroupLogSeq;
            if (groupLog) sGroupLogSeq = b.seq;
            sGroupLogNow = groupLog;
            if (groupLog) {
                sExcluded.assign(b.patches.size(), 0);
                sExclStats.clear();
                sPivotStats.clear();
            }
            struct GStat {
                u32 members = 0, outliers = 0, erraticNow = 0, erraticHeld = 0, fast = 0, skin = 0, skew = 0, turn90 = 0, unpaired = 0;
                u32 srcLo = ~0u, srcHi = 0;
            };
            static std::unordered_map<u64, GStat> gstat;
            gstat.clear();
            if (groupLog) {
                for (const u64 g : live) {
                    const std::vector<u32>& idx = findRec(g)->members;
                    GStat& st = gstat[g];
                    st.members = u32(idx.size());
                    for (const u32 m : idx) {
                        const u32 src = b.patches[members[m].i].source;
                        st.srcLo = std::min(st.srcLo, src), st.srcHi = std::max(st.srcHi, src);
                        st.outliers += outlier[m];
                    }
                }
                for (u32 i = 0; i < b.patches.size(); i++) {
                    const Patch& p = b.patches[i];
                    if (p.group && b.partner[i] < 0 && p.kind == Kind::PosMtx && p.count == 12 && p.role == 0 && gstat.count(p.group))
                        gstat[p.group].unpaired++;
                }
            }
            for (size_t mi = 0; mi < members.size(); mi++) {
                const u32 i = members[mi].i;
                const s32 j = members[mi].j;
                const Patch& p = b.patches[i];
                GroupRec& rec = groupRec(p.group);  // (already live: the member loop above made it)
                const M34& wa = members[mi].wa;
                const M34& wb = members[mi].wb;
                // (No flip or twist resolution here: done per matrix it breaks a skinned mesh, whose
                // envelope matrices are weighted sums of its joints.)
                const float prev = a->worldStep ? a->worldStep[j].load(std::memory_order_relaxed) : -1.0f;
                const float dist = members[mi].dist;
                const f32* const centre = members[mi].centre;
                const float step = members[mi].step;
                // (Recorded here too: a switched instance skips the per-matrix path that records it.)
                if (b.worldStep && b.worldStep[i].load(std::memory_order_relaxed) < 0.0f)
                    b.worldStep[i].store(step, std::memory_order_relaxed);
                const float prevTurn = turnPolicy(p, noteTurn(a, j, b, i, wa.data(), wb.data(), 4));
                if (outlier[mi]) {
                    if (groupLog) sExcluded[i] = 1;
                    continue;
                }
                // An erratic member has no say for the instance (it is still blended or switched on its
                // own below): one that moves by over 150 units, a quarter of its camera distance and three
                // times its instance's median step (+60) in one game frame, or did so in the last 16 game
                // frames. The nursery's floating teddy bears (one J3D model, 49 matrices) carry draw
                // matrices that land 800-1000 units elsewhere every few game frames (unused or weighted
                // zero) while the bears move a few units, with runs of small steps in between: they
                // switched all the bears to 30 Hz (sweep B). A whole instance moving fast (Chauncey's
                // swoops) moves alike and keeps its say.
                {
                    const u8 held = a->worldErratic ? a->worldErratic[j].load(std::memory_order_relaxed) : 0;
                    const bool now = step > 150.0f && step > 0.25f * dist && step > 3.0f * groupStep[mi] + 60.0f;
                    if (b.worldErratic) b.worldErratic[i].store(now ? 16 : held > 0 ? u8(held - 1) : 0, std::memory_order_relaxed);
                    if (groupLog && (held > 0 || now)) {
                        GStat& st = gstat[p.group];
                        (now ? st.erraticNow : st.erraticHeld)++;
                        if (st.members >= 4)
                            std::fprintf(sGroupLog, "E %llu %016llX key %016llX src %08X site %08X %s step %.0f median %.0f dist %.0f\n",
                                         (unsigned long long)b.seq, (unsigned long long)p.group, (unsigned long long)p.key, p.source, p.site,
                                         now ? "erratic" : "held", step, groupStep[mi], dist);
                    }
                    if (groupLog && (held > 0 || now)) sExcluded[i] = now ? 2 : 3;
                    // (A half turn of an erratic member tears its model all the same: below.)
                    if ((held > 0 || now) && sScrewBlend && i < b.multiMember.size() && b.multiMember[i] &&
                        turnDegrees(wa.data(), wb.data(), 4) > 170.0f)
                        rec.tear = 1;
                    if (held > 0 || now) continue;
                }
                const bool near = blendableWorld(wa.data(), wb.data(), dist, prev, prevTurn, p.tumble, centre);
                // blendableWorld's limit is max(200, 0.5*dist, 2*prev + 60) against the second call's
                // 5e8: while dist < 1e9 and 2*prev + 60 <= 5e8, "near" implies "any distance" and the
                // second call would repeat the decompose and placeStep for nothing. (A NaN distance or
                // a step past the bound makes the first limit exceed the second's: call it, as before.)
                bool anyDist = true;
                if (!near || !(dist < 1e9f) || 2.0f * prev + 60.0f > 5e8f)
                    anyDist = blendableWorld(wa.data(), wb.data(), 1e9f, -1.0f, prevTurn, p.tumble, centre);
                rec.voters++;
                if (!near && anyDist) {  // only the place jumped
                    if (sTrace && rec.jumped == 0)
                        log("[interp] tape %llu: instance %016llX jump vote by key %016llX site %08X step %.0f prev %.0f dist %.0f median %.0f (%zu members)",
                            (unsigned long long)b.seq, (unsigned long long)p.group, (unsigned long long)p.key, p.site, step, prev, dist,
                            groupStep[mi], rec.members.size());
                    rec.jumped++;
                    if (sScrewBlend && i < b.multiMember.size() && b.multiMember[i]) rec.tear = 1;
                    continue;
                }
                // A member that cannot be blended rigidly at all (a mirror flip, a turn over ~150 degrees
                // without spin evidence, a 16x scale) would be switched on its own while the rest of its
                // model blends: the model tears for an interval (the suction swirl). The whole instance
                // blends entrywise instead, unless it is switched as a whole (below).
                if (!anyDist && sScrewBlend && i < b.multiMember.size() && b.multiMember[i]) rec.tear = 1;
                const bool flat = flatMatrix(wa.data()) || flatMatrix(wb.data());
                bool fast = !anyDist || flat, skin = flat;
                Rigid ra, rb;
                if (!decompose(wa.data(), 4, ra) || !decompose(wb.data(), 4, rb)) {
                    fast = skin = true;
                } else {
                    const float d = std::fabs(ra.q[0] * rb.q[0] + ra.q[1] * rb.q[1] + ra.q[2] * rb.q[2] + ra.q[3] * rb.q[3]);
                    fast = fast || d < 0.7071f;  // turned by more than 90 degrees
                    // A half turn (over ~170 degrees) has no knowable direction: a member of an articulated
                    // model blended the short way swept its geometry through either side of the model
                    // (the suction swirl, 400 units a game frame); the whole instance goes entrywise.
                    if (d < 0.087f && sScrewBlend && i < b.multiMember.size() && b.multiMember[i]) rec.tear = 1;
                    // An envelope matrix whose axes change length by over 30% in one game frame (the weights of
                    // its joints moved: a hose envelope went from 0.94 to 0.43 along two axes) is no rigid
                    // motion: blended as a turn and a scale its vertices left their path by half their step.
                    if (sScrewBlend && i < b.multiMember.size() && b.multiMember[i])
                        for (int k = 0; k < 3; k++) {
                            const float la = std::hypot(wa[k], wa[4 + k], wa[8 + k]), lb = std::hypot(wb[k], wb[4 + k], wb[8 + k]);
                            if (std::max(la, lb) > 1.3f * std::min(la, lb)) rec.tear = 1;
                        }
                    // A weighted (envelope) matrix of joints that point different ways is sheared.
                    for (const Rigid* r : {&ra, &rb}) {
                        const float diag = std::max({std::fabs(r->u[0][0]), std::fabs(r->u[1][1]), std::fabs(r->u[2][2]), 1e-6f});
                        if (std::max({std::fabs(r->u[0][1]), std::fabs(r->u[0][2]), std::fabs(r->u[1][2])}) > 0.08f * diag) skin = true;
                    }
                }
                // A sheared member flipping handedness (blended entrywise on its own, blendableWorld):
                // the instance is at a pose its skin cannot follow rigidly, so all of it goes entrywise.
                // Members blended rigidly next to entrywise ones skinned from different poses and
                // Chauncey's body tore into shards for half an interval at a spin (sweep C, frame 6990).
                // (Also a rigid member of an articulated instance whose handedness flips: switched on its own
                // it tore the Poltergust's suction swirl for an interval, LM_TRACK_LOG; entrywise like the rest.)
                const bool skew = skewFlip(wa.data(), wb.data()) || tumblerMirror(p, wa.data(), wb.data()) ||
                                  (sScrewBlend && i < b.multiMember.size() && b.multiMember[i] && (det3(wa.data()) < 0) != (det3(wb.data()) < 0));
                if (skew) fast = skin = true;
                if (fast) rec.fast = 1;
                if (skin) rec.skin = 1;
                if (groupLog) {
                    GStat& st = gstat[p.group];
                    st.fast += fast, st.skin += skin, st.skew += skew;
                    Rigid r1, r2;
                    if (decompose(wa.data(), 4, r1) && decompose(wb.data(), 4, r2) &&
                        std::fabs(r1.q[0] * r2.q[0] + r1.q[1] * r2.q[1] + r1.q[2] * r2.q[2] + r1.q[3] * r2.q[3]) < 0.7071f)
                        st.turn90++;
                }
                static const long long sGroupTape = [] { const char* v = std::getenv("LM_GROUP_TAPE"); return v ? std::atoll(v) : -1ll; }();
                if (sGroupTape >= 0 && u64(sGroupTape) == b.seq)
                    log("[group] tape %llu group %016llX key %016llX near %d any %d flat %d skew %d fast %d skin %d",
                        (unsigned long long)b.seq, (unsigned long long)p.group, (unsigned long long)p.key, int(near), int(anyDist), int(flat),
                        int(skew), int(fast), int(skin));
            }
            for (const u64 g : live) {
                GroupRec& r = *findRecMut(g);
                if (r.jumped > 0 && (r.voters < 4 || 2 * r.jumped >= r.voters)) {
                    r.snap = 1;
                    if (sTrace) log("[interp] tape %llu: instance %016llX switched (%u of %u members jumped)", (unsigned long long)b.seq,
                                    (unsigned long long)g, r.jumped, r.voters);
                }
            }
            // Own-camera model instances (see sOwnInstances): skinned ones blend entrywise as a whole.
            if (sOwnInstances)
                for (u32 i = 0; i < b.patches.size(); i++) {
                    const Patch& p = b.patches[i];
                    const s32 j = b.partner[i];
                    if (!p.ownCamera || !p.group || j < 0 || p.kind != Kind::PosMtx || p.count != 12 || p.role != 0) continue;
                    if (const GroupRec* r = findRec(p.group); r && r->force) continue;
                    f32 va[12], vb[12];
                    for (int e = 0; e < 12; e++) va[e] = val(*a, a->patches[j], e), vb[e] = val(b, p, e);
                    if (skinMatrix(va) || skinMatrix(vb)) {
                        groupRec(p.group).force = 1;
                        sOwnSkinned++;
                    }
                }
            // Linear only for a skinned instance (some matrix sheared or flat) with a fast turn: rigid
            // parts (the water's bubbles, which spin) keep their own rigid blend (entrywise, a part
            // turning half a turn shrank to a sliver: the bubbles faded between game frames).
            u32 snaps = 0, forced = 0;
            for (const u64 g : live) {
                GroupRec& r = *findRecMut(g);
                if ((r.fast && r.skin) || (r.tear && !r.snap)) r.force = 1;
                snaps += r.snap, forced += r.force;
            }
            for (u32 i = 0; i < b.patches.size(); i++) {
                const u64 g = b.patches[i].group;
                const GroupRec* r = g ? findRec(g) : nullptr;
                const u8 v = !r ? 0 : r->snap ? 1 : r->force ? 2 : 0;
                if (b.groupSnap) b.groupSnap[i].store(v, std::memory_order_relaxed);
            }
            sGroupSnaps += snaps;
            sGroupForced += forced;
            if (groupLog) {
                for (const auto& [g, st] : gstat) {
                    if (st.members < 4) continue;
                    const GroupRec* r = findRec(g);
                    std::fprintf(sGroupLog, "G %llu f%u %016llX members %u outliers %u erratic %u held %u voters %u jumped %u fast %u turn90 %u skin %u skew %u unpaired %u src %08X-%08X policy %d\n",
                                 (unsigned long long)b.seq, b.dumpFrame ? b.dumpFrame : b.gameFrame, (unsigned long long)g, st.members, st.outliers,
                                 st.erraticNow, st.erraticHeld, r ? r->voters : 0, r ? r->jumped : 0, st.fast, st.turn90, st.skin, st.skew, st.unpaired,
                                 st.srcLo, st.srcHi, r && r->snap ? 1 : r && r->force ? 2 : 0);
                }
                std::fflush(sGroupLog);
            }
        }
        // Which draw of a each draw of b is, by its own paired matrices / vertices (-1: none, -2: they
        // disagree): colours blend only within the same pair of draws.
        sBundleToA.assign(b.bundleIds.size(), -1);
        for (u32 i = 0; i < b.patches.size(); i++) {
            const Patch& p = b.patches[i];
            const s32 j = b.partner[i];
            if (j < 0 || (p.kind != Kind::PosMtx && p.kind != Kind::Verts) || p.bundle >= sBundleToA.size()) continue;
            const s32 qb = s32(a->patches[j].bundle);
            s32& m = sBundleToA[p.bundle];
            m = m == -1 ? qb : m == qb ? m : -2;
        }
        static std::vector<u32> unpairedProj, blendedProj;  // perspective/ortho projections of b by patch index
        unpairedProj.clear();
        blendedProj.clear();
        for (u32 i = 0; i < b.patches.size(); i++) {
            const Patch& p = b.patches[i];
            // Text boxes are blended as one layout (replayTextGroups); their 2D projections as usual.
            if (p.tgroup && p.kind != Kind::Projection) continue;
            if (p.kind == Kind::Color) {
                // Blended with the same register of the same identity (tagged), unless it jumped;
                // untagged ones stay as captured. 2D (HUD) colours blend too, under the same draw
                // check (a static HUD blends identical colours to themselves; the HUD fading in with
                // the room on arrival stepped at 30 Hz, sweep C).
                const s32 jc = b.partner[i];
                if (jc < 0) continue;
                const Patch& qc = a->patches[jc];
                // Only for the same thing drawn: the draw this colour belongs to must pair with the
                // draw of its partner by its own matrices or vertices. (A colour set in a helper the
                // item loop calls has no item tag: keyed by occurrence under the emitter, the n-th
                // bubble's colour paired with another bubble's and the bubbles dimmed between
                // game frames; tint check, water.)
                // A light's colour (noteColor `light`): its light must pair with the partner's light.
                const bool lightOk = p.slot && qc.slot && p.slot - 1 < b.patches.size() && b.partner[p.slot - 1] == s32(qc.slot - 1);
                // World scopes (a room's lighting, fn_80015BDC) are their own identity: no draw check.
                // An untagged 2D colour: the draw it belongs to must be the same kind of draw.
                const bool flatOk = p.posOffset == 1 && p.bundle < b.bundleIds.size() && qc.bundle < a->bundleIds.size() &&
                                    b.bundleIds[p.bundle] == a->bundleIds[qc.bundle];
                if (p.posOffset == 1 ? !flatOk
                    : p.slot ? !lightOk
                           : p.cat != TagCat::World &&
                                 (p.bundle >= sBundleToA.size() || sBundleToA[p.bundle] < 0 || s32(qc.bundle) != sBundleToA[p.bundle])) {
                    sColorsSwitched++;
                    continue;
                }
                if (blendColorWord(p.stride, a->bytes.data() + qc.offset, b.bytes.data() + p.offset, alpha, sReplay.data() + p.offset))
                    sColorsBlended++;
                else
                    sColorsSwitched++;
                continue;
            }
            const u8 groupPolicy = b.groupSnap ? b.groupSnap[i].load(std::memory_order_relaxed) : 0;
            const bool forced = groupPolicy == 2 && (p.kind == Kind::PosMtx || p.kind == Kind::NrmMtx);
            sLinearBlend = forced;
            if (groupPolicy == 1 && (p.kind == Kind::PosMtx || p.kind == Kind::NrmMtx) && b.partner[i] >= 0) {
                f32 va[16], vb[16];
                const Patch& q = a->patches[b.partner[i]];
                for (int k = 0; k < p.count && k < 16; k++) va[k] = val(*a, q, k), vb[k] = val(b, p, k);
                snap(p, va, vb);
                backStep(p);
                continue;
            }
            const s32 j = b.partner[i];
            // Static world geometry: only the camera moves it.
            const bool cameraOnly = p.cat == TagCat::World && (p.kind == Kind::PosMtx || p.kind == Kind::NrmMtx);
            if (j < 0 && p.role == 1) {  // a shadow receiver set up outside a tag scope
                const ViewPair* mvp = b.mainView < sViewToA.size() && sViewToA[b.mainView] >= 0
                                          ? viewPair(*a, u16(sViewToA[b.mainView]), b, b.mainView, alpha)
                                          : nullptr;
                f32 vb[12], out[12];
                for (int k = 0; k < 12 && p.count == 12; k++) vb[k] = val(b, p, k);
                if (p.count == 12 && shadowReceiverFromB(b, p, vb, mvp, out)) {
                    for (int k = 0; k < 12; k++) storeBE(sReplay.data() + p.offset + 4 * k, out[k]);
                    sCamOnly++;
                }
                continue;
            }
            if (j < 0 && p.kind == Kind::Projection && p.count == 6) {
                // Resolved after the loop (an identical projection of this frame).
                unpairedProj.push_back(i);
                continue;
            }
            if (j < 0 || cameraOnly) {
                if (j < 0 && backStep(p)) continue;
                if (placeUnderCamera(*a, b, p, alpha)) sCamOnly++;
                continue;
            }
            const Patch& q = a->patches[j];
            if (sNoBlendKinds & (1u << u32(p.kind))) {
                placeUnderCamera(*a, b, p, alpha);
                continue;
            }
            if (p.kind == Kind::Verts && p.role == 2) {
                // A 2D block: its direct colours blend while its positions are identical in both game
                // frames (a fade or a fading HUD quad), else it stays as captured.
                if (q.role != 2 || q.source != p.source || !p.clrBytes || q.clrBytes != p.clrBytes || q.clrOffset != p.clrOffset)
                    continue;
                bool same = true;
                for (u32 v = 0; v < p.count && same; v++)
                    same = std::memcmp(a->bytes.data() + q.offset + v * q.stride + q.posOffset,
                                       b.bytes.data() + p.offset + v * p.stride + p.posOffset, p.source) == 0;
                if (!same && !blend2DLayout(*a, q, j, b, p, i, alpha)) continue;
                bool jumped = false;
                for (u32 v = 0; v < p.count && !jumped; v++)
                    for (u32 c = 0; c < p.clrBytes; c++)
                        if (std::abs(int(a->bytes[q.offset + v * q.stride + q.clrOffset + c]) - int(b.bytes[p.offset + v * p.stride + p.clrOffset + c])) > 96)
                            jumped = true;
                if (jumped) {
                    sColorsSwitched++;
                    continue;
                }
                for (u32 v = 0; v < p.count; v++)
                    for (u32 c = 0; c < p.clrBytes; c++) {
                        const int x = a->bytes[q.offset + v * q.stride + q.clrOffset + c], y = b.bytes[p.offset + v * p.stride + p.clrOffset + c];
                        sReplay[p.offset + v * p.stride + p.clrOffset + c] = u8(std::lround(x + (y - x) * alpha));
                    }
                sColorsBlended++;
                continue;
            }
            if (p.kind == Kind::Verts) {
                // CPU-built geometry of one tagged item with the same vertex count: positions blended.
                float jump = 0;
                for (u32 v = 0; v < p.count; v++)
                    for (int c = 0; c < 3; c++) jump = std::max(jump, std::fabs(vtx(*a, q, v, c) - vtx(b, p, v, c)));
                if (!(jump <= 200.0f)) {
                    snap(p, nullptr, nullptr);
                    backStep(p);
                    continue;
                }
                // A slot reused by another particle (the water spray's points: a stream's slots are
                // refilled from the nozzle): its centre turns back against the way it was going.
                // Blending it drew the new droplet flying backwards from the old one's place (the
                // detector's identity check: 377 points there and back in 400 game frames of
                // spraying). Shown at the game frame's place instead.
                {
                    f32 ca[3] = {}, cb[3] = {};
                    for (u32 v = 0; v < p.count; v++)
                        for (int c = 0; c < 3; c++) ca[c] += vtx(*a, q, v, c), cb[c] += vtx(b, p, v, c);
                    f32 d[3], pd[3], dd = 0, pp = 0, dp = 0;
                    for (int c = 0; c < 3; c++) {
                        d[c] = (cb[c] - ca[c]) / float(std::max<u32>(p.count, 1));
                        pd[c] = a->vertStep ? a->vertStep[3 * j + c].load(std::memory_order_relaxed) : std::nanf("");
                        if (b.vertStep) b.vertStep[3 * i + c].store(d[c], std::memory_order_relaxed);
                        dd += d[c] * d[c], pp += pd[c] * pd[c], dp += d[c] * pd[c];
                    }
                    // (Not for a tracked block, tagTracked: its rebirth is detected where it is drawn, so
                    // turning back is its own motion: the element streams' points bounce off the ground
                    // in Spooky's garden and the courtyard fountain's basins, and were held at the game
                    // frame's place for the interval of every bounce.)
                    if (!p.tracked && std::isfinite(pp) && dd > 16.0f && pp > 1.0f && dp < -0.25f * std::sqrt(dd * pp)) {
                        sReversals++;
                        snap(p, nullptr, nullptr);
                        backStep(p);
                        continue;
                    }
                }
                for (u32 v = 0; v < p.count; v++)
                    for (int c = 0; c < 3; c++) {
                        const f32 va = vtx(*a, q, v, c), vb = vtx(b, p, v, c);
                        storeBE(sReplay.data() + p.offset + v * p.stride + p.posOffset + 4 * c, va + (vb - va) * alpha);
                    }
                // Vertex colours (particles fading, the water's bubbles): blended unless one jumped.
                if (p.clrBytes && q.clrBytes == p.clrBytes && q.clrOffset == p.clrOffset) {
                    bool jumped = false;
                    for (u32 v = 0; v < p.count && !jumped; v++)
                        for (u32 c = 0; c < p.clrBytes; c++) {
                            const int x = a->bytes[q.offset + v * q.stride + q.clrOffset + c], y = b.bytes[p.offset + v * p.stride + p.clrOffset + c];
                            if (std::abs(x - y) > 96) jumped = true;
                        }
                    if (!jumped)
                        for (u32 v = 0; v < p.count; v++)
                            for (u32 c = 0; c < p.clrBytes; c++) {
                                const int x = a->bytes[q.offset + v * q.stride + q.clrOffset + c], y = b.bytes[p.offset + v * p.stride + p.clrOffset + c];
                                sReplay[p.offset + v * p.stride + p.clrOffset + c] = u8(std::lround(x + (y - x) * alpha));
                            }
                }
                sBlended++;
                continue;
            }
            f32 va[16], vb[16], out[16];
            if (p.count > 16) continue;
            for (int k = 0; k < p.count; k++) va[k] = val(*a, q, k), vb[k] = val(b, p, k);
            // A projection is only comparable to one of the same type (the u32 after it).
            if (p.kind == Kind::Projection &&
                std::memcmp(a->bytes.data() + q.offset + 24, b.bytes.data() + p.offset + 24, 4) != 0)
                continue;
            if (p.kind == Kind::Projection) {
                // 2D (orthographic) projections are layout (the HUD's anchoring per element), never
                // animated: always b's. A perspective projection blends only with one of the same
                // aspect (the camera zooming), never with another view's.
                const u8* t = b.bytes.data() + p.offset + 24;
                if ((u32(t[0]) << 24 | u32(t[1]) << 16 | u32(t[2]) << 8 | t[3]) != 0) continue;  // GX_ORTHOGRAPHIC
                // (The shadow caster pass is the same light whatever its fitted frustum: "shadows at any
                // distance" refits its aspect every game frame; its receivers use the blended light, so
                // the caster projection must blend too, or the shadows swim.)
                const bool caster = p.bundle < b.bundlePass.size() && b.bundlePass[p.bundle] == 1;
                const float ra = va[2] != 0 ? va[0] / va[2] : 0, rb = vb[2] != 0 ? vb[0] / vb[2] : 0;
                if (!caster && !(std::fabs(ra - rb) <= 0.01f * std::max(std::fabs(ra), std::fabs(rb)))) continue;
            }
            // A light is blended only while it is the same light (its attenuation: a spot switched to
            // another shape, or off by its attenuation, is another light).
            const auto attnChanged = [&] {
                if (p.kind != Kind::Light || p.offset < 24 || q.offset < 24) return false;
                for (int k = 0; k < 6; k++) {
                    const f32 x = loadBE(a->bytes.data() + q.offset - 24 + 4 * k), y = loadBE(b.bytes.data() + p.offset - 24 + 4 * k);
                    if (!(std::fabs(x - y) <= 0.02f * std::max({std::fabs(x), std::fabs(y), 1e-3f}))) return true;
                }
                return false;
            };
            if (p.kind == Kind::Light && (p.source != q.source || attnChanged())) {
                snap(p, va, vb);
                continue;
            }
            const bool jump2D = p.role == 2 && p.kind == Kind::PosMtx && p.count == 12 &&
                                std::fabs(va[3] - vb[3]) + std::fabs(va[7] - vb[7]) + std::fabs(va[11] - vb[11]) > 96.0f;
            const u8 texMode = p.kind == Kind::TexMtx && i < b.texMode.size() ? b.texMode[i] : 0;
            if (texMode == 2) {  // a texture placed anew (texWrapPairs)
                snap(p, va, vb);
                continue;
            }
            if (texMode == 1) {  // a scroll: by its step, less the whole textures a wrap skipped
                for (int k = 0; k < p.count; k++) {
                    const float d = (k & 3) == 3 ? vb[k] - va[k] - b.texShift[i][k >> 2] : vb[k] - va[k];
                    storeBE(sReplay.data() + p.offset + 4 * k, vb[k] - d * (1.0f - alpha));  // exact b at alpha 1
                }
                sBlended++;
                continue;
            }
            if (p.kind == Kind::TexMtx && p.role == 0 && blendTexWrap(va, vb, p.count, alpha, out, p.sky)) {
                for (int k = 0; k < p.count; k++) storeBE(sReplay.data() + p.offset + 4 * k, out[k]);
                sBlended++;
                continue;
            }
            bool ok;
            // drawCentre is pure in (b, patch): a lower_bound over b.slotCentres, then the centre of
            // the draw's vertices. The paths below ask for it up to four times per patch; ask once.
            // Only position matrices use it.
            const f32* const ctr = p.kind == Kind::PosMtx ? drawCentre(b, p) : nullptr;
            const ViewPair* pivotVp = nullptr;  // a spinner / tumbler: its drawn centre is moved in a straight line
            if (p.kind == Kind::PosMtx && p.count == 12 && p.role == 0 && p.view < sViewToA.size() && sViewToA[p.view] == s32(q.view)) {
                const ViewPair* vp = viewPair(*a, q.view, b, p.view, alpha);
                if (vp && vp->ok && !vp->cut) {
                    M34 wa, wb;
                    compose(vp->ia.data(), va, wa.data());
                    compose(vp->ib.data(), vb, wb.data());
                    const float prevTurn = turnPolicy(p, noteTurn(a, j, b, i, wa.data(), wb.data(), 4));
                    const bool skew = skewFlip(wa.data(), wb.data()) || tumblerMirror(p, wa.data(), wb.data());
                    // (Not for a member of an articulated / skinned instance: its draw's centre is the
                    // centre of the whole mesh, not of what this matrix moves. Each spinning member of
                    // Luigi's model was shifted by its own correction of up to 115 units while the
                    // instance blended per matrix, for the 8 game frames a member counts as tumbling:
                    // the Poltergust hose and his limbs tore for a moment; LM_GROUP_LOG V lines.
                    // LM_PIVOT_MEMBERS=1: old behaviour.)
                    static const bool sPivotMembers = std::getenv("LM_PIVOT_MEMBERS") != nullptr;
                    const bool member = !sPivotMembers && i < b.multiMember.size() && b.multiMember[i];
                    if (prevTurn > kSpinnerTurn && !forced && !skew && !member) pivotVp = vp;
                    const bool excl = sGroupLogNow && i < sExcluded.size() && sExcluded[i];
                    const float turn = excl ? turnDegrees(wa.data(), wb.data(), 4) : 0.0f;
                    bool flipped = false, untwisted = false;
                    if (!skew) {
                        flipped = resolveFlip(wa.data(), wb.data(), 4);
                        untwisted = untwist(wa.data(), wb.data());
                    }
                    const float step = placeStep(wa.data(), wb.data(), ctr);
                    if (b.worldStep) b.worldStep[i].store(step, std::memory_order_relaxed);
                    const float prev = a->worldStep ? a->worldStep[j].load(std::memory_order_relaxed) : -1.0f;
                    ok = forced || blendableWorld(wa.data(), wb.data(), viewDist(vb, ctr), prev, prevTurn, p.tumble, ctr);
                    // A member of an articulated or skinned instance that was not switched as a whole and is not
                    // erratic moves with its instance: its place is not a jump (switched on its own it tore the
                    // model for an interval: the suction swirl swinging with the nozzle, 300 units a game frame).
                    if (!ok && sScrewBlend && member && groupPolicy == 0 && b.worldErratic && b.worldErratic[i].load(std::memory_order_relaxed) == 0)
                        ok = blendableWorld(wa.data(), wb.data(), 1e9f, -1.0f, prevTurn, p.tumble, ctr);
                    if (excl) {
                        ExclStat& st = sExclStats[p.group];
                        st.n++, st.snapped += !ok, st.flipped += flipped && !forced, st.untwisted += untwisted && !forced;
                        st.turn90 += turn > 90.0f && !forced;
                        st.policy = groupPolicy;
                    }
                } else {
                    ok = blendable(p.kind, va, vb, p.count);
                }
            } else if (p.kind == Kind::NrmMtx && p.count == 9 && p.role == 0 && p.view < sViewToA.size() && sViewToA[p.view] == s32(q.view)) {
                // Normals turn with their model: in world space, with the same flip resolution.
                const ViewPair* vp = viewPair(*a, q.view, b, p.view, alpha);
                if (vp && vp->ok && !vp->cut) {
                    f32 na[9], nb[9];
                    for (int c2 = 0; c2 < 3; c2++)
                        for (int r = 0; r < 3; r++) {
                            na[3 * r + c2] = vp->va[r] * va[c2] + vp->va[4 + r] * va[3 + c2] + vp->va[8 + r] * va[6 + c2];
                            nb[3 * r + c2] = vp->vb[r] * vb[c2] + vp->vb[4 + r] * vb[3 + c2] + vp->vb[8 + r] * vb[6 + c2];
                        }
                    const float prevTurn = turnPolicy(p, noteTurn(a, j, b, i, na, nb, 3));
                    const bool skew = skewFlip(na, nb, 3);  // (blended entrywise in blendFactored)
                    if (!skew) {
                        resolveFlip(na, nb, 3);
                        untwist(na, nb, 3);
                    }
                    ok = forced || skew || blendable(Kind::NrmMtx, na, nb, 9, prevTurn);
                } else {
                    ok = blendable(p.kind, va, vb, p.count);
                }
            } else if (p.ownCamera && p.kind == Kind::PosMtx && p.count == 12) {
                // A scene with its own camera (event portraits, door close-ups): the camera is static or
                // slow, so a model that moves by more than a tenth of its distance in one game frame was
                // placed anew (the call portrait parked off screen as the call closes): switch.
                // (Measured where its geometry is, placeStep: the event scenes' models have their origin
                // 4000-5600 units from their vertices, and a turn of a few degrees moved the origin by
                // 1000-10000 units: switched while the model moved 50-900, path check.)
                const float d = viewDist(vb, ctr);
                const float j = placeStep(va, vb, ctr);
                ok = blendable(p.kind, va, vb, p.count) && j <= std::max(20.0f, 0.1f * d);
            } else if (p.bundle < b.bundlePass.size() && b.bundlePass[p.bundle] == 1 && sLightPair.ok) {
                // The shadow caster pass, while its light blends: everything in it blends with the
                // light (the receivers use the blended light; a caster shown as captured would sit a
                // frame apart from its shadow lookup, and the shadow would swim).
                ok = true;
                for (int k = 0; k < p.count && ok; k++) ok = std::isfinite(va[k]) && std::isfinite(vb[k]);
            } else {
                ok = blendable(p.kind, va, vb, p.count);
            }
            if (jump2D || !ok) {
                snap(p, va, vb);
                backStep(p);
                continue;
            }
            if (p.kind == Kind::Projection && p.count == 6 && p.bundle < b.bundlePass.size() && b.bundlePass[p.bundle] == 1) {
                // Shadow caster pass: x/y blended, depth (near/far) as in b (see makeLightPair).
                for (int k = 0; k < 6; k++) out[k] = k >= 4 ? vb[k] : va[k] + (vb[k] - va[k]) * alpha;
            } else if (p.role == 1) {  // a shadow receiver: rebuilt from the blended light, or as captured
                if (!blendShadowReceiver(*a, q, va, b, p, vb, alpha, out)) std::copy(vb, vb + p.count, out);
            } else if (!(blendFactored(*a, q, va, b, p, vb, alpha, out) ||
                         (!forced && p.kind == Kind::PosMtx && p.count == 12 && blendRigid(va, vb, 4, alpha, out)) ||
                         (!forced && p.kind == Kind::NrmMtx && p.count == 9 && blendRigid(va, vb, 3, alpha, out)) ||
                         (p.kind == Kind::TexMtx && p.count == 8 && blendSpin(va, vb, alpha, out))))
                for (int k = 0; k < p.count; k++)
                    out[k] = std::isfinite(va[k]) && std::isfinite(vb[k]) ? va[k] + (vb[k] - va[k]) * alpha : vb[k];
            // An own-camera model matrix blended rigidly: about its draw's centre (see sOwnInstances).
            // (Not with the screw blend: it already turns each matrix about the axis of its own motion,
            // the same for neighbouring pieces of one model, whatever their origin; moving each piece's
            // centre in a straight line on top opened the seams of Luigi's glove in the opening map
            // scene. LM_OWNCAM_PIVOT=1: pivot anyway.)
            static const bool sOwnPivot = !sScrewBlend || std::getenv("LM_OWNCAM_PIVOT") != nullptr;
            if (sOwnInstances && sOwnPivot && p.ownCamera && p.kind == Kind::PosMtx && p.count == 12 && p.role == 0 && !forced &&
                ctr) {  // (the vertices of its own slot when known: hose-jump SlotCentres)
                f32 pa[3], pb[3], have[3];
                xform34(va, ctr, pa);
                xform34(vb, ctr, pb);
                xform34(out, ctr, have);
                for (int r = 0; r < 3; r++) out[4 * r + 3] += pa[r] + (pb[r] - pa[r]) * alpha - have[r];
                sOwnPivoted++;
            }
            // A spinner or tumbler turned about its matrix origin: geometry off that origin swings
            // along an arc (the water spray's chunks, rotated 150-180 degrees about an origin outside
            // the mesh, sat above both of their game-frame places mid-interval: sweep C, blips.py).
            // Its drawn centre (the draw's position bounding box) moves in a straight line instead.
            if (pivotVp && p.bundle < b.bundleCentre.size() && std::isfinite(b.bundleCentre[p.bundle][0])) {
                const f32* c = b.bundleCentre[p.bundle].data();
                M34 wa, wb;
                compose(pivotVp->ia.data(), va, wa.data());
                compose(pivotVp->ib.data(), vb, wb.data());
                f32 pa[3], pb[3], pw[3], want[3], have[3];
                xform34(wa.data(), c, pa);
                xform34(wb.data(), c, pb);
                for (int k = 0; k < 3; k++) pw[k] = pa[k] + (pb[k] - pa[k]) * alpha;
                xform34(pivotVp->vt.data(), pw, want);
                xform34(out, c, have);
                for (int r = 0; r < 3; r++) out[4 * r + 3] += want[r] - have[r];
                if (sGroupLogNow && p.group && !p.tumble) {
                    auto& st = sPivotStats[p.group];
                    st.first++;
                    st.second = std::max(st.second, std::hypot(want[0] - have[0], want[1] - have[1], want[2] - have[2]));
                }
            }
            if (p.kind == Kind::Light) {  // keep the direction's length (spot cone falloff)
                const float la = std::sqrt(va[3] * va[3] + va[4] * va[4] + va[5] * va[5]);
                const float lb = std::sqrt(vb[3] * vb[3] + vb[4] * vb[4] + vb[5] * vb[5]);
                const float lo = std::sqrt(out[3] * out[3] + out[4] * out[4] + out[5] * out[5]);
                // (Directions of lights that have none can hold any bits: only rescale sane ones.)
                if (lo > 1e-6f && la > 1e-6f && lb > 1e-6f && std::isfinite(la) && std::isfinite(lb) && std::isfinite(lo)) {
                    const float s = (la + (lb - la) * alpha) / lo;
                    for (int k = 3; k < 6; k++) out[k] *= s;
                }
            }
            for (int k = 0; k < p.count; k++) storeBE(sReplay.data() + p.offset + 4 * k, out[k]);
            sBlended++;
            if (p.kind == Kind::Projection && p.count == 6) blendedProj.push_back(i);
        }
        sLinearBlend = false;
        if (sGroupLogNow) {
            static FILE* sXLog = [] {
                const char* v = std::getenv("LM_GROUP_LOG");
                return v && *v ? std::fopen((std::string(v) + ".x").c_str(), "w") : nullptr;
            }();
            for (const auto& [g, st] : sExclStats)
                if (sXLog)
                    std::fprintf(sXLog, "X %llu %016llX excluded %u snapped %u flipped %u untwisted %u turn90 %u policy %u\n", (unsigned long long)b.seq,
                                 (unsigned long long)g, st.n, st.snapped, st.flipped, st.untwisted, st.turn90, st.policy);
            for (const auto& [g, st] : sPivotStats)
                if (sXLog)
                    std::fprintf(sXLog, "V %llu %016llX pivoted %u max %.1f\n", (unsigned long long)b.seq, (unsigned long long)g, st.first, st.second);
            if (sXLog) std::fflush(sXLog);
            sGroupLogNow = false;
        }
        // A projection without a partner that is the very projection of another (blended) one of this
        // game frame: the camera's, set again inside an effect's scope (fn_801191B4 in the foyer: its
        // scope's identity alternates between game frames, so it paired only every third one). Left
        // as b's while everything around it used the blended (zooming) projection, the draws after it
        // and enhanced lighting's shadow mask (it rebuilds view positions from the depth pre-pass with
        // the last world draw's projection) disagreed with the rest of the frame: the light of the
        // foyer's left sconce (20 units in front of the wall) was shadowed out on the wall and table
        // at the in-betweens of 2 of 3 game frames, fading back in with alpha (16:9 only; with
        // Original 30 there are no in-betweens). It now gets the same blended values.
        replayTextGroups(*a, b, alpha);
        for (const u32 i : unpairedProj) {
            const Patch& p = b.patches[i];
            const u8* pb = b.bytes.data() + p.offset;
            for (const u32 k : blendedProj) {
                const Patch& o = b.patches[k];
                if (std::memcmp(b.bytes.data() + o.offset, pb, 28) != 0) continue;  // 6 values + type
                std::memcpy(sReplay.data() + p.offset, sReplay.data() + o.offset, 24);
                sProjShared++;
                break;
            }
        }
    }
    // Extra lights (enhanced lighting): position, radius and intensity blended by id (the
    // enhanced-lighting code's own light identity; colours stay b's). Their positions are in view
    // space (the main camera at capture): blended in world space and seen from the blended main
    // camera; a light without a partner keeps its world place under the blended camera.
    static const bool sXlBoxSlide = std::getenv("LM_XL_BOX_SLIDE") != nullptr;  // test aid: the old room box (moved with the light)
    static const bool sNoBlendExtra = std::getenv("LM_INTERP_NOBLEND") &&(std::strtoul(std::getenv("LM_INTERP_NOBLEND"), nullptr, 0) & 0x80);  // bit 7: extra lights as captured
    // Test aid LM_XL_LOG=<file>: every replay's extra lights (identity, paired, intensity a / b / shown).
    static FILE* sXlLog = [] {
        const char* v = std::getenv("LM_XL_LOG");
        return v && *v ? std::fopen(v, "w") : nullptr;
    }();
    if (sXlLog) std::fprintf(sXlLog, "P %llu %.3f f%u a %llu lights %zu\n", (unsigned long long)b.seq, alpha, b.dumpFrame,
                             a ? (unsigned long long)a->seq : 0ull, b.lightKeys.size());
    if (a && alpha < 1.0f && !b.lights.empty() && !sNoBlendExtra) {
        constexpr u32 kWords = GX_AURORA_EXTRA_LIGHT_WORDS;
        auto u32At = [](const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; };
        const ViewPair* mvp = nullptr;
        if (b.mainView < sViewToA.size() && sViewToA[b.mainView] >= 0) {
            mvp = viewPair(*a, u16(sViewToA[b.mainView]), b, b.mainView, alpha);
            if (mvp && (!mvp->ok || mvp->cut)) mvp = nullptr;
        }
        const auto xform = [](const M34& m, const f32* v, f32* o) {
            for (int r = 0; r < 3; r++) o[r] = m[4 * r] * v[0] + m[4 * r + 1] * v[1] + m[4 * r + 2] * v[2] + m[4 * r + 3];
        };
        const auto rotate = [](const M34& m, const f32* v, f32* o) {
            for (int r = 0; r < 3; r++) o[r] = m[4 * r] * v[0] + m[4 * r + 1] * v[1] + m[4 * r + 2] * v[2];
        };
        // a's lights by identity (an identity seen twice in a is ambiguous: never paired).
        static std::unordered_map<u64, const u8*> byKey;
        byKey.clear();
        {
            u32 n = 0;
            for (const auto& [offA, countA] : a->lights)
                for (u32 j = 0; j < countA; j++, n++) {
                    const u64 k = n < a->lightKeys.size() ? a->lightKeys[n] : 0;
                    if (!k) continue;
                    auto [it, fresh] = byKey.try_emplace(k, a->bytes.data() + offA + j * kWords * 4);
                    if (!fresh) it->second = nullptr;
                }
        }
        u32 nb = 0;
        for (const auto& [offB, countB] : b.lights) {
            for (u32 i = 0; i < countB; i++, nb++) {
                const u8* lb = b.bytes.data() + offB + i * kWords * 4;
                u8* out = sReplay.data() + (lb - b.bytes.data());
                const u64 key = nb < b.lightKeys.size() ? b.lightKeys[nb] : 0;
                const u8* la = nullptr;
                if (key)
                    if (const auto it = byKey.find(key); it != byKey.end()) la = it->second;
                f32 pb[3] = {loadBE(lb), loadBE(lb + 4), loadBE(lb + 8)}, wb[3] = {}, wa[3] = {};
                if (mvp) {
                    xform(mvp->ib, pb, wb);
                    // The room box (relative to the position, view-space axes): b's box turned with
                    // the camera, b's view -> world -> blended view.
                    if (u32At(lb + 32) & 0x10u) {
                        M34 q;  // vt x vb^-1, rotation part
                        compose(mvp->vt.data(), mvp->ib.data(), q.data());
                        for (int g = 0; g < 4; g++) {
                            const f32 v[3] = {loadBE(lb + 40 + 12 * g), loadBE(lb + 44 + 12 * g), loadBE(lb + 48 + 12 * g)};
                            f32 o[3];
                            rotate(q, v, o);
                            for (int c = 0; c < 3; c++) storeBE(out + 40 + 12 * g + 4 * c, o[c]);
                        }
                    }
                }
                bool paired = la != nullptr;
                if (paired) {
                    const f32 pa[3] = {loadBE(la), loadBE(la + 4), loadBE(la + 8)};
                    if (mvp) xform(mvp->ia, pa, wa);
                    const float jump = mvp ? std::fabs(wa[0] - wb[0]) + std::fabs(wa[1] - wb[1]) + std::fabs(wa[2] - wb[2])
                                           : std::fabs(pa[0] - pb[0]) + std::fabs(pa[1] - pb[1]) + std::fabs(pa[2] - pb[2]);
                    if (jump > 200.0f) {  // teleport / a pooled source placed anew: b's place
                        paired = false;
                        sXlJumped++;
                    }
                }
                (paired ? sXlPaired : sXlNew)++;
                if (sXlLog) {
                    const f32 ib = loadBE(lb + 28), ia = paired ? loadBE(la + 28) : -1.0f;
                    std::fprintf(sXlLog, "R %llu %.3f id %08X key %016llX %s Ia %.3f Ib %.3f out %.3f flags %08X mvp %d wa %.1f %.1f %.1f wb %.1f %.1f %.1f\n",
                                 (unsigned long long)b.seq, alpha, u32At(lb + 36), (unsigned long long)key, paired ? "pair" : "new ", ia, ib,
                                 paired ? ia + (ib - ia) * alpha : ib, u32At(lb + 32), mvp != nullptr, wa[0], wa[1], wa[2], wb[0], wb[1], wb[2]);
                }
                if (!paired) {
                    if (mvp) {  // its world place, seen from the blended camera
                        f32 o[3];
                        xform(mvp->vt, wb, o);
                        for (int c = 0; c < 3; c++) storeBE(out + 4 * c, o[c]);
                    }
                    continue;
                }
                // Position (0..2), colour (3..5), radius (6) and intensity (7) blended; flags and the
                // shadow weight stay b's. (These are enhanced lighting's own lights, not game colour
                // registers: a ghost light's glow, its dramatic flares and the world lights' flicker
                // change every game frame. Kept at b's colour and intensity, big lit areas stepped at
                // 30 Hz: Bogmire's arena, lit by the shadow ghosts' coloured lights, pulsed in steps
                // with a still camera; screen capture with LM_FRAME_TAG: the change between presents of
                // one tape was half of that across a tape change.)
                for (int k = 0; k < 8; k++) {
                    const f32 va = loadBE(la + 4 * k), vb = loadBE(lb + 4 * k);
                    if (std::isfinite(va) && std::isfinite(vb)) storeBE(out + 4 * k, va + (vb - va) * alpha);
                }
                // The room box's centre is relative to the light's position: it stays at b's place in
                // the world (the room does not move with the light). Left relative to the blended
                // position, the box slid with the light by its whole step at alpha 0 and cut off the
                // part of the room the light was leaving (sweep B: Luigi's fire stream lighting the
                // 2F east hallway wall went dark at every game frame and came back by alpha 1).
                f32 bNow[3] = {pb[0], pb[1], pb[2]};  // b's position as seen in this replay
                if (mvp) {
                    f32 w[3], o[3];
                    for (int c = 0; c < 3; c++) w[c] = wa[c] + (wb[c] - wa[c]) * alpha;
                    xform(mvp->vt, w, o);
                    for (int c = 0; c < 3; c++) storeBE(out + 4 * c, o[c]);
                    xform(mvp->vt, wb, bNow);
                }
                if ((u32At(lb + 32) & 0x10u) && !sXlBoxSlide)
                    for (int c = 0; c < 3; c++)
                        storeBE(out + 40 + 4 * c, loadBE(out + 40 + 4 * c) + bNow[c] - loadBE(out + 4 * c));
            }
        }
    }
    // CPU-deformed position arrays (cloth, morphs) of the same tagged object with the same size and
    // format: positions blended (model space; the model's matrix is blended on its own).
    {
        static std::deque<std::vector<std::shared_ptr<std::vector<u8>>>> sKeep;  // until the FIFO has drained them
        std::vector<std::shared_ptr<std::vector<u8>>> keep;
        if (a && alpha < 1.0f)
            for (u32 i = 0; i < b.arrayRecs.size() && i < b.arrayPartner.size(); i++) {
                const s32 j = b.arrayPartner[i];
                if (j < 0) continue;
                const detail::ArrayRec& r = b.arrayRecs[i];
                const detail::ArrayRec& q = a->arrayRecs[j];
                const std::vector<u8>& db = b.arrays[r.snap];
                const std::vector<u8>& da = a->arrays[q.snap];
                if (da.size() != db.size() || r.stride == 0 || std::memcmp(da.data(), db.data(), db.size()) == 0) continue;
                static constexpr u32 kSize[] = {1, 1, 2, 2, 4};
                const u32 cs = kSize[r.type], nc = r.cnt == GX_POS_XYZ ? 3 : 2;
                if (cs * nc > r.stride) continue;
                const float scale = r.type == GX_F32 ? 1.0f : float(1u << r.frac);
                const auto rdc = [&](const u8* p) -> float {
                    u32 v = 0;
                    for (u32 k = 0; k < cs; k++) v = v << 8 | p[k];
                    switch (r.type) {
                    case GX_F32: return std::bit_cast<f32>(v);
                    case GX_S16: return float(s16(v)) / scale;
                    case GX_U16: return float(u16(v)) / scale;
                    case GX_S8: return float(s8(v)) / scale;
                    default: return float(u8(v)) / scale;
                    }
                };
                const auto wrc = [&](u8* p, float f) {
                    u32 v;
                    if (r.type == GX_F32) v = std::bit_cast<u32>(f);
                    else v = u32(s32(std::lround(f * scale)));
                    for (u32 k = 0; k < cs; k++) p[k] = u8(v >> (8 * (cs - 1 - k)));
                };
                const u32 n = u32(db.size()) / r.stride;
                float jump = 0;
                for (u32 v = 0; v < n && jump <= 200.0f; v++)
                    for (u32 c = 0; c < nc; c++)
                        jump = std::max(jump, std::fabs(rdc(db.data() + v * r.stride + c * cs) - rdc(da.data() + v * r.stride + c * cs)));
                if (!(jump <= 200.0f)) continue;  // another shape / a reset: b's
                auto out = std::make_shared<std::vector<u8>>(db);
                for (u32 v = 0; v < n; v++)
                    for (u32 c = 0; c < nc; c++) {
                        const u32 o = v * r.stride + c * cs;
                        const float x = rdc(da.data() + o), y = rdc(db.data() + o);
                        wrc(out->data() + o, x + (y - x) * alpha);
                    }
                const u64 ptr = u64(reinterpret_cast<uintptr_t>(out->data()));
                if (r.ptrAt + 8 > sReplay.size()) continue;
                for (int k = 0; k < 8; k++) sReplay[r.ptrAt + k] = u8(ptr >> (56 - 8 * k));
                keep.push_back(std::move(out));
                sArraysBlended++;
            }
        sKeep.push_back(std::move(keep));
        while (sKeep.size() > 8) sKeep.pop_front();
    }
    // Test aid LM_SKY_LOG=<file>,<first>,<last>: every sky value of the replays of those game frames
    // (a, b and what is shown) and the blended main camera's rotation.
    {
        static FILE* sSkyLog = nullptr;
        static u32 sSkyFirst = 0, sSkyLast = 0;
        static const bool sInit = [] {
            if (const char* v = std::getenv("LM_SKY_LOG")) {
                std::string s = v;
                const auto c1 = s.find(',');
                if (c1 != std::string::npos && std::sscanf(s.c_str() + c1 + 1, "%u,%u", &sSkyFirst, &sSkyLast) == 2)
                    sSkyLog = std::fopen(s.substr(0, c1).c_str(), "w");
            }
            return true;
        }();
        (void)sInit;
        if (sSkyLog && b.gameFrame >= sSkyFirst && b.gameFrame <= sSkyLast) {
            std::fprintf(sSkyLog, "P f%u seq %llu alpha %.3f a %d\n", b.gameFrame, (unsigned long long)b.seq, alpha, a && alpha < 1.0f);
            if (a && alpha < 1.0f && b.mainView < sViewToA.size() && sViewToA[b.mainView] >= 0)
                if (const ViewPair* vp = viewPair(*a, u16(sViewToA[b.mainView]), b, b.mainView, alpha); vp && vp->ok) {
                    std::fprintf(sSkyLog, "V");
                    for (int k = 0; k < 12; k++) std::fprintf(sSkyLog, " %.4f", vp->vt[k]);
                    std::fprintf(sSkyLog, "\n");
                }
            for (u32 i = 0; i < b.patches.size(); i++) {
                const Patch& p = b.patches[i];
                if (!p.sky) continue;
                const s32 j = a && alpha < 1.0f ? b.partner[i] : -1;
                std::fprintf(sSkyLog, "S %u kind %u slot %u site %08X view %u cat %u j %d n %u", i, u32(p.kind), p.slot, p.site, p.view,
                             u32(p.cat), j, p.count);
                if (p.kind == Kind::Color) {
                    const u8* x = b.bytes.data() + p.offset;
                    const u8* y = sReplay.data() + p.offset;
                    std::fprintf(sSkyLog, " b %02X%02X%02X%02X out %02X%02X%02X%02X", x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
                    if (j >= 0) {
                        const u8* z = a->bytes.data() + a->patches[j].offset;
                        std::fprintf(sSkyLog, " a %02X%02X%02X%02X", z[0], z[1], z[2], z[3]);
                    }
                } else if (p.kind != Kind::Verts && p.count <= 16) {
                    std::fprintf(sSkyLog, " b");
                    for (int k = 0; k < p.count; k++) std::fprintf(sSkyLog, " %.4f", val(b, p, k));
                    std::fprintf(sSkyLog, " out");
                    for (int k = 0; k < p.count; k++) std::fprintf(sSkyLog, " %.4f", loadBE(sReplay.data() + p.offset + 4 * k));
                    if (j >= 0) {
                        std::fprintf(sSkyLog, " a");
                        for (int k = 0; k < p.count; k++) std::fprintf(sSkyLog, " %.4f", val(*a, a->patches[j], k));
                    }
                }
                std::fprintf(sSkyLog, "\n");
            }
            std::fflush(sSkyLog);
        }
    }
    if (dump && b.dumpOffset != ~0u) {
        const u64 ptr = u64(reinterpret_cast<uintptr_t>(dump));
        for (int k = 0; k < 8; k++) sReplay[b.dumpOffset + k] = u8(ptr >> (56 - 8 * k));
    }
    // Test aid LM_SPRAY_LOG=<file>: per replay, every element-stream drop (tumbler model matrices) and
    // particle point (single-vertex blocks of particles) with its key, how it was shown (b: paired
    // and blended, n: no partner, s: born moving along its step) and its place in the world as shown
    // (tools/interp/spray_check.py follows each key across presents).
    {
        static FILE* sSprayLog = [] {
            const char* v = std::getenv("LM_SPRAY_LOG");
            return v && *v ? std::fopen(v, "w") : nullptr;
        }();
        if (sSprayLog) {
            const bool blend = a && alpha < 1.0f;
            std::fprintf(sSprayLog, "P %u %llu %.4f %d\n", sSprayPresent, (unsigned long long)b.seq, blend ? alpha : 1.0f, int(blend));
            for (u32 i = 0; i < b.patches.size(); i++) {
                const Patch& p = b.patches[i];
                if (!p.key || p.bundle >= b.bundlePass.size() || b.bundlePass[p.bundle] != 0) continue;
                const bool point = p.kind == Kind::Verts && p.count == 1 && p.role == 0 && p.cat == TagCat::Particle;
                const bool drop = p.kind == Kind::PosMtx && p.count == 12 && p.stream;
                if (!point && !drop) continue;
                const char how = !blend ? 'b' : b.partner[i] >= 0 ? 'b' : p.backStep >= 0 ? 's' : 'n';
                f32 w[3], o[3] = {0, 0, 0};
                if (point) {
                    for (int c = 0; c < 3; c++) w[c] = loadBE(sReplay.data() + p.offset + p.posOffset + 4 * c);
                } else {
                    if (p.view >= b.views.size()) continue;
                    M34 inv;
                    const ViewPair* vp = blend && p.view < sViewToA.size() && sViewToA[p.view] >= 0
                                             ? viewPair(*a, u16(sViewToA[p.view]), b, p.view, alpha)
                                             : nullptr;
                    if (vp && vp->ok && !vp->cut) inv = vp->it;
                    else if (!invertAffine(b.views[p.view].data(), inv.data())) continue;
                    f32 m[12], wm[12];
                    for (int c = 0; c < 12; c++) m[c] = loadBE(sReplay.data() + p.offset + 4 * c);
                    compose(inv.data(), m, wm);
                    const f32 zero[3] = {0, 0, 0};
                    const f32* ctr = p.bundle < b.bundleCentre.size() && std::isfinite(b.bundleCentre[p.bundle][0]) ? b.bundleCentre[p.bundle].data() : zero;
                    xform34(wm, ctr, w);
                    o[0] = wm[3], o[1] = wm[7], o[2] = wm[11];
                }
                std::fprintf(sSprayLog, "%c %c %016llX %.2f %.2f %.2f %.2f %.2f %.2f\n", point ? 'p' : 'd', how, (unsigned long long)p.key, w[0],
                             w[1], w[2], o[0], o[1], o[2]);
            }
        }
    }
    // Each replay starts from the GX state the tape started from on the console: the state after
    // the previous tape (Aurora replay state slots, patch 0021; from the SMS port). The end state of
    // each tape is saved after it is replayed (four slots: LM_INTERP_TEST replays tape N before the
    // late replays of N-1); a tape whose predecessor was never replayed (dropped, or drawn directly
    // since reset()) starts from the current state.
    const bool haveBStart = b.seq > 1 && sEndSeq[(b.seq - 1) & 3] == b.seq - 1;
    if (haveBStart) GXRestoreStateAurora(u8((b.seq - 1) & 3));
    // EFB copies made by this replay go into this tape's copy generation, so they don't replace the
    // previous frame's copies that this frame's early draws may still read (Aurora patch 0028).
    // Only the first replay of a tape reads its copies back into memory (bit 6 on the others).
    GXSetCopyGenerationAurora(u8((b.seq & 3) | 0x80 | (b.replayed.exchange(true) ? 0x40 : 0)));
    probeBeforeReplay(a, b, alpha, sReplay);
    {
        std::lock_guard lk(sReplayRecM);
        sReplayRecs[sReplayRecNext++ % sReplayRecs.size()] =
            ReplayRec{AuroraFifoStreamPos(), u32(sReplay.size()), b.seq, b.gameFrame, b.sites};
    }
    AuroraReplay(sReplay.data(), u32(sReplay.size()));
    GXSaveStateAurora(u8(b.seq & 3));
    sEndSeq[b.seq & 3] = b.seq;
}

// --- LM_INTERP_TEST host side ---
struct TestItem {
    std::shared_ptr<const Tape> a, b;
    float alpha;
    std::string name;
    void* target = nullptr;
    u32 frames = 0;
};
std::deque<TestItem> sTestItems;
std::deque<std::shared_ptr<const Tape>> sTestTapes;  // newest last

// LM_INTERP_TEST + LM_INTERP_TRACE: how well matrices are paired over three consecutive
// tapes. A correctly paired object moves smoothly: its translation's second difference is
// small next to its per-frame motion; a wrong pair (another object's matrix) jumps.
void diagnoseMatching(const Tape& t0, const Tape& t1, const Tape& t2) {
    if (t1.pairedWith != &t0 || t2.pairedWith != &t1) return;
    std::vector<s32> next(t1.patches.size(), -1);
    for (u32 k = 0; k < t2.patches.size(); k++)
        if (t2.partner[k] >= 0) next[t2.partner[k]] = s32(k);
    u32 total = 0, unmatched01 = 0, unmatched12 = 0, jerky = 0, logged = 0, bbTotal = 0, bbTracked = 0, bbJerky = 0;
    for (u32 i = 0; i < t1.patches.size(); i++) {
        const Patch& p = t1.patches[i];
        if (p.kind != Kind::PosMtx || p.role == 2) continue;
        total++;
        // Camera-facing sprites (particles, billboards): no rotation in view space.
        const bool billboard = std::fabs(val(t1, p, 1)) + std::fabs(val(t1, p, 2)) + std::fabs(val(t1, p, 4)) +
                                   std::fabs(val(t1, p, 6)) + std::fabs(val(t1, p, 8)) + std::fabs(val(t1, p, 9)) <
                               1e-3f;
        bbTotal += billboard;
        const s32 j0 = t1.partner[i], j2 = next[i];
        if (j0 < 0) unmatched01++;
        if (j2 < 0) unmatched12++;
        if (j0 < 0 || j2 < 0) continue;
        const Patch& q0 = t0.patches[j0];
        const Patch& q2 = t2.patches[j2];
        float jerk = 0, step = 0, rot = 0;
        for (int r = 0; r < 3; r++) {
            const int k = 4 * r + 3;
            const float a = val(t0, q0, k), b = val(t1, p, k), c = val(t2, q2, k);
            jerk = std::max(jerk, std::fabs(a - 2 * b + c));
            step = std::max({step, std::fabs(b - a), std::fabs(c - b)});
            for (int col = 0; col < 3; col++) {
                const int m = 4 * r + col;
                rot = std::max(rot, std::fabs(val(t0, q0, m) - 2 * val(t1, p, m) + val(t2, q2, m)));
            }
        }
        if (billboard) {
            bbTracked++;
            if (jerk > 0.5f * step + 3.0f && bbJerky++ < 6)
                log("[interp]   billboard bundle %u: (%.1f %.1f %.1f s%.2f) (%.1f %.1f %.1f s%.2f) (%.1f %.1f %.1f s%.2f)", p.bundle,
                    val(t0, q0, 3), val(t0, q0, 7), val(t0, q0, 11), val(t0, q0, 0), val(t1, p, 3), val(t1, p, 7),
                    val(t1, p, 11), val(t1, p, 0), val(t2, q2, 3), val(t2, q2, 7), val(t2, q2, 11), val(t2, q2, 0));
            else if (jerk > 0.5f * step + 3.0f) bbJerky++;
        }
        if (jerk > 0.5f * step + 5.0f || rot > 0.2f) {
            jerky++;
            if (logged++ < 4)
                log("[interp]   jerk %.1f (step %.1f, rot %.3f) src %08X slot %u", jerk, step, rot, p.source, p.slot);
        }
    }
    u32 kinds[kKinds] = {}, verts = 0;
    for (const Patch& p : t1.patches) kinds[u32(p.kind)]++;
    for (u32 i = 0; i < t1.patches.size(); i++)
        if (t1.patches[i].kind == Kind::Verts && t1.partner[i] >= 0) verts++;
    log("[interp] match f%u: %u pos matrices, %u not in previous, %u not in next, %u jerky; cut %d; nrm %u tex %u "
        "proj %u light %u verts %u (%u paired); billboards %u, %u tracked over 3 frames, %u jerky",
        t1.dumpFrame, total, unmatched01, unmatched12, jerky, int(t1.cut), kinds[1], kinds[2], kinds[3], kinds[4],
        kinds[5], verts, bbTotal, bbTracked, bbJerky);
}

bool testReplay() {
    sTestHostSeen = std::chrono::steady_clock::now().time_since_epoch().count();
    if (sTestItems.empty()) {
        bool pending;
        {
            std::lock_guard lk(sTestM);
            pending = sTestPending;
        }
        std::shared_ptr<const Tape> newest;
        {
            std::lock_guard lk(sM);
            newest = sNewest;
        }
        if (!newest) return false;
        if (!pending) {  // nothing new: keep showing the newest frame
            replayPair(nullptr, *newest, 1.0f, nullptr);
            return true;
        }
        if (sTestTapes.empty() || sTestTapes.back() != newest) sTestTapes.push_back(newest);
        while (sTestTapes.size() > 3) sTestTapes.pop_front();
        const auto& n = sTestTapes.back();
        if (sTrace && sTestTapes.size() == 3 && sTestTapes[1]->dumpOffset != ~0u)
            diagnoseMatching(*sTestTapes[0], *sTestTapes[1], *sTestTapes[2]);
        if (n->dumpOffset != ~0u) {
            sTestItems.push_back({nullptr, n, 1.0f, "f" + std::to_string(n->dumpFrame) + "_fresh"});
            if (sTestTapes.size() == 3 && sTestTapes[1]->dumpOffset != ~0u)
                for (int st = 1; st <= sTestSteps; st++)
                    sTestItems.push_back({sTestTapes[0], sTestTapes[1], float(st) / float(sTestSteps),
                                          "f" + std::to_string(sTestTapes[1]->dumpFrame) + "_s" + std::to_string(st)});
        }
        if (sTestItems.empty()) {
            std::lock_guard lk(sTestM);
            sTestPending = false;
            sTestCv.notify_all();
            replayPair(nullptr, *newest, 1.0f, nullptr);
            return true;
        }
    }
    TestItem& it = sTestItems.front();
    if (!it.target) it.target = efbcopy::namedDumpTarget(it.name.c_str());
    sLogSnaps = sTrace && it.frames == 0 && it.alpha < 1.0f && it.name.size() > 3 && it.name.compare(it.name.size() - 3, 3, "_s1") == 0;
    std::fill(std::begin(sLoggedSnaps), std::end(sLoggedSnaps), 0u);
    std::fill(std::begin(sSnapKinds), std::end(sSnapKinds), 0u);
    // LM_INTERP_POISON=<item>,<phys hex>,<size hex> (test aid): fill that guest memory with garbage during
    // that LM_INTERP_TEST replay, to find memory a replay still reads live (bisect the range).
    static const char* sPoison = std::getenv("LM_INTERP_POISON");
    std::vector<u8> saved;
    u32 pphys = 0, psize = 0;
    if (sPoison && it.frames == 0) {
        char nm[64] = {};
        if (std::sscanf(sPoison, "%63[^,],%x,%x", nm, &pphys, &psize) == 3 && it.name == nm) {
            saved.assign(g_mem + pphys, g_mem + pphys + psize);
            std::memset(g_mem + pphys, 0xA5, psize);
            log("[dbg] poisoned %08X+%X for %s", pphys, psize, nm);
        }
    }
    replayPair(it.a.get(), *it.b, it.alpha, it.target);
    probeFill(it.a, it.b);
    if (!saved.empty()) {
        AuroraGXSync();
        std::memcpy(g_mem + pphys, saved.data(), psize);
    }
    if (sLogSnaps) {
        u32 unpaired[kKinds] = {}, listed = 0;
        for (u32 i = 0; i < it.b->patches.size(); i++)
            if (it.b->partner.empty() || it.b->partner[i] < 0) {
                const Patch& p = it.b->patches[i];
                unpaired[u32(p.kind)]++;
                if (p.kind == Kind::PosMtx && listed++ < 6) {
                    const u64 id = p.bundle < it.b->bundleIds.size() ? it.b->bundleIds[p.bundle] : 0;
                    u32 same = 0;
                    if (it.a)
                        for (u64 x : it.a->bundleIds) same += x == id;
                    log("[interp]   unpaired pos src %08X slot %u role %u draw %016llX (%u such draws before) t %.0f %.0f %.0f", p.source,
                        p.slot, p.role, (unsigned long long)id, same, val(*it.b, p, 3), val(*it.b, p, 7), val(*it.b, p, 11));
                }
            }
        log("[interp] %s: snapped pos %u nrm %u tex %u proj %u light %u verts %u; unpaired pos %u nrm %u tex %u proj %u "
            "light %u verts %u crosscam %u%s",
            it.name.c_str(), sSnapKinds[0], sSnapKinds[1], sSnapKinds[2], sSnapKinds[3], sSnapKinds[4], sSnapKinds[5],
            unpaired[0], unpaired[1], unpaired[2], unpaired[3], unpaired[4], unpaired[5], sCrossCamera,
            it.b->cut ? " CUT" : "");
    }
    sLogSnaps = false;
    efbcopy::applyWritebacks();
    if (efbcopy::namedDumpWritten(it.target) || ++it.frames > 120) {
        if (it.frames > 120) log("[interp] test dump %s timed out", it.name.c_str());
        sTestItems.pop_front();
        if (sTestItems.empty()) {
            std::lock_guard lk(sTestM);
            sTestPending = false;
            sTestCv.notify_all();
        }
    }
    return true;
}

}  // namespace

// --- presentation clock (docs/INTERPOLATION.md "Pacing") ---------------------------------------
// Each tape is placed on the game's own timeline: its frame stamp (fields of 1001/60000 s at its
// present), smoothed (tapeTimes below). A present shows the game time G = (smoothed present time) - D,
// interpolated between the two tapes around it, so motion advances by exactly the time between
// presents. D, the delay, covers the slowest recent tape (a tape must be finished before G
// reaches its predecessor); G follows its target with at most +-5% speed changes, never runs
// backwards and never beyond the newest tape. The old rule (alpha from the time since the newest
// tape was finished, over its stamp step) made every in-between frame depend on when the game
// thread happened to finish a frame and on stamp steps of 1 and 3 fields: LM_INTERP_PACING=old.
struct Pacer {
    double clk = 0, prevClk = 0, period = 1.0 / 60, lastNow = -1;  // smoothed present clock (s)
    double g = 0;                                      // shown game time (s)
    bool haveG = false;
    double d = 0.05;                                   // delay (s)
    std::deque<double> samples;                        // per tape: finished - predecessor's game time
    u64 lastSeq = 0;
    u32 stalls = 0, snaps = 0, stallRun = 0;
    bool stalledNow = false;  // this present: g held at the newest tape
    double behindSum = 0;     // shown game time behind the newest game frame (s), summed per present
    u32 behindN = 0;
    // Smoothed tape times (s) by seq. The stamp of a frame is the retrace count at its present,
    // so a frame whose work ends just after a retrace is stamped a field late and the next one a
    // field early: steps of 1 and 3 fields in a steady 30 fps game (3-4% of frames in the foyer),
    // where the console shows every frame for two fields. Each tape is placed one nominal step
    // (the median of the recent stamp steps) after its predecessor, pulled 10% toward its stamp;
    // a stamp more than 1.5 fields off (a real slowdown, a load) is taken as is.
    std::deque<std::pair<u64, double>> times;
    std::deque<u32> steps;
    u64 lastTimed = 0;
    u32 lastStamp = 0;
    // Phase lock (see pace()): the delay actually used (s, 0: off), between d - 0.25 and d + 1
    // periods, that keeps the presents halfway between the game-frame changes.
    double lockD = 0;
    u32 lockWraps = 0;
};
Pacer sPacer;
const bool sOldPacing = [] {
    const char* v = std::getenv("LM_INTERP_PACING");
    return v && std::strcmp(v, "old") == 0;
}();
// LM_PHASE_LOCK=0: no phase lock (presents fall wherever the delay puts them within a game frame).
const bool sPhaseLock = [] {
    const char* v = std::getenv("LM_PHASE_LOCK");
    return !(v && *v == '0');
}();
double tapeTime(const Tape& t) {
    for (auto it = sPacer.times.rbegin(); it != sPacer.times.rend(); ++it)
        if (it->first == t.seq) return it->second;
    return double(t.retrace) * kRetrace;
}
void timeTapes(const std::vector<std::shared_ptr<const Tape>>& q) {
    Pacer& p = sPacer;
    for (const auto& t : q) {
        if (t->seq <= p.lastTimed) continue;
        const double raw = double(t->retrace) * kRetrace;
        double time = raw;
        if (p.lastTimed != 0 && t->seq == p.lastTimed + 1 && !p.times.empty()) {
            const u32 step = t->retrace - p.lastStamp;
            p.steps.push_back(step);
            if (p.steps.size() > 15) p.steps.pop_front();
            std::vector<u32> st(p.steps.begin(), p.steps.end());
            std::nth_element(st.begin(), st.begin() + st.size() / 2, st.end());
            const double nominal = double(std::clamp<u32>(st[st.size() / 2], 1, 6)) * kRetrace;
            const double pred = p.times.back().second + nominal;
            const double err = raw - pred;
            time = std::fabs(err) > 1.5 * kRetrace ? raw : pred + err * 0.1;
        } else {
            p.steps.clear();
        }
        p.times.emplace_back(t->seq, time);
        if (p.times.size() > 32) p.times.pop_front();
        p.lastTimed = t->seq;
        p.lastStamp = t->retrace;
    }
}
double hostSec(Clock::time_point t) { return std::chrono::duration<double>(t - sJudderT0).count(); }

// Picks the tapes (a, b) and alpha for this present from the queue `q` (consecutive tapes, oldest
// first). Returns the index of b in q (a = q[i - 1]) or -1 when there is no pair (b = the newest).
int pace(const std::vector<std::shared_ptr<const Tape>>& q, Clock::time_point nowTp, float& alpha) {
    Pacer& p = sPacer;
    const double now = hostSec(nowTp);
    // Present clock: advance by the measured period, pulled gently toward the real time; a
    // present that is off by more than 4 ms (a missed refresh, a hitch) resynchronizes.
    if (p.lastNow < 0) {
        p.clk = now;
    } else {
        const double dt = now - p.lastNow;
        if (dt > 0 && dt < 0.1) p.period += (std::clamp(dt, 1.0 / 500, 1.0 / 15) - p.period) * 0.05;
        const double pred = p.clk + p.period;
        const double err = now - pred;
        p.clk = std::fabs(err) > 0.004 ? now : pred + err * 0.1;
    }
    const double dClk = p.lastNow < 0 ? 0.0 : p.clk - p.prevClk;
    p.prevClk = p.clk;
    p.lastNow = now;
    timeTapes(q);
    // Delay: the slowest of the recent tapes (finish time - predecessor's game time) plus a margin.
    for (size_t i = 1; i < q.size(); i++) {
        if (q[i]->seq <= p.lastSeq) continue;
        p.lastSeq = q[i]->seq;
        if (q[i]->pairedWith != q[i - 1].get()) continue;
        p.samples.push_back(hostSec(q[i]->done) - tapeTime(*q[i - 1]));
        if (p.samples.size() > 90) p.samples.pop_front();
    }
    if (!p.samples.empty()) {
        std::vector<double> s(p.samples.begin(), p.samples.end());
        std::sort(s.begin(), s.end());
        const double median = s[s.size() / 2];
        double hi = median;
        for (double v : s)
            if (v <= median + 0.05) hi = std::max(hi, v);  // a load hitch is not the steady delay
        p.d = hi + 0.0015;
    }
    // Phase lock. Whatever is shown as the game frame (2D layout, text, anything without a
    // partner, 30 Hz texture animations) changes at the first present past a tape's time. With n
    // presents per game frame (120 / 30: 4) a present that falls right at a tape's time decides
    // by a fraction of a millisecond (the smoothed tape times move by ~1-2 ms) whether a change
    // shows one present early or late: cadences of 5 and 3 (or 3 and 1 at 60 fps) instead of
    // 4 and 4, whenever the drift between the game's 29.97 Hz and the display's rate or a change
    // of the measured delay brings the presents to the tape times (user clip at 60 fps: a text
    // box opening in steps of 5, 2 and 5 frames of a 120 Hz capture). The delay used (lockD)
    // follows the phase so that the target stays halfway between tape times: measured on the
    // target itself (the shown time g glides toward it; measuring g lagged and hunted), kept
    // between d - 0.25 and d + 1 periods (d has a margin of its own over the slowest recent tape),
    // moved by a whole period only when d leaves that range (small changes of d no longer move
    // the target at all). Average extra delay ~0.4 present.
    double target = p.clk - p.d;
    if (sPhaseLock && p.steps.size() >= 4 && p.period > 0) {
        std::vector<u32> st(p.steps.begin(), p.steps.end());
        std::nth_element(st.begin(), st.begin() + st.size() / 2, st.end());
        const double step = double(st[st.size() / 2]) * kRetrace;
        const double n = step / p.period, nr = std::round(n);
        if (nr >= 2 && nr <= 16 && std::fabs(n - nr) < 0.08) {
            if (p.lockD <= 0) p.lockD = p.d;
            const double x = (p.clk - p.lockD - tapeTime(*q.back())) / p.period;
            p.lockD += (x - std::floor(x) - 0.5) * p.period * 0.1;
            for (int k = 0; k < 4 && p.lockD < p.d - 0.25 * p.period; k++) p.lockD += p.period, p.lockWraps++;
            for (int k = 0; k < 4 && p.lockD > p.d + p.period; k++) p.lockD -= p.period, p.lockWraps++;
            target = p.clk - p.lockD;
        } else {
            p.lockD = 0;  // not an integer ratio: the plain delay
        }
    }
    const double newest = tapeTime(*q.back());
    if (!p.haveG || std::fabs(target - (p.g + dClk)) > 0.1) {
        if (p.haveG) p.snaps++;
        p.g = target;
        p.haveG = true;
    } else {
        const double next = p.g + dClk;
        p.g = next + std::clamp((target - next) * 0.1, -0.05 * dClk, 0.05 * dClk);
    }
    p.stalledNow = p.g > newest;
    p.behindSum += std::max(0.0, newest - p.g);
    p.behindN++;
    if (p.g > newest) {
        p.g = newest;
        p.stalls++;
        // Stalled for a quarter second (the timeline moved: a pause, the VI timer resynchronized):
        // measure the delay afresh.
        if (++p.stallRun > 30) {
            p.samples.clear();
            p.stallRun = 0;
            p.haveG = false;
        }
    } else {
        p.stallRun = 0;
    }
    if (p.g < tapeTime(*q.front()) - 0.1) p.haveG = false;  // behind every tape kept: resynchronize next time
    // The pair around g.
    alpha = 1.0f;
    for (size_t i = 1; i < q.size(); i++) {
        const double ta = tapeTime(*q[i - 1]), tb = tapeTime(*q[i]);
        if (p.g > tb && i + 1 < q.size()) continue;
        if (tb <= ta) return int(i);
        // A step longer than 6 fields (a load hitch): hold a, then move over the last 2 fields.
        const double start = tb - ta > 6 * kRetrace ? tb - 2 * kRetrace : ta;
        alpha = float(std::clamp((p.g - start) / (tb - start), 0.0, 1.0));
        return int(i);
    }
    return -1;
}

PresentInfo sLastPresent;
u64 sStatsSeq = 0;  // newest tape at the start of the stats window: game frames made = newest - this
PresentInfo lastPresent() { return sLastPresent; }

bool replay(bool final) {
    if (sTestSteps && !final) return testReplay();
    std::shared_ptr<const Tape> a, b;
    float alpha = 1.0f;
    const auto r0 = Clock::now();
    if (sOldPacing || final) {
        {
            std::lock_guard lk(sM);
            a = sPrevious;
            b = sNewest;
        }
        if (!b) return false;
        // One game frame behind: blend from a (alpha 0) to b (alpha 1) over the time
        // between the two game frames, starting when b was finished.
        if (a && !final) {
            const double interval = std::clamp((b->retrace - a->retrace) * kRetrace, 0.008, 0.1);
            const double since = std::chrono::duration<double>(Clock::now() - b->done).count();
            alpha = float(std::clamp(since / interval, 0.0, 1.0));
        }
    } else {
        static std::vector<std::shared_ptr<const Tape>> q;
        {
            std::lock_guard lk(sM);
            q.assign(sQueue.begin(), sQueue.end());
        }
        if (q.empty()) return false;
        const int i = pace(q, pacing::frameTime(), alpha);
        if (i < 0) {
            b = q.back();
        } else {
            b = q[i];
            a = q[i - 1];
            // Tapes before a are done with: retire them (the FIFO may still read their copies).
            if (i > 1) {
                std::lock_guard lk(sM);
                while (!sQueue.empty() && sQueue.front()->seq < a->seq) {
                    sRetired.push_back(std::move(sQueue.front()));
                    sQueue.pop_front();
                }
                while (sRetired.size() > 6) sRetired.pop_front();
            }
        }
        q.clear();
    }
    sLastPresent.present++;
    sLastPresent.bSeq = b->seq;
    sLastPresent.alpha = final || !a ? 1.0f : alpha;
    sLastPresent.stalled = sPacer.stalledNow;
    sPacer.stalledNow = false;
    {
        std::lock_guard lk(sM);
        if (sNewest) {
            sLastPresent.newestSeq = sNewest->seq;
            sLastPresent.newestDone = std::chrono::duration<double>(sNewest->done.time_since_epoch()).count();
        }
    }
    judderLine("R %.1f %llu %llu %.4f %.3f %.2f %u %llu\n", judderUs(r0), (unsigned long long)b->seq,
               (unsigned long long)(a && !final ? a->seq : 0), alpha,
               a && !final ? double(a->retrace) + alpha * double(b->retrace - a->retrace) : double(b->retrace),
               sPacer.d * 1000.0, sLastPresent.present, (unsigned long long)aurora_frame_counter());
    // Test aid LM_PRESENT_DUMP=1 (with LM_FRAME_DUMP): every present of the tapes in the dump range as
    // its own file, f<frame>_p<present>_a<alpha x 1000>.ppm (real time, unlike LM_INTERP_TEST).
    static const bool sPresentDump = std::getenv("LM_PRESENT_DUMP") != nullptr;
    void* presentDump = nullptr;
    if (sPresentDump && b->dumpOffset != ~0u) {
        char nm[64];
        std::snprintf(nm, sizeof nm, "f%05u_p%06u_a%04d", b->dumpFrame, sLastPresent.present,
                      int(std::lround(sLastPresent.alpha * 1000.0f)));
        presentDump = efbcopy::namedDumpTarget(nm);
    }
    sSprayPresent = sLastPresent.present;
    replayPair(final ? nullptr : a.get(), *b, alpha, presentDump);
    probeFill(a, b);
    if (sTrackOut && !final && (!a || b->cut || b->pairedWith != a.get()))  // the whole picture at b (a camera cut, a gap)
        trackLine("HC %u %llu f%u %.3f %s\n", sLastPresent.present, (unsigned long long)b->seq, b->gameFrame, alpha,
                  !a ? "no-a" : b->cut ? "cut" : "not-consecutive");
    if (sJudder && a && !final && alpha < 1.0f) {
        // V present m[12]: the blended camera of b's main camera (most position matrices), the one
        // static scenery is drawn with: tools/interp/judder.py --camera checks that a fixed point
        // moves evenly against the frame time (interpolation math, apart from what the screen shows).
        const u16 best = b->mainView;
        for (const ViewPair& vp : sViewPairs)
            if (u16(vp.key) == best && vp.ok && !vp.cut) {
                judderLine("V %u %.5f %.5f %.5f %.3f %.5f %.5f %.5f %.3f %.5f %.5f %.5f %.3f\n", sLastPresent.present, vp.vt[0], vp.vt[1],
                           vp.vt[2], vp.vt[3], vp.vt[4], vp.vt[5], vp.vt[6], vp.vt[7], vp.vt[8], vp.vt[9], vp.vt[10], vp.vt[11]);
                break;
            }
    }
    {
        // The FIFO thread reads these tapes' copies (arrays, textures) until the frame is drained in
        // aurora_end_frame, after replay() returned: keep the last few replays' tapes alive here,
        // whatever the guest retires meanwhile (a stalled host could otherwise free them: a
        // crash reading a vertex array copy in push_gx_draw under heavy load).
        static std::deque<std::shared_ptr<const Tape>> sInFlight;
        sInFlight.push_back(b);
        if (a) sInFlight.push_back(a);
        while (sInFlight.size() > 8) sInFlight.pop_front();
    }
    sReplayUs += u64(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - r0).count());
    if (sTrace) {
        static Clock::time_point last = Clock::now();
        const auto now = Clock::now();
        log("[interp] alpha %.3f dt %.1f ms tape %u%s, snapped pos %u nrm %u tex %u proj %u light %u verts %u, "
            "paired %u unpaired %u untagged %u",
            alpha, std::chrono::duration<double, std::milli>(now - last).count(), b->retrace, b->cut ? " (cut)" : "",
            sSnapKinds[0], sSnapKinds[1], sSnapKinds[2], sSnapKinds[3], sSnapKinds[4], sSnapKinds[5], b->paired, b->unpaired, b->untagged);
        last = now;
        std::fill(std::begin(sSnapKinds), std::end(sSnapKinds), 0u);
    }

    // Stats, every 5 s.
    sPresented++;
    static u32 lastRetrace = 0;
    if (b->retrace != lastRetrace) {
        lastRetrace = b->retrace;
        sTapes++;
        sTapeBytes += b->bytes.size();
        sArrayBytes += b->arrayBytes;
        sTexBytes += b->textureBytes;
        sPairedSum += b->paired;
        sGuestUs += b->guestUs;
        for (int k = 0; k < 5; k++) sPhaseUs[k] += b->phaseUs[k];
        sUnpairedSum += b->unpaired;
        sUntaggedSum += b->untagged;
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - sStatsStart).count();
    if (elapsed >= 5.0) {
        log("[interp] presented %.1f fps, game %.1f fps (shown %.1f), tape %.0f KiB + arrays %.0f KiB + new texture copies %.0f KiB, "
            "per tape %.0f paired / %.0f unpaired / %.0f untagged values; per present %u blended / %u snapped / %u from the newest "
            "frame under the camera; %u cut presents; tape close %.2f ms (guest: arrays %.2f, "
            "copy %.2f, pairing %.2f, identity check %.2f, cut %.2f), replay %.2f ms (host); shown %.1f ms behind the newest game frame, %u stalls, %u resyncs; phase lock +%.1f ms (%u wraps)",
            sPresented / elapsed, sStatsSeq ? double(sLastPresent.newestSeq - sStatsSeq) / elapsed : 0.0, sTapes / elapsed, sTapes ? double(sTapeBytes) / sTapes / 1024.0 : 0.0,
            sTapes ? double(sArrayBytes) / sTapes / 1024.0 : 0.0, sTapes ? double(sTexBytes) / sTapes / 1024.0 : 0.0,
            sTapes ? double(sPairedSum) / sTapes : 0.0,
            sTapes ? double(sUnpairedSum) / sTapes : 0.0, sTapes ? double(sUntaggedSum) / sTapes : 0.0,
            sPresented ? sBlended / sPresented : 0, sPresented ? sSnapped / sPresented : 0, sPresented ? sCamOnly / sPresented : 0, sCuts, sTapes ? double(sGuestUs) / sTapes / 1000.0 : 0.0,
            sTapes ? double(sPhaseUs[0]) / sTapes / 1000.0 : 0.0, sTapes ? double(sPhaseUs[1]) / sTapes / 1000.0 : 0.0,
            sTapes ? double(sPhaseUs[2]) / sTapes / 1000.0 : 0.0, sTapes ? double(sPhaseUs[3]) / sTapes / 1000.0 : 0.0,
            sTapes ? double(sPhaseUs[4]) / sTapes / 1000.0 : 0.0,
            sPresented ? double(sReplayUs) / sPresented / 1000.0 : 0.0, sPacer.behindN ? sPacer.behindSum / sPacer.behindN * 1000.0 : 0.0, sPacer.stalls, sPacer.snaps,
            sPacer.lockD > 0 ? (sPacer.lockD - sPacer.d) * 1000.0 : 0.0, sPacer.lockWraps);
        sPacer.lockWraps = 0;
        sPacer.behindSum = 0;
        sPacer.behindN = 0;
        sPacer.stalls = sPacer.snaps = 0;
        const double presentedWindow = std::max(1u, sPresented);
        sPresented = sTapes = sBlended = sSnapped = sCuts = sCamOnly = 0;
        sTapeBytes = sArrayBytes = sTexBytes = sPairedSum = sUnpairedSum = sUntaggedSum = sGuestUs = sReplayUs = 0;
        if (sProbeOut && sProbeWindow.replays) {
            const auto& w = sProbeWindow;
            log("[judder] %llu in-between frames, %llu tagged draws judged. DEFECTS (off the ideal by more than %.1f px): blend %llu, "
                "camera %llu, other %llu; visible (full view, >= %.0f px2): blend %llu, camera %llu, other %llu. Game-frame switches "
                "(policy): turn %llu, switch %llu, jump %llu (visible %llu / %llu / %llu). Worst %.1f px (visible %.1f); identity check: "
                "%llu of %llu tagged matrices and %llu of %llu CPU-built blocks jumped there and back",
                (unsigned long long)w.replays, (unsigned long long)w.judged, sProbeThresh, (unsigned long long)w.cause[0],
                (unsigned long long)w.cause[2], (unsigned long long)w.cause[4], kVisiblePx2, (unsigned long long)w.visCause[0],
                (unsigned long long)w.visCause[2], (unsigned long long)w.visCause[4], (unsigned long long)w.cause[1],
                (unsigned long long)w.cause[5], (unsigned long long)w.cause[6], (unsigned long long)w.visCause[1],
                (unsigned long long)w.visCause[5], (unsigned long long)w.visCause[6], w.maxErr, w.visMaxErr,
                (unsigned long long)sIdTotals.flagged, (unsigned long long)sIdTotals.checked,
                (unsigned long long)sIdTotals.vertsFlagged, (unsigned long long)sIdTotals.vertsChecked);
            sProbeWindow = {};
        }
        if (sTrackOut && sTrackWindow.replays) {
            const auto& w = sTrackWindow;
            log("[track] %llu in-betweens: %llu vertices near Luigi judged, %llu off their path (> %.0f px; worst %.1f); near the nozzle "
                "%llu judged, %llu off; %llu without a known matrix, %llu unpaired",
                (unsigned long long)w.replays, (unsigned long long)w.judged, (unsigned long long)w.flagged, sTrackPx, w.maxPx,
                (unsigned long long)w.nozJudged, (unsigned long long)w.nozFlagged, (unsigned long long)w.unknown,
                (unsigned long long)w.unpaired);
            sTrackWindow = {};
        }
        if (sXlPaired + sXlNew)
            log("[interp] extra lights per present: %.1f blended by identity, %.1f without partner (%.2f whose identity jumped >200 units)",
                double(sXlPaired) / presentedWindow, double(sXlNew) / presentedWindow, double(sXlJumped) / presentedWindow);
        sXlPaired = sXlNew = sXlJumped = 0;
        if (sProjShared) log("[interp] unpaired projections per present given an identical projection's blend: %.2f", double(sProjShared) / presentedWindow);
        sProjShared = 0;
        if (sColorsBlended + sColorsSwitched)
            log("[interp] colour registers per present: %.1f blended, %.1f switched (jumped); model instances blended linearly %.1f, "
                "switched %.1f; particles shown at the game frame (turned back) %.1f; born moving along their step %.1f",
                double(sColorsBlended) / presentedWindow, double(sColorsSwitched) / presentedWindow,
                double(sGroupForced) / presentedWindow, double(sGroupSnaps) / presentedWindow, double(sReversals) / presentedWindow,
                double(sBornMoving) / presentedWindow);
        sColorsBlended = sColorsSwitched = sGroupForced = sGroupSnaps = sReversals = sBornMoving = 0;
        if (sOwnSkinned + sOwnPivoted)
            log("[interp] own-camera scenes per present: %.1f skinned model instances blended entrywise, %.1f rigid model matrices blended about their draw",
                double(sOwnSkinned) / presentedWindow, double(sOwnPivoted) / presentedWindow);
        sOwnSkinned = sOwnPivoted = 0;
        if (s2DBlended + s2DKept)
            log("[interp] 2D pictures that moved, per present: %.2f blended, %.2f as captured (not the same picture, glyph-sized or jumped)",
                double(s2DBlended) / presentedWindow, double(s2DKept) / presentedWindow);
        s2DBlended = s2DKept = 0;
        if (sTextGroupsBlended)
            log("[interp] text boxes per present: %.2f blended as one layout (box moving %.2f, text scrolling %.2f, letters fading in %.2f)",
                double(sTextGroupsBlended) / presentedWindow, double(sTextBoxMoved) / presentedWindow,
                double(sTextScrolled) / presentedWindow, double(sTextFaded) / presentedWindow);
        sTextGroupsBlended = sTextBoxMoved = sTextScrolled = sTextFaded = 0;
        if (sArraysBlended) log("[interp] %.1f CPU-deformed position arrays blended per present", double(sArraysBlended) / presentedWindow);
        sArraysBlended = 0;
        std::fill(std::begin(sPhaseUs), std::end(sPhaseUs), 0ull);
        sStatsStart = Clock::now();
        sStatsSeq = sLastPresent.newestSeq;
    }
    return true;
}

bool judderLogOn() { return sJudder != nullptr; }
void logPresentTiming(u64 frame, int64_t submitNs, int64_t presentNs, int64_t doneNs) {
    if (!sJudder) return;
    // P frame submit present gpu-done (us, judder time base; Aurora patch 0197)
    const double t0 = double(std::chrono::duration_cast<std::chrono::nanoseconds>(sJudderT0.time_since_epoch()).count());
    const auto us = [t0](int64_t ns) { return ns ? (double(ns) - t0) / 1000.0 : 0.0; };
    judderLine("P %llu %.1f %.1f %.1f\n", (unsigned long long)frame, us(submitNs), us(presentNs), us(doneNs));
}
void hostMark(int phase) {
    if (!sJudder) return;
    judderLine("H %d %.1f\n", phase, judderUs(Clock::now()));
    if (phase == 4) {
        static u32 n = 0;
        if (++n % 256 == 0) {
            std::lock_guard lk(sJudderM);
            std::fflush(sJudder);
        }
    }
}

void reset() {
    std::lock_guard lk(sM);
    // Frames are drawn directly from now on: the saved replay states are out of date.
    std::fill(std::begin(sEndSeq), std::end(sEndSeq), 0);
    // The FIFO may still be reading the last replay's array copies.
    if (sPrevious) sRetired.push_back(std::move(sPrevious));
    if (sNewest) sRetired.push_back(std::move(sNewest));
    for (auto& t : sQueue) sRetired.push_back(std::move(t));
    sQueue.clear();
    while (sRetired.size() > 6) sRetired.pop_front();
    sPacer = {};
}

}  // namespace recomp::interp
