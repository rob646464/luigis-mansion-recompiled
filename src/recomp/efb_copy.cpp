// EFB copies to RAM (docs/RENDERING.md): runtime side of Aurora's efb_ram (patch 0007).
//  - F1 > Visuals > Rendering "EFB copies to RAM (accurate)" (LM_EFB_RAM=0/1 overrides).
//  - DCInvalidateRange over copied memory = the CPU is going to read it: copies there are
//    read back into guest memory.
//  - Finished readbacks are written into guest memory at each guest frame boundary.
//  - Test aids: LM_EFB_READBACK_ALL=1 (read back every copy), LM_COPY_LOG=1 (stats),
//    LM_FRAME_DUMP=<dir>,<first>,<last>[,<step>] (native-resolution EFB of those guest frames
//    as .ppm files, taken through the copy-to-RAM path).
#include "recomp/efb_copy.h"

#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/interp.h"

#include <aurora/aurora.h>
#include <aurora/efb_ram.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace recomp::efbcopy {
namespace {

int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}

const int sEnvEnabled = envInt("LM_EFB_RAM", -1);
const bool sLog = std::getenv("LM_COPY_LOG") != nullptr;
// Test aid LM_DUMP_EXIT=1: quit once the LM_SHOT or LM_FRAME_DUMP range is written (automated runs).
const bool sDumpExit = envInt("LM_DUMP_EXIT", 0) != 0;

// A named one-off dump (requestNamedDump): written by the next guest frame.
struct Named {
    std::string path;
    int shrink = 1;
    std::function<void(const u8* rgb, u32 w, u32 h)> done;
};

struct Dump {
    std::string dir;
    u32 first = 0, last = 0, step = 1;
    std::vector<u8> buf[4];  // ring of GX RGBA8 copy targets (host memory, outside MEM1)
    u32 frameOf[4] = {};
    Named named[4];          // per buffer: a named request instead of a frame number
    u32 next = 0;
    bool active = false;     // LM_FRAME_DUMP range
    bool buffers = false;
    bool namedPending = false;
    Named pending;
} sDump;

void ensureBuffers() {
    if (sDump.buffers) return;
    sDump.buffers = true;
    for (auto& b : sDump.buf) {
        b.resize(640 * 480 * 4);
        aurora_efb_ram_watch(b.data(), u32(b.size()));
    }
}

void writeNamed(const u8* gx, u32 w, u32 h, Named& n) {
    std::vector<u8> rgb(size_t(w) * h * 3);
    const u32 tilesPerRow = (w + 3) / 4;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u32 tile = (y / 4) * tilesPerRow + x / 4;
            const u32 t = (y % 4) * 4 + x % 4;
            const u8* p = gx + tile * 64 + t * 2;
            u8* o = &rgb[(size_t(y) * w + x) * 3];
            o[0] = p[1];
            o[1] = p[32];
            o[2] = p[33];
        }
    if (n.done) n.done(rgb.data(), w, h);
    if (!n.path.empty()) {
        const u32 k = n.shrink > 1 ? u32(n.shrink) : 1u, ow = w / k, oh = h / k;
        std::vector<u8> out(size_t(ow) * oh * 3);
        for (u32 y = 0; y < oh; y++)
            for (u32 x = 0; x < ow; x++)
                for (int c = 0; c < 3; c++) {
                    u32 sum = 0;
                    for (u32 j = 0; j < k; j++)
                        for (u32 i = 0; i < k; i++) sum += rgb[(size_t(y * k + j) * w + x * k + i) * 3 + c];
                    out[(size_t(y) * ow + x) * 3 + c] = u8(sum / (k * k));
                }
        if (FILE* f = std::fopen(n.path.c_str(), "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", ow, oh);
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
        }
    }
    n = {};
}

// Interpolation test dumps (interp.cpp, LM_INTERP_TEST): a ring of named copy targets.
struct NamedDump {
    std::vector<u8> buf;
    std::string name;
    bool written = true;
};
NamedDump sNamed[64];
u32 sNamedNext = 0;

void writePpmPath(const u8* gx, u32 w, u32 h, const char* path);

void writePpm(const u8* gx, u32 w, u32 h, u32 frameNo) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/f%05u.ppm", sDump.dir.c_str(), frameNo);
    writePpmPath(gx, w, h, path);
}

void writePpmPath(const u8* gx, u32 w, u32 h, const char* path) {
    std::vector<u8> rgb(size_t(w) * h * 3);
    const u32 tilesPerRow = (w + 3) / 4;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u32 tile = (y / 4) * tilesPerRow + x / 4;
            const u32 t = (y % 4) * 4 + x % 4;
            const u8* p = gx + tile * 64 + t * 2;
            u8* o = &rgb[(size_t(y) * w + x) * 3];
            o[0] = p[1];
            o[1] = p[32];
            o[2] = p[33];
        }
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
    }
}

void onWriteback(const void* dest, u32 size, u32, u32 w, u32 h, void*) {
    interp::efbWrittenBack(dest, size);
    if (!sDump.buffers && !sDump.active) return;
    for (auto& n : sNamed)
        if (!n.written && dest == n.buf.data()) {
            writePpmPath(n.buf.data(), w, h, (sDump.dir + "/" + n.name + ".ppm").c_str());
            n.written = true;
        }
    if (!sDump.buffers) return;
    for (int i = 0; i < 4; i++) {
        if (dest != sDump.buf[i].data()) continue;
        if (sDump.named[i].done || !sDump.named[i].path.empty()) writeNamed(sDump.buf[i].data(), w, h, sDump.named[i]);
        else if (sDump.active) writePpm(sDump.buf[i].data(), w, h, sDump.frameOf[i]);
    }
}

bool init() {
    if (const char* v = std::getenv("LM_FRAME_DUMP")) {
        std::string s = v;
        const auto c1 = s.find(',');
        if (c1 != std::string::npos) {
            sDump.dir = s.substr(0, c1);
            unsigned a = 0, b = 0, st = 1;
            const int n = std::sscanf(s.c_str() + c1 + 1, "%u,%u,%u", &a, &b, &st);
            if (n >= 2) {
                sDump.first = a;
                sDump.last = b;
                sDump.step = st ? st : 1;
                sDump.active = true;
                ensureBuffers();
                log("[efb] dumping guest frames %u..%u (every %u) to %s", a, b, sDump.step, sDump.dir.c_str());
            }
        }
    }
    aurora_efb_ram_set_writeback_callback(onWriteback, nullptr);
    aurora_efb_ram_set_readback_all(envInt("LM_EFB_READBACK_ALL", 0) != 0);
    return true;
}

}  // namespace

void onFrame(u32 frameNo) {
    static const bool sInit = init();
    (void)sInit;
    const bool want = sEnvEnabled >= 0 ? sEnvEnabled != 0 : port::settings().rendering.efbCopyToRam;
    if (want != aurora_efb_ram_enabled()) {
        aurora_efb_ram_set_enabled(want);
        log("[efb] copies to RAM %s", want ? "on" : "off");
    }
    aurora_efb_ram_apply();
    // Test aid LM_SHOT=<dir>,<first>,<last>[,<step>[,<count>]]: the game image at render resolution (before
    // post-processing and UI) of those guest frames (count consecutive ones every step; 2 with
    // LM_AO_AB gives off/on pairs) as <dir>/s<frame>.ppm (Aurora 0224). Taken at
    // the next presented frame, so with frame interpolation set 30 fps for exact game frames.
    static struct Shot {
        std::string dir;
        unsigned first = 0, last = 0, step = 1, count = 1;
        bool on = false;
    } sShot = [] {
        Shot s;
        if (const char* v = std::getenv("LM_SHOT")) {
            const std::string str = v;
            const auto c = str.find(',');
            if (c != std::string::npos &&
                std::sscanf(str.c_str() + c + 1, "%u,%u,%u,%u", &s.first, &s.last, &s.step, &s.count) >= 2) {
                s.dir = str.substr(0, c);
                s.step = s.step ? s.step : 1;
                s.on = true;
            }
        }
        return s;
    }();
    if (sShot.on && frameNo >= sShot.first && frameNo <= sShot.last && (frameNo - sShot.first) % sShot.step < sShot.count) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/s%05u.ppm", sShot.dir.c_str(), frameNo);
        aurora_request_screenshot(path);
    }
    if (sShot.on && sDumpExit && !sDump.active && frameNo > sShot.last + 10) {
        log("[efb] screenshot range done, exiting (LM_DUMP_EXIT)");
        std::fflush(nullptr);
        std::_Exit(0);
    }
    // Test aid LM_DUMP_EXIT=1: quit once the LM_FRAME_DUMP range is written (automated runs).
    if (sDumpExit && sDump.active && frameNo > sDump.last + 10) {
        log("[efb] frame dump range done, exiting (LM_DUMP_EXIT)");
        std::fflush(nullptr);
        std::_Exit(0);
    }
    if (sLog && frameNo % 300 == 0) {
        AuroraEfbRamStats s;
        aurora_efb_ram_get_stats(&s);
        log("[efb] f%u copies %u encodes %u decodes %u readbacks %u writebacks %u skipped %u (late %u) cpu-overwrites %u "
            "hashed %.1f MB cpu %.1f ms",
            frameNo, s.copies, s.encodes, s.decodes, s.readbacks, s.writebacks, s.skipped, s.staleSkipped, s.cpuOverwrites,
            double(s.hashedBytes) / 1048576.0, double(s.cpuNs) / 1e6);
    }
}

void cpuRead(u32 addr, u32 size) {
    if (size) aurora_efb_ram_cpu_read(hostp(addr), size);
}

// The game issued a texture copy to guest memory. Aurora executes it later on its own thread; with
// frame interpolation the first replay of the frame (the one that reads its copies back) can come
// several game frames later, more so when the CPU is busy. A readback used to land whenever the
// memory still matched what Aurora found when executing the copy, so a buffer the game had freed
// and reused in the meantime got the old copy written over its new contents (at a map change: the
// old map's 600 KB Z16 shadow map over the new map's heap data), corrupting game data that crashed
// the game later (docs/RENDERING.md "Late copy readbacks"). Now the check is against the memory as
// the game left it when it issued the copy.
void guestCopy(u32 addr, u32 width, u32 height, u32 fmt) {
    if (width && height) aurora_efb_ram_guest_copy(hostp(addr), width, height, fmt);
}

void requestNamedDump(const std::string& path, int shrink, std::function<void(const u8* rgb, u32 w, u32 h)> done) {
    ensureBuffers();
    sDump.pending = {path, shrink, std::move(done)};
    sDump.namedPending = true;
}

void* dumpTarget(u32 frameNo) {
    if (sDump.namedPending) {
        sDump.namedPending = false;
        const u32 i = sDump.next++ % 4;
        sDump.named[i] = std::move(sDump.pending);
        sDump.pending = {};
        return sDump.buf[i].data();
    }
    if (!sDump.active || frameNo < sDump.first || frameNo > sDump.last || (frameNo - sDump.first) % sDump.step) return nullptr;
    // Readbacks land a frame or two later: rotate through the buffers.
    const u32 i = sDump.next++ % 4;
    sDump.frameOf[i] = frameNo;
    return sDump.buf[i].data();
}

void* namedDumpTarget(const char* name) {
    if (!sDump.active) return nullptr;
    NamedDump& n = sNamed[sNamedNext++ % 64];
    if (n.buf.empty()) {
        n.buf.resize(640 * 480 * 4);
        aurora_efb_ram_watch(n.buf.data(), u32(n.buf.size()));
    }
    n.name = name;
    n.written = false;
    return n.buf.data();
}

bool namedDumpWritten(const void* target) {
    for (const auto& n : sNamed)
        if (n.buf.data() == target) return n.written;
    return true;
}

void applyWritebacks() { aurora_efb_ram_apply(); }

}  // namespace recomp::efbcopy
