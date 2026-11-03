#include "recomp/mem_report.h"

#include "recomp/frame.h"
#include "recomp/hle.h"
#include "recomp/texture_packs.h"

#include <aurora/mem_stats.h>

#include <SDL3/SDL_thread.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

namespace recomp {
void gxObjectTables(u32& tex, u32& texFree, u32& tlut, u32& tlutFree);  // hle_gx.cpp
}

namespace recomp::mem {
namespace {

using Clock = std::chrono::steady_clock;

struct Proc {
    double privateMb = -1, workingMb = -1, peakMb = -1;  // process (private = commit charge on Windows)
    double gpuLocalMb = -1, gpuSharedMb = -1;            // GPU memory this process uses (Windows)
    double heapMb = -1;                                  // allocated in the C/C++ heaps (Windows)
};

#ifdef _WIN32
// DXGI per-process video memory usage, summed over the adapters (only the one Dawn uses has any).
void gpuUsage(Proc& p) {
    static HMODULE sDxgi = LoadLibraryW(L"dxgi.dll");
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    static auto sCreate = sDxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(sDxgi, "CreateDXGIFactory1")) : nullptr;
    static IDXGIFactory1* sFactory = [] {
        IDXGIFactory1* f = nullptr;
        if (sCreate && FAILED(sCreate(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) f = nullptr;
        return f;
    }();
    if (!sFactory) return;
    // The adapters are enumerated once (issue #43: no driver enumeration every report).
    static IDXGIAdapter3* sAdapters[8] = {};
    static UINT sCount = [] {
        UINT n = 0;
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; n < 8 && sFactory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            IDXGIAdapter3* a3 = nullptr;
            if (SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3)))) sAdapters[n++] = a3;
            a->Release();
        }
        return n;
    }();
    double local = 0, shared = 0;
    bool any = false;
    for (UINT i = 0; i < sCount; i++) {
        DXGI_QUERY_VIDEO_MEMORY_INFO l{}, n{};
        if (SUCCEEDED(sAdapters[i]->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l)) &&
            SUCCEEDED(sAdapters[i]->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &n))) {
            local += double(l.CurrentUsage) / (1024.0 * 1024.0);
            shared += double(n.CurrentUsage) / (1024.0 * 1024.0);
            any = true;
        }
    }
    if (any) p.gpuLocalMb = local, p.gpuSharedMb = shared;
}

// Issue #43: HeapSummary walks every block of a heap while holding that heap's lock: 0.8-0.9 s
// for the ~1.1-1.4 GB the game's heaps hold at 4K with Enhanced lighting, during which every thread
// that allocates (render worker, Dawn, the driver, the guest's HLE calls) waits. Done every 60 s on
// the host thread in 1.1.5, it froze the game and its sound for most of a second once a minute.
// So the heap total is only taken on request (LM_MEM_HEAPS=1, or with LM_MEM_TRACK), knowing it
// stalls the game for that long.
bool heapsWanted() {
    static const bool s = [] {
        const char* h = std::getenv("LM_MEM_HEAPS");
        const char* t = std::getenv("LM_MEM_TRACK");
        return (h && *h == '1') || (t && (*t == '1' || *t == '2'));
    }();
    return s;
}

// C/C++ heaps (malloc/new of the game, Aurora, Dawn, SDL): bytes allocated in the process heaps.
// The rest of the private bytes are direct virtual allocations (guest memory, the graphics
// driver's own memory, thread stacks).
void heapUsage(Proc& p) {
    HANDLE heaps[64];
    const DWORD n = GetProcessHeaps(64, heaps);
    double allocated = 0;
    static const bool sDetail = [] { const char* v = std::getenv("LM_MEM_TRACK"); return v && (*v == '1' || *v == '2'); }();
    char detail[512] = "";
    int len = 0;
    for (DWORD i = 0; i < n && i < 64; i++) {
        HEAP_SUMMARY s{};
        s.cb = sizeof(s);
        if (!HeapSummary(heaps[i], 0, &s)) continue;
        allocated += double(s.cbAllocated) / (1024.0 * 1024.0);
        if (sDetail && s.cbAllocated > (16u << 20) && len < int(sizeof(detail)) - 64) {
            // Which module the heap's memory belongs to says little; its handle and whether it is
            // the process heap (the C runtime's malloc) do.
            len += std::snprintf(detail + len, sizeof(detail) - len, " %p%s %.0f MB", heaps[i],
                                 heaps[i] == GetProcessHeap() ? " (process heap)" : "", s.cbAllocated / 1048576.0);
        }
    }
    p.heapMb = allocated;
    if (sDetail && len) log("[mem] heaps over 16 MB:%s", detail);
}
#endif

Proc sample() {
    Proc p;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof(c);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c))) {
        p.privateMb = double(c.PrivateUsage) / (1024.0 * 1024.0);
        p.workingMb = double(c.WorkingSetSize) / (1024.0 * 1024.0);
        p.peakMb = double(c.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
    gpuUsage(p);
    if (heapsWanted()) heapUsage(p);
#elif defined(__APPLE__)
    task_vm_info_data_t vm{};
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &n) == KERN_SUCCESS) {
        p.privateMb = double(vm.phys_footprint) / (1024.0 * 1024.0);
        p.workingMb = double(vm.resident_size) / (1024.0 * 1024.0);
        p.peakMb = double(vm.resident_size_peak) / (1024.0 * 1024.0);
    }
#else
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            long kb = 0;
            if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) p.workingMb = kb / 1024.0;
            else if (std::sscanf(line, "VmHWM: %ld kB", &kb) == 1) p.peakMb = kb / 1024.0;
            else if (std::sscanf(line, "RssAnon: %ld kB", &kb) == 1) p.privateMb = kb / 1024.0;
        }
        std::fclose(f);
    }
#endif
    return p;
}

void writeReport(double t) {
    const auto t0 = Clock::now();
    const Proc p = sample();
    char gpu[96] = "", heap[48] = "";
    if (p.gpuLocalMb >= 0) std::snprintf(gpu, sizeof(gpu), ", GPU local %.0f MB, GPU shared %.0f MB", p.gpuLocalMb, p.gpuSharedMb);
    if (p.heapMb >= 0) std::snprintf(heap, sizeof(heap), ", heaps %.0f MB", p.heapMb);
    log("[mem] %.0f s, guest frame %u: private %.0f MB%s, working set %.0f MB (peak %.0f MB)%s", t, frame::guestFrames(),
        p.privateMb, heap, p.workingMb, p.peakMb, gpu);
    AuroraMemStats a{};
    aurora_mem_stats(&a);
    constexpr double kMb = 1024.0 * 1024.0;
    log("[mem] aurora: textures %llu (%.0f MB), texture objects %llu, content cache %llu (%.0f MB), TLUTs %llu, copy "
        "textures %llu, copy cache %llu (%.0f MB), bind groups %llu, samplers %llu, pipelines %llu, shader modules %llu, "
        "EFB-to-RAM copies %llu, byte buffers %llu (%.0f MB), decoded %llu, written %llu, guest copies %llu",
        (unsigned long long)a.textures, a.textureBytes / kMb, (unsigned long long)a.texObjects,
        (unsigned long long)a.texContentEntries, a.texContentBytes / kMb, (unsigned long long)a.tlutCaches,
        (unsigned long long)a.copyTextures, (unsigned long long)a.copyCacheEntries, a.copyCacheBytes / kMb,
        (unsigned long long)a.bindGroups, (unsigned long long)a.samplers, (unsigned long long)a.pipelines,
        (unsigned long long)a.shaderModules, (unsigned long long)a.efbEntries, (unsigned long long)a.efbPoolBuffers,
        a.efbPoolBytes / kMb, (unsigned long long)a.efbDecoded, (unsigned long long)a.efbWritten,
        (unsigned long long)a.efbGuestCopies);
    u32 tex = 0, texFree = 0, tlut = 0, tlutFree = 0;
    gxObjectTables(tex, texFree, tlut, tlutFree);
    log("[mem] port: texture objects %u (%u free), TLUT objects %u (%u free)", tex, texFree, tlut, tlutFree);
    texpacks::memReport();
    trackReport();
    // Issue #43: what the report cost (on its own thread; the game does not wait for it).
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (ms > 20.0) log("[mem] report took %.0f ms (sampler thread)", ms);
}

// Issue #43: the report runs on its own low-priority thread, never on the host (frame) thread: even
// a millisecond there is a late frame at 165 fps. The host thread only wakes it.
std::mutex sMutex;
std::condition_variable sWake;
bool sPending = false;
double sPendingT = 0;

void samplerThread() {
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (;;) {
        double t;
        {
            std::unique_lock lock{sMutex};
            sWake.wait(lock, [] { return sPending; });
            sPending = false;
            t = sPendingT;
        }
        writeReport(t);
    }
}

}  // namespace

void report() {
    static const double sEvery = [] { const char* e = std::getenv("LM_MEM_LOG"); return e ? std::atof(e) : 60.0; }();
    if (sEvery <= 0) return;
    static const Clock::time_point sStart = Clock::now();
    static Clock::time_point sLast = sStart;
    const auto now = Clock::now();
    if (std::chrono::duration<double>(now - sLast).count() < sEvery) return;
    sLast = now;
    static const bool sStarted = [] {
        std::thread(samplerThread).detach();  // waits on sWake for the rest of the run
        return true;
    }();
    (void)sStarted;
    {
        std::lock_guard lock{sMutex};
        sPending = true;
        sPendingT = std::chrono::duration<double>(now - sStart).count();
    }
    sWake.notify_one();
}

}  // namespace recomp::mem
