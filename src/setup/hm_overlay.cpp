// Builds the PAL Hidden Mansion data overlay for the US base from the user's US + PAL
// discs (C++ version of tools/hidden_mansion_data.py; docs/HIDDEN_MANSION.md):
//
//   Map/map2u.szp      copied unchanged from the PAL disc (served for /Map/map2.szp while
//                      the hidden-mansion flag is set).
//   Event/eventNN.szp  the US archive with only text/eventNN.txt (the language-neutral event
//                      script) swapped for the PAL one, stored as literal-only Yay0.
//   Event/event08.szp  E. Gadd's lab: the US archive with the PAL script plus a follow-up
//                      choice "The Hidden Mansion (USA)" / "(PAL)" (two message lines added);
//                      served whichever version is selected ([always]).
//   manifest.txt       one "guest path -> overlay file [condition]" line per redirect.
//
// and the reverse, the USA Hidden Mansion for the European base (usa/, docs/SETUP.md):
//
//   usa/Map/map2.szp      the US /Map/map2.szp (served for /Map/map2u.szp).
//   usa/Event/eventNN.szp the PAL archive (all languages) with text/eventNN.txt swapped for
//                         the US script; event08 is the PAL archive with the lab choice.
//   usa/manifest.txt
#include "setup/hm_overlay.h"

#include "port/install.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

namespace setup {
namespace {

using Bytes = std::vector<std::uint8_t>;

// Event scripts the PAL release changed for the Hidden Mansion.
constexpr const char* kEvents[] = {"08", "16", "73"};

std::uint32_t be32(const Bytes& b, size_t o) {
    if (o + 4 > b.size()) throw std::runtime_error("truncated archive");
    return std::uint32_t(b[o]) << 24 | std::uint32_t(b[o + 1]) << 16 | std::uint32_t(b[o + 2]) << 8 | b[o + 3];
}
std::uint16_t be16(const Bytes& b, size_t o) {
    if (o + 2 > b.size()) throw std::runtime_error("truncated archive");
    return std::uint16_t(b[o] << 8 | b[o + 1]);
}
void put32(Bytes& b, size_t o, std::uint32_t v) {
    b[o] = std::uint8_t(v >> 24);
    b[o + 1] = std::uint8_t(v >> 16);
    b[o + 2] = std::uint8_t(v >> 8);
    b[o + 3] = std::uint8_t(v);
}

Bytes yay0(const Bytes& d) {
    const std::uint32_t size = be32(d, 4);
    std::uint32_t linkOff = be32(d, 8), chunkOff = be32(d, 12), maskOff = 16, mask = 0;
    int bits = 0;
    Bytes out;
    out.reserve(size);
    while (out.size() < size) {
        if (bits == 0) {
            mask = be32(d, maskOff);
            maskOff += 4;
            bits = 32;
        }
        if (mask & 0x80000000u) {
            if (chunkOff >= d.size()) throw std::runtime_error("bad Yay0 data");
            out.push_back(d[chunkOff++]);
        } else {
            const std::uint16_t link = be16(d, linkOff);
            linkOff += 2;
            const size_t dist = (link & 0xFFF) + 1u;
            size_t n = link >> 12;
            if (n == 0) {
                if (chunkOff >= d.size()) throw std::runtime_error("bad Yay0 data");
                n = d[chunkOff++] + 0x12u;
            } else {
                n += 2;
            }
            if (dist > out.size()) throw std::runtime_error("bad Yay0 back-reference");
            for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
        }
        mask <<= 1;
        bits--;
    }
    out.resize(size);
    return out;
}

Bytes yaz0(const Bytes& d) {
    const std::uint32_t size = be32(d, 4);
    Bytes out;
    out.reserve(size);
    size_t src = 16;
    while (out.size() < size) {
        if (src >= d.size()) throw std::runtime_error("bad Yaz0 data");
        const std::uint8_t code = d[src++];
        for (int bit = 0; bit < 8 && out.size() < size; bit++) {
            if (code & (0x80 >> bit)) {
                if (src >= d.size()) throw std::runtime_error("bad Yaz0 data");
                out.push_back(d[src++]);
            } else {
                if (src + 1 >= d.size()) throw std::runtime_error("bad Yaz0 data");
                const std::uint8_t b1 = d[src], b2 = d[src + 1];
                src += 2;
                const size_t dist = (size_t(b1 & 0xF) << 8 | b2) + 1;
                size_t n = b1 >> 4;
                if (n == 0) {
                    if (src >= d.size()) throw std::runtime_error("bad Yaz0 data");
                    n = d[src++] + 0x12u;
                } else {
                    n += 2;
                }
                if (dist > out.size()) throw std::runtime_error("bad Yaz0 back-reference");
                for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
            }
        }
    }
    return out;
}

Bytes decompress(const Bytes& d) {
    if (d.size() >= 16 && std::memcmp(d.data(), "Yay0", 4) == 0) return yay0(d);
    if (d.size() >= 16 && std::memcmp(d.data(), "Yaz0", 4) == 0) return yaz0(d);
    return d;
}

// A valid Yay0 stream made only of literal bytes (what tools/hidden_mansion_data.py writes).
Bytes yay0Store(const Bytes& data) {
    const std::uint32_t nwords = std::uint32_t((data.size() + 31) / 32);
    const std::uint32_t linkOff = 16 + 4 * nwords;
    Bytes out(16 + 4 * size_t(nwords));
    std::memcpy(out.data(), "Yay0", 4);
    put32(out, 4, std::uint32_t(data.size()));
    put32(out, 8, linkOff);
    put32(out, 12, linkOff);
    for (std::uint32_t i = 0; i < nwords; i++) put32(out, 16 + 4 * i, 0xFFFFFFFFu);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

struct RarcEntry {
    size_t entryOffset;  // of the 20-byte file entry
    std::uint8_t flags;
    std::uint32_t dataRel, size;
};

// Every file in an uncompressed RARC archive: path -> entry.
std::map<std::string, RarcEntry> rarcFiles(const Bytes& arc) {
    if (arc.size() < 0x40 || std::memcmp(arc.data(), "RARC", 4) != 0) throw std::runtime_error("not a RARC archive");
    const size_t info = 0x20;
    const size_t nodeOff = be32(arc, info + 4) + info;
    const size_t entryOff = be32(arc, info + 12) + info;
    const size_t strOff = be32(arc, info + 20) + info;
    auto name = [&](size_t off) {
        std::string s;
        for (size_t i = strOff + off; i < arc.size() && arc[i]; i++) s += char(arc[i]);
        return s;
    };
    std::map<std::string, RarcEntry> files;
    int depth = 0;
    auto walk = [&](auto& self, std::uint32_t node, const std::string& prefix) -> void {
        if (++depth > 64) throw std::runtime_error("RARC directory loop");
        const size_t n = nodeOff + size_t(node) * 16;
        const std::uint16_t count = be16(arc, n + 10);
        const std::uint32_t first = be32(arc, n + 12);
        for (std::uint32_t i = first; i < first + count; i++) {
            const size_t e = entryOff + size_t(i) * 20;
            const std::uint32_t flagsName = be32(arc, e + 4);
            const std::uint32_t a = be32(arc, e + 8), size = be32(arc, e + 12);
            const std::string ename = name(flagsName & 0xFFFFFF);
            if (ename == "." || ename == "..") continue;
            if ((flagsName >> 24) & 0x02) self(self, a, prefix + ename + "/");
            else files[prefix + ename] = {e, std::uint8_t(flagsName >> 24), a, size};
        }
        --depth;
    };
    walk(walk, 0, "");
    return files;
}

Bytes member(const Bytes& arc, const RarcEntry& e) {
    const size_t dataOff = be32(arc, 0x0C) + 0x20;
    if (dataOff + e.dataRel + e.size > arc.size()) throw std::runtime_error("RARC member out of range");
    return Bytes(arc.begin() + std::ptrdiff_t(dataOff + e.dataRel), arc.begin() + std::ptrdiff_t(dataOff + e.dataRel + e.size));
}

// Copy of a RARC archive with one file's contents replaced: the new data is appended to
// the data section; directory/entry/string tables stay, so ids and names are stable.
Bytes rarcReplace(const Bytes& arc, const RarcEntry& target, const Bytes& data) {
    const size_t dataOff = be32(arc, 0x0C) + 0x20;
    const std::uint32_t dataLen = be32(arc, 0x10), mram = be32(arc, 0x14), aram = be32(arc, 0x18);
    Bytes out(arc.begin(), arc.begin() + std::ptrdiff_t(dataOff + dataLen));
    out.resize(out.size() + ((32 - out.size() % 32) % 32), 0);
    const std::uint32_t newRel = std::uint32_t(out.size() - dataOff);
    out.insert(out.end(), data.begin(), data.end());
    out.resize(out.size() + ((32 - out.size() % 32) % 32), 0);
    const std::uint32_t grow = std::uint32_t(out.size() - (dataOff + dataLen));
    put32(out, target.entryOffset + 8, newRel);
    put32(out, target.entryOffset + 12, std::uint32_t(data.size()));
    put32(out, 0x04, std::uint32_t(out.size()));
    put32(out, 0x10, dataLen + grow);
    if (target.flags & 0x10) put32(out, 0x14, mram + grow);       // preloaded to main RAM
    else if (target.flags & 0x20) put32(out, 0x18, aram + grow);  // preloaded to ARAM
    return out;
}

// ---- E. Gadd's lab menu: both Hidden Mansions (docs/HIDDEN_MANSION.md) -----------------
// Same transformation as tools/hidden_mansion_data.py (lab_script / lab_table).
constexpr int kLabFlagPal = 8;          // the PAL hidden-mansion save flag (recomp/hidden_mansion.h)
constexpr size_t kLabHiddenLine = 38;                    // "The Hidden Mansion" in every language

std::vector<Bytes> split(const Bytes& b, const std::string& sep) {
    std::vector<Bytes> out;
    size_t pos = 0;
    for (;;) {
        auto it = std::search(b.begin() + std::ptrdiff_t(pos), b.end(), sep.begin(), sep.end());
        out.emplace_back(b.begin() + std::ptrdiff_t(pos), it);
        if (it == b.end()) break;
        pos = size_t(it - b.begin()) + sep.size();
    }
    return out;
}
Bytes join(const std::vector<Bytes>& parts, const std::string& sep) {
    Bytes out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out.insert(out.end(), sep.begin(), sep.end());
        out.insert(out.end(), parts[i].begin(), parts[i].end());
    }
    return out;
}
Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
bool startsWith(const Bytes& b, const std::string& p) {
    return b.size() >= p.size() && std::equal(p.begin(), p.end(), b.begin(),
                                              [](char x, std::uint8_t y) { return std::uint8_t(x) == y; });
}

std::vector<Bytes> tableEntries(const Bytes& table) {
    auto e = split(table, "\r\n");
    while (!e.empty() && e.back().empty()) e.pop_back();
    return e;
}

// A message table (CRLF-terminated entries, LF inside an entry) with entries n and n+1 =
// "<Hidden Mansion line> (USA)" / "(PAL)"; shorter tables are padded with empty entries so the
// indices match in every language.
Bytes labTable(const Bytes& table, size_t n, bool japanese) {
    auto e = tableEntries(table);
    if (e.size() <= kLabHiddenLine || e.size() > n) throw std::runtime_error("unexpected lab message table");
    Bytes base = e[kLabHiddenLine];
    while (!base.empty() && (base.back() == '.' || base.back() == ' ')) base.pop_back();
    e.resize(n);
    // Japanese: full-width parentheses and letters in Shift-JIS.
    static const std::string kJp[2] = {"\x81\x69\x82\x74\x82\x72\x82\x60\x81\x6A", "\x81\x69\x82\x6F\x82\x60\x82\x6B\x81\x6A"};
    static const std::string kEn[2] = {" (USA)", " (PAL)"};
    for (int i = 0; i < 2; i++) {
        Bytes line = base;
        const std::string& s = japanese ? kJp[i] : kEn[i];
        line.insert(line.end(), s.begin(), s.end());
        e.push_back(line);
    }
    Bytes out = join(e, "\r\n");
    const std::string tail = "\r\n\r\n";
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

// event08 script with the "urayasiki" case turned into a USA/PAL follow-up choice.
Bytes labScript(const Bytes& script, size_t n) {
    const auto lines = split(script, "\r\n");
    const auto start = std::find(lines.begin(), lines.end(), bytes("<CASE>\"urayasiki\""));
    if (start == lines.end()) throw std::runtime_error("event08 has no Hidden Mansion case");
    const auto end = std::find_if(start + 1, lines.end(), [](const Bytes& l) { return startsWith(l, "<END>"); });
    if (end == lines.end()) throw std::runtime_error("event08 Hidden Mansion case has no end");
    std::vector<Bytes> body;
    for (auto it = start + 1; it != end + 1; ++it)
        if (!startsWith(*it, "<FLAGON>(8)") && !startsWith(*it, "<FLAGOFF>(8)")) body.push_back(*it);
    const auto ura = std::find(body.begin(), body.end(), bytes("<URALUIGI>"));
    if (ura == body.end()) throw std::runtime_error("event08 Hidden Mansion case has no URALUIGI");
    std::vector<Bytes> out(lines.begin(), start);
    out.push_back(bytes("<CASE>\"urayasiki\""));
    out.push_back(bytes("<CHOICE>\"ura_us\"(" + std::to_string(n) + ")\"ura_pal\"(" + std::to_string(n + 1) + ")<LISTEND>"));
    out.push_back(bytes("<ANYKEY>"));
    for (const bool pal : {false, true}) {
        out.push_back(bytes(pal ? "<CASE>\"ura_pal\"" : "<CASE>\"ura_us\""));
        out.insert(out.end(), body.begin(), ura);
        out.push_back(bytes(std::string(pal ? "<FLAGON>(" : "<FLAGOFF>(") + std::to_string(kLabFlagPal) + ")"));
        out.insert(out.end(), ura, body.end());
    }
    out.insert(out.end(), end + 1, lines.end());
    return join(out, "\r\n");
}

// event08 archive (either release, every language it has) with the lab choice.
Bytes labArchive(const Bytes& arc, const Bytes& palScript) {
    const auto files = rarcFiles(arc);
    size_t n = 0;
    for (const auto& [k, e] : files)
        if (k.rfind("message/", 0) == 0) n = std::max(n, tableEntries(member(arc, e)).size());
    Bytes out = rarcReplace(arc, files.at("text/event08.txt"), labScript(palScript, n));
    for (const auto& [k, e] : files) {
        if (k.rfind("message/", 0) != 0) continue;
        const Bytes table = labTable(member(arc, e), n, k.find("japanese") != std::string::npos);
        out = rarcReplace(out, rarcFiles(out).at(k), table);
    }
    const auto check = rarcFiles(out);
    if (check.size() != files.size()) throw std::runtime_error("archive rewrite failed");
    for (const auto& [k, e] : files)
        if (k != "text/event08.txt" && k.rfind("message/", 0) != 0 && member(out, check.at(k)) != member(arc, e))
            throw std::runtime_error("archive rewrite failed");
    return out;
}

bool writeFile(const std::filesystem::path& p, const Bytes& b) {
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return bool(o);
}

// ---- Per-version lab menu (docs/VERSION_DIFFERENCES.md "Lab menu design") ---------------
using port::GameVersion;
constexpr int kVersionFlagA = 60, kVersionFlagB = 76;  // recomp/hidden_mansion.h
constexpr size_t kLabMansionLine = 3;                  // "The Mansion" / やしきにいく

// Rules files of a release (served from versions/<ID>/ for another release's mansion).
constexpr const char* kVersionFiles[] = {"/Map/map2.szp", "/Map/map4.szp", "/Iwamoto/map2/room_70.arc",
                                         "/Iwamoto/map2/room_08.arc", "/model/kopabody.szp"};

struct LabChoice {
    GameVersion v;
    size_t message;  // index of its line in every message table
};

// " (USA)" / " (Japan)" / " (PAL)"; Japanese tables: full-width (ＵＳＡ) / (日本) / (ＰＡＬ).
std::string versionSuffix(GameVersion v, bool japanese, bool english) {
    if (japanese) {
        if (v == GameVersion::USA) return "\x81\x69\x82\x74\x82\x72\x82\x60\x81\x6A";
        if (v == GameVersion::JPN) return "\x81\x69\x93\xFA\x96\x7B\x81\x6A";
        return "\x81\x69\x82\x6F\x82\x60\x82\x6B\x81\x6A";
    }
    if (v == GameVersion::USA) return " (USA)";
    if (v == GameVersion::JPN) return english ? " (Japan)" : " (JPN)";
    return " (PAL)";
}

// The lab's message table with the follow-up lines appended (padded to `n` entries first, so
// the indices are the same in every language of the archive).
Bytes versionLabTable(const Bytes& table, size_t n, bool japanese, bool english, const std::vector<LabChoice>& mansion,
                      const std::vector<LabChoice>& hidden) {
    auto e = tableEntries(table);
    if (e.size() <= kLabHiddenLine || e.size() > n) throw std::runtime_error("unexpected lab message table");
    auto line = [&](size_t i) {
        Bytes b = e[i];
        while (!b.empty() && (b.back() == '.' || b.back() == ' ')) b.pop_back();
        return b;
    };
    const Bytes mansionLine = line(kLabMansionLine), hiddenLine = line(kLabHiddenLine);
    e.resize(n);
    for (const auto* list : {&mansion, &hidden})
        for (const LabChoice& c : *list) {
            Bytes l = list == &mansion ? mansionLine : hiddenLine;
            const std::string s = versionSuffix(c.v, japanese, english);
            l.insert(l.end(), s.begin(), s.end());
            if (e.size() != c.message) throw std::runtime_error("lab message index mismatch");
            e.push_back(l);
        }
    Bytes out = join(e, "\r\n");
    const std::string tail = "\r\n\r\n";
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

// The flag lines that record version v as the choice on base `base` ((60, 76), flag 8 = PAL Hidden).
std::vector<Bytes> versionFlagLines(GameVersion v, GameVersion base, bool hidden) {
    const bool own = v == base;
    const bool a = !own && (v == GameVersion::USA || v == GameVersion::PAL);
    const bool b = !own && (v == GameVersion::JPN || v == GameVersion::PAL);
    auto flag = [](bool on, int n) { return bytes(std::string(on ? "<FLAGON>(" : "<FLAGOFF>(") + std::to_string(n) + ")"); };
    std::vector<Bytes> out{flag(a, kVersionFlagA), flag(b, kVersionFlagB)};
    if (hidden) out.push_back(flag(v == GameVersion::PAL, 8));
    return out;
}

// One destination case ("back" = The Mansion, "urayasiki" = The Hidden Mansion) turned into a
// follow-up choice; `marker` is the command the flags go before (<OMOTELUIGI> / <URALUIGI>).
std::vector<Bytes> versionCase(const std::vector<Bytes>& lines, const std::string& caseName, const std::string& marker,
                               const std::vector<LabChoice>& choices, GameVersion base, bool hidden) {
    const auto start = std::find(lines.begin(), lines.end(), bytes("<CASE>\"" + caseName + "\""));
    if (start == lines.end()) throw std::runtime_error("event08 has no \"" + caseName + "\" case");
    const auto end = std::find_if(start + 1, lines.end(), [](const Bytes& l) { return startsWith(l, "<END>"); });
    if (end == lines.end()) throw std::runtime_error("event08 \"" + caseName + "\" case has no end");
    const std::vector<Bytes> body(start + 1, end + 1);
    const auto at = std::find(body.begin(), body.end(), bytes(marker));
    if (at == body.end()) throw std::runtime_error("event08 \"" + caseName + "\" case has no " + marker);
    std::vector<Bytes> out(lines.begin(), start);
    out.push_back(*start);
    if (choices.size() > 1) {
        std::string list = "<CHOICE>";
        for (const LabChoice& c : choices)
            list += "\"" + caseName + "_" + port::gameId(c.v) + "\"(" + std::to_string(c.message) + ")";
        out.push_back(bytes(list + "<LISTEND>"));
        out.push_back(bytes("<ANYKEY>"));
    }
    for (size_t i = 0; i < choices.size(); i++) {
        if (choices.size() > 1) out.push_back(bytes("<CASE>\"" + caseName + "_" + port::gameId(choices[i].v) + "\""));
        out.insert(out.end(), body.begin(), at);
        const auto flags = versionFlagLines(choices[i].v, base, hidden);
        out.insert(out.end(), flags.begin(), flags.end());
        out.insert(out.end(), at, body.end());
    }
    out.insert(out.end(), end + 1, lines.end());
    return out;
}

Bytes versionLabScript(const Bytes& script, GameVersion base, const std::vector<LabChoice>& mansion,
                       const std::vector<LabChoice>& hidden) {
    std::vector<Bytes> lines;
    // The choice is cleared on entering the lab (as the European script clears flag 8), so a
    // save made in the lab and every new visit start from the base's own version.
    lines.push_back(bytes("<FLAGOFF>(8)"));
    lines.push_back(bytes("<FLAGOFF>(" + std::to_string(kVersionFlagA) + ")"));
    lines.push_back(bytes("<FLAGOFF>(" + std::to_string(kVersionFlagB) + ")"));
    for (Bytes& l : split(script, "\r\n"))
        if (!startsWith(l, "<FLAGON>(8)") && !startsWith(l, "<FLAGOFF>(8)")) lines.push_back(std::move(l));
    lines = versionCase(lines, "back", "<OMOTELUIGI>", mansion, base, false);
    lines = versionCase(lines, "urayasiki", "<URALUIGI>", hidden, base, true);
    return join(lines, "\r\n");
}

}  // namespace

bool buildVersionsOverlay(const std::vector<const Disc*>& discs, const std::filesystem::path& outDir, std::string& error) {
    const std::filesystem::path out = port::longPath(outDir);
    try {
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        std::filesystem::create_directories(out);
        auto have = [&](GameVersion v) {
            return std::any_of(discs.begin(), discs.end(), [&](const Disc* d) { return d->version() == v; });
        };
        std::string manifest;
        for (const Disc* d : discs) {
            const auto dir = out / port::gameId(d->version());
            for (const char* f : kVersionFiles) {
                Bytes b;
                if (!d->readFile(f, b)) throw std::runtime_error(std::string("the ") + port::regionName(d->version()) + " disc has no " + f);
                const auto dst = dir / std::filesystem::path(f + 1).make_preferred();  // (extended paths: '\' only)
                std::filesystem::create_directories(dst.parent_path());
                if (!writeFile(dst, b)) throw std::runtime_error("could not write the versions overlay");
                manifest += std::string(port::gameId(d->version())) + " " + f + "\n";
            }
        }
        for (const Disc* baseDisc : discs) {
            const GameVersion base = baseDisc->version();
            // The base's own version first (A twice = today's behaviour), then USA, Japan, PAL.
            std::vector<GameVersion> order{base};
            for (GameVersion v : {GameVersion::USA, GameVersion::JPN, GameVersion::PAL})
                if (v != base && have(v)) order.push_back(v);
            Bytes raw;
            if (!baseDisc->readFile("/Event/event08.szp", raw)) throw std::runtime_error("missing /Event/event08.szp");
            const Bytes arc = decompress(raw);
            const auto files = rarcFiles(arc);
            size_t n = 0;
            for (const auto& [k, e] : files)
                if (k.rfind("message/", 0) == 0) n = std::max(n, tableEntries(member(arc, e)).size());
            std::vector<LabChoice> mansion, hidden;
            size_t next = n;
            for (GameVersion v : order) mansion.push_back({v, next++});
            for (GameVersion v : order) {
                // Hidden Mansions offered: the European one on the Japanese base is not done yet; the
                // European one on the USA base and the USA one on the European base need both discs
                // (the Hidden Mansion overlay), which holds whenever both are in `order`.
                if (base == GameVersion::JPN && v == GameVersion::PAL) continue;
                hidden.push_back({v, next++});
            }
            const auto script = files.find("text/event08.txt");
            if (script == files.end()) throw std::runtime_error("event08 has no script");
            Bytes lab = rarcReplace(arc, script->second, versionLabScript(member(arc, script->second), base, mansion, hidden));
            for (const auto& [k, e] : files) {
                if (k.rfind("message/", 0) != 0) continue;
                const bool japanese = k.find("japanese") != std::string::npos || base == GameVersion::JPN;
                const bool english = k.find("english") != std::string::npos || base == GameVersion::USA;
                lab = rarcReplace(lab, rarcFiles(lab).at(k), versionLabTable(member(arc, e), n, japanese, english, mansion, hidden));
            }
            const auto check = rarcFiles(lab);
            if (check.size() != files.size()) throw std::runtime_error("archive rewrite failed");
            const auto dst = out / "lab" / port::gameId(base) / "Event";
            std::filesystem::create_directories(dst);
            if (!writeFile(dst / "event08.szp", yay0Store(lab))) throw std::runtime_error("could not write the versions overlay");
            manifest += std::string("lab ") + port::gameId(base) + " /Event/event08.szp\n";
        }
        std::ofstream(out / "manifest.txt", std::ios::binary) << manifest;
        return true;
    } catch (const std::exception& e) {
        error = std::string("Building the per-version mansion data failed: ") + e.what();
        return false;
    }
}

namespace {
}  // namespace

bool buildHiddenMansionOverlay(const Disc& us, const Disc& pal, const std::filesystem::path& outDir, std::string& error) {
    // Extended path: a deep %APPDATA% works without the long path policy (docs/SETUP.md "Long paths").
    const std::filesystem::path out = port::longPath(outDir);
    try {
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        std::filesystem::create_directories(out / "Map");
        std::filesystem::create_directories(out / "Event");
        std::string manifest;
        Bytes map;
        if (!pal.readFile("/Map/map2u.szp", map)) throw std::runtime_error("the PAL disc has no /Map/map2u.szp");
        if (!writeFile(out / "Map" / "map2u.szp", map)) throw std::runtime_error("could not write the overlay");
        manifest += "/Map/map2.szp -> Map/map2u.szp [hidden]\n";
        // Event archives: `into` with its script replaced by the one from `from`.
        auto events = [&](const Disc& into, const Disc& from, const std::filesystem::path& dir, const std::string& prefix,
                          std::string& list) {
            for (const char* ev : kEvents) {
                const std::string file = std::string("event") + ev + ".szp";
                const std::string path = "/Event/" + file, script = std::string("text/event") + ev + ".txt";
                Bytes baseRaw, fromRaw;
                if (!into.readFile(path.c_str(), baseRaw) || !from.readFile(path.c_str(), fromRaw))
                    throw std::runtime_error("missing " + path);
                const Bytes u = decompress(baseRaw), p = decompress(fromRaw);
                const auto uf = rarcFiles(u), pf = rarcFiles(p);
                const auto ui = uf.find(script), pi = pf.find(script);
                if (ui == uf.end() || pi == pf.end()) throw std::runtime_error(path + " has no " + script);
                if (std::string(ev) == "08") {
                    // The lab menu: served whichever version is chosen, so it can ask. Its script
                    // is the European one (which keeps flag 8 in step) on both games.
                    const Bytes palScript = prefix.empty() ? member(p, pi->second) : member(u, ui->second);
                    if (!writeFile(dir / "Event" / file, yay0Store(labArchive(u, palScript))))
                        throw std::runtime_error("could not write the overlay");
                    list += path + " -> " + prefix + "Event/" + file + " [always]\n";
                    continue;
                }
                const Bytes newScript = member(p, pi->second);
                if (member(u, ui->second) == newScript) continue;  // identical: nothing to serve
                const Bytes patched = rarcReplace(u, ui->second, newScript);
                // Same checks as the Python tool: the swapped member, and nothing else changed.
                const auto cf = rarcFiles(patched);
                if (member(patched, cf.at(script)) != newScript) throw std::runtime_error("archive rewrite failed");
                for (auto& [k, e] : uf)
                    if (k != script && member(patched, cf.at(k)) != member(u, e)) throw std::runtime_error("archive rewrite failed");
                const Bytes packed = yay0Store(patched);
                if (decompress(packed) != patched) throw std::runtime_error("Yay0 packing failed");
                if (!writeFile(dir / "Event" / file, packed)) throw std::runtime_error("could not write the overlay");
                list += path + " -> " + prefix + "Event/" + file + "\n";
            }
        };
        events(us, pal, out, "", manifest);
        std::ofstream(out / "manifest.txt", std::ios::binary) << manifest;

        // The reverse: USA Hidden Mansion data for the European game.
        const auto usa = out / "usa";
        std::filesystem::create_directories(usa / "Map");
        std::filesystem::create_directories(usa / "Event");
        std::string reverse;
        Bytes usMap;
        if (!us.readFile("/Map/map2.szp", usMap)) throw std::runtime_error("the USA disc has no /Map/map2.szp");
        if (!writeFile(usa / "Map" / "map2.szp", usMap)) throw std::runtime_error("could not write the overlay");
        reverse += "/Map/map2u.szp -> usa/Map/map2.szp [hidden]\n";
        events(pal, us, usa, "usa/", reverse);
        std::ofstream(usa / "manifest.txt", std::ios::binary) << reverse;
        return true;
    } catch (const std::exception& e) {
        error = std::string("Building the Hidden Mansion data failed: ") + e.what();
        return false;
    }
}

}  // namespace setup
