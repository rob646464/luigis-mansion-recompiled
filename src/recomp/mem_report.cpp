#include "recomp/mem_report.h"

#include "recomp/frame.h"
#include "recomp/hle.h"
#include "recomp/texture_packs.h"

#include <aurora/mem_stats.h>

#include <chrono>
#include <cstdint>
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
    double local = 0, shared = 0;
    bool any = false;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; sFactory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIAdapter3* a3 = nullptr;
        if (SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO l{}, n{};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l)) &&
                SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &n))) {
                local += double(l.CurrentUsage) / (1024.0 * 1024.0);
                shared += double(n.CurrentUsage) / (1024.0 * 1024.0);
                any = true;
            }
            a3->Release();
        }
        a->Release();
    }
    if (any) p.gpuLocalMb = local, p.gpuSharedMb = shared;
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
    heapUsage(p);
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

}  // namespace

void report() {
    static const double sEvery = [] { const char* e = std::getenv("LM_MEM_LOG"); return e ? std::atof(e) : 60.0; }();
    if (sEvery <= 0) return;
    static const Clock::time_point sStart = Clock::now();
    static Clock::time_point sLast = sStart;
    const auto now = Clock::now();
    if (std::chrono::duration<double>(now - sLast).count() < sEvery) return;
    sLast = now;
    const Proc p = sample();
    const double t = std::chrono::duration<double>(now - sStart).count();
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
}

}  // namespace recomp::mem
