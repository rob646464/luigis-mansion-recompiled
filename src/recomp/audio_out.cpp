// Host audio output. The AI DMA blocks the game hands to AIInitDMA (big-endian s16
// stereo at the console's 32028 Hz) are turned into the output layout chosen in
// F1 > Sound (stereo / mono / matrix surround, or discrete 5.1 / 7.1 built from the DSP's
// own centre and surround buses), resampled to the device rate with a windowed-sinc
// filter (or by SDL), and queued on an SDL3 audio stream. docs/AUDIO.md.
//
// LM_AUDIO_WAV=<path> records exactly what goes to the device (all channels, device
// rate); LM_AUDIO_WAV_RAW=<path> records the AI stream itself (stereo, left first, the
// console's rate, before volume, swap, layout and resampling). LM_AUDIO=0 disables the
// device.
#include "port/settings.h"
#include "recomp/hidden_mansion.h"
#include "recomp/audio.h"
#include "recomp/hle.h"
#include "recomp/skip.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace recomp::audio {
namespace {

// Channel order of the AI DMA frames: the console plays the first sample of each
// stereo frame on the right. JAudio's Jac_VframeWork interleaves the DSP's second
// output buffer (bus 0x0D60) into that slot and bus 0x0D00 into the second, so
// 0x0D00 is left (docs/AUDIO.md, "Channel order", has the in-game check).
constexpr bool kRightFirst = true;

// ---- Windowed-sinc resampler ----
// Polyphase FIR: 32 zero crossings each side (64 taps), Kaiser window (beta 8), cutoff at
// 0.92 of the lower Nyquist frequency. For 32028 -> 48000 Hz: flat (-0.1 dB) to 13.8 kHz,
// -80 dB from 16.0 kHz (the source's Nyquist), so the images of the 32 kHz signal that a
// linear or short filter leaves between 16 and 24 kHz are removed. 512 phases with linear
// interpolation between them; every phase is normalised to unity gain at DC.
constexpr double kPi = 3.14159265358979323846;

class Resampler {
public:
    // Widest layout the output can ask for (7.1): outPushGuestBlock picks 2, 6 or 8 channels.
    static constexpr int kMaxChannels = 8;

    void configure(double inRate, double outRate, int channels) {
        if (inRate == mIn && outRate == mOut && channels == mCh) return;
        mIn = inRate;
        mOut = outRate;
        mCh = channels;
        mStep = inRate / outRate;
        const double fc = 0.92 * std::min(1.0, outRate / inRate);
        mTable.assign(size_t(kPhases + 1) * kTaps, 0.0f);
        auto bessel0 = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 50; k++) {
                term *= (x / (2 * k)) * (x / (2 * k));
                sum += term;
            }
            return sum;
        };
        const double beta = 8.0, i0b = bessel0(beta);
        for (int p = 0; p <= kPhases; p++) {
            float* h = &mTable[size_t(p) * kTaps];
            double sum = 0;
            for (int j = 0; j < kTaps; j++) {
                // Tap j multiplies x[i0 + j - (kZero - 1)]; its distance from the output point.
                const double t = double(p) / kPhases + (kZero - 1) - j;
                const double x = fc * t;
                const double sinc = x == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const double r = t / kZero;
                const double w = std::abs(r) >= 1 ? 0.0 : bessel0(beta * std::sqrt(1 - r * r)) / i0b;
                h[j] = float(fc * sinc * w);
                sum += h[j];
            }
            for (int j = 0; j < kTaps; j++) h[j] = float(h[j] / sum);
        }
        reset();
    }
    void reset() {
        mHist.assign(size_t(kZero - 1) * mCh, 0.0f);  // leading silence: the first output is at sample 0
        mPos = kZero - 1;
    }
    // Appends `frames` interleaved input frames and writes every output frame that is now
    // complete to `out` (appended).
    void process(const float* in, size_t frames, std::vector<float>& out) {
        mHist.insert(mHist.end(), in, in + frames * mCh);
        const size_t avail = mHist.size() / mCh;
        while (true) {
            const size_t i0 = size_t(mPos);
            if (i0 + kZero >= avail) break;
            const double fp = (mPos - double(i0)) * kPhases;
            const int ph = std::min(int(fp), kPhases - 1);
            const float a = float(fp - ph);
            const float* h0 = &mTable[size_t(ph) * kTaps];
            const float* h1 = h0 + kTaps;
            const float* x = &mHist[(i0 - (kZero - 1)) * mCh];
            // One accumulator per channel rather than a read-modify-write of the output vector: x and
            // the output can alias as far as the compiler knows, so the partial sums stay in
            // registers. Same products, same order of additions.
            float acc[kMaxChannels] = {};
            for (int j = 0; j < kTaps; j++) {
                const float h = h0[j] + (h1[j] - h0[j]) * a;
                for (int c = 0; c < mCh; c++) acc[c] += h * x[j * mCh + c];
            }
            const size_t o = out.size();
            out.resize(o + mCh, 0.0f);
            for (int c = 0; c < mCh; c++) out[o + c] = acc[c];
            mPos += mStep;
        }
        // Drop the input no future output needs.
        const size_t drop = std::min(size_t(mPos) - (kZero - 1), avail);
        mHist.erase(mHist.begin(), mHist.begin() + drop * mCh);
        mPos -= double(drop);
    }

private:
    static constexpr int kZero = 32, kTaps = 2 * kZero, kPhases = 512;
    double mIn = 0, mOut = 0, mStep = 1, mPos = 0;
    int mCh = 0;
    std::vector<float> mTable, mHist;
};

// ---- WAV capture ----
struct Wav {
    std::FILE* f = nullptr;
    u32 bytes = 0, rate = 0;
    int channels = 0;
    bool stopped = false;

    void header() {
        auto le32 = [](u8* p, u32 v) { p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24); };
        u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0};
        h[22] = u8(channels);
        le32(h + 4, 36 + bytes);
        le32(h + 24, rate);
        le32(h + 28, rate * channels * 2);
        h[32] = u8(channels * 2);
        h[34] = 16;
        std::memcpy(h + 36, "data", 4);
        le32(h + 40, bytes);
        std::fseek(f, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof h, f);
        std::fseek(f, 0, SEEK_END);
    }
    void open(const char* path) {
        f = std::fopen(path, "wb");
        if (f) log("[audio] recording to %s", path);
    }
    // Interleaved s16 frames. The format is fixed by the first write; a later change of
    // channels or rate ends the recording.
    void write(const s16* data, size_t frames, int ch, u32 r) {
        if (!f || stopped || !frames) return;
        if (!channels) {
            channels = ch;
            rate = r;
            header();
        } else if (ch != channels || r != rate) {
            log("[audio] WAV recording stopped: the output changed to %d channels at %u Hz", ch, r);
            stopped = true;
            header();
            std::fflush(f);
            return;
        }
        const u32 n = u32(frames * ch * 2);
        std::fwrite(data, 1, n, f);
        const u32 before = bytes;
        bytes += n;
        if ((before >> 16) != (bytes >> 16)) {  // keep the file valid while running (runs get killed)
            header();
            std::fflush(f);
        }
    }
};

std::mutex sM;
// The per-block side data (centre / surround buses for discrete layouts) has its own lock, sSideM:
// outTagBlock runs on a guest thread (Jac_imixcopy), so it must never wait behind sM, which the AI
// DMA thread holds across the resampler, the limiter and the SDL calls. sSideM is only ever held for
// one copy of one block. See docs/AUDIO.md "AI output and pacing".
std::mutex sSideM;
SDL_AudioStream* sStream = nullptr;
bool sTried = false;
double sRate = kConsoleRate32;  // AI sample rate of the blocks
Wav sWav, sWavRaw;
std::atomic<int> sDevChannels{0};
std::atomic<int> sDevRate{0};
u64 sDevQueryTick = 0;
// Format currently set as the SDL stream's input.
int sInChannels = 2, sInRate = 32028;
Resampler sResampler;
char sStatus[128] = "No audio device";

// Side data per AI block (outTagBlock), keyed by the block's guest address.
struct BlockSide {
    u32 dac = 0;
    std::vector<SideSample> v;
};
std::array<BlockSide, 6> sBlockSide;
u32 sBlockNext = 0;

void queryDevice() {
    if (!sStream) return;
    SDL_AudioSpec dev{};
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(sStream), &dev, nullptr)) {
        sDevChannels = dev.channels;
        sDevRate = dev.freq;
    }
    sDevQueryTick = SDL_GetTicks();
}

void openDevice() {
    sTried = true;
    if (const char* wav = std::getenv("LM_AUDIO_WAV"); wav && *wav) sWav.open(wav);
    if (const char* wav = std::getenv("LM_AUDIO_WAV_RAW"); wav && *wav) sWavRaw.open(wav);
    if (const char* e = std::getenv("LM_AUDIO"); e && *e == '0') return;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log("[audio] SDL audio init failed: %s", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, sInChannels, sInRate};
    sStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!sStream) {
        log("[audio] no audio device: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(sStream);
    queryDevice();
    log("[audio] output started; device: %d channels at %d Hz; console rate %.2f Hz", sDevChannels.load(), sDevRate.load(), sRate);
}

// Copies the side data tagged for the block at `dac` into `out` (zero-filled when there is none,
// or when the tag is shorter than the block). AI DMA thread only.
void sideFor(u32 dac, SideSample* out, u32 frames) {
    std::lock_guard lk(sSideM);
    for (const auto& b : sBlockSide) {
        if (b.dac != dac || b.v.empty()) continue;
        const u32 n = std::min<u32>(frames, u32(b.v.size()));
        std::copy(b.v.begin(), b.v.begin() + n, out);
        std::fill(out + n, out + frames, SideSample{});
        return;
    }
    std::fill(out, out + frames, SideSample{});
}

}  // namespace

void outStart(double rate) {
    std::lock_guard lk(sM);
    if (sTried) return;
    sRate = rate;
    openDevice();
}

void outSetRate(double rate) {
    std::lock_guard lk(sM);
    sRate = rate;
}

std::atomic<bool> sPaused{false};

void outSetPaused(bool paused) {
    if (sPaused.exchange(paused) == paused) return;
    std::lock_guard lk(sM);
    if (!sStream) return;
    if (paused) {
        SDL_PauseAudioStreamDevice(sStream);
    } else {
        SDL_ClearAudioStream(sStream);
        sResampler.reset();
        SDL_ResumeAudioStreamDevice(sStream);
    }
}

bool outPaused() { return sPaused.load(); }

void outTagBlock(u32 dac, u32 bufL, u32 frames) {
    // Called on a guest thread, from Jac_imixcopy (hle_audio.cpp): fill a reused buffer outside the
    // lock, then publish it with one copy (sSideM above).
    static thread_local std::vector<SideSample> v;
    v.resize(frames);
    if (!dspSideData(bufL, v.data(), frames)) return;
    std::lock_guard lk(sSideM);
    BlockSide* b = nullptr;
    for (auto& x : sBlockSide)
        if (x.dac == dac) b = &x;
    if (!b) {
        b = &sBlockSide[sBlockNext];
        sBlockNext = (sBlockNext + 1) % sBlockSide.size();
        b->dac = dac;
    }
    b->v = v;
}

std::atomic<unsigned long long> sRawFrames{0};
unsigned long long outRawFrames() { return sRawFrames.load(); }

void outPushGuestBlock(u32 addr, u32 bytes) {
    const u32 frames = bytes / 4;
    if (!frames) return;
    const auto& as = port::settings().audio;
    std::lock_guard lk(sM);
    if (sStream && SDL_GetTicks() - sDevQueryTick > 1000) queryDevice();

    // Raw AI stream, left first (what the console's DAC gets).
    static thread_local std::vector<s16> raw;
    raw.resize(frames * 2);
    for (u32 i = 0; i < frames; i++) {
        const s16 a = s16(rd16(addr + i * 4)), b = s16(rd16(addr + i * 4 + 2));
        raw[i * 2 + 0] = kRightFirst ? b : a;
        raw[i * 2 + 1] = kRightFirst ? a : b;
    }
    sWavRaw.write(raw.data(), frames, 2, u32(std::lround(sRate)));
    sRawFrames += frames;

    // Output layout. Discrete surround needs a device with at least 6 channels (without a
    // device, for WAV captures, the chosen layout is used as is); otherwise it falls back to
    // the matrix-encoded stereo the game produces in surround mode.
    int ch = 2;
    const int dev = sStream ? sDevChannels.load() : 8;
    if (as.output == port::AudioOutput::Surround51 || as.output == port::AudioOutput::Surround71) {
        const int want = as.output == port::AudioOutput::Surround71 ? 8 : 6;
        ch = dev >= want ? want : dev >= 6 ? 6 : 2;
    }
    const bool mono = as.output == port::AudioOutput::Mono;
    // Swap left/right (F1), and the PAL Hidden Mansion mirrors the picture, so the speakers too.
    const bool swap = as.swapChannels != hm::mirrorActive();
    const float gain = std::clamp(as.masterVolume, 0.0f, 1.0f) * skip::audioGain() / 32768.0f;
    // Snapshot the tagged side data in one copy under sSideM, then work on it without the lock.
    static thread_local std::vector<SideSample> sideBuf;
    if (ch > 2) {
        sideBuf.resize(frames);
        sideFor(addr, sideBuf.data(), frames);
    }
    const SideSample* side = ch > 2 ? sideBuf.data() : nullptr;

    static thread_local std::vector<float> buf;
    buf.assign(size_t(frames) * ch, 0.0f);
    for (u32 i = 0; i < frames; i++) {
        float l = raw[i * 2], r = raw[i * 2 + 1];
        float* o = &buf[size_t(i) * ch];
        if (ch == 2) {
            if (mono) l = r = (l + r) * 0.5f;
            if (swap) std::swap(l, r);
            o[0] = l * gain;
            o[1] = r * gain;
            continue;
        }
        // Discrete: the fronts without the centre / surround / rear-right parts, those
        // buses on their own speakers. The centre went into each side at 0.707 (a phantom
        // centre), so the centre speaker plays it at full level; the mono surround bus went
        // in at +1 / -1, so each surround speaker plays it at 1 (5.1) or 0.707 (7.1), which
        // keeps its power. LFE stays silent (the game has no LFE bus).
        const SideSample s = side ? side[i] : SideSample{};
        float fl = l - s.lExtra, fr = r - s.rExtra;
        float sl = s.s, sr = float(s.s) + s.rr, bl = 0, br = 0;
        if (ch == 8) {
            constexpr float k = 0.70710678f;
            sl = s.s * k;
            sr = s.s * k + s.rr * k;
            bl = sl;
            br = sr;
        }
        if (swap) {
            std::swap(fl, fr);
            std::swap(sl, sr);
            std::swap(bl, br);
        }
        o[0] = fl * gain;
        o[1] = fr * gain;
        o[2] = s.c * gain;
        o[3] = 0;
        if (ch == 6) {  // SDL 5.1: FL FR FC LFE BL BR (BL/BR may be the side pair)
            o[4] = sl * gain;
            o[5] = sr * gain;
        } else {  // SDL 7.1: FL FR FC LFE BL BR SL SR
            o[4] = bl * gain;
            o[5] = br * gain;
            o[6] = sl * gain;
            o[7] = sr * gain;
        }
    }

    // Resampling. Without a device, captures use 48 kHz.
    const int devRate = sStream && sDevRate.load() > 0 ? sDevRate.load() : 48000;
    static thread_local std::vector<float> res;
    const float* outData = buf.data();
    size_t outFrames = frames;
    int outRate = int(std::lround(sRate));
    static bool sWasSinc = false;
    if (as.sincResampler) {
        sResampler.configure(sRate, devRate, ch);
        if (!sWasSinc) sResampler.reset();  // drop history from before it was switched off
        res.clear();
        sResampler.process(buf.data(), frames, res);
        outData = res.data();
        outFrames = res.size() / ch;
        outRate = devRate;
    }
    sWasSinc = as.sincResampler;
    // Peak limiter. The DSP output is saturated 16-bit, so the stream itself never exceeds
    // full scale, but the windowed-sinc filter rings above it (Gibbs overshoot) on the loudest,
    // already saturated peaks (Bogmire's fight: up to 0.5 dB over), and the device would clip
    // those hard. Above -1 dB the level bends smoothly towards full scale (tanh knee, slope 1
    // at the threshold), so nothing below it changes. docs/AUDIO.md "Peaks".
    {
        constexpr float kKnee = 0.891f;  // -1 dBFS
        float* p = as.sincResampler ? res.data() : buf.data();
        const size_t n = outFrames * size_t(ch);
        for (size_t k = 0; k < n; k++) {
            const float a = std::fabs(p[k]);
            if (a > kKnee) p[k] = std::copysign(kKnee + (1.0f - kKnee) * std::tanh((a - kKnee) / (1.0f - kKnee)), p[k]);
        }
    }
    if (sStream) {
        if (ch != sInChannels || outRate != sInRate) {
            sInChannels = ch;
            sInRate = outRate;
            const SDL_AudioSpec spec{SDL_AUDIO_F32, ch, outRate};
            SDL_SetAudioStreamFormat(sStream, &spec, nullptr);
        }
        SDL_PutAudioStreamData(sStream, outData, int(outFrames * ch * sizeof(float)));
    }
    std::snprintf(sStatus, sizeof sStatus, "%s, %d Hz (%s)%s",
                  ch == 8 ? "7.1" : ch == 6 ? "5.1" : mono ? "Mono"
                  : as.output == port::AudioOutput::Stereo ? "Stereo" : "Matrix-encoded surround (stereo)", outRate,
                  as.sincResampler ? "windowed-sinc from 32028 Hz" : "SDL resampler",
                  sStream ? "" : ", no device");
    if (sWav.f) {
        static thread_local std::vector<s16> pcm;
        pcm.resize(outFrames * ch);
        for (size_t i = 0; i < pcm.size(); i++)
            pcm[i] = s16(std::clamp(std::lround(outData[i] * 32768.0f), -32768L, 32767L));
        sWav.write(pcm.data(), outFrames, ch, u32(outRate));
    }
}

double outQueuedSeconds() {
    std::lock_guard lk(sM);
    if (!sStream) return 0;
    return SDL_GetAudioStreamQueued(sStream) / (double(sizeof(float)) * sInChannels * sInRate);
}

void outShutdown() {
    std::lock_guard lk(sM);
    if (sStream) SDL_DestroyAudioStream(sStream);
    sStream = nullptr;
    sTried = true;  // no reopening while shutting down
}

bool outHasDevice() {
    std::lock_guard lk(sM);
    return sStream != nullptr;
}

int outDeviceChannels() { return sDevChannels.load(); }

const char* outStatus() { return sStatus; }

}  // namespace recomp::audio
