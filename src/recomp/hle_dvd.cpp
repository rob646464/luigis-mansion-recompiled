// HLE: DVD file system, on top of Aurora's disc reader. Synchronous reads block the calling
// thread; async reads run on a host "drive" thread and complete as interrupts.
#include "recomp/hidden_mansion.h"
#include "recomp/room_swap.h"
#include "recomp/unused_maps.h"
#include "recomp/unused2.h"
#include "recomp/cpu_qos.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"

#include <cstring>
#include <dolphin/dvd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace recomp {
namespace {

// A destination the game may fill with file data: main memory above the OS globals (MEM1 from
// 0x80003100, the DOL's first section) or the host window above MEM1 (ctx.h). A failed game heap
// allocation (null buffer) would otherwise land in low memory: the bus clock at 0x800000F8, thread
// and arena pointers (it did in event01 up to 1.1: hle_os.cpp OSInit). Refused writes are logged.
bool destOk(u32 dest, u32 size, const char* what, u32 lr) {
    const u64 end = u64(dest) + size;
    if ((dest >= 0x80003100u && end <= 0x81800000u) || (dest >= 0x81810000u && end <= 0x80000000ull + kHostMemSize))
        return true;
    log("[dvd] ERROR: %s of %X bytes to %08X refused (lr %08X): the game's buffer allocation failed?", what, size,
        dest, lr);
    return false;
}

// Host-served files: leftover-room visits (room_swap.cpp), the rebuilt unused maps 7/8
// (unused_maps.cpp), then the PAL Hidden Mansion overlay.
s32 ovEntry(const char* path) {
    s32 e = rooms::overlayEntry(path);
    if (e < 0) e = unused2::overlayEntry(path);
    if (e < 0) e = unused_maps::overlayEntry(path);
    return e >= 0 ? e : hm::overlayEntry(path);
}
bool ovIs(s32 e) {
    return rooms::isOverlayEntry(e) || unused2::isOverlayEntry(e) || unused_maps::isOverlayEntry(e) || hm::isOverlayEntry(e);
}
u32 ovLength(s32 e) {
    if (rooms::isOverlayEntry(e)) return rooms::overlayLength(e);
    if (unused2::isOverlayEntry(e)) return unused2::overlayLength(e);
    if (unused_maps::isOverlayEntry(e)) return unused_maps::overlayLength(e);
    return hm::overlayLength(e);
}
s32 ovRead(s32 e, void* dst, s32 len, s32 off) {
    if (rooms::isOverlayEntry(e)) return rooms::overlayRead(e, dst, len, off);
    if (unused2::isOverlayEntry(e)) return unused2::overlayRead(e, dst, len, off);
    if (unused_maps::isOverlayEntry(e)) return unused_maps::overlayRead(e, dst, len, off);
    return hm::overlayRead(e, dst, len, off);
}

std::unordered_map<u32, DVDFileInfo> sFiles;  // guest DVDFileInfo -> host
std::unordered_map<u32, u32> sNames;          // FST entry -> guest copy of its name
std::unordered_map<u32, s32> sOverlay;        // guest DVDFileInfo -> hm overlay entry

// Disc latency. The game relies on reads taking time (e.g. BootScene switches the
// current heap right after queueing an async open whose callback allocates), so
// a read keeps the requesting thread blocked while other threads run. Faster
// than the console's drive, but never instantaneous.
double readLatency(u32 bytes) {
    static const double scale = [] {
        const char* env = std::getenv("LM_DVD_SPEED");  // 1 = console speed
        return env ? 1.0 / std::atof(env) : 0.1;
    }();
    return scale * (0.010 + bytes / 3.0e6);
}

DVDFileInfo* hostFile(u32 guest) {
    auto it = sFiles.find(guest);
    return it == sFiles.end() ? nullptr : &it->second;
}

void syncGuest(u32 guest, const DVDFileInfo& fi) {
    wr32(guest + 0x0C, 0);  // DVD_STATE_END
    wr32(guest + 0x30, fi.startAddr);
    wr32(guest + 0x34, fi.length);
}

// Files replaced by the PAL Hidden Mansion overlay (hidden_mansion.cpp) are served from host
// memory instead of the disc image.
bool openOverlay(u32 guest, s32 entry) {
    sFiles.erase(guest);
    sOverlay[guest] = entry;
    wr32(guest + 0x0C, 0);
    wr32(guest + 0x30, 0);
    wr32(guest + 0x34, ovLength(entry));
    return true;
}

s32 overlayOf(u32 guest) {
    auto it = sOverlay.find(guest);
    return it == sOverlay.end() ? -1 : it->second;
}

// ---- The drive: async reads (issue #25) ----
// On the console DVDReadAsyncPrio only queues the command and returns; no other guest code runs
// before it returns. JAudio's AFC streamer depends on that: LoadADPCM (fn_801ABB40) passes its
// file offset to DVDReadAsyncPrio and advances it only after the call returns. Up to 1.1.5 the
// HLE did the host read inside the call with the guest CPU released, so the previous piece's
// completion interrupt (__LoadFin -> LoadADPCM for the next buffer) could run in that window and
// issue a second read at the same, not yet advanced offset: a stream piece played twice, the next
// one never, and the ADPCM history was wrong until the next reset (loud crackle, e.g. the thunder
// in TMansion.afc on the walk up to the mansion, mostly with slow reads: RVZ, first load after
// boot). Now the call only queues the read for this host thread, like the drive, and returns;
// the completion interrupt is raised the modelled latency after the data is in guest memory (the
// same timing as before, minus the guest thread being held). Completions stay in issue order.
// LM_DVD_ASYNC=old restores the old path (A/B only).
struct DriveReq {
    u32 guest;
    std::function<s32()> read;
    double latency;  // modelled drive time, counted from the end of the host read (as before)
    std::function<void(Ctx&, s32)> done;
};
std::mutex sDriveM;
std::condition_variable sDriveCv, sDriveIdleCv;
std::deque<DriveReq> sDriveQ;
std::unordered_map<u32, int> sHostBusy;  // guest DVDFileInfo -> host reads queued or running
std::unordered_map<u32, int> sPending;   // guest DVDFileInfo -> async reads not completed yet
std::unordered_map<u32, u64> sLastAsync; // guest DVDFileInfo -> last async (offset << 32 | size)
std::chrono::steady_clock::time_point sLastDue;

bool oldAsyncPath() {
    static const bool v = [] {
        const char* e = std::getenv("LM_DVD_ASYNC");
        return e && std::strcmp(e, "old") == 0;
    }();
    return v;
}
bool dvdLog() {
    static const bool v = std::getenv("LM_DVD_LOG") != nullptr;
    return v;
}

// Test aid: LM_DVD_HOST_DELAY=<ms> makes every host read take that much longer (a slow disc
// image: RVZ blocks, an SD card, a busy drive), which widened the issue #25 race.
void hostDelay() {
    static const double ms = [] {
        const char* e = std::getenv("LM_DVD_HOST_DELAY");
        return e ? std::atof(e) : 0.0;
    }();
    if (ms > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}

void driveMain() {
    cpu::setCurrent(cpu::Role::Guest);  // it does what the guest thread did before (issue #18 policy)
    std::unique_lock lk(sDriveM);
    for (;;) {
        sDriveCv.wait(lk, [] { return !sDriveQ.empty(); });
        DriveReq r = std::move(sDriveQ.front());
        sDriveQ.pop_front();
        lk.unlock();
        const s32 result = r.read();
        lk.lock();
        if (--sHostBusy[r.guest] <= 0) sHostBusy.erase(r.guest);
        sDriveIdleCv.notify_all();
        const auto now = std::chrono::steady_clock::now();
        auto due = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(r.latency));
        if (due < sLastDue) due = sLastDue;  // complete in issue order
        sLastDue = due;
        const double delay = due > now ? std::chrono::duration<double>(due - now).count() : 0.0;
        sched::postIrqAfter(delay, [done = std::move(r.done), result](Ctx& ic) { done(ic, result); });
    }
}

void driveQueue(DriveReq r) {
    static std::once_flag sStart;
    std::call_once(sStart, [] { std::thread(driveMain).detach(); });
    std::lock_guard lk(sDriveM);
    ++sHostBusy[r.guest];
    sDriveQ.push_back(std::move(r));
    sDriveCv.notify_one();
}

// Waits until the drive thread no longer uses this file's host state (before it is closed,
// reopened or read synchronously). Only the host read is waited for; its completion interrupt
// still comes later. Does not need the guest CPU, so holding it here cannot deadlock.
void driveDrain(u32 guest, bool reopen = false) {
    std::unique_lock lk(sDriveM);
    sDriveIdleCv.wait(lk, [guest] { return sHostBusy.find(guest) == sHostBusy.end(); });
    if (reopen) sLastAsync.erase(guest);
}

// Diagnostic: on the console a DVDFileInfo can only carry one command, so an async read issued
// while the previous one on the same file has not completed, or one repeating the previous
// offset, means the game's bookkeeping was broken (the issue #25 duplicate stream piece).
std::atomic<u32> sDupReads{0}, sBusyReads{0};
void noteAsync(u32 guest, u32 off, u32 size, u32 lr) {
    std::lock_guard lk(sDriveM);
    const bool busy = sPending[guest]++ > 0;
    const u64 key = u64(off) << 32 | size;
    auto it = sLastAsync.find(guest);
    const bool dup = it != sLastAsync.end() && it->second == key && size != 0;
    sLastAsync[guest] = key;
    if (busy) {
        const u32 n = ++sBusyReads;
        if (n <= 20 || n % 100 == 0)
            log("[dvd] async read on file %08X while its previous read is still in flight (offset %X, %X "
                "bytes, lr %08X; %u since start)", guest, off, size, lr, n);
    }
    if (dup) {
        const u32 n = ++sDupReads;
        if (n <= 20 || n % 100 == 0)
            log("[dvd] async read on file %08X repeats the previous offset %X (%X bytes, lr %08X; %u since "
                "start)", guest, off, size, lr, n);
    }
    if (dvdLog())
        log("[dvd] async read file %08X offset %X size %X lr %08X thread %08X%s%s", guest, off, size, lr,
            sched::currentGuest(), sched::inIrq() ? " in irq" : "", busy ? " (busy)" : "");
}
void noteAsyncDone(u32 guest) {
    std::lock_guard lk(sDriveM);
    auto it = sPending.find(guest);
    if (it != sPending.end() && --it->second <= 0) sPending.erase(it);
}

bool openWith(u32 guest, BOOL ok, const DVDFileInfo& fi) {
    if (!ok) return false;
    sOverlay.erase(guest);
    sFiles[guest] = fi;
    syncGuest(guest, fi);
    return true;
}

}  // namespace

// The game's Yay0 decompressor for loaded files (LMDvdFileInfoBlock, run on the DVD threads):
// (Yay0 source r3, destination r4). Its caller does not check the destination allocation; a null
// one is refused here (logged) instead of decompressing over low memory.
HLE(fn_800071D0__18LMDvdFileInfoBlockFP18LMDvdFileInfoBlock) {
    const u32 src = c.r[3], dest = c.r[4];
    const u32 size = src >= 0x80000000u && src < 0x81800000u ? rd32(src + 4) : 0;
    if (!destOk(dest, size, "Yay0 decompression", c.lr)) return;
    callOriginal(c, "fn_800071D0__18LMDvdFileInfoBlockFP18LMDvdFileInfoBlock");
}

HLE(DVDInit) {}
HLE(DVDGetDriveStatus) { c.r[3] = 0; }
HLE(DVDCheckDisk) { c.r[3] = 1; }
HLE(DVDGetCommandBlockStatus) { c.r[3] = rd32(c.r[3] + 0x0C); }
HLE(DVDConvertPathToEntrynum) {
    if (const s32 e = ovEntry(gstr(c.r[3])); e >= 0) {
        c.r[3] = u32(e);
        return;
    }
    const s32 n = DVDConvertPathToEntrynum(gstr(c.r[3]));
    if (n < 0) log("[dvd] path not found: %s", gstr(c.r[3]));
    else if (dvdLog()) log("[dvd] entry %d = %s", n, gstr(c.r[3]));
    c.r[3] = u32(n);
}

HLE(DVDOpen) {
    driveDrain(c.r[4], true);
    DVDFileInfo fi{};
    const char* path = gstr(c.r[3]);
    if (dvdLog())
        log("[dvd] open %s as file %08X (thread %08X prio %d%s, lr %08X)", path, c.r[4], sched::currentGuest(),
            sched::currentGuest() ? sched::priority(sched::currentGuest()) : -1, sched::inIrq() ? " in irq" : "", c.lr);
    if (const s32 e = ovEntry(path); e >= 0) {
        c.r[3] = openOverlay(c.r[4], e);
        return;
    }
    c.r[3] = openWith(c.r[4], DVDOpen(path, &fi), fi);
    if (!c.r[3]) log("[dvd] open failed: %s", path);
}
HLE(DVDFastOpen) {
    driveDrain(c.r[4], true);
    if (dvdLog()) log("[dvd] open entry %d as file %08X", s32(c.r[3]), c.r[4]);
    if (ovIs(s32(c.r[3]))) {
        c.r[3] = openOverlay(c.r[4], s32(c.r[3]));
        return;
    }
    DVDFileInfo fi{};
    c.r[3] = openWith(c.r[4], DVDFastOpen(s32(c.r[3]), &fi), fi);
}
HLE(DVDClose) {
    driveDrain(c.r[3], true);
    sOverlay.erase(c.r[3]);
    if (DVDFileInfo* fi = hostFile(c.r[3])) {
        DVDClose(fi);
        sFiles.erase(c.r[3]);
    }
    c.r[3] = 1;
}

HLE(DVDReadPrio) {
    driveDrain(c.r[3]);
    DVDFileInfo* fi = hostFile(c.r[3]);
    watchRange(c.r[4], c.r[5], "DVDReadPrio");
    const s32 ov = overlayOf(c.r[3]);
    // The host read (disc image, RVZ decompression) can take several milliseconds for a big
    // read: give up the CPU meanwhile, as the console's thread sleeps while the drive works.
    s32 result = -1;
    const bool ok = destOk(c.r[4], c.r[5], "DVDReadPrio", c.lr);
    if (ok) sched::withCpuReleased([&] {
        hostDelay();
        result = ov >= 0 ? ovRead(ov, hostp(c.r[4]), s32(c.r[5]), s32(c.r[6]))
                 : fi    ? DVDReadPrio(fi, hostp(c.r[4]), s32(c.r[5]), s32(c.r[6]), s32(c.r[7]))
                         : -1;
    });
    // Block this thread until the "drive" finishes.
    static std::atomic<u32> sWaitKey{0xF0000000};  // sleep-queue keys need not be real memory
    const u32 done = sWaitKey.fetch_add(8);
    std::atomic<bool>* flag = new std::atomic<bool>(false);
    sched::postIrqAfter(readLatency(c.r[5]), [done, flag](Ctx&) {
        flag->store(true);
        sched::wakeup(done);
    });
    while (!flag->load()) sched::sleepOn(done);
    delete flag;
    c.r[3] = u32(result);
}

HLE(DVDReadAsyncPrio) {
    const u32 guest = c.r[3], cb = c.r[7];
    DVDFileInfo* fi = hostFile(guest);
    const s32 ov = overlayOf(guest);
    if (!fi && ov < 0) {
        c.r[3] = 0;
        return;
    }
    watchRange(c.r[4], c.r[5], "DVDReadAsyncPrio");
    const u32 dst = c.r[4], len = c.r[5], off = c.r[6], prio = c.r[8];
    const bool ok = destOk(dst, len, "DVDReadAsyncPrio", c.lr);
    noteAsync(guest, off, len, c.lr);
    const auto read = [ok, ov, fi, dst, len, off, prio]() -> s32 {
        if (!ok) return -1;
        hostDelay();
        return ov >= 0 ? ovRead(ov, hostp(dst), s32(len), s32(off))
                       : DVDReadPrio(fi, hostp(dst), s32(len), s32(off), s32(prio));
    };
    const auto complete = [guest, cb](Ctx& ic, s32 result) {
        noteAsyncDone(guest);
        wr32(guest + 0x20, u32(result));
        wr32(guest + 0x0C, 0);
        if (cb) guestCall(ic, cb, u32(result), guest);
    };
    wr32(guest + 0x0C, 1);  // busy until the completion interrupt
    wr32(guest + 0x38, cb);
    wr32(guest + 0x20, 0);
    if (!oldAsyncPath()) {
        driveQueue({guest, read, readLatency(len), complete});
    } else {
        // 1.1.5 and before (LM_DVD_ASYNC=old, A/B only): the read in the call, with the guest CPU
        // released unless interrupts are off, so other guest code could run before it returned.
        s32 result = -1;
        if (sched::interruptsEnabled()) sched::withCpuReleased([&] { result = read(); });
        else result = read();
        sched::postIrqAfter(readLatency(len), [complete, result](Ctx& ic) { complete(ic, result); });
    }
    c.r[3] = 1;
}

HLE(DVDPrepareStreamAsync) {  // streamed audio is not emulated yet
    const u32 guest = c.r[3], cb = c.r[6];
    sched::postIrq([guest, cb](Ctx& ic) {
        if (cb) guestCall(ic, cb, 0, guest);
    });
    c.r[3] = 1;
}
HLE(DVDCancelStream) { c.r[3] = 0; }

HLE(DVDOpenDir) {
    DVDDir d{};
    const int ok = DVDOpenDir(gstr(c.r[3]), &d);
    if (ok) {
        wr32(c.r[4], d.entryNum);
        wr32(c.r[4] + 4, d.location);
        wr32(c.r[4] + 8, d.next);
    }
    c.r[3] = u32(ok);
}
HLE(DVDReadDir) {
    DVDDir d{rd32(c.r[3]), rd32(c.r[3] + 4), rd32(c.r[3] + 8)};
    DVDDirEntry e{};
    const int ok = DVDReadDir(&d, &e);
    if (ok) {
        wr32(c.r[3] + 4, d.location);
        u32& name = sNames[e.entryNum];
        if (!name) {
            const std::string s = e.name ? e.name : "";
            name = hleAlloc(u32(s.size() + 1), 4);
            std::memcpy(hostp(name), s.c_str(), s.size() + 1);
        }
        wr32(c.r[4], e.entryNum);
        wr32(c.r[4] + 4, u32(e.isDir));
        wr32(c.r[4] + 8, name);
    }
    c.r[3] = u32(ok);
}
HLE(DVDCloseDir) { c.r[3] = 1; }

}  // namespace recomp
