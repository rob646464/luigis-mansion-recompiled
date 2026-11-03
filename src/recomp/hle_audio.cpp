// HLE: audio interface (AI) and the JAudio DSP driver.
//
// AI: the game hands finished 16-bit stereo blocks to AIInitDMA; a host thread
// plays them through SDL3 (audio_out.cpp) and raises the AI DMA interrupt each
// time a block starts, paced by the host device so the game's audio clock
// follows the sound card. Without a device the interrupt runs off a timer.
//
// DSP: JAudio's DSP driver (jaudio/dspboot.c, dspproc.c) talks to its microcode
// through a mailbox. Its entry points are hooked (tools/recomp/hooks.txt):
// command packets go to our microcode stand-in (audio_dsp.cpp), and every
// DSPReleaseHalt mixes one subframe and then raises the DSP interrupt that
// JAudio's audio thread waits for. See docs/AUDIO.md.
#include "port/settings.h"
#include "recomp/audio.h"
#include "recomp/cpu_qos.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/sched.h"
#include "recomp/silly_world.h"
#include "recomp/unused2.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace recomp {
namespace {
std::atomic<u32> sDmaCallback{0};
u32 sStreamCallback = 0;
std::atomic<u32> sDmaAddr{0};
std::atomic<u32> sDmaLength{0};
std::atomic<bool> sDmaRunning{false};
std::atomic<u32> sDspRate{0};  // 0 = 32 kHz, 1 = 48 kHz
std::thread sDmaThread;

// The console's real rates (audio.h): the game's audio clock runs at these, so music
// tempo and pitch match the console (0.09% above the nominal 32000 Hz).
double rateHz() { return sDspRate.load() ? audio::kConsoleRate48 : audio::kConsoleRate32; }

// Set when the AI interrupt is raised, cleared once JAudio has mixed for it (Jac_imixcopy).
std::atomic<bool> sAwaitingMix{false};

// JAudio's DSP overload check (UpdateDSPchannelAll) times its DSP updates: when a subframe
// takes nearly as long as the gap before the frame's first one, it decides the DSP cannot
// keep up and force-stops a voice, whatever its priority. On the console a frame starts at
// an AI interrupt, ~17 ms after the previous one, and is mixed long before the next. Here
// two interrupts could reach the game together (a host stall, or the device draining its
// queue in big chunks) and the second frame then started right after the first: every
// subframe looked "slow" and a voice was killed (sound effects cut short; the movie
// player's audio stream stopped, which ended the movie). So the next interrupt waits until
// the game has mixed for the previous one and the DSP frame it started is done, for at most
// `maxSeconds` (100 ms: the game may be stalled). Returns false when it gave up.
//
// The SMS port also bounds the wait by the audio still queued on the device (keeping 4 ms),
// so a slow host replays the AI block instead of running the device dry. Measured in LM it
// does more harm than good (docs/AUDIO.md, "AI output and pacing"): by the time the bound
// expires the queue is empty anyway, and the early interrupt starts a DSP frame on top of
// the unfinished one, so LM's overload check (which stops voices of any priority) cut 2-6
// voices each time. Kept as a test aid: LM_AI_QUEUE_BOUND=1.
bool waitForGuestMix(double maxSeconds) {
    using clock = std::chrono::steady_clock;
    const auto deadline =
        clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(maxSeconds));
    while ((sAwaitingMix.load() || audio::dspFrameBusy()) && clock::now() < deadline && !audio::outPaused())
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    const bool mixed = !sAwaitingMix.load();
    const auto quiet = std::min(audio::dspFrameEnd() + std::chrono::milliseconds(2), deadline);
    if (clock::now() < quiet) std::this_thread::sleep_until(quiet);
    return mixed;
}

// LM_AUDIO_LOG=1: AI pacing statistics every ~10 s (cumulative): interrupt spacing, how long
// the interrupt waited for the game's mix (and how often the wait gave up), and the device
// queue when each block was pushed ("dry" = the SDL stream had run empty: an audible gap).
struct PacingStats {
    using clock = std::chrono::steady_clock;
    u64 blocks = 0, dry = 0, low = 0, gaveUp = 0, close = 0;
    double minQueued = 1e9, sumQueued = 0, maxWait = 0, sumWait = 0, minGap = 1e9, maxGap = 0;
    clock::time_point last{}, report = clock::now();

    void block(double secs, double queued, double waited, bool timedOut) {
        if (!audio::g_log) return;
        const auto now = clock::now();
        if (blocks) {
            const double gap = std::chrono::duration<double>(now - last).count();
            minGap = std::min(minGap, gap);
            maxGap = std::max(maxGap, gap);
            if (gap < secs * 0.5) close++;  // two interrupts nearly back to back
        }
        last = now;
        blocks++;
        if (queued <= 0) dry++;
        if (queued < 0.004) low++;
        minQueued = std::min(minQueued, queued);
        sumQueued += queued;
        maxWait = std::max(maxWait, waited);
        sumWait += waited;
        if (timedOut) gaveUp++;
        if (now - report < std::chrono::seconds(10)) return;
        report = now;
        log("[audio] pacing: %llu blocks, gap %.1f-%.1f ms (%llu < half a block), mix wait avg %.2f max %.1f ms "
            "(%llu gave up), queue avg %.1f min %.1f ms, %llu below 4 ms, %llu dry",
            (unsigned long long)blocks, minGap * 1e3, maxGap * 1e3, (unsigned long long)close, sumWait / blocks * 1e3,
            maxWait * 1e3, (unsigned long long)gaveUp, sumQueued / blocks * 1e3, minQueued * 1e3,
            (unsigned long long)low, (unsigned long long)dry);
    }
} sPacing;

// Device underruns (issue #25, docs/AUDIO.md "Underruns"). When the game's mix comes late (the
// guest CPU held by another guest thread, a host stall) the device queue runs dry and the
// device plays silence: a click, or a crackle when it repeats. Each underrun raises the queue
// target by one block, up to kMaxExtraBlocks (about 100 ms more latency), so a scene that
// stalls now and then stops crackling after the first click; after kRelaxSeconds without one the
// target steps back down. Always on; every minute with an underrun gives one line in the log
// (last_run.log), so user reports show it.
struct Underruns {
    using clock = std::chrono::steady_clock;
    static constexpr int kMaxExtraBlocks = 6;
    static constexpr double kRelaxSeconds = 30;
    int extraBlocks = 0;
    u64 pushes = 0;
    u32 count = 0, total = 0;  // since the last log line, since start
    double maxWait = 0, maxGap = 0;
    clock::time_point lastPush{}, lastDry = clock::now(), lastLine = clock::now();

    static int forcedExtra() {
        static const int v = [] {
            const char* e = std::getenv("LM_AUDIO_EXTRA_BLOCKS");  // test aid: fixed extra blocks
            return e && *e ? std::atoi(e) : -1;
        }();
        return v;
    }
    double extraSeconds(double block) const {
        const int f = forcedExtra();
        return (f >= 0 ? f : extraBlocks) * block;
    }
    void reset() {  // a new start (host pause, no DMA): the empty queue is not an underrun
        pushes = 0;
        lastPush = {};
    }
    // Called just before a block is pushed, with the device queue at that moment.
    void push(double queued, double block, double waited) {
        const auto now = clock::now();
        const double gap = pushes ? std::chrono::duration<double>(now - lastPush).count() : 0;
        lastPush = now;
        if (++pushes > 20 && queued <= 0.0005) {  // the first blocks fill an empty queue
            count++;
            total++;
            maxWait = std::max(maxWait, waited);
            maxGap = std::max(maxGap, gap);
            lastDry = now;
            if (extraBlocks < kMaxExtraBlocks) extraBlocks++;
            if (total == 1)
                log("[audio] device queue ran dry (waited %.1f ms for the game's mix, %.1f ms since the last block); "
                    "queue target now %.0f ms", waited * 1e3, gap * 1e3, (std::max(3 * block, 0.040) + extraSeconds(block)) * 1e3);
        } else if (extraBlocks > 0 && now - lastDry > std::chrono::duration<double>(kRelaxSeconds)) {
            extraBlocks--;
            lastDry = now;
        }
        if (now - lastLine >= std::chrono::seconds(60)) {
            if (count)
                log("[audio] last 60 s: device queue ran dry %u times (%u since start; longest wait for the game's mix "
                    "%.1f ms, longest gap between blocks %.1f ms); queue target %.0f ms",
                    count, total, maxWait * 1e3, maxGap * 1e3, (std::max(3 * block, 0.040) + extraSeconds(block)) * 1e3);
            count = 0;
            maxWait = maxGap = 0;
            lastLine = now;
        }
    }
} sUnderruns;

void dmaThread() {
    cpu::setCurrent(cpu::Role::Audio);
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    double smoothed = -1;
    for (;;) {
        if (audio::outPaused()) {  // game paused by the host: no audio interrupts
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            next = clock::now();
            sUnderruns.reset();
            continue;
        }
        const u32 len = sDmaLength.load();
        const double secs = len ? (len / 4.0) / rateHz() : 0.005;  // 16-bit stereo frames
        if (audio::outHasDevice()) {
            // Keep about three blocks queued on the device so its clock paces the game, with
            // the interrupts evenly spaced like the console's AI (see waitForGuestMix): one
            // block length apart, stretched or shortened by at most 5% from the smoothed queue
            // level to follow the device clock. (Pushing whenever the queue dropped below the
            // target raised two interrupts back to back now and then: the device drains its
            // queue in whole periods of ~10 ms.)
            const double target = std::max(3 * secs, 0.040) + sUnderruns.extraSeconds(secs);  // + after underruns
            const double queued = audio::outQueuedSeconds();
            smoothed = smoothed < 0 ? queued : smoothed + (queued - smoothed) * 0.05;
            double scale = std::clamp(1.0 + (smoothed - target) * 2.0, 0.95, 1.05);
            if (queued < secs) scale = 0.5;  // about to run dry (start, or after a stall): refill quickly
            const auto now = clock::now();
            next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(secs * scale));
            if (next < now) next = now;  // late (host stall): never catch up with interrupts back to back
            std::this_thread::sleep_until(next);
        } else {
            next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(secs));
            if (clock::now() > next + std::chrono::milliseconds(100)) next = clock::now();
            std::this_thread::sleep_until(next);
        }
        const u32 cb = sDmaCallback.load();
        if (!sDmaRunning.load()) {
            if (audio::outHasDevice()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            sUnderruns.reset();
            continue;
        }
        const auto waitStart = clock::now();
        bool mixed = true;
        if (cb) {
            static const bool sQueueBound = [] {
                const char* e = std::getenv("LM_AI_QUEUE_BOUND");
                return e && *e == '1';
            }();
            const double maxWait = sQueueBound && audio::outHasDevice()
                                       ? std::clamp(audio::outQueuedSeconds() - 0.004, 0.0, 0.1)
                                       : 0.1;
            mixed = waitForGuestMix(maxWait);
            if (!mixed && audio::g_log)
                log("[audio] AI interrupt without the game's mix (waited %.1f ms, %.1f ms queued)", maxWait * 1e3,
                    audio::outQueuedSeconds() * 1e3);
        }
        if (audio::outHasDevice()) {
            const double queued = audio::outQueuedSeconds();
            const double waited = std::chrono::duration<double>(clock::now() - waitStart).count();
            sUnderruns.push(queued, secs, waited);
            if (audio::g_log) sPacing.block(secs, queued, waited, !mixed);
        }
        // The block programmed by the last AIInitDMA starts playing now (the AI repeats
        // the previous block if the game did not supply a new one in time).
        if (const u32 addr = sDmaAddr.load(); addr && len) audio::outPushGuestBlock(addr, len);
        if (cb) {
            sAwaitingMix = true;
            sched::postIrq([cb](Ctx& ic) { guestCall(ic, cb); });
        }
    }
}
}  // namespace

// The game has mixed the DSP output into the next AI block for the last AI interrupt
// (Jac_imixcopy; shared with the SMS port, whose mix hook is game code).
void audio::noteGuestMixed() { sAwaitingMix = false; }

HLE(AIInit) {}
HLE(AIInitDMA) {
    sDmaAddr = c.r[3];
    sDmaLength = c.r[4];
}
HLE(AIStartDMA) {
    sDmaRunning = true;
    audio::outStart(rateHz());
    static bool sStarted = false;  // (a detached thread is never joinable: test a flag)
    if (!sStarted) {
        sStarted = true;
        sDmaThread = std::thread(dmaThread);
        sDmaThread.detach();
    }
}
HLE(AIStopDMA) { sDmaRunning = false; }
HLE(AIGetDMAStartAddr) { c.r[3] = sDmaAddr; }
HLE(AISetDSPSampleRate) {
    sDspRate = c.r[3];
    audio::outSetRate(rateHz());
}
HLE(AIGetDSPSampleRate) { c.r[3] = sDspRate; }
HLE(AIRegisterDMACallback) {
    const u32 old = sDmaCallback;
    sDmaCallback = c.r[3];
    c.r[3] = old;
}
HLE(AIRegisterStreamCallback) {
    const u32 old = sStreamCallback;
    sStreamCallback = c.r[3];
    c.r[3] = old;
}
// DVD audio streaming (DTK). JAudio's StreamMain (jaudio/streamctrl.c) supports it,
// but the disc has no DTK tracks: LM's streamed music is AFC played through the DSP.
HLE(AISetStreamSampleRate) {}
HLE(AISetStreamVolLeft) {}
HLE(AISetStreamVolRight) {}
HLE(AIResetStreamSampleCount) {}
HLE(AISetStreamPlayState) {}
HLE(AISetStreamTrigger) {}
HLE(AIGetStreamSampleCount) { c.r[3] = 0; }
HLE(AIGetStreamTrigger) { c.r[3] = 0; }

HLE(DSPInit) {}
HLE(DSPCheckMailToDSP) { c.r[3] = 0; }
HLE(DSPCheckMailFromDSP) { c.r[3] = 0; }
HLE(DSPReadMailFromDSP) { c.r[3] = 0; }
HLE(DSPSendMailToDSP) {}
HLE(DSPAssertInt) {}

}  // namespace recomp

namespace recomp {
// ---- The game's sound mode (docs/AUDIO.md, "Surround") ----
// The options menu, and the code that applies saved options (fn_8005582C, fn_800558C8,
// options struct byte 0: 0 mono, 1 stereo, 2 surround), all go through fn_8018A70C(mode),
// which stores the mode in JAIBasic +0x41 (sequences then take their Dolby surround
// send from their own table in mode 2, 0 otherwise), sets JAudio's output mode (0 mono:
// centred pan, no surround send; 1 otherwise) and the AFC streamer's mono flag. The port's
// F1 > Sound > Output is the same setting: saved options are overridden with it, the
// game's own menu changes it, and changing it in F1 applies it to the game.
namespace {
int sGameMode = -1;           // last mode applied through fn_8018A70C
bool sApplyingSaved = false;  // inside fn_8005582C / fn_800558C8
bool sPortApplying = false;   // our own call (syncSoundMode)
u32 sOptions = 0;             // the game's options struct (byte 0 = sound mode)

int modeFor(port::AudioOutput o) {
    switch (o) {
    case port::AudioOutput::Mono: return 0;
    case port::AudioOutput::Stereo: return 1;
    default: return 2;
    }
}

// The game asked for `mode` from its own options menu: F1 follows (keeping 5.1 / 7.1 when
// the game chose surround and one of those was already selected).
void adoptGameMode(int mode) {
    auto& a = port::settings().audio;
    if (modeFor(a.output) == mode) return;
    a.output = mode == 0 ? port::AudioOutput::Mono : mode == 1 ? port::AudioOutput::Stereo : port::AudioOutput::Surround;
    port::saveSettings();
    if (audio::g_log) log("[audio] game options: sound mode %d -> F1 output %d", mode, int(a.output));
}

void applySavedOptions(Ctx& c, const char* name) {
    sOptions = c.r[3];
    const int want = modeFor(port::settings().audio.output);
    wr8(sOptions, u32(want));
    sApplyingSaved = true;
    callOriginal(c, name);
    sApplyingSaved = false;
    wr8(sOptions, u32(want));  // the original forces mono when the console's SRAM says mono
}

// F1 changed the output: apply it to the game (from JAudio's audio thread, where the
// mode is read). Only once the game has set a mode itself (JAudio is up).
void syncSoundMode(Ctx& c) {
    // Test aid: LM_AUDIO_SWITCH=<subframe>:<output> changes F1 > Sound > Output (0 stereo,
    // 1 mono, 2 surround, 3 5.1, 4 7.1) after that many DSP subframes (400 per second).
    static const char* sSwitch = std::getenv("LM_AUDIO_SWITCH");
    if (sSwitch && *sSwitch) {
        static u32 sCount = 0, sAt = 0, sTo = 0;
        if (!sCount) std::sscanf(sSwitch, "%u:%u", &sAt, &sTo);
        if (++sCount == sAt) port::settings().audio.output = port::AudioOutput(sTo);
    }
    if (sGameMode < 0) return;
    const int want = modeFor(port::settings().audio.output);
    if (want == sGameMode) return;
    static const u32 fn = guestSymbol("fn_8018A70C");
    if (!fn) return;
    sPortApplying = true;
    guestCall(c, fn, u32(want));
    sPortApplying = false;
    if (sOptions) wr8(sOptions, u32(want));  // the options menu shows it, saves keep it
}
}  // namespace

int audio::gameSoundMode() { return sGameMode; }

HLE(fn_8005582C) { applySavedOptions(c, "fn_8005582C"); }
HLE(fn_800558C8) { applySavedOptions(c, "fn_800558C8"); }
HLE(fn_8018A70C) {
    const int req = int(c.r[3]);
    if (sApplyingSaved || sPortApplying) {
        c.r[3] = u32(modeFor(port::settings().audio.output));
    } else {
        adoptGameMode(req);
    }
    if (audio::g_log && int(c.r[3]) != sGameMode)
        log("[audio] sound mode %d (game asked %d%s)", int(c.r[3]), req,
            sApplyingSaved ? ", saved options" : sPortApplying ? ", F1" : ", options menu");
    sGameMode = int(c.r[3]);
    callOriginal(c, "fn_8018A70C");
}

// Jac_imixcopy(bufR, bufL, dac, frames): Jac_VframeWork interleaves the finished DSP
// buffer pair into the next AI block. The output keeps that buffer's side data (centre /
// surround buses) for when the block plays (discrete 5.1 / 7.1).
HLE(Jac_imixcopy__FPsPsPsl) {
    // Test aid LM_TEST_MIX_STALL=<ms>,<period s>: the game's mix comes <ms> late every <period>
    // seconds, as when the guest CPU is held elsewhere (underrun handling, docs/AUDIO.md "Underruns").
    static const std::pair<int, double> sStall = [] {
        const char* e = std::getenv("LM_TEST_MIX_STALL");
        if (!e) return std::pair<int, double>{0, 0};
        const char* comma = std::strchr(e, ',');
        return std::pair<int, double>{std::atoi(e), comma ? std::atof(comma + 1) : 5.0};
    }();
    if (sStall.first > 0) {
        static auto sNext = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        if (std::chrono::steady_clock::now() >= sNext) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sStall.first));
            sNext = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                           std::chrono::duration<double>(sStall.second));
        }
    }
    const u32 bufL = c.r[4], dac = c.r[5], frames = c.r[6];
    callOriginal(c, "Jac_imixcopy__FPsPsPsl");
    audio::outTagBlock(dac, bufL, frames);
    audio::noteGuestMixed();  // the game has mixed for the last AI interrupt (waitForGuestMix)
}

// ---- Music / sound-effect split (F1 > Sound volume sliders; docs/AUDIO.md "Volume sliders") ----
// JAudio keeps 64 physical channels (dspch_, 16 bytes: +0 DSP voice number, +8 owner) in one
// array. AllocDSPchannel returns an entry of it, which gives the array's address on either
// release. The owner is the logical channel (jc_) the note plays on; jc_ +4 is its channel
// manager (jcs_), which lives at +0xD8 of a sequence track (seqp_, NoteON passes the track's or
// its parent's); seqp_ +0 is the sequence data every track of one sequence shares
// (Init_Track). Notes of the sound-effect sequence are effects, notes of any other sequence are
// music. Same JAudio code and struct layout on GLME01 and GLMP01.
namespace {
std::atomic<u32> sDspchBase{0};
std::atomic<u32> sSeData{0};  // data of the sound-effect sequence (LuigiSe.bms)
}  // namespace

HLE(AllocDSPchannel__FUlUl) {
    callOriginal(c, "AllocDSPchannel__FUlUl");
    const u32 ch = c.r[3];
    if (!gu::inMem1(ch)) return;
    const u32 idx = rd8(ch);
    if (idx < 64 && !sDspchBase.load(std::memory_order_relaxed)) {
        sDspchBase = ch - idx * 16;
        if (audio::g_log) log("[audio] physical channel table at %08X", ch - idx * 16);
    }
}

// Play_1shot / Play_1shot_Perc (jcs_*, SOUNDID* {bank, program/key, key, velocity}, ...) start
// a note on a logical channel they return: remember its instrument bank.
namespace {
std::mutex sBankM;
std::unordered_map<u32, u16> sJcBank;  // jc_ -> bank << 8 | program
void notePlay(Ctx& c, const char* name) {
    const u32 id = c.r[4];
    const u16 bp = gu::inMem1(id) ? u16(rd8(id) << 8 | rd8(id + 1)) : 0xFFFF;
    callOriginal(c, name);
    if (c.r[3] && bp != 0xFFFF) {
        std::lock_guard lk(sBankM);
        sJcBank[c.r[3]] = bp;
    }
}
}  // namespace
HLE(Play_1shot__FP4jcs_8SOUNDID_Ul) { notePlay(c, "Play_1shot__FP4jcs_8SOUNDID_Ul"); }
HLE(Play_1shot_Perc__FP4jcs_8SOUNDID_Ul) { notePlay(c, "Play_1shot_Perc__FP4jcs_8SOUNDID_Ul"); }

int audio::voiceBank(u32 voice) {
    const u32 base = sDspchBase.load(std::memory_order_relaxed);
    if (!base || voice >= 64) return -1;
    u32 e = base + voice * 16;
    if (rd8(e) != voice) return -1;
    const u32 jc = rd32(e + 8);
    std::lock_guard lk(sBankM);
    const auto it = sJcBank.find(jc);
    return it == sJcBank.end() ? -1 : it->second;
}

u32 audio::voiceSequence(u32 voice) {
    const u32 base = sDspchBase.load(std::memory_order_relaxed);
    if (!base || voice >= 64) return 0;
    const u32 e = base + voice * 16;
    if (rd8(e) != voice) return 0;
    const u32 jc = rd32(e + 8);
    if (!gu::inMem1(jc)) return 0;
    const u32 jcs = rd32(jc + 4);
    return gu::inMem1(jcs) ? jcs - 0xD8 : 0;
}

void audio::noteSequenceData(u32 data, u32 size) {
    static const u32 seSize = unused2::seSequenceSize();
    if (seSize && size == seSize && gu::inMem1(data)) {
        sSeData = data;
        if (g_log) log("[audio] sound-effect sequence at %08X (%X bytes)", data, size);
    }
}

audio::VoiceKind audio::voiceKind(u32 voice) {
    const u32 base = sDspchBase.load(std::memory_order_relaxed), se = sSeData.load(std::memory_order_relaxed);
    if (!base || !se || voice >= 64) return VoiceKind::Unknown;
    u32 e = base + voice * 16;
    if (rd8(e) != voice) {  // entries not in voice order: find it
        e = 0;
        for (u32 k = 0; k < 64 && !e; k++)
            if (rd8(base + k * 16) == voice) e = base + k * 16;
        if (!e) return VoiceKind::Unknown;
    }
    const u32 jc = rd32(e + 8);
    if (!gu::inMem1(jc)) return VoiceKind::Unknown;
    const u32 jcs = rd32(jc + 4);
    if (!gu::inMem1(jcs) || !gu::inMem1(jcs - 0xD8)) return VoiceKind::Unknown;
    const u32 data = rd32(jcs - 0xD8);
    if (!gu::inMem1(data)) return VoiceKind::Unknown;
    return data == se ? VoiceKind::Effect : VoiceKind::Music;
}

// JAudio's DSP overload check (UpdateDSPchannelAll, the only caller): every DSP subframe update
// takes OSGetTime; when the time since the previous one exceeds 1/1.1 of the gap before the
// frame's first update, it decides the DSP cannot keep up and force-stops the lowest active
// voice, whatever its priority, through BreakLowerActiveDSPchannel(0xFF). Our DSP stand-in
// mixes a subframe in microseconds and never falls behind; the check fires only when the guest
// audio thread itself was held up between two subframes (another guest thread holding the guest
// CPU through a long host call, a host stall, the AFC streamer's start-up work): at the title the
// stream's right channel was cut in every run, and in busy scenes sounds and music voices were cut
// short (issue #25, docs/AUDIO.md "The DSP overload check"). The stop is skipped; logged (the first
// 5, then every 50th). A/B: LM_DSP_OVERLOAD_CHECK=1 lets it stop voices as before.
HLE(BreakLowerActiveDSPchannel__FUc) {
    static const bool sOn = [] {
        const char* e = std::getenv("LM_DSP_OVERLOAD_CHECK");
        return e && *e == '1';
    }();
    static u32 sCount = 0;
    const u32 prio = c.r[3];
    if (++sCount <= 5 || sCount % 50 == 0)
        log("[audio] JAudio's DSP overload check %s a voice (priority limit %u, %u since start)", sOn ? "stopped" : "would stop",
            prio, sCount);
    if (sOn) callOriginal(c, "BreakLowerActiveDSPchannel__FUc");
    else c.r[3] = 0;
}

// JAudio DSP driver hooks (tools/recomp/hooks.txt).
HLE(DspBoot__Fv) {}  // nothing to upload: the "microcode" is audio_dsp.cpp

// DSPSendCommands(u32* cmds, u32 count): one mailbox packet.
HLE(DSPSendCommands__FPUlUl) {
    u32 words[16];
    const u32 n = std::min<u32>(c.r[4], 16);
    for (u32 i = 0; i < n; i++) words[i] = rd32(c.r[3] + i * 4);
    audio::dspCommand(words, n);
    c.r[3] = 0;
}

// The DSP halts after every subframe until the CPU has updated the voice blocks.
// Releasing it mixes the next subframe; its completion interrupt (__DspSync ->
// DspSync) wakes JAudio's audio thread for the next update.
HLE(DSPReleaseHalt__Fv) {
    syncSoundMode(c);
    if (!audio::dspRunSubframe()) return;
    static const u32 dspSync = guestSymbol("DspSync__Fv");
    sched::postIrq([](Ctx& ic) { guestCall(ic, dspSync); });
}
HLE(DSPWaitFinish__Fv) {}
HLE(DiplSec__FUl) {}

// ---- JAI sound-effect starts: no interrupt between taking a JAISound and setting it up ----
// Each SE category keeps a free list and an active list of JAISounds (descriptor at
// [JAISeMgr+0x1E8] + cat*0xC: +0 free head, +4 active head; links prev +0x28, next +0x2C).
// The start functions below take a sound off the free list onto the active list
// (fn_8018FF88) and only then fill it in (+0x38 parameter block, +0x01 state, fn_801974C0).
// The game also starts sounds from its scene-loading thread (priority 17, e.g. during the map
// setup after the opening): if an interrupt wakes the main thread inside that window, the
// main thread's SE update (fn_80191A1C, fn_80191F38) sees the half-made sound, releases it
// into the wrong category's free list (fn_8018FFE0) and the lists become circular; the next
// SE update then never returns (game frozen, window not responding). On the console the
// window is a few instructions long; here interrupts are taken at loop back-edges inside it,
// and while a cutscene is skipped (fast-forward: retrace interrupts back to back) it happened
// in about 1 of 15 opening skips. Running the start with interrupts off (as if the game had
// wrapped it in OSDisableInterrupts) closes the window; the interrupt is taken right after.
namespace {
void callWithInterruptsOff(Ctx& c, const char* name) {
    const u32 ee = c.msr & 0x8000u;
    c.msr &= ~0x8000u;
    callOriginal(c, name);
    c.msr |= ee;
    if (ee) c.poll();
}
}  // namespace
HLE(fn_8019754C) { callWithInterruptsOff(c, "fn_8019754C"); }
// Sound-effect start (r4 = the JAISound** handle, r6 = the sound id): Luigi's voice pitch,
// silly_world.cpp.
HLE(fn_80197810) {
    const u32 handle = c.r[4], id = c.r[6];
    callWithInterruptsOff(c, "fn_80197810");
    sillyworld::onSeStarted(c, id, handle);
}
HLE(fn_80197B30) { callWithInterruptsOff(c, "fn_80197B30"); }
// The AFC stream scheduler (US fn_80193B20, issue #25) has the same kind of window: it runs on
// the main thread (priority 16) and on the sound thread (priority 17), and for each request in
// state 2 it starts the stream (fn_8019480C: DVDOpen, header, the first 0x1680-byte piece via
// DVDReadAsyncPrio) and only then sets state 3. A main thread woken inside a start saw the request
// still in state 2 and started the stream again: the first piece was read twice, the second
// never, and the stale completion of the first read was taken for the new one, so the ADPCM
// stream played with broken history (a loud crackle/burst over the first second, e.g. TMOpen at
// the opening, TMansion's thunder on the walk up to the mansion). Here the window spans HLE calls
// where interrupts are taken (in 1.1.5 also the whole host read, see hle_dvd.cpp); with
// interrupts off, as for the SE starts, the check, start and state change are one step.
HLE(fn_80193B20) { callWithInterruptsOff(c, "fn_80193B20"); }

// JAI sound-effect frame update (US fn_80191A1C, r3 = JAI SE manager): for each category
// (count at r13-0x6B4) walks the active JAISound list (head: [[mgr+0]+0x1E8] + cat*0xC + 4,
// next at +0x2C). Diagnostic: a list that loops back on itself hangs the game in here.
HLE(fn_80191A1C) {
    static const bool sCheck = std::getenv("LM_JAI_CHECK") != nullptr;
    if (sCheck) {
        const u32 mgr = c.r[3];
        const u32 cats = rd32(c.r[13] - 0x6B4);
        const u32 base = rd32(rd32(mgr) + 0x1E8);
        static bool sReported = false;
        for (u32 cat = 0; cat < cats && cat < 64 && !sReported; cat++) {
            u32 slow = rd32(base + cat * 0xC + 4), fast = slow;
            bool loop = false;
            while (fast && rd32(fast + 0x2C)) {
                fast = rd32(rd32(fast + 0x2C));
                slow = rd32(slow + 0x2C);
                if (fast == slow && fast) {
                    loop = true;
                    break;
                }
            }
            if (!loop) continue;
            sReported = true;
            std::string chain;
            u32 n = rd32(base + cat * 0xC + 4);
            for (int i = 0; i < 24 && n; i++, n = rd32(n + 0x2C)) {
                char b[16];
                std::snprintf(b, sizeof(b), " %08X", n);
                chain += b;
            }
            log("[jai] SE category %u list loops (mgr %08X, list base %08X):%s ...", cat, mgr, base, chain.c_str());
            sched::dumpThreads();
        }
    }
    callOriginal(c, "fn_80191A1C");
}
}  // namespace recomp
