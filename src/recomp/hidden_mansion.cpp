// PAL Hidden Mansion rules on the US recompiled build -- prototype (see hidden_mansion.h and
// docs/HIDDEN_MANSION.md). Original port code: it reads the game's own flag, serves files
// from an overlay the user builds from their own discs, and adjusts a few literals.
#include "recomp/hidden_mansion.h"

#include "port/install.h"
#include "port/options.h"
#include "port/settings.h"
#include "recomp/gameplay.h"
#include "recomp/hle.h"
#include "recomp/mirror_world.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <vector>

namespace recomp::hm {
namespace {

// The game's hidden-mansion flag and sCurMapNo (USA addresses, found with tools/pal_diff.py
// `var pal 804C5784`; VA() gives the running game's: PAL 0x804C5784 / 0x804C578C).
#define kUraFlag VA(0x804D80C0)
#define kCurMapNo VA(0x804D80C8)

// On the USA game the PAL rules are added by the overlay and the hooks below; on the European
// game they are its own, and the USA rules are restored by the reverse overlay and hooks
// (docs/SETUP.md, "USA Hidden Mansion on the European game").
bool usBase() { return port::install().running == port::GameVersion::USA; }
// The Japanese game keeps its own Hidden Mansion: no overlay, no other rules, no flag hiding
// (docs/HIDDEN_MANSION.md "The Japanese game"; port::hiddenMansionPal/Us are NotOnJpn there).
bool jpBase() { return port::install().running == port::GameVersion::JPN; }

// LM_HM_OVERLAY, else the overlay setup built in the data directory, else the first
// orig/hidden_mansion (tools/hidden_mansion_data.py) found walking up from the working
// directory (same search as the disc image).
std::string overlayDir() {
    static const std::string dir = [] {
        const char* env = std::getenv("LM_HM_OVERLAY");
        std::filesystem::path d;
        if (env && *env) {
            d = env;
        } else if (std::filesystem::exists(port::longPath(port::hiddenMansionOverlayDir() / "manifest.txt"))) {
            d = port::hiddenMansionOverlayDir();  // built by setup (docs/SETUP.md)
        } else {
            for (auto p = std::filesystem::current_path();; p = p.parent_path()) {
                if (std::filesystem::exists(p / "orig" / "hidden_mansion" / "manifest.txt")) {
                    d = p / "orig" / "hidden_mansion";
                    break;
                }
                if (p == p.parent_path()) {
                    d = "orig/hidden_mansion";
                    break;
                }
            }
        }
        const std::u8string u8 = d.generic_u8string();  // UTF-8: any user name survives
        return std::string(u8.begin(), u8.end()) + "/";
    }();
    return dir;
}

// A file of the overlay (UTF-8, '/' separators, from overlayDir()) as an extended path, so a
// deep %APPDATA% works without Windows' long path policy (docs/SETUP.md "Long paths").
std::filesystem::path hostFile(const std::string& utf8) {
    return port::longPath(std::filesystem::path(std::u8string(utf8.begin(), utf8.end())));
}

struct Redirect {
    const char* guestPath;  // as the game spells it
    const char* overlayFile;
    bool onlyWhileHidden;  // map data: only while the ura flag is set
    bool labMenu = false;  // E. Gadd's lab (event08): served whichever variant applies, so it can ask
};

// Mirrors the manifest written by tools/hidden_mansion_data.py / setup (hm_overlay.cpp).
// PAL rules on the USA game:
constexpr Redirect kRedirects[] = {
    {"/Map/map2.szp", "Map/map2u.szp", true},  // PAL MissionMode::init: "%s%du.szp" when ura && map 2
    {"/Event/event08.szp", "Event/event08.szp", false, true},
    {"/Event/event16.szp", "Event/event16.szp", false},
    {"/Event/event73.szp", "Event/event73.szp", false},
};
// USA rules on the European game (the reverse): the USA map data for the hidden mansion and
// the European event archives with the USA scripts.
constexpr Redirect kRedirectsUsa[] = {
    {"/Map/map2u.szp", "usa/Map/map2.szp", true},
    {"/Event/event08.szp", "usa/Event/event08.szp", false, true},
    {"/Event/event16.szp", "usa/Event/event16.szp", false},
    {"/Event/event73.szp", "usa/Event/event73.szp", false},
};

bool samePath(const char* a, const char* b) {
    // DVD paths are case-insensitive; the game sometimes omits the leading slash.
    if (*a == '/') ++a;
    if (*b == '/') ++b;
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

struct OverlayFile {
    std::string hostPath;
    std::vector<u8> bytes;
    bool loaded = false;
};
constexpr s32 kEntryBase = 0x40000000;  // well above any real FST entry
std::mutex sOverlayMutex;
std::vector<OverlayFile> sOverlay;

OverlayFile* entryFile(s32 entry) {
    const s32 i = entry - kEntryBase;
    return (i >= 0 && i < s32(sOverlay.size())) ? &sOverlay[size_t(i)] : nullptr;
}

bool ensureLoaded(OverlayFile& f) {
    if (f.loaded) return !f.bytes.empty();
    f.loaded = true;
    std::ifstream in(hostFile(f.hostPath), std::ios::binary);
    if (!in) {
        log("[hm] overlay file missing: %s (run tools/hidden_mansion_data.py)", f.hostPath.c_str());
        return false;
    }
    f.bytes.assign(std::istreambuf_iterator<char>(in), {});
    return !f.bytes.empty();
}

// ---- constant patches ----
struct ConstPatch {
    u32 addr;      // US .sdata2 literal, referenced by exactly one function
    float usValue;
    float palValue;
    bool partial;  // PAL also changes a code path that has no literal to patch (needs a hook)
    const char* what;
};

constexpr ConstPatch kConstPatches[] = {
    // KtEnMoku (Chauncey's rocking horses), US fn_800A09D0 / PAL fn_800A3A50: scale x2.
    {0x804DB278, 1.8f, 3.6f, false, "rocking horse scale"},
    // KtEnMoku2, US fn_800A1BCC / PAL fn_800A4DC4: value passed to fn_8008907C 50 -> 100.
    {0x804DB2B0, 50.0f, 100.0f, false, "rocking horse (2) fn_8008907C argument"},
    // (Room ambient x0.2 is a hook on fn_800168EC: hidden_mansion_hooks.cpp.)
};

u32 fbits(float f) { return std::bit_cast<u32>(f); }

// hooks.txt instruction patches for the per-version Chauncey distance.
constexpr u32 kPatchChaunceyTo10 = 61, kPatchChaunceyTo20 = 62, kPatchChaunceyTo15 = 63;

}  // namespace

namespace {
// The event interpreter object; game flag n is bit n&7 of byte +0x659 + n/8
// (getFlagValue__FUc, same layout in both releases). Saved with the file.
#define kEventFlags (VA(0x803D2D40) + 0x659)
bool gameFlag(unsigned n) { return (rd8(kEventFlags + n / 8) >> (n % 8)) & 1; }
void setGameFlag(unsigned n, bool on) {
    const u32 a = kEventFlags + n / 8;
    const u8 bit = u8(1u << (n % 8));
    wr8(a, on ? u8(rd8(a) | bit) : u8(rd8(a) & ~bit));
}

// Overlays built with the lab choice mark event08 "[always]" in their manifest (format 3);
// older ones only had the variant's own script, served while that variant was selected.
bool labOverlay() {
    static const bool sLab = [] {
        std::ifstream in(hostFile(overlayDir() + (usBase() ? "manifest.txt" : "usa/manifest.txt")));
        for (std::string line; std::getline(in, line);)
            if (line.find("event08.szp") != std::string::npos && line.find("[always]") != std::string::npos) return true;
        return false;
    }();
    return sLab;
}

// The setup-built per-version data (docs/VERSION_DIFFERENCES.md): <overlay>/versions/<ID>/...
// (each installed disc's rules files) and versions/lab/<base ID>/Event/event08.szp (the lab
// menu asking for the version). LM_VERSIONS_OVERLAY overrides the folder (testing).
std::string versionsDir() {
    static const std::string dir = [] {
        std::filesystem::path d;
        if (const char* env = std::getenv("LM_VERSIONS_OVERLAY"); env && *env) d = env;
        else d = port::hiddenMansionOverlayDir() / "versions";
        const std::u8string u8 = d.generic_u8string();
        return std::string(u8.begin(), u8.end()) + "/";
    }();
    return dir;
}

Variant ofVersion(port::GameVersion v) {
    return v == port::GameVersion::PAL ? Variant::Pal : v == port::GameVersion::JPN ? Variant::Jp : Variant::Us;
}
port::GameVersion versionOf(Variant v) {
    return v == Variant::Pal ? port::GameVersion::PAL : v == Variant::Jp ? port::GameVersion::JPN : port::GameVersion::USA;
}

// "us" / "pal" / "jp" (also "usa", "eur", "japan", "jpn"); nullopt for anything else.
std::optional<Variant> parseVariant(std::string v) {
    for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (v == "us" || v == "usa") return Variant::Us;
    if (v == "pal" || v == "eur" || v == "europe") return Variant::Pal;
    if (v == "jp" || v == "jpn" || v == "japan") return Variant::Jp;
    return std::nullopt;
}

// The default (F1 > Enhancements > Hidden Mansion version) or a testing override.
bool sForced = false;  // --hidden-mansion-variant / LM_HIDDEN_MANSION_VARIANT given: beats the lab's choice
Variant initialVariant() {
    const char* env = std::getenv("LM_HIDDEN_MANSION_VARIANT");
    if (!env || !*env) env = std::getenv("LM_HIDDEN_MANSION");
    std::string v = env && *env ? env : port::options().hiddenMansion;
    sForced = !v.empty();
    const auto& e = port::settings().enhancements;
    Variant want = e.hiddenMansionJp ? Variant::Jp : e.hiddenMansionPal ? Variant::Pal : Variant::Us;
    if (const auto p = parseVariant(v)) want = *p;
    // Only what this install offers (a single-disc install: its own release's rules).
    if (!hiddenVariantOffered(want)) want = baseVariant();
    const bool have = jpBase() || std::filesystem::exists(hostFile(overlayDir() + (usBase() ? "manifest.txt" : "usa/manifest.txt")));
    log("[hm] Hidden Mansion variant: %s on the %s game%s (overlay %s%s%s)", variantName(want), variantName(baseVariant()),
        sForced ? ", forced" : "", overlayDir().c_str(),
        have || want == baseVariant() ? "" : ": missing, run tools/hidden_mansion_data.py",
        have && !jpBase() && !labOverlay() ? "; old format without the lab menu choice, rebuild it" : "");
    return want;
}
std::atomic<int> sVariant{-1};

Variant defaultVariant() {
    int v = sVariant.load();
    if (v < 0) {
        int want = int(initialVariant());
        sVariant.compare_exchange_strong(v, want);
        v = sVariant.load();
    }
    return Variant(v);
}

// Normal mansion: LM_MANSION_VARIANT / --mansion-variant (testing), else F1's fallback.
bool sMansionForced = false;
Variant initialMansion() {
    const char* env = std::getenv("LM_MANSION_VARIANT");
    Variant want = baseVariant();
    const int m = port::settings().enhancements.mansionVersion;
    if (m == 1) want = Variant::Us;
    else if (m == 2) want = Variant::Jp;
    else if (m == 3) want = Variant::Pal;
    if (env && *env)
        if (const auto p = parseVariant(env)) want = *p, sMansionForced = true;
    if (!variantInstalled(want)) want = baseVariant();
    if (want != baseVariant()) log("[rules] mansion: %s rules on the %s game%s", variantName(want), variantName(baseVariant()),
                                   sMansionForced ? " (forced)" : "");
    return want;
}
}  // namespace

const char* variantName(Variant v) { return v == Variant::Pal ? "PAL" : v == Variant::Jp ? "Japan" : "USA"; }
Variant baseVariant() { return ofVersion(port::install().running); }

bool variantInstalled(Variant v) {
    if (v == baseVariant()) return true;
    const auto& i = port::install();
    if (!i.ready(versionOf(v))) return false;
    static const bool sHave = std::filesystem::exists(hostFile(versionsDir() + "manifest.txt"));
    return sHave;
}

bool hiddenVariantOffered(Variant v) {
    if (v == baseVariant()) return true;
    const auto& i = port::install();
    if (!i.ready(versionOf(v))) return false;
    if (jpBase() && v == Variant::Pal) return false;  // (European rules on the Japanese game: not yet)
    // USA <-> European: the Hidden Mansion overlay (both discs); Japan on either / either on Japan:
    // the versions overlay.
    if (v == Variant::Pal && usBase()) return port::install().hmOverlayReady;  // (map2u, the PAL scripts)
    if (v == Variant::Us && !usBase() && !jpBase()) return port::install().hmOverlayReady || variantInstalled(v);
    return variantInstalled(v);
}

bool versionMenuAvailable() {
    static const bool sOn = [] {
        if (port::install().readyCount() < 2) return false;
        const std::string lab = versionsDir() + "lab/" + port::gameId(port::install().running) + "/Event/event08.szp";
        return std::filesystem::exists(hostFile(lab));
    }();
    return sOn;
}

std::optional<Variant> versionField() {
    if (!versionMenuAvailable()) return std::nullopt;
    const bool a = gameFlag(kVersionFlagA), b = gameFlag(kVersionFlagB);
    if (!a && !b) return std::nullopt;
    return a && b ? Variant::Pal : a ? Variant::Us : Variant::Jp;
}

bool labChoiceAvailable() {
    if (versionMenuAvailable()) return true;  // (the versions lab menu supersedes the two-variant one)
    return labOverlay() && port::hiddenMansionPal() == port::Avail::Yes && port::hiddenMansionUs() == port::Avail::Yes;
}

// The lab's two Hidden Mansion branches set or clear game flag 8 right before <URALUIGI>: the
// European release's own "in the hidden mansion" flag, which its scripts already keep in step
// (cleared again when the lab is entered) and which is saved with the file. With the versions
// menu the field (flags 60/76) says which; flag 8 alone (a save from the two-variant menu, or the
// European base's own) still means PAL.
std::optional<Variant> labChoice() {
    if (!labChoiceAvailable()) return std::nullopt;
    if (const auto f = versionField()) return *f;
    if (gameFlag(kLabFlagPal)) return Variant::Pal;
    return versionMenuAvailable() ? baseVariant() : Variant::Us;
}

Variant variant() {
    const Variant def = defaultVariant();
    if (sForced) return def;
    if (const auto lab = labChoice()) return hiddenVariantOffered(*lab) ? *lab : baseVariant();
    return def;
}

Variant mansionVariant() {
    static const Variant sDefault = initialMansion();
    if (sMansionForced) return sDefault;
    if (versionMenuAvailable()) {
        const auto f = versionField();
        return f && variantInstalled(*f) ? *f : baseVariant();
    }
    return sDefault;
}

Variant rulesVariant() { return hiddenActive() ? variant() : mansionVariant(); }

Rules rules(Variant v, bool hidden) {
    Rules r{};
    switch (v) {
    case Variant::Us: r = {hidden ? 400 : 200, hidden, 100000000u, 15.0f, false}; break;
    case Variant::Jp: r = {100, false, 100000000u, 10.0f, false}; break;
    case Variant::Pal: r = {hidden ? 1000 : 200, hidden, 150000000u, hidden ? 40.0f : 20.0f, true}; break;
    }
    return r;
}

void setVariant(Variant v) {
    if (sVariant.exchange(int(v)) != int(v)) log("[hm] Hidden Mansion default variant -> %s", variantName(v));
}

namespace {
// See hidden_mansion.h, "USA rules on the European game". While sHidden, the flag in guest
// memory reads 0 and its real value is sHiddenSaved.
bool sHidden = false;
u32 sHiddenSaved = 0;
}  // namespace

void callWithoutFlag(Ctx& c, const char* name) {
    if (!sHidden && !usaOnPalActive()) return callOriginal(c, name);
    const bool outer = !sHidden;
    if (outer) {
        sHiddenSaved = rd32(kUraFlag);
        wr32(kUraFlag, 0);
        sHidden = true;
    }
    callOriginal(c, name);
    if (outer && sHidden) {
        // (Nothing inside sets the flag; if something did, keep its value.)
        if (rd32(kUraFlag) == 0) wr32(kUraFlag, sHiddenSaved);
        sHidden = false;
    }
}

void callWithFlag(Ctx& c, const char* name) {
    if (!sHidden) return callOriginal(c, name);
    wr32(kUraFlag, sHiddenSaved);
    sHidden = false;
    callOriginal(c, name);
    sHiddenSaved = rd32(kUraFlag);
    wr32(kUraFlag, 0);
    sHidden = true;
}

bool hiddenActive() { return sHidden ? sHiddenSaved != 0 : rd32(kUraFlag) != 0; }

void callWithFlagAs(Ctx& c, const char* name, bool shown) {
    const u32 want = shown ? 1 : 0, cur = rd32(kUraFlag);
    if (cur == want) return callOriginal(c, name);
    wr32(kUraFlag, want);
    callOriginal(c, name);
    if (rd32(kUraFlag) == want) wr32(kUraFlag, cur);
}

void onFrame() {
    // LM_HM_FORCE=1: keep the hidden-mansion flag set (testing without a cleared save).
    static const bool sForce = [] {
        const char* env = std::getenv("LM_HM_FORCE");
        return env && *env == '1';
    }();
    if (sForce && !hiddenActive()) {
        log("[hm] LM_HM_FORCE: setting the hidden-mansion flag");
        wr32(kUraFlag, 1);
        // With the lab menu the variant is whatever the lab recorded; force the default.
        if (labChoiceAvailable()) setGameFlag(kLabFlagPal, defaultVariant() == Variant::Pal);
        if (versionMenuAvailable()) {
            const Variant v = defaultVariant();
            const bool own = v == baseVariant();
            setGameFlag(kVersionFlagA, !own && (v == Variant::Us || v == Variant::Pal));
            setGameFlag(kVersionFlagB, !own && (v == Variant::Jp || v == Variant::Pal));
        }
    }
    static bool sWas = false;
    if (hiddenActive() != sWas) {
        sWas = hiddenActive();
        log("[hm] hidden-mansion flag %s (%s variant%s)", sWas ? "set" : "cleared", variantName(variant()),
            sForced ? ", forced" : labChoice() ? ", chosen in the lab" : "");
    }
    // Which release's rules apply (docs/VERSION_DIFFERENCES.md): logged on every change.
    static int sLastRules = -1;
    const int now = int(rulesVariant()) * 2 + (hiddenActive() ? 1 : 0);
    if (now != sLastRules) {
        sLastRules = now;
        const Rules r = rules(rulesVariant(), hiddenActive());
        log("[rules] %s %s rules on the %s game: heart threshold %d, damage x%d, A rank %u, Chauncey %.0f, Bowser bombs %s",
            hiddenActive() ? "Hidden Mansion" : "Mansion", variantName(rulesVariant()), variantName(baseVariant()),
            r.heartThreshold, r.doubleDamage ? 2 : 1, r.rankA, double(r.chaunceyDist), r.bowserBomb90 ? "90" : "retail");
    }
    syncConstants();
    mirror::onFrame();
}
bool palActive() { return usBase() && variant() == Variant::Pal && hiddenActive(); }
// European game with the USA (or Japanese) Hidden Mansion rules: the flag-hiding machinery.
bool usaOnPalActive() { return !usBase() && !jpBase() && variant() != Variant::Pal && hiddenActive(); }
bool palRulesActive() { return !jpBase() && variant() == Variant::Pal && hiddenActive(); }

namespace {
// Rules files of a release, served from versions/<ID>/ while its mansion is chosen on another
// base (docs/VERSION_DIFFERENCES.md 1.2 and 3; written by setup, hm_overlay.cpp).
constexpr const char* kVersionFiles[] = {"/Map/map2.szp", "/Map/map4.szp", "/Iwamoto/map2/room_70.arc",
                                         "/Iwamoto/map2/room_08.arc", "/model/kopabody.szp"};
}  // namespace

std::optional<std::string> redirect(const char* guestPath) {
    if (!guestPath) return std::nullopt;
    const Variant base = baseVariant();
    const bool hidden = hiddenActive();
    // The versions lab menu (two or more discs) replaces E. Gadd's lab script on every base.
    if (versionMenuAvailable() && samePath(guestPath, "/Event/event08.szp"))
        return versionsDir() + "lab/" + port::gameId(port::install().running) + "/Event/event08.szp";
    // Another release's mansion: its rules files. The Hidden Mansion of the USA/Japanese releases
    // is their map2.szp too; the European one is handled below (map2u, the PAL scripts).
    const Variant want = rulesVariant();
    if (want != base && !(want == Variant::Pal && hidden)) {
        if (variantInstalled(want)) {
            const std::string dir = versionsDir() + port::gameId(versionOf(want));
            for (const char* f : kVersionFiles) {
                // European base, hidden: its MissionMode::init loads map2u.szp instead of map2.szp.
                const bool map = samePath(f, "/Map/map2.szp");
                const char* asked = map && hidden && base == Variant::Pal ? "/Map/map2u.szp" : f;
                if (samePath(guestPath, asked)) return dir + f;
            }
        }
    }
    if (jpBase()) return std::nullopt;
    // Hidden Mansion, USA <-> European (and Japan on the European base): the Hidden Mansion overlay
    // (PAL rules on the USA game; the USA scripts on the European game).
    const bool reverse = !usBase();
    const Variant hv = variant();
    const bool own = reverse ? hv == Variant::Pal : hv == Variant::Us || hv == Variant::Jp;
    for (const Redirect& r : reverse ? std::span<const Redirect>(kRedirectsUsa) : std::span<const Redirect>(kRedirects)) {
        if (!samePath(guestPath, r.guestPath)) continue;
        if (r.labMenu && labChoiceAvailable()) return overlayDir() + r.overlayFile;
        if (own) return std::nullopt;
        if (r.onlyWhileHidden && !(hidden && rd32(kCurMapNo) == 2)) return std::nullopt;
        // (Japanese rules on the European game: its map2.szp came from the versions overlay above;
        // its event16/73 scripts are identical to the USA ones, which the reverse overlay serves.)
        return overlayDir() + r.overlayFile;
    }
    return std::nullopt;
}

s32 overlayEntry(const char* guestPath) {
    const auto host = redirect(guestPath);
    if (!host) return -1;
    std::lock_guard lock(sOverlayMutex);
    for (size_t i = 0; i < sOverlay.size(); ++i)
        if (sOverlay[i].hostPath == *host) return kEntryBase + s32(i);
    sOverlay.push_back({*host, {}, false});
    if (!ensureLoaded(sOverlay.back())) {
        sOverlay.pop_back();
        return -1;  // fall back to the US disc file
    }
    log("[hm] %s -> %s", guestPath, host->c_str());
    return kEntryBase + s32(sOverlay.size() - 1);
}

bool isOverlayEntry(s32 entry) {
    std::lock_guard lock(sOverlayMutex);
    return entryFile(entry) != nullptr;
}

u32 overlayLength(s32 entry) {
    std::lock_guard lock(sOverlayMutex);
    const OverlayFile* f = entryFile(entry);
    return f ? u32(f->bytes.size()) : 0;
}

s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset) {
    std::lock_guard lock(sOverlayMutex);
    const OverlayFile* f = entryFile(entry);
    if (!f || offset < 0 || length < 0 || size_t(offset) > f->bytes.size()) return -1;
    const size_t n = std::min(size_t(length), f->bytes.size() - size_t(offset));
    std::memcpy(dst, f->bytes.data() + offset, n);
    // Disc reads are padded to 32 bytes; zero the tail like a read past EOF would.
    if (n < size_t(length)) std::memset(static_cast<u8*>(dst) + n, 0, size_t(length) - n);
    return s32(n);
}

void syncConstants() {
    // A rank (docs/VERSION_DIFFERENCES.md 3): the money table's last entry, read by fn_8000D0AC
    // when a rank is shown: 150,000,000 G with the European rules, 100,000,000 G otherwise.
    {
        static const u32 kRankA = VA(0x8020BE18);
        const u32 want = rules(rulesVariant(), hiddenActive()).rankA, cur = kRankA ? rd32(kRankA) : 0;
        if (cur != want && (cur == 100000000u || cur == 150000000u)) {
            wr32(kRankA, want);
            log("[rules] A rank needs %u G", want);
        }
    }
    // Chauncey's approach (fn_8009FD54): the distance he stops at, as two instruction patches on
    // the loads after the first (hooks.txt 61-63): USA 15, Japan 10, PAL 20 (the European Hidden
    // Mansion's 40 is the PAL-rules hook on the USA game). Not on the European base yet.
    {
        const float want = rules(rulesVariant(), hiddenActive()).chaunceyDist;
        const bool pal40 = want > 30.0f;
        setPatch(kPatchChaunceyTo10, usBase() && want == 10.0f);
        setPatch(kPatchChaunceyTo20, (usBase() || jpBase()) && want == 20.0f && !pal40);
        setPatch(kPatchChaunceyTo15, jpBase() && want == 15.0f);
    }
    if (jpBase()) return;  // (the patched literals below are USA / European addresses)
    if (!usBase()) {
        // European game: the hidden mansion's room ambient factor is a .sdata float (PAL-only
        // 0x804BD728, 0.2, read by the PAL-only fn_800179F8 when the flag is set). 1.0 = USA.
        constexpr u32 kPalAmbient = 0x804BD728;
        const u32 want = fbits(variant() != Variant::Pal ? 1.0f : 0.2f);
        const u32 cur = rd32(kPalAmbient);
        if (cur != want && (cur == fbits(1.0f) || cur == fbits(0.2f))) wr32(kPalAmbient, want);
        return;
    }
    static bool sApplied = false;
    static bool sChecked = false, sOk = true;
    if (!sChecked) {  // refuse to touch memory that does not hold the expected US literal
        sChecked = true;
        for (const ConstPatch& p : kConstPatches) {
            if (rd32(p.addr) != fbits(p.usValue)) {
                log("[hm] unexpected literal at %08X (%s); constant patches disabled", p.addr, p.what);
                sOk = false;
            }
        }
    }
    if (!sOk) return;
    const bool want = palActive();  // switching variant or leaving the hidden mansion restores US values
    if (want == sApplied) return;
    for (const ConstPatch& p : kConstPatches) wr32(p.addr, fbits(want ? p.palValue : p.usValue));
    sApplied = want;
}

bool mirrorWorld() {
    // LM_MIRROR=1/0 (test aid) overrides the setting (and LM_GAMEPLAY=mirror=...).
    static const int sEnv = [] {
        const char* env = std::getenv("LM_MIRROR");
        if (!env || !*env) return -1;
        log("[mirror] LM_MIRROR=%s", env);
        return *env == '0' ? 0 : 1;
    }();
    const bool on = !jpBase() && (sEnv >= 0 ? sEnv == 1 : gameplay::current().mirrorWorld);  // (not on the Japanese game yet)
    // Only in the main game scene (the scene manager's current scene, lbl_804CFF98), where the
    // mirror code runs; the title, file select and Options screens are other scenes.
    return on && rd32(VA(0x804CFF98)) == VA(0x8021DB68);
}

bool mirrorOn() {
    static const bool sOff = [] {  // LM_HM_MIRROR=0 disables the Hidden Mansion mirror (debugging)
        const char* env = std::getenv("LM_HM_MIRROR");
        return env && *env == '0';
    }();
    return (!sOff && palRulesActive()) || mirrorWorld();
}

bool mirrorActive() { return usBase() && mirrorOn(); }

void callMirror(Ctx& c, const char* name) {
    // Same bookkeeping as callWithoutFlag: while sHidden the real value is in sHiddenSaved, so
    // the functions that read the real flag (callWithFlag) still see it from inside the call.
    const u32 want = mirrorOn() ? 1 : 0;
    const u32 shown = rd32(kUraFlag);
    if (shown == want) return callOriginal(c, name);
    const bool outer = !sHidden;
    if (outer) {
        sHiddenSaved = shown;
        sHidden = true;
    }
    wr32(kUraFlag, want);
    callOriginal(c, name);
    if (outer) {
        if (sHidden && rd32(kUraFlag) == want) wr32(kUraFlag, sHiddenSaved);
        sHidden = false;
    } else if (rd32(kUraFlag) == want) {
        wr32(kUraFlag, shown);
    }
}

void mirrorProjection(float m[4][4], bool perspective) {
    if (!perspective || !mirrorActive()) return;
    for (int i = 0; i < 4; ++i) m[0][i] = -m[0][i];
}

u32 mirrorCullMode(u32 gxCullMode) {
    if (!mirrorActive()) return gxCullMode;
    if (gxCullMode == 1) return 2;  // GX_CULL_FRONT -> GX_CULL_BACK
    if (gxCullMode == 2) return 1;
    return gxCullMode;
}

float mirrorScreenX(float x, float width) { return mirrorActive() ? width - x : x; }

void mirrorStereo(s16* interleavedStereo, std::size_t frames) {
    if (!mirrorActive() || !interleavedStereo) return;
    for (std::size_t i = 0; i < frames; ++i) std::swap(interleavedStereo[2 * i], interleavedStereo[2 * i + 1]);
}

}  // namespace recomp::hm
