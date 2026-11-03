#include "setup/disc_check.h"

#include "setup/disc.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>

namespace setup {
namespace {

struct RetailFile {
    const char* path;
    std::uint32_t size;
    std::uint32_t crc;
};

constexpr RetailFile kFilesGLME01[] = {
#include "setup/disc_files_GLME01.inc"
};
constexpr RetailFile kFilesGLMP01[] = {
#include "setup/disc_files_GLMP01.inc"
};
constexpr RetailFile kFilesGLMJ01[] = {
#include "setup/disc_files_GLMJ01.inc"
};

// Bump when the tables or the cache format change.
constexpr const char* kCacheFormat = "disc-check 1";

std::span<const RetailFile> retailFiles(port::GameVersion v) {
    if (v == port::GameVersion::PAL) return kFilesGLMP01;
    if (v == port::GameVersion::JPN) return kFilesGLMJ01;
    return kFilesGLME01;
}

std::filesystem::path utf8Path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

std::filesystem::path cacheFile(port::GameVersion v) {
    return port::dataDir() / (std::string("disc_check_") + port::gameId(v) + ".txt");
}

// Identifies the image file: a different file, or the same one rewritten, is checked again.
std::string imageKey(const std::string& path) {
    std::error_code ec;
    const auto p = port::longPath(utf8Path(path));
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) return {};
    const auto time = std::filesystem::last_write_time(p, ec);
    if (ec) return {};
    return std::string(kCacheFormat) + "|" + path + "|" + std::to_string(size) + "|" +
           std::to_string(time.time_since_epoch().count());
}

}  // namespace

std::string DiscCheck::summary(size_t maxNames) const {
    if (bad.empty()) return "all " + std::to_string(files) + " game files match the retail disc";
    std::string s = std::to_string(bad.size()) + " of " + std::to_string(files) + " game files differ from the retail disc: ";
    for (size_t i = 0; i < bad.size() && i < maxNames; i++) s += (i ? ", " : "") + bad[i];
    if (bad.size() > maxNames) s += ", ...";
    return s;
}

bool discCheckEnabled() {
    const char* e = std::getenv("LM_DISC_CHECK");
    return !(e && std::strcmp(e, "0") == 0);
}
bool discCheckForced() {
    const char* e = std::getenv("LM_DISC_CHECK");
    return e && std::strcmp(e, "force") == 0;
}

std::optional<DiscCheck> cachedDiscCheck(const std::string& path, port::GameVersion v) {
    if (discCheckForced()) return std::nullopt;
    const std::string key = imageKey(path);
    if (key.empty()) return std::nullopt;
    std::ifstream in(port::longPath(cacheFile(v)), std::ios::binary);
    std::string line;
    auto next = [&] {  // one line, CR LF tolerated (a file edited by hand)
        if (!std::getline(in, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    if (!next() || line != key) return std::nullopt;
    DiscCheck c;
    if (!next() || line.rfind("files ", 0) != 0) return std::nullopt;
    c.files = std::atoi(line.c_str() + 6);
    while (next()) {
        if (line == "accepted") c.accepted = true;
        else if (line.rfind("bad ", 0) == 0) c.bad.push_back(line.substr(4));
    }
    return c;
}

void saveDiscCheck(const std::string& path, port::GameVersion v, const DiscCheck& c) {
    const std::string key = imageKey(path);
    if (key.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(port::longPath(port::dataDir()), ec);
    std::ofstream out(port::longPath(cacheFile(v)), std::ios::binary | std::ios::trunc);
    out << key << "\nfiles " << c.files << "\n";
    for (const auto& b : c.bad) out << "bad " << b << "\n";
    if (c.accepted) out << "accepted\n";
}

std::optional<DiscCheck> checkDiscFiles(const std::string& path, port::GameVersion v, const std::atomic<bool>* cancel,
                                        const std::function<void(float)>& progress) {
    Disc disc;
    std::string err;
    DiscCheck c;
    if (!disc.open(path, err)) {
        c.bad.push_back("the image could not be read (" + err + ")");
        return c;
    }
    const auto files = retailFiles(v);
    std::uint64_t total = 0, done = 0;
    for (const auto& f : files) total += f.size;
    for (const auto& f : files) {
        if (cancel && cancel->load()) return std::nullopt;
        std::uint32_t size = 0, crc = 0;
        bool found = false;
        const bool read = disc.fileCrc(f.path, size, crc, found);
        if (!found) c.bad.push_back(std::string(f.path) + " (missing)");
        else if (size != f.size) c.bad.push_back(std::string(f.path) + " (size)");
        else if (!read) c.bad.push_back(std::string(f.path) + " (unreadable)");
        else if (crc != f.crc) c.bad.push_back(std::string(f.path) + " (contents)");
        c.files++;
        done += f.size;
        if (progress) progress(total ? float(double(done) / double(total)) : 1.0f);
    }
    return c;
}

}  // namespace setup
