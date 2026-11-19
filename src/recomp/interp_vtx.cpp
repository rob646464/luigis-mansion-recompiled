// Frame interpolation: vertex-array snapshots (interp.h, docs/INTERPOLATION.md).
//
// A tape is replayed while the guest is already running the next game frame, and
// Aurora reads indexed vertex attributes from guest memory when it processes a draw.
// Data the CPU rewrites every frame (skinned or animated meshes, particles, anything
// built into a reused buffer) would then be drawn with the next frame's contents,
// half-written, or garbage. So while capturing, the draws are parsed (display lists,
// and the raw vertex stream after GXBegin) to find the range of each array they
// index, that range is copied at the time of the draw (when the console's GPU would
// read it), and the tape's array loads are pointed at the copies.
//
// Each GXSetArray in a tape becomes its own array-load command; copies are shared per
// guest address until GXInvalidateVtxCache (the game's signal that it rewrote vertex
// data), after which the current arrays are loaded again so later draws get new copies.
// Every tape starts by loading the current arrays, so it never depends on another
// tape's copies.
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/interp.h"

#include <dolphin/gx.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace recomp::interp {
namespace {

constexpr int kArrays = 16;  // CP array slots: GX_VA_POS..GX_VA_TEX7, then the 4 matrix/light arrays

struct Binding {
    u32 guest = 0, stride = 0;
    bool valid = false;
    u64 key = 0;  // position array bound in a tag scope: identity (kept for reloads of the binding)
    int cmd = -1;  // index into sCmds: the array load in the current tape
};
Binding sBind[kArrays];

struct Cmd {
    u32 at;      // tape offset before the load was written
    int snap;    // index into sSnaps, -1: not used by any draw (left as is)
    u64 key = 0;  // position arrays bound in a tag scope: identity (detail::arrayKey)
    int fmt = -1;  // vertex format of the first draw that read it
    u32 stride = 0;
    // Its position format as that draw read it. (Taken at the draw: the format slot is often set
    // up again for another layout before the tape ends; read at the end, the bathroom's shower
    // curtain, F32 positions, was taken for S16 and its blend was refused as a jump.)
    u8 posCnt = 0, posType = 0xFF, posFrac = 0;
};
int sScanFmt = -1;  // vertex format of the draw being scanned
std::vector<Cmd> sCmds;

struct Snap {
    u32 guest;
    std::vector<u8> data;
};
std::vector<Snap> sSnaps;
std::unordered_map<u32, int> sSnapOf;  // guest address -> snapshot (this vertex-cache epoch)

// Streaming parser for raw FIFO writes (vertex data after GXBegin, inlined commands).
std::vector<u8> sPending;
u32 sPendingPos = 0;

// Stats (LM_INTERP_TRACE).
u32 sParseTapeBase = ~0u;  // tape offset of the display list being parsed (~0: raw stream)
u32 sBadOp = 0;  // last unparseable opcode | 0x100 if in the raw stream | offset << 12
u32 sUnparsed = 0, sDlCpWrites = 0, sAuroraInDl = 0, sStreamMtxLoads = 0, sStreamIndexedLoads = 0;

bool inMem1(u32 a) { return (a >> 28) == 0x8 || (a >> 28) == 0xC; }

void emitLoad(int slot) {
    Binding& b = sBind[slot];
    const u32 at = mark();
    GXSetArray(GXAttr(GX_VA_POS + slot), hostp(b.guest), 0, u8(b.stride), false);
    b.cmd = int(sCmds.size());
    sCmds.push_back({at, -1, b.key, -1, b.stride});
}

// Draw-time snapshot mode (snapDisplayList/snapEnd): use() only collects what each slot needs.
bool sSnapParse = false;
struct Need {
    u32 lo, end, extra;  // elements [lo, end) plus `extra` bytes; end == 0: unused
};
Need sNeed[kArrays];
u32 sFmtSeen = 0;  // vertex formats the parsed draws used (bit per format)
const u8* sSnapBound[kArrays] = {};  // the copy Aurora has bound per slot, nullptr: the live array
struct SnapCopy {
    const u8* ptr = nullptr;
    u32 len = 0;
};
std::unordered_map<u32, SnapCopy> sSnapCopyOf;  // guest address -> copy (this vertex-cache epoch)

// A draw reads elements [idxLo, idxEnd) of the array in `slot`, plus `extra` bytes past the last.
void use(int slot, u32 idxLo, u32 idxEnd, u32 extra) {
    if (sSnapParse) {
        Need& n = sNeed[slot];
        n.lo = n.end ? std::min(n.lo, idxLo) : idxLo;
        n.end = std::max(n.end, std::max(idxEnd, 1u));
        n.extra = std::max(n.extra, extra);
        return;
    }
    Binding& b = sBind[slot];
    u32 extent = idxEnd * b.stride + extra;
    if (!b.valid || b.cmd < 0 || !inMem1(b.guest)) return;
    const u32 phys = b.guest & 0x01FFFFFF;
    if (phys >= kMem1Size) return;
    extent = std::min(extent, kMem1Size - phys);
    Cmd& cmd = sCmds[b.cmd];
    if (slot == 0 && cmd.fmt < 0) {
        cmd.fmt = sScanFmt;
        if (sScanFmt >= 0 && !gx::vtxAttrFormat(sScanFmt, GX_VA_POS, cmd.posCnt, cmd.posType, cmd.posFrac)) cmd.posType = 0xFF;
    }
    if (cmd.snap < 0) {
        auto [it, fresh] = sSnapOf.try_emplace(b.guest, int(sSnaps.size()));
        if (fresh) sSnaps.push_back({b.guest, {}});
        cmd.snap = it->second;
    }
    std::vector<u8>& d = sSnaps[cmd.snap].data;
    if (extent > d.size()) {  // copy what this draw reads beyond the copy so far, as it is now
        const u32 old = u32(d.size());
        // The tape takes this buffer at the end of the game frame (vtxEndTape), so it starts empty and
        // grows through the frame's draws that share the snapshot: without headroom each growth
        // re-copied everything appended so far (~456 KiB per game frame).
        if (d.capacity() < extent) d.reserve(extent + extent / 4);
        d.resize(extent);
        std::memcpy(d.data() + old, hostp(b.guest) + old, extent - old);
    }
}

// What the GX desc/format state makes of format `fmt`: bytes and GXAttrType per attribute, and the
// vertex size. Both are pure in the layout state, which only the GXClearVtxDesc / GXSetVtxDesc* /
// GXSetVtxAttrFmt* HLEs change (each bumping gx::vtxLayoutVersion), so an entry is rebuilt when the
// version moves. All 17 attributes of every draw came from here and scanVertices (hottest leaf in the
// process: 363 samples; attrSize 104 more).
struct VtxLayout {
    int size[GX_VA_TEX7 + 1];  // bytes per attribute, -1 unknown (as gx::vtxAttrSize)
    u8 desc[GX_VA_TEX7 + 1];   // its GXAttrType (as gx::vtxDesc)
    u32 stride = 0;            // vertexSize(fmt)
    u32 version = ~0u;         // the gx::vtxLayoutVersion it was built from
};
// Last entry: no such format (every attribute unknown), so an out-of-range fmt needs no special case.
VtxLayout sLayout[GX_MAX_VTXFMT + 1];

const VtxLayout& layout(int fmt) {
    const bool known = fmt >= 0 && fmt < GX_MAX_VTXFMT;
    VtxLayout& l = sLayout[known ? fmt : GX_MAX_VTXFMT];
    const u32 version = gx::vtxLayoutVersion();
    if (l.version == version) return l;
    u32 stride = 0;
    bool all = true;
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) {
        l.desc[a] = gx::vtxDesc(a);
        l.size[a] = known ? gx::vtxAttrSize(fmt, a) : -1;
        if (l.size[a] < 0) all = false;
        else stride += u32(l.size[a]);
    }
    l.stride = all ? stride : 0;
    l.version = version;
    return l;
}

// Bytes per vertex of format `fmt`, or 0 if unknown.
u32 vertexSize(int fmt) { return layout(fmt).stride; }

// Bounding box of the indexed positions of the display list being parsed (vtxDisplayList): its centre
// is the draw's pivot for tumbling things (interp.cpp, replayPair).
bool sBoxOn = false;
f32 sBoxLo[3], sBoxHi[3];
bool sBoxAny = false;
// The same per position matrix slot (vertices with a direct PNMTXIDX): where the geometry each
// matrix moves is (a joint matrix's origin is often far from it; interp.cpp placeStep).
f32 sSlotLo[10][3], sSlotHi[10][3];
u16 sSlotAny = 0;
void boxPositions(int fmt, const u8* v, u32 count, u32 vtxSize, u32 offset, u32 idxSize) {
    const bool pnIdx = gx::vtxDesc(GX_VA_PNMTXIDX) == GX_DIRECT;
    const Binding& b = sBind[0];
    if (!b.valid || !inMem1(b.guest) || b.stride == 0) return;
    u8 cnt = 0, type = 0, frac = 0;
    if (!gx::vtxAttrFormat(fmt, GX_VA_POS, cnt, type, frac)) return;
    static constexpr u32 kSize[] = {1, 1, 2, 2, 4};
    if (type > 4) return;
    const u32 cs = kSize[type], nc = cnt == GX_POS_XYZ ? 3 : 2;
    const float scale = type == GX_F32 ? 1.0f : 1.0f / float(1u << frac);
    const u32 step = std::max(1u, count / 64);  // a sample is enough for a centre
    for (u32 i = 0; i < count; i += step) {
        const u8* p = v + i * vtxSize + offset;
        const u32 idx = idxSize == 2 ? u32(p[0] << 8 | p[1]) : p[0];
        if (idx == (idxSize == 2 ? 0xFFFFu : 0xFFu)) continue;
        const u32 phys = (b.guest & 0x01FFFFFF) + idx * b.stride;
        if (phys + cs * nc > kMem1Size) continue;
        const u8* e = hostp(0x80000000u | phys);
        for (u32 c = 0; c < 3; c++) {
            float f = 0.0f;
            if (c < nc) {
                u32 raw = 0;
                for (u32 k = 0; k < cs; k++) raw = raw << 8 | e[c * cs + k];
                switch (type) {
                case GX_F32: f = std::bit_cast<float>(raw); break;
                case GX_S16: f = float(s16(raw)) * scale; break;
                case GX_U16: f = float(u16(raw)) * scale; break;
                case GX_S8: f = float(s8(raw)) * scale; break;
                default: f = float(u8(raw)) * scale; break;
                }
            }
            if (!std::isfinite(f)) return;
            if (!sBoxAny) sBoxLo[c] = sBoxHi[c] = f;
            sBoxLo[c] = std::min(sBoxLo[c], f), sBoxHi[c] = std::max(sBoxHi[c], f);
            if (pnIdx) {
                const u32 slot = v[i * vtxSize] / 3;
                if (slot < 10) {
                    if (!(sSlotAny & (1u << slot))) sSlotLo[slot][c] = sSlotHi[slot][c] = f;
                    sSlotLo[slot][c] = std::min(sSlotLo[slot][c], f), sSlotHi[slot][c] = std::max(sSlotHi[slot][c], f);
                    if (c == 2) sSlotAny |= u16(1u << slot);
                }
            }
        }
        sBoxAny = true;
    }
}

// The indexed attributes of `count` vertices at `v`: extents of the arrays they read.
void scanVertices(int fmt, const u8* v, u32 count, u32 vtxSize) {
    sScanFmt = fmt;
    const VtxLayout& l = layout(fmt);
    u32 offset = 0;
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) {
        const int s = l.size[a];
        const u8 type = l.desc[a];
        if ((type == GX_INDEX8 || type == GX_INDEX16) && a >= GX_VA_POS) {
            const u32 idxSize = type == GX_INDEX16 ? 2 : 1;
            const u32 skip = type == GX_INDEX16 ? 0xFFFF : 0xFF;
            u32 maxIdx = 0, minIdx = ~0u;
            bool any = false;
            for (u32 i = 0; i < count; i++) {
                const u8* p = v + i * vtxSize + offset;
                for (u32 k = 0; k < u32(s); k += idxSize) {
                    const u32 idx = idxSize == 2 ? u32(p[k] << 8 | p[k + 1]) : p[k];
                    if (idx == skip) continue;
                    maxIdx = std::max(maxIdx, idx);
                    minIdx = std::min(minIdx, idx);
                    any = true;
                }
            }
            if (any && a == GX_VA_POS && sBoxOn && !sSnapParse) boxPositions(fmt, v, count, vtxSize, offset, idxSize);
            if (any) {
                const int slot = a - GX_VA_POS;
                // Up to one element past the last index start: 36 bytes covers any element
                // (an NBT normal of 9 floats) when the stride is smaller than the element; NBT3
                // indices may read two strides more (Aurora sizes arrays that way).
                use(slot, minIdx, maxIdx + 3, 36);
            }
        }
        offset += u32(s);
    }
}

// Parses GX commands in [p, p + size). Returns bytes consumed; stops early (returning
// how far it got) when a command is incomplete and `partial` is allowed, or on a
// command it can't size (counted in sUnparsed, returns size).
u32 parse(const u8* p, u32 size, bool partial) {
    u32 i = 0;
    auto be16 = [&](u32 o) { return u32(p[o] << 8 | p[o + 1]); };
    auto be32 = [&](u32 o) { return u32(p[o]) << 24 | u32(p[o + 1]) << 16 | u32(p[o + 2]) << 8 | p[o + 3]; };
    while (i < size) {
        const u8 op = p[i];
        u32 len = 0;
        if (op == 0x00 || op == 0x48) {  // NOP, INVAL_VTX
            len = 1;
        } else if (op == 0x08) {  // LOAD_CP_REG
            len = 6;
            if (i + len <= size) {
                const u8 reg = p[i + 1];
                if ((reg & 0xF0) >= 0x50 && (reg & 0xF0) <= 0xB0) sDlCpWrites++;  // VCD/VAT/arrays
            }
        } else if (op == 0x10) {  // LOAD_XF_REG
            if (i + 5 > size) {
                if (partial) return i;
                break;
            }
            len = 5 + 4 * (((be32(i + 1) >> 16) & 0xFFFF) + 1);
            if ((be32(i + 1) & 0xFFFF) < 0x600) {  // matrix memory, not noted for blending
                sStreamMtxLoads++;
                detail::xfMatrixWritten(be32(i + 1) & 0xFFFF, ((be32(i + 1) >> 16) & 0xFFFF) + 1);
            } else if (sParseTapeBase != ~0u && i + len <= size) {
                // Channel ambient/material colours (0x100A-0x100D) and light colours written by a
                // display list (material display lists): blended with its draw, like the BP colours.
                const u32 addr = be32(i + 1) & 0xFFFF, n = ((be32(i + 1) >> 16) & 0xFFFF) + 1;
                for (u32 k = 0; k < n; k++) {
                    const u32 a = addr + k;
                    if ((a >= 0x100A && a <= 0x100D) || (a >= 0x600 && a < 0x680 && (a & 0xF) == 3))
                        detail::noteDrawColor(sParseTapeBase + i + 5 + 4 * k, a, 3);
                }
            }
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {  // LOAD_INDX_A..D
            len = 5;
            if (i + len <= size) {
                const int slot = 12 + (op - 0x20) / 8;
                const u32 idx = be16(i + 1), n = (be16(i + 3) >> 12) + 1;
                use(slot, idx, idx, n * 4);
                sStreamIndexedLoads++;
                if (op == 0x20) detail::xfMatrixWritten(be16(i + 3) & 0xFFF, n);
            }
        } else if (op == 0x40) {  // CALL_DL (Aurora ignores nested calls)
            len = 9;
        } else if (op == 0x61) {  // LOAD_BP_REG
            len = 5;
            // TEV color/konst and fog color registers set by a display list: blended with its draw.
            if (sParseTapeBase != ~0u && i + len <= size) {
                const u32 reg = p[i + 1];
                if (reg >= 0xE0 && reg <= 0xE7) detail::noteDrawColor(sParseTapeBase + i + 1, reg, 1);
                else if (reg == 0xF2) detail::noteDrawColor(sParseTapeBase + i + 1, reg, 2);
            }
        } else if (op >= 0x80 && op <= 0xBF) {  // draw
            if (i + 3 > size) {
                if (partial) return i;
                break;
            }
            const int fmt = op & 7;
            sFmtSeen |= 1u << fmt;
            const u32 vs = vertexSize(fmt);
            const u32 count = be16(i + 1);
            if (vs == 0 && count != 0) {
                sUnparsed++;
                return size;
            }
            len = 3 + count * vs;
            if (i + len <= size) scanVertices(fmt, p + i + 3, count, vs);
        } else if (op == GX_AURORA) {  // only in display lists the game recorded through Aurora
            sAuroraInDl++;
            sUnparsed++;
            return size;
        } else {
            sUnparsed++;
            sBadOp = op | (partial ? 0x100 : 0) | (i << 12);
            static int dumped = 0;
            if (std::getenv("LM_INTERP_TRACE") && dumped++ < 3) {
                char buf[400];
                int n = 0;
                for (u32 k = 0; k < size && k < 96 && n < 380; k++) n += std::snprintf(buf + n, sizeof(buf) - n, "%02X", p[k]);
                log("[interp] unparsed %s stream at +%u: %s", partial ? "raw" : "list", i, buf);
            }
            return size;
        }
        if (i + len > size) {
            if (partial) return i;
            sUnparsed++;
            return size;
        }
        i += len;
    }
    return i;
}

// The open immediate-mode draw with direct float positions (vtxBegin).
struct {
    bool active = false;
    u32 at = ~0u, bytes = 0, count = 0, stride = 0, posOffset = 0;
    u64 id = 0;
    u32 clrOffset = 0xFFFF, clrBytes = 0;  // a direct RGB8/RGBX8/RGBA8 colour (CLR0) in each vertex
    u32 posBytes2D = 0;  // a 2D quad (orthographic, direct positions of this size, direct colours)
    u32 posFmt2D = 0, uvOffset = 0xFFFF, uvBytes = 0;  // its position format and direct texture coordinates
    s32 proj = -1;  // the projection current when its data was written (widescreen emits it lazily)
} sDirect;

// The immediate-mode draw opened by GXBegin: its vertices are scanned once complete, with the
// vertex layout as it was at GXBegin (the game changes it right after GXEnd, so parsing the
// stream later would size the vertices wrongly).
struct {
    bool active = false;
    int fmt = 0;
    u32 count = 0, size = 0;
    std::vector<u8> bytes;
} sImm;

void finishDirect() {
    sImm.active = false;
    if (!sDirect.active) return;
    sDirect.active = false;
    if (sDirect.at != ~0u && sDirect.stride && sDirect.bytes == sDirect.count * sDirect.stride)
        detail::noteVertices(sDirect.at, sDirect.count, sDirect.stride, sDirect.posOffset, sDirect.id, sDirect.clrOffset, sDirect.clrBytes,
                             sDirect.posBytes2D, sDirect.posFmt2D, sDirect.uvOffset, sDirect.uvBytes, sDirect.proj);
}

void flushPending() {
    if (sPendingPos >= sPending.size()) {
        sPending.clear();
        sPendingPos = 0;
        return;
    }
    sPendingPos += parse(sPending.data() + sPendingPos, u32(sPending.size() - sPendingPos), true);
    if (sPendingPos == sPending.size() || sPendingPos > (1u << 20)) {
        sPending.erase(sPending.begin(), sPending.begin() + sPendingPos);
        sPendingPos = 0;
    }
}

}  // namespace

void arrayBase(u32 at, u32 attr, u32 guestAddr, u32 stride) {
    // GX_VA_NBT shares the normal array slot (as on the hardware and in Aurora's GXSetArray).
    const int slot = attr == GX_VA_NBT ? GX_VA_NRM - GX_VA_POS : int(attr) - GX_VA_POS;
    if (slot < 0 || slot >= kArrays) return;
    Binding& b = sBind[slot];
    b.guest = guestAddr;
    b.stride = stride;
    b.key = slot == 0 && capturing() ? detail::arrayKey() : 0;
    b.valid = guestAddr != 0;
    b.cmd = -1;
    sSnapBound[slot] = nullptr;  // Aurora now reads the live guest array
    if (capturing() && at != ~0u && b.valid) {
        b.cmd = int(sCmds.size());
        sCmds.push_back({at, -1, b.key, -1, stride});
    }
}

void invalidateVtxCache() {
    sSnapCopyOf.clear();  // draw-time snapshots: the game rewrote vertex data, later draws copy again
    if (!capturing()) return;
    finishDirect();
    flushPending();
    sSnapOf.clear();  // the game rewrote vertex data: later draws get new copies
    for (int s = 0; s < kArrays; s++)
        if (sBind[s].valid && sBind[s].cmd >= 0 && sCmds[sBind[s].cmd].snap >= 0) emitLoad(s);
}

static u32 sDrawCount = 0;
u32 drawCount() { return sDrawCount; }

void vtxBegin(u32 prim, u32 fmt, u32 nVerts, bool perspective) {
    sDrawCount++;
    finishDirect();
    flushPending();
    fmt &= 7;
    // What is drawn: primitive, vertex layout and the position array (if positions are indexed:
    // with direct positions the bound array is whatever the last indexed draw left, which changes
    // from frame to frame, and then no draw of this kind ever found its partner; from the SMS port).
    const bool posIndexed = gx::vtxDesc(GX_VA_POS) == GX_INDEX8 || gx::vtxDesc(GX_VA_POS) == GX_INDEX16;
    u64 id = u64(prim & 0xF8 | fmt) << 56 ^ u64(vertexSize(int(fmt))) << 40 ^ (posIndexed ? sBind[0].guest : 0);
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) id = detail::mix(id ^ gx::vtxDesc(a));
    detail::noteDraw(id);
    detail::drawStart(mark());
    detail::drawLog("begin prim %02X fmt %u n %u vsize %u %s pos desc %u size %d tex0 desc %u size %d clr0 desc %u",
                    prim & 0xF8, fmt, nVerts, vertexSize(int(fmt)), perspective ? "persp" : "ortho", gx::vtxDesc(GX_VA_POS),
                    gx::vtxAttrSize(int(fmt), GX_VA_POS), gx::vtxDesc(GX_VA_TEX0), gx::vtxAttrSize(int(fmt), GX_VA_TEX0),
                    gx::vtxDesc(GX_VA_CLR0));
    // CPU-built 3D geometry with direct float positions: blend the positions too.
    if (perspective && gx::vtxDesc(GX_VA_POS) == GX_DIRECT && gx::vtxAttrSize(int(fmt), GX_VA_POS) == 12) {
        u32 posOffset = 0;
        for (int a = GX_VA_PNMTXIDX; a < GX_VA_POS; a++) posOffset += u32(std::max(0, gx::vtxAttrSize(int(fmt), a)));
        sDirect = {true, ~0u, 0, nVerts, vertexSize(int(fmt)), posOffset, detail::mix(id ^ nVerts)};
        if (gx::vtxDesc(GX_VA_CLR0) == GX_DIRECT) {
            u8 cnt, type, frac;
            if (gx::vtxAttrFormat(int(fmt), GX_VA_CLR0, cnt, type, frac) && (type == GX_RGB8 || type == GX_RGBX8 || type == GX_RGBA8)) {
                u32 off = 0;
                for (int a = GX_VA_PNMTXIDX; a < GX_VA_CLR0; a++) off += u32(std::max(0, gx::vtxAttrSize(int(fmt), a)));
                sDirect.clrOffset = off;
                sDirect.clrBytes = type == GX_RGBA8 ? 4 : 3;
            }
        }
        detail::vertsBegin();
    } else if (!perspective && gx::vtxDesc(GX_VA_POS) == GX_DIRECT && gx::vtxDesc(GX_VA_CLR0) == GX_DIRECT &&
               gx::vtxAttrSize(int(fmt), GX_VA_POS) > 0 && nVerts <= 64) {
        // A 2D quad with direct vertex colours (HUD fades, full-screen fade pictures): its colours
        // blend while its positions stay put (the HUD fading in with the room on arrival stepped at
        // 30 Hz, sweep C); a picture whose layout animates (a message window opening) blends its
        // positions too (replayPair). The texture it shows is part of what is drawn.
        u8 cnt, type, frac;
        if (gx::vtxAttrFormat(int(fmt), GX_VA_CLR0, cnt, type, frac) && (type == GX_RGB8 || type == GX_RGBX8 || type == GX_RGBA8)) {
            u32 posOffset = 0, off = 0;
            for (int a = GX_VA_PNMTXIDX; a < GX_VA_POS; a++) posOffset += u32(std::max(0, gx::vtxAttrSize(int(fmt), a)));
            for (int a = GX_VA_PNMTXIDX; a < GX_VA_CLR0; a++) off += u32(std::max(0, gx::vtxAttrSize(int(fmt), a)));
            const bool tex0 = gx::vtxDesc(GX_VA_TEX0) == GX_DIRECT && gx::vtxAttrSize(int(fmt), GX_VA_TEX0) > 0;
            const u64 texId = tex0 ? u64(boundTexture(0)) << 8 : 0;
            sDirect = {true, ~0u, 0, nVerts, vertexSize(int(fmt)), posOffset, detail::mix(id ^ nVerts ^ 0x2D00ull ^ texId)};
            sDirect.clrOffset = off;
            sDirect.clrBytes = type == GX_RGBA8 ? 4 : 3;
            sDirect.posBytes2D = u32(gx::vtxAttrSize(int(fmt), GX_VA_POS));
            u8 pc, pt, pf;
            if (gx::vtxAttrFormat(int(fmt), GX_VA_POS, pc, pt, pf) && pt <= GX_F32) sDirect.posFmt2D = 0x100u | pt | u32(pf & 31) << 3;
            if (tex0) {
                u32 uv = 0;
                for (int a = GX_VA_PNMTXIDX; a < GX_VA_TEX0; a++) uv += u32(std::max(0, gx::vtxAttrSize(int(fmt), a)));
                sDirect.uvOffset = uv;
                sDirect.uvBytes = u32(gx::vtxAttrSize(int(fmt), GX_VA_TEX0));
            }
            detail::vertsBegin(true);
        }
    }
    const u32 vs = vertexSize(int(fmt));
    sImm.active = vs != 0 && nVerts != 0;
    sImm.fmt = int(fmt);
    sImm.count = nVerts;
    sImm.size = vs;
    sImm.bytes.clear();
    if (!sImm.active && nVerts != 0) sUnparsed++;
}

void vtxBytes(u32 v, int size) {
    if (sDirect.active) {
        // Called after the bytes were written (hle_gx fifoWrite): a draw's first FIFO write can emit
        // its projection first (widescreen's prepareDraw), so the mark taken before it pointed there.
        if (sDirect.at == ~0u) sDirect.at = mark() - u32(size), sDirect.proj = detail::currentProjection();
        sDirect.bytes += u32(size);
    }
    if (sImm.active) {
        for (int b = size - 1; b >= 0; b--) sImm.bytes.push_back(u8(v >> (8 * b)));
        if (sImm.bytes.size() >= size_t(sImm.count) * sImm.size) {
            scanVertices(sImm.fmt, sImm.bytes.data(), sImm.count, sImm.size);
            sImm.active = false;
        }
        return;
    }
    for (int b = size - 1; b >= 0; b--) sPending.push_back(u8(v >> (8 * b)));
    if (sPending.size() - sPendingPos >= 4096) flushPending();
}

void vtxDataAt(u32 at) {
    if (sDirect.active && at != ~0u) sDirect.at = at, sDirect.proj = detail::currentProjection();
}

void vtxDisplayList(u32 guestAddr, const u8* data, u32 size) {
    sDrawCount++;
    finishDirect();
    flushPending();
    detail::noteDraw(detail::mix(u64(guestAddr) << 24 ^ size ^ 0xD15Bull << 48));
    {
        // Content identity (FNV-1a over 8-byte words): the same mesh wherever its list is stored.
        u64 h = 0xCBF29CE484222325ull ^ size;
        u32 i = 0;
        for (; i + 8 <= size; i += 8) {
            u64 w;
            std::memcpy(&w, data + i, 8);
            h = (h ^ w) * 0x100000001B3ull;
        }
        for (; i < size; i++) h = (h ^ data[i]) * 0x100000001B3ull;
        detail::noteDrawAlt(h);
    }
    detail::drawLog("dlist %08X size %u", guestAddr, size);
    const u32 end = mark();
    sParseTapeBase = end != ~0u && end >= size ? end - size : ~0u;
    if (sParseTapeBase != ~0u) detail::drawRange(sParseTapeBase, end);
    sBoxOn = true;
    sBoxAny = false;
    sSlotAny = 0;
    parse(data, size, false);
    sBoxOn = false;
    if (sBoxAny) {
        const f32 c[3] = {0.5f * (sBoxLo[0] + sBoxHi[0]), 0.5f * (sBoxLo[1] + sBoxHi[1]), 0.5f * (sBoxLo[2] + sBoxHi[2])};
        detail::noteDrawCentre(c);
    }
    if (sSlotAny) {
        f32 c[10][3];
        for (int s = 0; s < 10; s++)
            for (int k = 0; k < 3; k++) c[s][k] = (sSlotAny & (1u << s)) ? 0.5f * (sSlotLo[s][k] + sSlotHi[s][k]) : 0.0f;
        detail::noteDrawSlotCentres(c, sSlotAny);
    }
    sParseTapeBase = ~0u;
}

namespace detail {

void vtxBeginTape() {
    sCmds.clear();
    sSnaps.clear();
    sSnapOf.clear();
    sPending.clear();
    sPendingPos = 0;
    sDirect.active = false;
    sImm.active = false;
    // Self-contained tapes: load the arrays the guest has bound now.
    for (int s = 0; s < kArrays; s++) {
        sBind[s].cmd = -1;
        if (sBind[s].valid) emitLoad(s);
    }
}

void vtxEndTape(u8* tape, u32 size, std::vector<std::vector<u8>>& keep, u64& snapshotBytes, std::vector<ArrayRec>& recs) {
    finishDirect();
    flushPending();
    recs.clear();
    keep.clear();
    keep.reserve(sSnaps.size());
    snapshotBytes = 0;
    for (Snap& s : sSnaps) {
        snapshotBytes += s.data.size();
        keep.push_back(std::move(s.data));
    }
    // LM_INTERP_ARRAYS=0 (testing): leave the array loads pointing at live guest memory.
    static const bool sOff = [] { const char* v = std::getenv("LM_INTERP_ARRAYS"); return v && *v == '0'; }();
    for (const Cmd& c : sCmds) {
        if (c.snap < 0 || sOff) continue;
        const std::vector<u8>& d = keep[c.snap];
        if (d.empty()) continue;
        // The load is GX_AURORA, u16 subcommand, u64 host pointer (big-endian), u32 size, u8.
        const u64 want = u64(reinterpret_cast<uintptr_t>(hostp(sSnaps[c.snap].guest)));
        for (u32 i = c.at; i + 12 <= size && i < c.at + 16; i++) {
            u64 v = 0;
            for (int k = 0; k < 8; k++) v = v << 8 | tape[i + k];
            if (v != want) continue;
            const u64 ptr = u64(reinterpret_cast<uintptr_t>(d.data()));
            const u32 len = u32(d.size());
            for (int k = 0; k < 8; k++) tape[i + k] = u8(ptr >> (56 - 8 * k));
            for (int k = 0; k < 4; k++) tape[i + 8 + k] = u8(len >> (24 - 8 * k));
            if (c.key && c.fmt >= 0 && c.posType <= GX_F32)
                recs.push_back({c.key, u32(c.snap), i, u16(c.stride), c.posCnt, c.posType, c.posFrac});
            break;
        }
    }
    sCmds.clear();
    sSnaps.clear();
    sSnapOf.clear();
    if (sUnparsed || sDlCpWrites || sStreamMtxLoads || sStreamIndexedLoads) {
        static u32 logged = 0;
        if (logged++ < 20)
            log("[interp] vertex scan: %u unparsed streams (%u with Aurora commands), %u CP layout writes, %u matrix "
                "loads and %u indexed loads in lists/raw writes (last bad op %X)",
                sUnparsed, sAuroraInDl, sDlCpWrites, sStreamMtxLoads, sStreamIndexedLoads, sBadOp);
        sUnparsed = sDlCpWrites = sAuroraInDl = sStreamMtxLoads = sStreamIndexedLoads = 0;
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------------
// Draw-time snapshots without a tape (interp.h, docs/RENDERING.md "Vertex snapshots").
//
// At 30 fps the guest writes straight into Aurora's FIFO and Aurora's FIFO thread reads the
// guest arrays a draw indexes when it processes the draw. The console's GPU reads them
// while the CPU waits nowhere either, but games time their buffer reuse against the real
// GPU; a FIFO thread that lags differently (a pipeline compile, a big upload, the OS
// scheduler) can see a buffer the CPU already rewrote for a later draw (Dolphin's dual-core
// mode has the same race in this game: Luigi's mouth, Dolphin issue 12903). Here every
// draw's indexed ranges are copied when the draw is issued and Aurora is pointed at the
// copies, so the processing time no longer matters. Copies are shared per guest address
// until GXInvalidateVtxCache (as for tapes) and live for three frames (lockstep: the host
// frame that processed them has finished by then).
// ---------------------------------------------------------------------------------
}  // namespace recomp::interp

namespace aurora::gx::fifo {
uint64_t stream_position() noexcept;
uint64_t processed_position() noexcept;
uint64_t published_position() noexcept;
uint32_t get_buffer_size();
const uint8_t* get_buffer_data();
void patch_u32(uint32_t offset, uint32_t val);
bool in_display_list();
}  // namespace aurora::gx::fifo

#include "port/settings.h"

#include <chrono>
#include <deque>
#include <memory>

namespace recomp::interp {
namespace {
namespace fifo = aurora::gx::fifo;

int envFlag(const char* name) {
    const char* e = std::getenv(name);
    return e && *e ? (*e == '0' ? 0 : 1) : -1;
}
const int sSnapEnv = envFlag("LM_VTX_SNAP");  // overrides the F1 setting for testing
const bool sRaceOn = envFlag("LM_VTX_RACE") == 1;
const bool sSnapLog = sRaceOn || envFlag("LM_VTX_SNAP_LOG") == 1;
bool sSnapEnabled = sSnapEnv >= 0 ? sSnapEnv != 0 : true;

// Bump arena per frame generation; chunks are kept and reused.
struct Gen {
    std::vector<std::unique_ptr<u8[]>> chunks;  // kChunk bytes each
    std::vector<std::unique_ptr<u8[]>> big;     // larger copies, freed when the generation is reused
    size_t chunk = 0;
    u32 used = 0;
};
constexpr u32 kChunk = 1u << 20;
constexpr int kGens = 3;
Gen sGens[kGens];
int sGen = 0;

u8* arenaAlloc(u32 n) {
    Gen& g = sGens[sGen];
    n = (n + 31) & ~31u;
    if (n > kChunk / 4) {
        g.big.push_back(std::make_unique<u8[]>(n));
        return g.big.back().get();
    }
    if (g.chunk >= g.chunks.size() || g.used + n > kChunk) {
        if (g.chunk < g.chunks.size()) g.chunk++;
        if (g.chunk >= g.chunks.size()) g.chunks.push_back(std::make_unique<u8[]>(kChunk));
        g.used = 0;
    }
    u8* p = g.chunks[g.chunk].get() + g.used;
    g.used += n;
    return p;
}

// Stats (LM_VTX_SNAP_LOG=1 / LM_VTX_RACE=1), per log period.
u64 sStatBytes = 0, sStatNs = 0;
u32 sStatDraws = 0, sStatLiveDraws = 0, sStatLoads = 0, sStatFrames = 0;
u64 sRaceHits = 0, sRaceTotal = 0;
u32 sFrameNo = 0;

// A copy of what `slot` reads up to `need` bytes, as guest memory is now; nullptr if the
// array is not in main memory.
const u8* snapCopy(int slot, u32 need) {
    const Binding& b = sBind[slot];
    if (!b.valid || !inMem1(b.guest)) return nullptr;
    const u32 phys = b.guest & 0x01FFFFFF;
    if (phys >= kMem1Size) return nullptr;
    need = std::min(need, kMem1Size - phys);
    SnapCopy& c = sSnapCopyOf[b.guest];
    if (c.ptr && need <= c.len) return c.ptr;
    // A new block (never grown in place: earlier draws may still be read from the old one).
    // Capacity is the power of two Aurora rounds a size-0 array's upload to, so its upload
    // stays inside the block.
    u32 cap = 256;
    while (cap < need) cap *= 2;
    u8* p = arenaAlloc(cap);
    std::memcpy(p, hostp(b.guest), need);
    sStatBytes += need;
    c = {p, need};
    return p;
}

void bindCopy(int slot, const u8* p) {
    if (sSnapBound[slot] == p) return;
    GXSetArray(GXAttr(GX_VA_POS + slot), p, 0, u8(sBind[slot].stride), false);
    sSnapBound[slot] = p;
    sStatLoads++;
}

// Points every slot that has a copy bound back at the live guest array (before a draw this
// code could not parse, so it never reads past a copy).
void unbindAll() {
    for (int s = 0; s < kArrays; s++) {
        if (!sSnapBound[s]) continue;
        GXSetArray(GXAttr(GX_VA_POS + s), sBind[s].valid ? hostp(sBind[s].guest) : nullptr, 0, u8(sBind[s].stride),
                   false);
        sSnapBound[s] = nullptr;
    }
}

// Race detector: array ranges that draws read live, until Aurora has processed the draw.
struct Pending {
    u32 lo, hi;
    u64 pos;
};
std::deque<Pending> sRacePend;
std::vector<std::pair<u32, u32>> sDrawLive;  // this draw's live ranges (phys)
u32 sPendLo = ~0u, sPendHi = 0;

// Bytes [0, n) of the array in `slot` a need covers.
u32 needBytes(int slot, const Need& n) { return n.end * sBind[slot].stride + n.extra; }

void noteLive(int slot, const Need& n) {
    const Binding& b = sBind[slot];
    if (!b.valid || !inMem1(b.guest)) return;
    const u32 base = b.guest & 0x01FFFFFF;
    sDrawLive.push_back({base + n.lo * b.stride, base + needBytes(slot, n)});
}

void beginNeeds() {
    std::memset(sNeed, 0, sizeof(sNeed));
    sFmtSeen = 0;
    sDrawLive.clear();
}

// Parsed display lists: what each one indexes, in elements (independent of the bound arrays).
// Display lists are mostly static model data; an entry is reused while the list's size, its
// first and last bytes and the vertex layout of the formats it uses are unchanged.
struct DlEntry {
    u64 check = 0;
    u32 fmtMask = 0;
    bool unparsed = false;
    Need need[kArrays];
};
struct DlSlot {
    static constexpr u32 kVariants = 4;
    DlEntry v[kVariants];
    u32 count = 0, next = 0;
};
std::unordered_map<u64, DlSlot> sDlCache;
u32 sStatCacheHits = 0;

u64 dlCheck(const u8* data, u32 size, u32 fmtMask) {
    u64 h = 0xCBF29CE484222325ull ^ size;
    auto mixByte = [&](u8 b) { h = (h ^ b) * 0x100000001B3ull; };
    for (u32 i = 0; i < size && i < 64; i++) mixByte(data[i]);
    for (u32 i = size > 64 ? std::max(64u, size - 64) : size; i < size; i++) mixByte(data[i]);
    const VtxLayout& l0 = layout(0);  // the desc bytes are per attribute, not per format
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) mixByte(l0.desc[a]);
    for (int f = 0; f < 8; f++) {
        if (!(fmtMask >> f & 1)) continue;
        const VtxLayout& l = layout(f);
        for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) mixByte(u8(l.size[a] + 1));
    }
    return h;
}

// Immediate-mode draw in progress (snapBegin..snapEnd).
struct {
    bool active = false;
    int fmt = 0;
    u32 count = 0, size = 0;
    std::vector<u8> bytes;
    u32 at[kArrays];      // FIFO buffer offset of the placeholder array load, ~0u: none
    u64 streamAt = 0;     // stream position at snapBegin (placeholders must still be unpublished)
} sImmS;

void patchPointer(u32 at, const void* from, const void* to) {
    const u8* buf = fifo::get_buffer_data();
    const u32 end = fifo::get_buffer_size();
    const u64 want = u64(reinterpret_cast<uintptr_t>(from));
    for (u32 i = at; i + 8 <= end && i < at + 16; i++) {
        u64 v = 0;
        for (int k = 0; k < 8; k++) v = v << 8 | buf[i + k];
        if (v != want) continue;
        const u64 ptr = u64(reinterpret_cast<uintptr_t>(to));
        fifo::patch_u32(i, u32(ptr >> 32));
        fifo::patch_u32(i + 4, u32(ptr));
        return;
    }
}

}  // namespace

bool snapActive() { return sSnapEnabled && !capturing() && !fifo::in_display_list(); }
bool raceDetectorOn() { return sRaceOn; }

void snapDisplayList(const u8* data, u32 size) {
    const bool snap = snapActive();
    if (!snap && !sRaceOn) return;
    if (capturing() || fifo::in_display_list()) return;
    const auto t0 = std::chrono::steady_clock::now();
    beginNeeds();
    const u64 key = u64(reinterpret_cast<uintptr_t>(data)) ^ u64(size) << 48;
    DlEntry* e = nullptr;
    DlSlot* slot = nullptr;
    if (auto it = sDlCache.find(key); it != sDlCache.end()) {
        slot = &it->second;
        // The same list is drawn with several vertex layouts (e.g. shadow and main pass).
        u32 lastMask = ~0u;
        u64 check = 0;
        for (u32 i = 0; i < slot->count && !e; i++) {
            if (slot->v[i].fmtMask != lastMask) check = dlCheck(data, size, lastMask = slot->v[i].fmtMask);
            if (slot->v[i].check == check) e = &slot->v[i];
        }
    }
    if (e) {
        sStatCacheHits++;
    } else {
        const u32 unparsed = sUnparsed;
        sSnapParse = true;
        parse(data, size, false);
        sSnapParse = false;
        if (!slot) {
            if (sDlCache.size() > 50000) sDlCache.clear();
            slot = &sDlCache[key];
        }
        e = &slot->v[slot->count < DlSlot::kVariants ? slot->count++ : slot->next++ % DlSlot::kVariants];
        e->unparsed = sUnparsed != unparsed;
        sUnparsed = unparsed;
        e->fmtMask = sFmtSeen;
        std::memcpy(e->need, sNeed, sizeof(sNeed));
        e->check = dlCheck(data, size, e->fmtMask);
    }
    if (e->unparsed) {
        // Unknown contents (e.g. a list recorded through Aurora): it reads the live arrays.
        if (snap) unbindAll();
        for (int s = 0; s < kArrays; s++)
            if (sBind[s].valid) noteLive(s, Need{0, 0x10000 / std::max(sBind[s].stride, 1u), 0});
        sStatLiveDraws++;
    } else {
        bool live = false;
        for (int s = 0; s < kArrays; s++) {
            const Need& n = e->need[s];
            if (!n.end) continue;
            const u8* p = snap ? snapCopy(s, needBytes(s, n)) : nullptr;
            if (p) {
                bindCopy(s, p);
            } else {
                if (sSnapBound[s]) {  // not copyable: back to live
                    GXSetArray(GXAttr(GX_VA_POS + s), hostp(sBind[s].guest), 0, u8(sBind[s].stride), false);
                    sSnapBound[s] = nullptr;
                }
                noteLive(s, n);
                live = true;
            }
        }
        if (live) sStatLiveDraws++;
    }
    sStatDraws++;
    sStatNs += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

void snapBegin(u32 fmt, u32 nVerts) {
    sImmS.active = false;
    const bool snap = snapActive();
    if (!snap && !sRaceOn) return;
    if (capturing() || fifo::in_display_list()) return;
    fmt &= 7;
    const u32 vs = vertexSize(int(fmt));
    bool indexed = false;
    for (int a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
        const u8 t = gx::vtxDesc(a);
        sImmS.at[a - GX_VA_POS] = ~0u;
        if (t != GX_INDEX8 && t != GX_INDEX16) continue;
        indexed = true;
        const int s = a - GX_VA_POS;
        const int slot = a == GX_VA_NBT ? GX_VA_NRM - GX_VA_POS : s;
        if (!snap || slot != s) continue;
        // Placeholder load of the live array, re-pointed at the copy once the indices are known.
        if (!sBind[slot].valid || !inMem1(sBind[slot].guest) || vs == 0) {
            if (sSnapBound[slot]) {
                GXSetArray(GXAttr(a), hostp(sBind[slot].guest), 0, u8(sBind[slot].stride), false);
                sSnapBound[slot] = nullptr;
            }
            continue;
        }
        sImmS.at[slot] = fifo::get_buffer_size();
        GXSetArray(GXAttr(a), hostp(sBind[slot].guest), 0, u8(sBind[slot].stride), false);
        sSnapBound[slot] = nullptr;
    }
    if (!indexed) return;
    sImmS.active = vs != 0 && nVerts != 0;
    sImmS.fmt = int(fmt);
    sImmS.count = nVerts;
    sImmS.size = vs;
    sImmS.bytes.clear();
    sImmS.streamAt = fifo::stream_position();
}

void snapBytes(u32 v, int size) {
    if (!sImmS.active) return;
    for (int b = size - 1; b >= 0; b--) sImmS.bytes.push_back(u8(v >> (8 * b)));
}

void snapEnd() {
    if (!sImmS.active) return;
    sImmS.active = false;
    const auto t0 = std::chrono::steady_clock::now();
    beginNeeds();
    if (sImmS.bytes.size() < size_t(sImmS.count) * sImmS.size) return;
    sSnapParse = true;
    scanVertices(sImmS.fmt, sImmS.bytes.data(), sImmS.count, sImmS.size);
    sSnapParse = false;
    // Placeholders are patchable only while nothing after them was published.
    const u64 base = fifo::stream_position() - fifo::get_buffer_size();
    bool live = false;
    for (int s = 0; s < kArrays; s++) {
        if (!sNeed[s].end) continue;
        const u32 at = sImmS.at[s];
        const u8* p = nullptr;
        if (at != ~0u && base + at >= fifo::published_position()) p = snapCopy(s, needBytes(s, sNeed[s]));
        if (p) {
            patchPointer(at, hostp(sBind[s].guest), p);
            sSnapBound[s] = p;
        } else {
            noteLive(s, sNeed[s]);
            live = true;
        }
    }
    if (live) sStatLiveDraws++;
    sStatDraws++;
    sStatNs += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

void snapAfterDraw() {
    if (!sRaceOn || sDrawLive.empty()) return;
    const u64 pos = fifo::stream_position();
    for (const auto& [lo, hi] : sDrawLive) {
        sRacePend.push_back({lo, hi, pos});
        sPendLo = std::min(sPendLo, lo);
        sPendHi = std::max(sPendHi, hi);
    }
    sDrawLive.clear();
}

bool raceStore(u32 addr, int size) {
    if (sRacePend.empty()) return false;
    const u32 lo = addr & 0x01FFFFFF, hi = lo + u32(size);
    if (hi <= sPendLo || lo >= sPendHi) return false;
    const u64 done = fifo::processed_position();
    while (!sRacePend.empty() && sRacePend.front().pos <= done) sRacePend.pop_front();
    if (sRacePend.empty()) {
        sPendLo = ~0u;
        sPendHi = 0;
        return false;
    }
    for (const Pending& p : sRacePend) {
        if (hi > p.lo && lo < p.hi) {
            sRaceHits++;
            if (sRaceHits > 40) return false;  // log the first ones per period
            log("[vtxrace] frame %u: store to %08X, read by a draw %llu bytes ahead of Aurora (range %08X-%08X)",
                sFrameNo, addr, (unsigned long long)(p.pos - done), p.lo | 0x80000000u, p.hi | 0x80000000u);
            return true;
        }
    }
    return false;
}

void snapEndFrame() {
    sImmS.active = false;
    if (sSnapEnv < 0) sSnapEnabled = port::settings().rendering.vtxSnapshot;
    // Leave Aurora on the live arrays between frames: nothing outside this frame reads a copy.
    if (capturing()) {
        std::fill(std::begin(sSnapBound), std::end(sSnapBound), nullptr);
    } else if (!fifo::in_display_list()) {
        unbindAll();
    }
    sSnapCopyOf.clear();
    sGen = (sGen + 1) % kGens;
    Gen& g = sGens[sGen];
    g.big.clear();
    g.chunk = 0;
    g.used = 0;
    sStatFrames++;
    sFrameNo++;
    if (sSnapLog && sStatFrames >= 300) {
        sRaceTotal += sRaceHits;
        log("[vtxsnap] %s: %u frames, %u draws (%u parsed, %u read live), %u loads, %.1f KiB/frame copied, %.3f ms/frame; "
            "stores into unprocessed live arrays: %llu (total %llu)",
            sSnapEnabled ? "on" : "off", sStatFrames, sStatDraws, sStatDraws - sStatCacheHits, sStatLiveDraws, sStatLoads,
            double(sStatBytes) / 1024.0 / sStatFrames, double(sStatNs) / 1e6 / sStatFrames,
            (unsigned long long)sRaceHits, (unsigned long long)sRaceTotal);
        sStatBytes = sStatNs = 0;
        sStatDraws = sStatLiveDraws = sStatLoads = sStatFrames = sStatCacheHits = 0;
        sRaceHits = 0;
    }
}

}  // namespace recomp::interp
