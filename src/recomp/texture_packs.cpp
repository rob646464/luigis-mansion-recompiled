// Texture packs and the texture dumper (docs/TEXTURE_PACKS.md). Ported from our City Folk port
// (same file there); game IDs GLME01 (USA), GLMP01 (Europe), GLMJ01 (Japan), GLM = every region.
// Off by default ("Use texture packs"), and nothing is created on disk until it is used: the packs
// folder only by "Open texture packs folder", the dump folder only by dumping.
//
// Packs live in <data folder>/texture_packs (port::dataDir(): %APPDATA%\Luigi's Mansion
// Recompiled, or <program folder>/portable in portable mode), one subfolder per pack. Inside a
// pack, the files for the running game are found the way Dolphin finds them in Load/Textures:
//   <pack>/GLME01/..., <pack>/Load/Textures/GLME01/..., <pack>/Textures/GLME01/... (the game ID),
//   the same with GLM (every region), a marker file <pack>/GLME01.txt or GLM.txt (the whole pack),
//   or a pack folder that is itself named GLME01 / GLM. A pack with none of these applies to every
//   region. A pack whose ID folders are all for other regions is listed but not loaded.
// Every enabled pack's folder goes to aurora::texture::load_replacement_directory with a priority
// from its place in the list (top = wins when two packs replace the same texture). Aurora parses
// the Dolphin names into source keys (XXH64 of the base level as the console stores it, XXH64 of
// the TLUT entries the texture uses, size, format; "$" wildcards) and streams the files in on its
// worker threads; until a file is decoded the original texture is drawn.
//
// The dumper (Aurora 0360, aurora::texture::set_texture_dump) writes every texture the game loads
// once, as PNG with the same names Dolphin's "Dump Textures" gives them, into
// <data folder>/texture_dump/<game id>/, ready to be edited and dropped into a pack.
//
// Settings: [texture_packs] in settings.ini (this module's SettingsExtension): enabled, budget_mb,
// dump_mips, and pack<N> = <0|1>,<folder name> in priority order. Dumping itself is not saved
// (it is a pack-maker session switch).
//
// Test aids: LM_TEXPACK_DIR=<dir> (packs folder), LM_TEXPACKS=0|1 (master switch),
// LM_TEXDUMP=1|<dir> (dump from the start), LM_TEXPACK_AT=<frame>:<action>[,...][;...] (or '|') with
// actions reload, on, off, dump, nodump, budget=<MB>, pack=<name>:<0|1>, top=<name> (guest frames),
// LM_TEXPACK_LOG=<s> (stats line every s seconds, default 5 when set to 1).
#include "recomp/texture_packs.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/frame.h"
#include "recomp/hle.h"
#include "recomp/menu.h"

#include <dolphin/gx.h>  // GXTexObj for aurora/texture.hpp
#include <aurora/texture.hpp>
#include <imgui.h>

#include <SDL3/SDL_misc.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <ostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;

namespace recomp::texpacks {
namespace {

constexpr const char* kIds[] = {"GLME01", "GLMP01", "GLMJ01"};
constexpr const char* kRegionFree = "GLM";

// ---- settings ---------------------------------------------------------------------------------

struct PackConf {
    std::string name;
    bool on = true;
};

struct Conf {
    bool enabled = false;  // master switch (off by default)
    int budgetMb = 2048;  // replacement GPU cache
    bool dumpMips = true;
    std::vector<PackConf> packs;  // priority order, first wins
};

std::mutex sMutex;  // sConf, sPacks, sStatus
Conf sConf;
bool sDump = false;  // session only

void resetConf() {
    std::lock_guard lk(sMutex);
    sConf = Conf{};
}

void loadConf(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "texture_packs") return;
    std::lock_guard lk(sMutex);
    if (key == "enabled") sConf.enabled = value != "0";
    else if (key == "budget_mb") sConf.budgetMb = std::clamp(std::atoi(value.c_str()), 256, 65536);
    else if (key == "dump_mips") sConf.dumpMips = value != "0";
    else if (key.rfind("pack", 0) == 0) {
        const size_t comma = value.find(',');
        if (comma == std::string::npos || comma + 1 >= value.size()) return;
        PackConf p{value.substr(comma + 1), value.substr(0, comma) != "0"};
        for (const auto& q : sConf.packs)
            if (q.name == p.name) return;
        sConf.packs.push_back(std::move(p));
    }
}

void saveConf(std::ostream& out) {
    std::lock_guard lk(sMutex);
    out << "\n[texture_packs]\n";
    out << "enabled=" << (sConf.enabled ? 1 : 0) << "\n";
    out << "budget_mb=" << sConf.budgetMb << "\n";
    out << "dump_mips=" << (sConf.dumpMips ? 1 : 0) << "\n";
    for (size_t i = 0; i < sConf.packs.size(); i++)
        out << "pack" << i << "=" << (sConf.packs[i].on ? 1 : 0) << "," << sConf.packs[i].name << "\n";
}

const bool sRegistered = [] {
    port::addSettingsExtension({&resetConf, &loadConf, &saveConf});
    return true;
}();

// ---- packs on disk ----------------------------------------------------------------------------

struct PackInfo {
    std::string name;
    fs::path dir;
    fs::path root;                 // folder registered for the running game (empty: not for it)
    std::string layout;            // how root was found (menu tooltip)
    std::vector<std::string> ids;  // game IDs the pack names (empty: every region)
    size_t files = 0;              // replacement files under root (mip sidecars not counted)
    size_t registered = 0;         // registrations Aurora made (after loading)
    bool loaded = false;
};

std::string sGameId = "GLME01";
fs::path sPackRoot, sDumpRoot;
std::vector<PackInfo> sPacks;     // last scan, in priority order (conf order, then new by name)
std::string sStatus = "Not loaded yet.";
std::atomic<bool> sBusy = false;

std::string upper(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path fromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool isTextureFile(const fs::path& p) {
    const std::string ext = upper(u8(p.extension()));
    if (ext != ".DDS" && ext != ".PNG") return false;
    const std::string stem = u8(p.stem());
    if (stem.rfind("tex1_", 0) != 0) return false;
    // "<name>_mip<N>" sidecars belong to their base file
    size_t i = stem.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(stem[i - 1]))) i--;
    return !(i < stem.size() && i >= 4 && stem.compare(i - 4, 4, "_mip") == 0);
}

size_t countFiles(const fs::path& root) {
    size_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && isTextureFile(it->path())) n++;
    return n;
}

// Subfolder of dir whose name is `want` (any case), or empty.
fs::path childDir(const fs::path& dir, const std::string& want) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && upper(u8(it->path().filename())) == want) return it->path();
    return {};
}

bool hasMarker(const fs::path& dir, const std::string& id) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && upper(u8(it->path().filename())) == id + ".TXT") return true;
    return false;
}

// Finds the pack's folder for the running game (Dolphin's Load/Textures conventions, see the top).
void resolvePack(PackInfo& p) {
    const std::string self = upper(p.name);
    for (const char* id : kIds)
        if (self == id) {
            p.ids = {id};
            if (sGameId == id) p.root = p.dir, p.layout = "folder named after the game ID";
            return;
        }
    if (self == kRegionFree) {
        p.root = p.dir;
        p.layout = "folder named GLM (every region)";
        return;
    }
    // Candidate parents of ID folders: the pack itself, Load/Textures, Textures, User/Load/Textures.
    std::vector<std::pair<fs::path, std::string>> parents = {{p.dir, ""}};
    if (fs::path load = childDir(p.dir, "LOAD"); !load.empty())
        if (fs::path t = childDir(load, "TEXTURES"); !t.empty()) parents.push_back({t, "Load/Textures/"});
    if (fs::path t = childDir(p.dir, "TEXTURES"); !t.empty()) parents.push_back({t, "Textures/"});
    if (fs::path user = childDir(p.dir, "USER"); !user.empty())
        if (fs::path load = childDir(user, "LOAD"); !load.empty())
            if (fs::path t = childDir(load, "TEXTURES"); !t.empty()) parents.push_back({t, "User/Load/Textures/"});
    bool anyId = false;
    for (const auto& [parent, prefix] : parents) {
        for (const char* id : kIds)
            if (fs::path d = childDir(parent, id); !d.empty()) {
                anyId = true;
                p.ids.push_back(id);
                if (sGameId == id && p.root.empty()) p.root = d, p.layout = prefix + id;
            }
        if (p.root.empty())
            if (fs::path d = childDir(parent, kRegionFree); !d.empty()) {
                anyId = true;
                p.root = d;
                p.layout = prefix + "GLM (every region)";
            }
        if (anyId) break;
    }
    if (anyId) return;
    for (const char* id : kIds)
        if (hasMarker(p.dir, id)) p.ids.push_back(id);
    if (!p.ids.empty()) {
        if (std::find(p.ids.begin(), p.ids.end(), sGameId) != p.ids.end())
            p.root = p.dir, p.layout = "marker file " + sGameId + ".txt";
        return;
    }
    p.root = p.dir;
    p.layout = hasMarker(p.dir, kRegionFree) ? "marker file GLM.txt (every region)" : "whole folder (every region)";
}

std::vector<PackInfo> scanPacks(std::vector<PackConf>& conf) {
    std::vector<PackInfo> found;
    std::error_code ec;  // no folder yet: no packs (created only by "Open texture packs folder")
    for (fs::directory_iterator it(sPackRoot, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        PackInfo p;
        p.dir = it->path();
        p.name = u8(p.dir.filename());
        resolvePack(p);
        if (!p.root.empty()) p.files = countFiles(p.root);
        found.push_back(std::move(p));
    }
    std::sort(found.begin(), found.end(), [](const PackInfo& a, const PackInfo& b) { return a.name < b.name; });
    // Order: the saved order first, then packs seen for the first time (enabled).
    std::vector<PackInfo> ordered;
    for (const auto& c : conf)
        for (auto& p : found)
            if (!p.dir.empty() && p.name == c.name) {
                ordered.push_back(std::move(p));
                p.dir.clear();
            }
    for (auto& p : found)
        if (!p.dir.empty()) {
            conf.push_back({p.name, true});
            ordered.push_back(std::move(p));
        }
    return ordered;
}

// ---- loader thread ----------------------------------------------------------------------------

std::thread sWorker;
std::mutex sWorkMutex;
std::condition_variable sWorkCv;
bool sWorkPending = false, sWorkStop = false;
std::vector<aurora::texture::ReplacementGroup> sGroups;  // worker thread only

bool packOn(const Conf& c, const std::string& name) {
    for (const auto& p : c.packs)
        if (p.name == name) return p.on;
    return true;
}

void loadAll() {
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& g : sGroups) aurora::texture::unregister_replacements(g);
    sGroups.clear();
    Conf conf;
    {
        std::lock_guard lk(sMutex);
        conf = sConf;
    }
    std::vector<PackInfo> packs = scanPacks(conf.packs);
    size_t total = 0, loadedPacks = 0;
    if (conf.enabled) {
        const int n = int(packs.size());
        for (int i = 0; i < n; i++) {
            PackInfo& p = packs[i];
            if (p.root.empty() || !packOn(conf, p.name)) continue;
            // Higher priority for packs higher in the list.
            auto group = aurora::texture::load_replacement_directory(p.root, {.priority = n - i});
            p.registered = group.registrations.size();
            p.loaded = true;
            total += p.registered;
            loadedPacks++;
            sGroups.push_back(std::move(group));
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    char status[256];
    if (!conf.enabled)
        std::snprintf(status, sizeof status, "Texture packs are off.");
    else if (packs.empty())
        std::snprintf(status, sizeof status, "No texture packs installed.");
    else
        std::snprintf(status, sizeof status, "%zu replacement textures from %zu pack%s (scanned in %.0f ms).", total,
                      loadedPacks, loadedPacks == 1 ? "" : "s", ms);
    log("[texpacks] %s", status);
    for (const auto& p : packs)
        log("[texpacks]   %s: %s%s, %zu files, %zu registered (%s)", p.name.c_str(),
            p.root.empty() ? "not for " : (p.loaded ? "loaded" : "off"), p.root.empty() ? sGameId.c_str() : "",
            p.files, p.registered, p.layout.c_str());
    std::lock_guard lk(sMutex);
    // New packs found by the scan join the saved order.
    for (const auto& c : conf.packs) {
        bool known = false;
        for (const auto& s : sConf.packs) known |= s.name == c.name;
        if (!known) sConf.packs.push_back(c);
    }
    sPacks = std::move(packs);
    sStatus = status;
}

void workerMain() {
    for (;;) {
        {
            std::unique_lock lk(sWorkMutex);
            sWorkCv.wait(lk, [] { return sWorkPending || sWorkStop; });
            if (sWorkStop) break;
            sWorkPending = false;
        }
        sBusy = true;
        loadAll();
        sBusy = false;
    }
    for (const auto& g : sGroups) aurora::texture::unregister_replacements(g);
    sGroups.clear();
}

void requestReload() {
    {
        std::lock_guard lk(sWorkMutex);
        sWorkPending = true;
    }
    sBusy = true;  // shown at once
    sWorkCv.notify_one();
}

void applyDump() {
    bool mips;
    {
        std::lock_guard lk(sMutex);
        mips = sConf.dumpMips;
    }
    aurora::texture::set_texture_dump(sDumpRoot, sDump, mips);
}

void applyBudget() {
    int mb;
    {
        std::lock_guard lk(sMutex);
        mb = sConf.budgetMb;
    }
    aurora::texture::set_replacement_cache_budget(uint64_t(mb) << 20);
}

void openFolder(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", port::plainPath(dir).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string url = "file://" + u8(dir);
    for (size_t i = 0; i < url.size(); i++)
        if (url[i] == ' ') url.replace(i, 1, "%20");
    SDL_OpenURL(url.c_str());
#endif
}

bool sInit = false;

}  // namespace

void init(const char* gameId) {
    if (gameId && std::strlen(gameId) == 6) sGameId = upper(gameId);
    sPackRoot = port::dataDir() / "texture_packs";
    if (const fs::path p = port::envPath("LM_TEXPACK_DIR"); !p.empty()) sPackRoot = p;
    sDumpRoot = port::dataDir() / "texture_dump" / sGameId;
    if (const char* v = std::getenv("LM_TEXPACKS"); v && *v) {
        std::lock_guard lk(sMutex);
        sConf.enabled = *v != '0';
    }
    if (const char* v = std::getenv("LM_TEXDUMP"); v && *v && std::strcmp(v, "0") != 0) {
        sDump = true;
        if (std::strcmp(v, "1") != 0) sDumpRoot = port::envPath("LM_TEXDUMP");
    }
    applyBudget();
    applyDump();
    if (const char* v = std::getenv("LM_TEXPACK_LOG"); v && *v && *v != '0') aurora::texture::set_replacement_log(true);
    sWorker = std::thread(workerMain);
    requestReload();
    sInit = true;
}

void shutdown() {
    if (!sInit) return;
    sInit = false;
    aurora::texture::set_texture_dump({}, false);
    {
        std::lock_guard lk(sWorkMutex);
        sWorkStop = true;
    }
    sWorkCv.notify_one();
    if (sWorker.joinable()) sWorker.join();
}

void memReport() {
    const auto s = aurora::texture::replacement_stats();
    constexpr double kMb = 1024.0 * 1024.0;
    log("[mem] texture packs: %llu replacements registered, %llu on the GPU (%.0f MB of %.0f MB budget), %llu "
        "loading, %llu failed, %llu texture loads replaced; dump: %llu written, %llu already there, %llu queued",
        (unsigned long long)s.registrations, (unsigned long long)s.cachedTextures, s.cachedBytes / kMb,
        s.budgetBytes / kMb, (unsigned long long)s.pendingLoads, (unsigned long long)s.failed,
        (unsigned long long)s.hits, (unsigned long long)s.dumped, (unsigned long long)s.dumpSkipped,
        (unsigned long long)s.dumpQueued);
}

void hostFrame() {
    if (!sInit) return;
    // LM_TEXPACK_LOG=<s>: the stats line every s seconds.
    static const double sLogEvery = [] {
        const char* v = std::getenv("LM_TEXPACK_LOG");
        if (!v || !*v) return 0.0;
        const double s = std::atof(v);
        return s <= 1.0 ? 5.0 : s;
    }();
    if (sLogEvery > 0) {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point sLast = Clock::now();
        if (std::chrono::duration<double>(Clock::now() - sLast).count() >= sLogEvery) {
            sLast = Clock::now();
            memReport();
        }
    }
    // LM_TEXPACK_AT=<frame>:<action>[,<action>...][;<frame>:...] (see the top of the file).
    struct Step {
        unsigned frame;
        std::string action;
    };
    static std::vector<Step> sSteps = [] {
        std::vector<Step> out;
        const char* v = std::getenv("LM_TEXPACK_AT");
        if (!v) return out;
        std::string s = v;
        std::replace(s.begin(), s.end(), '|', ';');  // '|' too: tools/routes/run.ps1 -Env splits at ';'
        size_t pos = 0;
        while (pos < s.size()) {
            size_t end = s.find(';', pos);
            if (end == std::string::npos) end = s.size();
            const std::string group = s.substr(pos, end - pos);
            pos = end + 1;
            const size_t colon = group.find(':');
            if (colon == std::string::npos) continue;
            const unsigned frame = unsigned(std::strtoul(group.c_str(), nullptr, 10));
            size_t p = colon + 1;
            while (p < group.size()) {
                size_t e = group.find(',', p);
                if (e == std::string::npos) e = group.size();
                out.push_back({frame, group.substr(p, e - p)});
                p = e + 1;
            }
        }
        return out;
    }();
    if (sSteps.empty()) return;
    const unsigned now = frame::guestFrames();
    bool reload = false;
    for (auto it = sSteps.begin(); it != sSteps.end();) {
        if (now < it->frame) {
            ++it;
            continue;
        }
        const std::string& a = it->action;
        log("[texpacks] frame %u: %s", now, a.c_str());
        if (a == "reload") reload = true;
        else if (a == "on" || a == "off") {
            std::lock_guard lk(sMutex);
            sConf.enabled = a == "on";
            reload = true;
        } else if (a == "dump" || a == "nodump") {
            sDump = a == "dump";
            applyDump();
        } else if (a.rfind("budget=", 0) == 0) {
            {
                std::lock_guard lk(sMutex);
                sConf.budgetMb = std::clamp(std::atoi(a.c_str() + 7), 64, 65536);
            }
            applyBudget();
        } else if (a.rfind("pack=", 0) == 0) {
            const size_t c = a.rfind(':');
            if (c != std::string::npos && c > 5) {
                const std::string name = a.substr(5, c - 5);
                std::lock_guard lk(sMutex);
                for (auto& p : sConf.packs)
                    if (p.name == name) p.on = a.substr(c + 1) != "0";
                reload = true;
            }
        } else if (a.rfind("top=", 0) == 0) {
            const std::string name = a.substr(4);
            std::lock_guard lk(sMutex);
            auto& v = sConf.packs;
            if (auto f = std::find_if(v.begin(), v.end(), [&](const PackConf& p) { return p.name == name; }); f != v.end())
                std::rotate(v.begin(), f, f + 1);
            reload = true;
        }
        it = sSteps.erase(it);
    }
    if (reload) requestReload();
}

bool drawMenu() {
    bool dirty = false;
    Conf conf;
    std::vector<PackInfo> packs;
    std::string status;
    {
        std::lock_guard lk(sMutex);
        conf = sConf;
        packs = sPacks;
        status = sStatus;
    }
    const bool busy = sBusy;
    bool reload = false;

    if (recomp::menu::Checkbox("Use texture packs", &conf.enabled)) {
        dirty = reload = true;
    }
    ImGui::SetItemTooltip("Replacement textures (Dolphin's format: tex1_..._<hash>.dds / .png) from the packs below.\n"
                          "Off: every texture is the game's own. Packs made for Dolphin work unchanged.");
    ImGui::TextDisabled("Folder: %s", port::displayPath(sPackRoot).c_str());
    if (recomp::menu::Button("Open texture packs folder")) openFolder(sPackRoot);
    ImGui::SetItemTooltip("Put each pack in its own folder here. Dolphin packs can be copied as they are\n"
                          "(a GLME01 folder, or Load/Textures/GLME01 inside the pack's folder).");
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (recomp::menu::Button(busy ? "Loading...###reload" : "Reload packs###reload")) reload = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Scans the folder again (new, changed or removed packs) and applies them now;\n"
                          "no restart needed. Edited files are picked up too.");
    ImGui::TextWrapped("%s", status.c_str());

    if (!packs.empty()) {
        ImGui::SeparatorText("Installed packs");
        ImGui::TextDisabled("Top of the list wins where two packs replace the same texture.");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("packs", 4, flags)) {
            ImGui::TableSetupColumn("On");
            ImGui::TableSetupColumn("Pack", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Textures");
            ImGui::TableSetupColumn("Order");
            ImGui::TableHeadersRow();
            int moveFrom = -1, moveTo = -1;
            for (int i = 0; i < int(packs.size()); i++) {
                const PackInfo& p = packs[i];
                ImGui::PushID(p.name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool on = packOn(conf, p.name);
                const bool usable = !p.root.empty();
                ImGui::BeginDisabled(!usable || !conf.enabled);
                if (ImGui::Checkbox("##on", &on)) {
                    for (auto& c : conf.packs)
                        if (c.name == p.name) c.on = on;
                    dirty = reload = true;
                }
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                if (usable) ImGui::TextUnformatted(p.name.c_str());
                else ImGui::TextDisabled("%s", p.name.c_str());
                if (ImGui::IsItemHovered()) {
                    std::string ids;
                    for (const auto& id : p.ids) ids += (ids.empty() ? "" : ", ") + id;
                    ImGui::SetTooltip("%s\n%s%s", port::displayPath(p.dir).c_str(),
                                      usable ? ("Files: " + p.layout).c_str() : "",
                                      usable ? "" : ("Made for " + ids + ", not " + sGameId + ": not loaded.").c_str());
                }
                ImGui::TableNextColumn();
                if (!usable) ImGui::TextDisabled("other region");
                else if (p.loaded) ImGui::Text("%zu", p.registered);
                else ImGui::TextDisabled("%zu", p.files);
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(i == 0);
                if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveFrom = i, moveTo = i - 1;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 == int(packs.size()));
                if (ImGui::ArrowButton("##down", ImGuiDir_Down)) moveFrom = i, moveTo = i + 1;
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (moveFrom >= 0) {
                // Reorder the saved list by the shown order (packs not on disk keep their place at the end).
                std::vector<PackConf> order;
                for (const auto& p : packs) order.push_back({p.name, packOn(conf, p.name)});
                std::swap(order[moveFrom], order[moveTo]);
                for (const auto& c : conf.packs) {
                    bool shown = false;
                    for (const auto& o : order) shown |= o.name == c.name;
                    if (!shown) order.push_back(c);
                }
                conf.packs = std::move(order);
                dirty = reload = true;
            }
        }
    }

    ImGui::SeparatorText("Memory");
    static const int kBudgets[] = {512, 1024, 2048, 4096, 8192};
    static const char* kBudgetNames[] = {"512 MB", "1 GB", "2 GB", "4 GB", "8 GB"};
    int bi = 2;
    for (int i = 0; i < 5; i++)
        if (conf.budgetMb == kBudgets[i]) bi = i;
    if (ImGui::Combo("Texture memory for packs", &bi, kBudgetNames, 5)) {
        conf.budgetMb = kBudgets[bi];
        dirty = true;
    }
    ImGui::SetItemTooltip("Graphics memory replacement textures may use. Past it the least recently drawn\n"
                          "ones are released and loaded again when needed (a short pop-in).\n"
                          "Large HD packs: 2-4 GB on cards with 8 GB or more.");
    const auto st = aurora::texture::replacement_stats();
    ImGui::TextDisabled("On the GPU: %llu textures, %.0f MB%s", (unsigned long long)st.cachedTextures,
                        st.cachedBytes / (1024.0 * 1024.0),
                        st.pendingLoads ? (", loading " + std::to_string(st.pendingLoads)).c_str() : "");

    ImGui::SeparatorText("For pack makers");
    bool dump = sDump;
    if (recomp::menu::Checkbox("Dump textures", &dump)) {
        sDump = dump;
        applyDump();
    }
    ImGui::SetItemTooltip("Writes every texture the game loads (once each) as a PNG with Dolphin's file name\n"
                          "into the dump folder: edit or upscale them and put them in a pack. The textures of\n"
                          "the current scene are written at once. Not saved: off again at the next start.");
    ImGui::SameLine();
    if (recomp::menu::Checkbox("Include mipmaps", &conf.dumpMips)) {
        dirty = true;
        std::lock_guard lk(sMutex);
        sConf.dumpMips = conf.dumpMips;
    }
    ImGui::SetItemTooltip("Also the smaller levels of mipmapped textures (<name>_mip1.png, ...), as Dolphin does.");
    if (sDump || st.dumped || st.dumpSkipped)
        ImGui::TextDisabled("Dumped this session: %llu written, %llu already there%s", (unsigned long long)st.dumped,
                            (unsigned long long)st.dumpSkipped,
                            st.dumpQueued ? (", " + std::to_string(st.dumpQueued) + " queued").c_str() : "");
    ImGui::TextDisabled("Folder: %s", port::displayPath(sDumpRoot).c_str());
    if (recomp::menu::Button("Open dump folder")) openFolder(sDumpRoot);

    if (dirty) {
        std::lock_guard lk(sMutex);
        sConf.enabled = conf.enabled;
        sConf.budgetMb = conf.budgetMb;
        sConf.dumpMips = conf.dumpMips;
        sConf.packs = conf.packs;
    }
    if (dirty) {
        applyBudget();
        if (sDump) applyDump();
    }
    if (reload) requestReload();
    return dirty;
}

}  // namespace recomp::texpacks
