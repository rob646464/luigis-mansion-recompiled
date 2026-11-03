#include "recomp/ctx.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"
#include "port/install.h"
#include "port/recovery.h"

#include <dolphin/gx.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace recomp {

const GameModule* g_module = nullptr;
u8* g_mem = nullptr;
bool g_traceOn = false;

#ifdef _WIN32
static bool sWriteWatch = false;
#endif

u8* allocGuestMemory() {
#ifdef _WIN32
    if (void* p = VirtualAlloc(nullptr, kHostMemSize, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE)) {
        sWriteWatch = true;
        return static_cast<u8*>(p);
    }
#endif
    return static_cast<u8*>(std::calloc(1, kHostMemSize));
}

void guestWrittenRangesImpl(void (*fn)(u32 offset, u32 len, void* user), void* user) {
#ifdef _WIN32
    if (sWriteWatch) {
        static std::vector<void*> sPages(kHostMemSize / 4096);
        ULONG_PTR count = sPages.size();
        ULONG granularity = 0;
        if (GetWriteWatch(0, g_mem, kHostMemSize, sPages.data(), &count, &granularity) == 0 && granularity) {
            u32 start = 0, end = 0;
            for (ULONG_PTR i = 0; i < count; i++) {
                const u32 off = u32(static_cast<u8*>(sPages[i]) - g_mem);
                if (end != 0 && off == end) {
                    end += granularity;
                    continue;
                }
                if (end != 0) fn(start, end - start, user);
                start = off;
                end = off + granularity;
            }
            if (end != 0) fn(start, end - start, user);
            return;
        }
    }
#endif
    fn(0, kHostMemSize, user);
}

// Instruction patch switches (hooks.txt "patch <id> ..."); see setPatch().
u8 g_patchStore[kMaxPatches];
u8* g_patches = g_patchStore;
void setPatch(u32 id, bool on) {
    if (id < kMaxPatches) g_patchStore[id] = on ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Tracing (LM_TRACE=all | ring | <hex addr>,<hex addr>...)
// ---------------------------------------------------------------------------

namespace {
enum class TraceMode { Off, Ring, Set, All };
TraceMode sTraceMode = TraceMode::Off;
std::unordered_set<u32> sTraceSet;
constexpr int kRingSize = 64;
thread_local u32 tRing[kRingSize];
thread_local u32 tRingLr[kRingSize];
thread_local unsigned tRingPos = 0;
}  // namespace

void initTrace() {
    const char* env = std::getenv("LM_TRACE");
    if (!env || !*env) return;
    const std::string v = env;
    if (v == "all") sTraceMode = TraceMode::All;
    else if (v == "ring") sTraceMode = TraceMode::Ring;
    else {
        sTraceMode = TraceMode::Set;
        size_t pos = 0;
        while (pos < v.size()) {
            const size_t comma = v.find(',', pos);
            sTraceSet.insert(u32(std::strtoul(v.substr(pos, comma - pos).c_str(), nullptr, 16)));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    g_traceOn = true;
}

void trace_enter(u32 addr, Ctx& c) {
    c.lastFn = addr;
    c.ring[c.ringPos++ & 31] = addr;
    tRing[tRingPos % kRingSize] = addr;
    tRingLr[tRingPos % kRingSize] = c.lr;
    tRingPos++;
#ifdef _WIN32
    // Runaway guest recursion (e.g. a cycle in corrupted game data) would otherwise exhaust
    // this host thread's stack and end the process without a word; with tracing on it is
    // reported, with the call ring, while there is still stack left to do so.
    {
        static thread_local uintptr_t tStackLo = 0, tStackHi = 0;
        if (!tStackLo) {
            ULONG_PTR lo = 0, hi = 0;
            GetCurrentThreadStackLimits(&lo, &hi);
            tStackLo = lo;
            tStackHi = hi;
        }
        volatile char probe = 0;
        if (uintptr_t(&probe) - tStackLo < 128 * 1024)
            fatal("host stack exhausted (%zu KiB used; runaway guest recursion?) guest r1=%08X",
                  size_t(tStackHi - uintptr_t(&probe)) / 1024, c.r[1]);
    }
#endif
    if (sTraceMode == TraceMode::All || (sTraceMode == TraceMode::Set && sTraceSet.count(addr)))
        log("[trace] %08X lr %08X r3 %08X r4 %08X r5 %08X", addr, c.lr, c.r[3], c.r[4], c.r[5]);
}

// LM_WATCH=<hex addr>[,<hex len>[,<hex stride>,<hex offset>]]: report every guest store into the range, with
// the function that made it (needs LM_TRACE, e.g. LM_TRACE=ring, for the name).
bool g_watchOn = false;
namespace {
u32 sWatchLo = 0, sWatchHi = 0, sWatchStride = 0, sWatchOffset = 0;
}  // namespace

void initWatch() {
    // LM_VTX_RACE=1: every guest store is checked against vertex arrays still unprocessed draws read
    // live (interp_vtx.cpp).
    if (interp::raceDetectorOn()) {
        g_watchOn = true;
        sWatchLo = sWatchHi = 0;
    }
    const char* env = std::getenv("LM_WATCH");
    if (!env || !*env) return;
    char* end = nullptr;
    sWatchLo = u32(std::strtoul(env, &end, 16));
    sWatchHi = sWatchLo + (end && *end == ',' ? u32(std::strtoul(end + 1, &end, 16)) : 4);
    // Optional ",<stride>,<offset>" (hex): only stores to that field of an array of structs.
    if (end && *end == ',') {
        sWatchStride = u32(std::strtoul(end + 1, &end, 16));
        if (end && *end == ',') sWatchOffset = u32(std::strtoul(end + 1, nullptr, 16));
    }
    g_watchOn = true;
}

void watch_store(u32 a, u32 v, int size) {
    if (interp::raceDetectorOn() && interp::raceStore(a, size)) [[unlikely]] {
        const u32 fn = tRingPos ? tRing[(tRingPos - 1) % kRingSize] : 0;
        log("[vtxrace] store%d %08X while an unprocessed draw reads it live, in %08X (thread %08X)", size * 8, a, fn,
            sched::currentGuest());
    }
    const u32 phys = a & kHostMemMask;
    if (phys + size <= (sWatchLo & kHostMemMask) || phys >= (sWatchHi & kHostMemMask)) return;
    if (sWatchStride && (phys - (sWatchLo & kHostMemMask)) % sWatchStride != sWatchOffset) return;
    const u32 fn = tRingPos ? tRing[(tRingPos - 1) % kRingSize] : 0;
    log("[watch] store%d %08X = %08X in %08X (thread %08X%s)", size * 8, a, v, fn, sched::currentGuest(),
        sched::inIrq() ? ", interrupt" : "");
    // LM_WATCH_RING=<n>: also the n functions entered before it (with LM_TRACE=ring), newest first;
    // the store itself may be in a caller of the function named above.
    static const int sRing = std::getenv("LM_WATCH_RING") ? std::atoi(std::getenv("LM_WATCH_RING")) : 0;
    if (sRing > 0 && tRingPos) {
        char buf[256];
        int n = 0;
        for (int i = 2; i <= sRing + 1 && u32(i) <= tRingPos && n < 230; i++)
            n += std::snprintf(buf + n, sizeof(buf) - n, " %08X", tRing[(tRingPos - u32(i)) % kRingSize]);
        log("[watch]   before:%s", buf);
    }
}

void watchRange(u32 a, u32 len, const char* what) {
    if (!g_watchOn) return;
    const u32 lo = a & kHostMemMask, hi = lo + len;
    if (hi <= (sWatchLo & kHostMemMask) || lo >= (sWatchHi & kHostMemMask)) return;
    log("[watch] %s writes %08X+%X", what, a, len);
}

void dumpTraceRing() {
    if (!g_traceOn) return;
    std::fprintf(stderr, "last guest calls on this thread (oldest first):\n");
    const unsigned n = tRingPos < kRingSize ? tRingPos : kRingSize;
    for (unsigned i = tRingPos - n; i < tRingPos; i++)
        std::fprintf(stderr, "  %08X (from %08X)\n", tRing[i % kRingSize], tRingLr[i % kRingSize]);
}

void log(const char* fmt, ...) {
    // LM_LOG_TIME=1: prefix each line with the host time since start (ms), for timing races.
    static const bool sTime = std::getenv("LM_LOG_TIME") != nullptr;
    if (sTime) {
        static const auto t0 = std::chrono::steady_clock::now();
        std::fprintf(stdout, "%9.2f ", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    va_list ap;
    va_start(ap, fmt);
    if (port::recovery::runLog()) {  // Windows: also into last_run.log (port/recovery.h)
        char buf[2048];
        va_list copy;
        va_copy(copy, ap);
        const int n = std::vsnprintf(buf, sizeof(buf), fmt, copy);
        va_end(copy);
        if (n > 0) port::recovery::logLine(buf, std::min<size_t>(size_t(n), sizeof(buf) - 1));
    }
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    va_list copy;
    va_copy(copy, ap);
    std::vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    {
        // Into the recovery state too, so the next start's "[recovery] ... crash" line says it.
        std::string note = buf;
        if (const std::string& disc = port::install().discProblem; !disc.empty()) note += "; probable cause: " + disc;
        port::recovery::noteFatal(note.c_str());
    }
    if (port::recovery::runLog()) {
        const std::string line = std::string("[recomp FATAL] ") + buf;
        port::recovery::logLine(line.c_str(), line.size());
    }
    std::fprintf(stderr, "[recomp FATAL] ");
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    // A damaged or modified disc image is the likely cause then (setup's disc check, issue #37).
    if (const std::string& disc = port::install().discProblem; !disc.empty()) {
        const std::string line = "[recomp FATAL] probable cause: " + disc;
        if (port::recovery::runLog()) port::recovery::logLine(line.c_str(), line.size());
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    dumpTraceRing();
    std::fflush(stderr);
    std::abort();
}

// ---------------------------------------------------------------------------
// Hardware registers
// ---------------------------------------------------------------------------

namespace {
std::mutex sMmioLogM;
std::unordered_set<u32> sMmioLogged;

void logMmio(const char* what, u32 a, u32 v) {
    std::lock_guard lk(sMmioLogM);
    if (sMmioLogged.insert(a).second) log("[mmio] unhandled %s %08X (%08X) in %08X from %08X thread %08X", what, a, v, tRingPos ? tRing[(tRingPos - 1) % kRingSize] : 0, tRingPos ? tRingLr[(tRingPos - 1) % kRingSize] : 0, sched::currentGuest());
}
}  // namespace

u32 mmio_rd(u32 a, int size) {
    switch (a & 0xFFFFF000) {
    case 0xCC002000:  // VI
        return hw::viRead(a, size);
    default:
        logMmio("read", a, 0);
        return 0;
    }
}

namespace hw {
u32 (*fifoFilter)(u32 v, int size) = nullptr;
}

void mmio_wr(u32 a, u32 v, int size) {
    if ((a & 0xFFFFFFE0) == 0xCC008000) {  // write-gather pipe -> GX FIFO
        if (hw::fifoFilter) [[unlikely]] v = hw::fifoFilter(v, size);
        hw::fifoWrite(v, size);
        return;
    }
    logMmio("write", a, v);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

GuestFn lookupFunc(u32 addr) {
    const FuncEntry* end = g_module->funcs + g_module->funcCount;
    const FuncEntry* it =
        std::lower_bound(g_module->funcs, end, addr, [](const FuncEntry& e, u32 a) { return e.addr < a; });
    return it != end && it->addr == addr ? it->fn : nullptr;
}

void Ctx::call(u32 addr) {
    if (GuestFn fn = lookupFunc(addr)) [[likely]] {
        fn(*this);
        return;
    }
    callMissing(*this, addr);
}

void callMissing(Ctx& c, u32 addr) {
    if (addr == 0) {
        // Usually a virtual call: show the object and its vtable.
        const u32 obj = c.r[3];
        const u32 vt = (obj >> 24) == 0x80 || (obj >> 24) == 0x81 ? rd32(obj) : 0;
        fatal("call to null (lr=%08X) r3=%08X vtable=%08X r12=%08X", c.lr, obj, vt, c.r[12]);
    }
    fatal("call to %08X: no function starts there (lr=%08X)", addr, c.lr);
}

void Ctx::unimplemented(u32 pc, u32 insn) { fatal("unimplemented instruction %08X at %08X", insn, pc); }

u32 guestSymbol(const char* name) {
    static const auto* map = [] {
        auto* m = new std::unordered_map<std::string, u32>;
        for (u32 i = 0; i < g_module->symCount; i++) m->emplace(g_module->syms[i].name, g_module->syms[i].addr);
        return m;
    }();
    auto it = map->find(name);
    if (it == map->end()) fatal("guest symbol %s not found", name);
    return it->second;
}

// "name+0xOFF" of the symbol at or below a guest code address (diagnostics), or the bare address.
std::string guestSymbolize(u32 addr) {
    static const auto* sorted = [] {
        auto* v = new std::vector<std::pair<u32, const char*>>;
        for (u32 i = 0; i < g_module->symCount; i++) v->emplace_back(g_module->syms[i].addr, g_module->syms[i].name);
        std::sort(v->begin(), v->end());
        return v;
    }();
    char buf[160];
    auto it = std::upper_bound(sorted->begin(), sorted->end(), std::pair<u32, const char*>{addr, nullptr},
                               [](const auto& a, const auto& b) { return a.first < b.first; });
    if (it == sorted->begin() || addr - std::prev(it)->first > 0x10000)
        std::snprintf(buf, sizeof(buf), "%08X", addr);
    else
        std::snprintf(buf, sizeof(buf), "%s+0x%X (%08X)", std::prev(it)->second, addr - std::prev(it)->first, addr);
    return buf;
}

void callOriginal(Ctx& c, const char* name) {
    // Hooks call this on hot paths (frame interpolation's tag hooks run thousands of times per
    // frame): the original is looked up once per name (callers pass string literals) per thread.
    thread_local std::unordered_map<const char*, HleFn> sCache;
    if (const auto it = sCache.find(name); it != sCache.end()) return it->second(c);
    const u32 addr = guestSymbol(name);
    for (u32 i = 0; i < g_module->origCount; i++) {
        if (g_module->orig[i].addr == addr) {
            sCache.emplace(name, g_module->orig[i].fn);
            break;
        }
    }
    for (u32 i = 0; i < g_module->origCount; i++) {
        if (g_module->orig[i].addr == addr) {
            g_module->orig[i].fn(c);
            return;
        }
    }
    fatal("callOriginal: %s is not listed in tools/recomp/hooks.txt", name);
}

u32 guestCall(Ctx& c, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    const u32 savedLr = c.lr;
    c.r[3] = a0;
    c.r[4] = a1;
    c.r[5] = a2;
    c.r[6] = a3;
    c.call(fn);
    c.lr = savedLr;
    return c.r[3];
}

// ---------------------------------------------------------------------------
// HLE registry
// ---------------------------------------------------------------------------

namespace {
std::unordered_map<std::string, HleFn>& registry() {
    static std::unordered_map<std::string, HleFn> r;
    return r;
}
}  // namespace

HleReg::HleReg(const char* name, HleFn fn) { registry()[name] = fn; }

HleFn hleLookup(const char* name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

void hleMissing(const char* name, u32 addr, Ctx& c) {
    fatal("SDK function %s (%08X) has no HLE implementation (lr=%08X)", name, addr, c.lr);
}

namespace {
// Guest objects the host makes (boot thread, interrupt stack, card/DVD scratch, unused-content
// data) live in the host window above MEM1 (ctx.h kHostMemSize), so the whole of MEM1 below the
// FST is the game's arena as on the console (hle_os.cpp OSInit). The model viewer keeps
// 0x81810000-0x81812000 and early Luigi's model copies start at 0x81F00000. 1.5 MiB: the rest up to
// 0x81F00000 is unused2's extraAlloc (both started at 0x81820000 from 1.1.1 to 1.1.3, so the Mario
// model and other unused content were built over the boot thread, card/DVD scratch and FIFO object).
constexpr u32 kHleHeapStart = 0x81820000, kHleHeapEnd = 0x819A0000;
u32 sHleHeap = kHleHeapStart;
std::mutex sHleHeapM;
}  // namespace

u32 hleAlloc(u32 size, u32 align) {
    std::lock_guard lk(sHleHeapM);
    sHleHeap = (sHleHeap + align - 1) & ~(align - 1);
    const u32 p = sHleHeap;
    sHleHeap += size;
    if (sHleHeap > kHleHeapEnd) fatal("HLE guest heap exhausted");
    std::memset(hostp(p), 0, size);
    return p;
}

// ---------------------------------------------------------------------------
// Special registers and time base
// ---------------------------------------------------------------------------

namespace {
u32 sSpr[1024];
const auto sBootClock = std::chrono::steady_clock::now();
u64 sTbBias = 0;
}  // namespace

void initTimeBase() {
    // The console's clock counts from 2000-01-01 local time.
    const std::time_t now = std::time(nullptr);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &now);
#else
    gmtime_r(&now, &g);
#endif
    g.tm_isdst = -1;
    const s64 tz = s64(now) - s64(std::mktime(&g));
    const s64 secs = s64(now) + tz - 946684800;
    sTbBias = u64(double(secs) * kTimerClock);
}

u64 timeBase() {
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - sBootClock).count();
    return sTbBias + u64(double(ns) * (kTimerClock / 1e9));
}

void setTimeBaseBias(u64 bias) { sTbBias = bias; }

u32 Ctx::mfspr(u32 spr) { return sSpr[spr & 1023]; }
void Ctx::mtspr(u32 spr, u32 v) { sSpr[spr & 1023] = v; }
u32 Ctx::mftb(u32 spr) {
    const u64 tb = timeBase();
    return spr == 269 ? u32(tb >> 32) : u32(tb);
}

}  // namespace recomp

// Host-side OSReport for shared port code (e.g. port/dol.cpp).
extern "C" void OSReport(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}
