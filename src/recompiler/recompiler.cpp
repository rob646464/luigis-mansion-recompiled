// Static recompiler: main.dol -> C++. See recompiler.h and docs/RECOMPILER.md.
#include "recompiler/recompiler.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace lmrc {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using u64 = std::uint64_t;

// Translation units replaced by host (HLE) implementations.
constexpr std::string_view kHleTuPrefixes[] = {"os/", "gx/", "vi/", "pad/", "dvd/", "ar/", "card/", "ai/", "dsp/", "db/",
                                               "si/", "exi/", "TRK_MINNOW_DOLPHIN/", "OdemuExi2/", "amcstubs/",
                                               "odenotstub/"};
// SDK units that are plain computation on guest memory: recompiled like game code.
constexpr std::string_view kHleTuExclude[] = {"os/OSAlloc.c", "os/OSArena.c", "pad/Padclamp.c"};

bool isHleTu(std::string_view tu) {
    for (auto e : kHleTuExclude)
        if (tu == e) return false;
    for (auto p : kHleTuPrefixes)
        if (tu.substr(0, p.size()) == p) return true;
    return false;
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    const int n = std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    if (n < int(sizeof(buf))) return std::string(buf, n > 0 ? size_t(n) : 0);
    std::string s(size_t(n) + 1, '\0');
    va_start(ap, f);
    std::vsnprintf(s.data(), s.size(), f, ap);
    va_end(ap);
    s.resize(size_t(n));
    return s;
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        pos = nl + 1;
    }
    return lines;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool parseHex(std::string_view s, u32& out) {
    if (s.empty() || s.size() > 8) return false;
    u32 v = 0;
    for (char ch : s) {
        int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else return false;
        v = v << 4 | u32(d);
    }
    out = v;
    return true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

struct Section {
    u32 addr, off, size;
};

struct Dol {
    const std::vector<u8>& data;
    std::vector<Section> sections;  // in header order, non-empty only
    size_t textCount = 0;           // the first textCount entries are text sections

    explicit Dol(const std::vector<u8>& d) : data(d) {
        if (d.size() < 0x100) throw std::runtime_error("main.dol is too small");
        for (int i = 0; i < 18; i++) {
            const u32 off = be(u32(i * 4)), addr = be(u32(72 + i * 4)), size = be(u32(144 + i * 4));
            if (size) {
                if (size_t(off) + size > d.size()) throw std::runtime_error("main.dol section out of range");
                sections.push_back({addr, off, size});
                if (i < 7) textCount = sections.size();
            }
        }
    }
    u32 be(u32 off) const {
        return u32(data[off]) << 24 | u32(data[off + 1]) << 16 | u32(data[off + 2]) << 8 | data[off + 3];
    }
    u32 word(u32 addr) const {
        for (const auto& s : sections)
            if (s.addr <= addr && addr < s.addr + s.size) return be(s.off + addr - s.addr);
        throw std::runtime_error(fmt("address %08X is outside main.dol", addr));
    }
    bool has(u32 addr) const {
        for (const auto& s : sections)
            if (s.addr <= addr && addr < s.addr + s.size) return true;
        return false;
    }
};

struct Func {
    std::string name;
    u32 size;
};

// funcs: addr -> (name, size); objs: name -> (addr, size).
void loadSymbols(std::string_view text, std::map<u32, Func>& funcs, std::map<std::string, std::pair<u32, u32>>& objs) {
    std::map<u32, std::string> labels;
    for (std::string_view line : splitLines(text)) {
        // "<name> = .<section>:0x<addr>; // type:<type>[ size:0x<size>]..."
        const size_t sp = line.find(' ');
        if (sp == 0 || sp == std::string_view::npos) continue;
        const std::string_view name = line.substr(0, sp);
        std::string_view rest = line.substr(sp);
        if (rest.substr(0, 4) != " = .") continue;
        rest.remove_prefix(4);
        const size_t colon = rest.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view section = rest.substr(0, colon);
        bool word = !section.empty();
        for (char ch : section)
            if (!(std::isalnum(u8(ch)) || ch == '_')) word = false;
        if (!word) continue;
        rest.remove_prefix(colon + 1);
        if (rest.substr(0, 2) != "0x") continue;
        rest.remove_prefix(2);
        size_t n = 0;
        while (n < rest.size() && std::isxdigit(u8(rest[n]))) n++;
        u32 addr;
        if (!parseHex(rest.substr(0, n), addr)) continue;
        rest.remove_prefix(n);
        if (rest.substr(0, 10) != "; // type:") continue;
        rest.remove_prefix(10);
        n = 0;
        while (n < rest.size() && (std::isalnum(u8(rest[n])) || rest[n] == '_')) n++;
        const std::string_view type = rest.substr(0, n);
        if (type.empty()) continue;
        rest.remove_prefix(n);
        u32 size = 0;
        if (rest.substr(0, 8) == " size:0x") {
            rest.remove_prefix(8);
            n = 0;
            while (n < rest.size() && std::isxdigit(u8(rest[n]))) n++;
            if (n) parseHex(rest.substr(0, n), size);
        }
        const bool code = section == "text" || section == "init";
        if (type == "function" && code) funcs[addr] = {std::string(name), size};
        else if (type == "object") objs[std::string(name)] = {addr, size};
        else if (type == "label" && code) labels[addr] = std::string(name);
    }
    // A label inside a function is an alternate entry point: it becomes its own
    // function running to the end of the containing one.
    std::vector<std::pair<u32, u32>> starts;  // (addr, end)
    for (auto& [a, f] : funcs) starts.push_back({a, a + f.size});
    for (auto& [addr, name] : labels) {
        auto it = std::upper_bound(starts.begin(), starts.end(), addr,
                                   [](u32 v, const std::pair<u32, u32>& s) { return v < s.first; });
        if (it == starts.begin()) continue;
        --it;
        if (addr != it->first && addr < it->second) funcs[addr] = {name, it->second - addr};
    }
}

struct TuRange {
    u32 lo, hi;
};

std::vector<TuRange> loadHleRanges(std::string_view text) {
    std::vector<TuRange> ranges;
    std::string cur;
    for (std::string_view line : splitLines(text)) {
        if (!line.empty() && line[0] != '\t' && line[0] != ' ' && line.back() == ':' && line != "Sections:") {
            cur = std::string(line.substr(0, line.size() - 1));
        } else if (!cur.empty() && isHleTu(cur)) {
            // "\t.text       start:0x... end:0x..."
            std::string_view s = line;
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i == 0) continue;
            s.remove_prefix(i);
            if (s.substr(0, 5) == ".text") s.remove_prefix(5);
            else if (s.substr(0, 5) == ".init") s.remove_prefix(5);
            else continue;
            i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i == 0) continue;
            s.remove_prefix(i);
            if (s.substr(0, 8) != "start:0x") continue;
            s.remove_prefix(8);
            const size_t sp = s.find(' ');
            if (sp == std::string_view::npos) continue;
            u32 lo, hi;
            if (!parseHex(s.substr(0, sp), lo)) continue;
            s.remove_prefix(sp + 1);
            if (s.substr(0, 6) != "end:0x") continue;
            s.remove_prefix(6);
            size_t n = 0;
            while (n < s.size() && std::isxdigit(u8(s[n]))) n++;
            if (!parseHex(s.substr(0, n), hi)) continue;
            ranges.push_back({lo, hi});
        }
    }
    return ranges;
}

// ---------------------------------------------------------------------------
// Instruction translation
// ---------------------------------------------------------------------------

constexpr u32 bits(u32 w, int start, int end) {  // IBM bit numbering (0 = MSB)
    return (w >> (31 - end)) & ((1u << (end - start + 1)) - 1);
}
constexpr s32 sext(u32 v, int n) { return (v & (1u << (n - 1))) ? s32(v) - s32(1u << n) : s32(v); }
constexpr s32 sext26(u32 v) { return (v & 0x02000000) ? s32(v | 0xFC000000) : s32(v); }

u32 mask(u32 mb, u32 me) {
    const u32 a = 0xFFFFFFFFu >> mb;
    const u32 b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}

struct Unsupported {
    std::string what;
};

class Recompiler;

class FuncEmitter {
public:
    FuncEmitter(Recompiler& rc, u32 addr, u32 size) : rc_(rc), start_(addr), end_(addr + size) {}
    std::vector<std::string> translate();

private:
    Recompiler& rc_;
    u32 start_, end_;
    u32 pc_ = 0;
    std::set<u32> labels_;
    std::vector<std::string> lines_;

    static std::string R(u32 n) { return fmt("c.r[%u]", n); }
    static std::string ra0(u32 n) { return n == 0 ? "0u" : fmt("c.r[%u]", n); }
    void E(const std::string& s) { lines_.push_back("    " + s); }
    static std::string rec(u32 reg) { return fmt("c.setCr0(s32(c.r[%u]));", reg); }
    static std::string h8(u32 v) { return fmt("0x%08Xu", v); }

    std::string branchCond(u32 bo, u32 bi);
    std::string targetCode(u32 target, bool link, const std::string& cond = "true");
    void insn(u32 w, u32 pc);
    void op31(u32 w, u32 d, u32 a, u32 b);
    void fp(u32 w, u32 op, u32 d, u32 a, u32 b);
    void ps(u32 w, u32 d, u32 a, u32 b);
};

class Recompiler {
public:
    Recompiler(const Dol& dol, std::map<u32, Func> funcs, const std::map<std::string, std::pair<u32, u32>>& objs,
               const std::vector<TuRange>& hleRanges, const std::map<u32, std::pair<std::string, std::string>>* vmap)
        : dol(dol), funcs(std::move(funcs)) {
        if (vmap) {
            // Version map: SDK membership and canonical names come from the map.
            std::unordered_set<std::string> mapped;
            for (auto& [addr, f] : this->funcs) {
                auto it = vmap->find(addr);
                if (it == vmap->end()) continue;
                f.name = it->second.first;
                mapped.insert(f.name);
                if (isHleTu(it->second.second)) hle.insert(addr);
            }
            // The map gives some functions their USA names (e.g. fn_80008D58 for the PAL
            // function at 0x8000935C). A PAL function that the map leaves alone but that happens
            // to be named the same (the unrelated PAL fn_80008D58) is renamed, so that names
            // (hooks, HLE, guestSymbol) stay unique.
            for (auto& [addr, f] : this->funcs)
                if (!vmap->count(addr) && mapped.count(f.name)) f.name = "ver_" + f.name;
        } else {
            for (auto& [addr, f] : this->funcs)
                for (const auto& r : hleRanges)
                    if (r.lo <= addr && addr < r.hi) hle.insert(addr);
        }
        addressEntries = addAddressTakenEntries(objs);
        while (addInteriorEntries()) {
        }
        findJumpTables(objs);
    }

    const Dol& dol;
    std::map<u32, Func> funcs;
    std::unordered_set<u32> hle;
    std::unordered_set<std::string> hooks;
    // Runtime-switchable instruction patches: address -> (switch id, replacement word).
    std::unordered_map<u32, std::vector<std::pair<u32, u32>>> patches;
    std::map<std::string, unsigned> unsupported;
    std::unordered_map<u32, std::vector<u32>> jumpTables;
    size_t addressEntries = 0;

    static std::string fname(u32 a) { return fmt("f_%08X", a); }
    std::string callExpr(u32 target) const {
        if (funcs.count(target)) return fname(target) + "(c)";
        return fmt("c.call(0x%08Xu)", target);
    }

    void run(Output& out, const std::string& version);

private:
    // Index of the function containing t among `starts` (sorted), or -1.
    static long containing(const std::vector<u32>& starts, u32 t) {
        auto it = std::upper_bound(starts.begin(), starts.end(), t);
        if (it == starts.begin()) return -1;
        return long(it - starts.begin()) - 1;
    }

    // Branches from one function into the middle of another make the target an extra
    // entry point (a function running to the end of the containing one).
    bool addInteriorEntries() {
        std::vector<u32> starts;
        for (auto& [a, f] : funcs) starts.push_back(a);
        std::map<u32, Func> extra;
        for (auto& [faddr, f] : funcs) {
            if (hle.count(faddr)) continue;
            for (u32 pc = faddr; pc < faddr + f.size; pc += 4) {
                const u32 w = dol.word(pc);
                const u32 op = bits(w, 0, 5);
                u32 t;
                if (op == 18) {
                    const s32 li = sext26(w & 0x03FFFFFC);
                    t = (w & 2) ? u32(li) : pc + u32(li);
                } else if (op == 16) {
                    t = pc + u32(sext(w & 0xFFFC, 16));
                } else {
                    continue;
                }
                if ((faddr <= t && t < faddr + f.size) || funcs.count(t)) continue;
                const long i = containing(starts, t);
                if (i < 0) continue;
                const u32 ca = starts[size_t(i)];
                const Func& cf = funcs.at(ca);
                const u32 cend = ca + cf.size;
                if (t < cend && !hle.count(ca)) extra[t] = {cf.name + fmt("_entry_%08X", t), cend - t};
            }
        }
        for (auto& [a, f] : extra) funcs[a] = f;
        return !extra.empty();
    }

    // Code addresses the program materialises (lis/addi pairs in code, words in data
    // sections) may be called indirectly: give interior ones an entry point.
    size_t addAddressTakenEntries(const std::map<std::string, std::pair<u32, u32>>& objs) {
        std::vector<u32> starts;
        for (auto& [a, f] : funcs) starts.push_back(a);
        std::vector<std::pair<u32, u32>> text;
        for (size_t i = 0; i < dol.textCount; i++)
            text.push_back({dol.sections[i].addr, dol.sections[i].addr + dol.sections[i].size});
        std::unordered_set<u32> jt;
        for (auto& [name, o] : objs)
            if (name.rfind("jumptable_", 0) == 0)
                for (u32 a = o.first; a < o.first + o.second; a += 4) jt.insert(a);
        auto inText = [&](u32 t) {
            if (t % 4) return false;
            for (auto& [lo, hi] : text)
                if (lo <= t && t < hi) return true;
            return false;
        };
        std::set<u32> cands;
        for (auto& [faddr, f] : funcs) {
            std::unordered_map<u32, u32> his;
            for (u32 pc = faddr; pc < faddr + f.size; pc += 4) {
                const u32 w = dol.word(pc);
                const u32 op = bits(w, 0, 5);
                if (op == 15 && bits(w, 11, 15) == 0) {
                    his[bits(w, 6, 10)] = (w & 0xFFFF) << 16;
                } else if ((op == 14 || op == 24) && his.count(bits(w, 11, 15))) {
                    const u32 lo = op == 14 ? u32(sext(w & 0xFFFF, 16)) : (w & 0xFFFF);
                    cands.insert(his[bits(w, 11, 15)] + lo);
                }
            }
        }
        for (const auto& s : dol.sections) {
            bool isText = false;
            for (auto& [lo, hi] : text)
                if (lo <= s.addr && s.addr < hi) isText = true;
            if (isText) continue;
            for (u32 i = 0; i + 3 < s.size; i += 4) {
                if (jt.count(s.addr + i)) continue;
                cands.insert(dol.be(s.off + i));
            }
        }
        std::map<u32, Func> extra;
        for (u32 t : cands) {
            if (!inText(t) || funcs.count(t)) continue;
            const long i = containing(starts, t);
            if (i < 0) continue;
            const u32 ca = starts[size_t(i)];
            const Func& cf = funcs.at(ca);
            const u32 cend = ca + cf.size;
            if (t < cend && !hle.count(ca)) extra[t] = {cf.name + fmt("_entry_%08X", t), cend - t};
        }
        for (auto& [a, f] : extra) funcs[a] = f;
        return extra.size();
    }

    // Map each bctr address to jump-table targets: every table whose base the function
    // materialises (lis/addi). A base can be loaded once into a register and used by
    // several bctrs, and a stray case is harmless (cases match the exact ctr value), so
    // each bctr gets the targets of all of the function's tables.
    void findJumpTables(const std::map<std::string, std::pair<u32, u32>>& objs) {
        std::unordered_map<u32, u32> byAddr;
        for (auto& [name, o] : objs)
            if (name.rfind("jumptable_", 0) == 0) byAddr[o.first] = o.second;
        for (auto& [faddr, f] : funcs) {
            if (hle.count(faddr)) continue;
            std::unordered_map<u32, u32> hi;
            std::vector<u32> targets, bctrs;
            for (u32 pc = faddr; pc < faddr + f.size; pc += 4) {
                const u32 w = dol.word(pc);
                const u32 op = bits(w, 0, 5);
                if (op == 15 && bits(w, 11, 15) == 0) {
                    hi[bits(w, 6, 10)] = (w & 0xFFFF) << 16;
                } else if (op == 14 && hi.count(bits(w, 11, 15))) {
                    const u32 base = hi[bits(w, 11, 15)] + u32(sext(w & 0xFFFF, 16));
                    if (auto it = byAddr.find(base); it != byAddr.end())
                        for (u32 i = 0; i < it->second / 4; i++) targets.push_back(dol.word(base + i * 4));
                } else if (op == 19 && bits(w, 21, 30) == 528 && !(w & 1)) {
                    bctrs.push_back(pc);
                }
            }
            if (!targets.empty())
                for (u32 pc : bctrs) jumpTables[pc] = targets;
        }
    }
};

std::string FuncEmitter::branchCond(u32 bo, u32 bi) {
    std::vector<std::string> parts;
    if (!(bo & 0b00100)) {
        E("c.ctr--;");
        parts.push_back((bo & 0b00010) ? "c.ctr == 0" : "c.ctr != 0");
    }
    if (!(bo & 0b10000)) {
        const std::string test = fmt("c.crbit(%u)", bi);
        parts.push_back((bo & 0b01000) ? test : "!" + test);
    }
    if (parts.empty()) return "true";
    std::string s = parts[0];
    for (size_t i = 1; i < parts.size(); i++) s += " && " + parts[i];
    return s;
}

std::string FuncEmitter::targetCode(u32 target, bool link, const std::string& cond) {
    const bool always = cond == "true";
    if (link) {
        const std::string call = rc_.callExpr(target);
        E(fmt("c.lr = 0x%08Xu;", pc_ + 4));
        return always ? call + ";" : "if (" + cond + ") { " + call + "; }";
    }
    if (start_ <= target && target < end_ && target != start_) {
        labels_.insert(target);
        std::string jump = fmt("goto L_%08X;", target);
        if (target <= pc_) jump = "{ c.poll(); " + jump + " }";  // loop back-edge: let pending interrupts run
        return always ? jump : "if (" + cond + ") " + jump;
    }
    // Tail call (branch to another function, or to our own start).
    const std::string call = rc_.callExpr(target);
    return always ? call + "; return;" : "if (" + cond + ") { " + call + "; return; }";
}

std::vector<std::string> FuncEmitter::translate() {
    std::vector<std::pair<size_t, u32>> labelLines;  // index in lines_ -> address
    for (u32 pc = start_; pc < end_; pc += 4) {
        pc_ = pc;
        const u32 w = rc_.dol.word(pc);
        labelLines.push_back({lines_.size(), pc});
        lines_.push_back("");  // placeholder for the label
        auto one = [&](u32 word) {
            try {
                insn(word, pc);
            } catch (const Unsupported& e) {
                E(fmt("c.unimplemented(0x%08Xu, 0x%08Xu); // ", pc, word) + e.what);
                rc_.unsupported[e.what]++;
            }
        };
        if (auto it = rc_.patches.find(pc); it != rc_.patches.end()) {
            // hooks.txt "patch": the replacement runs while its switch is on.
            for (size_t i = 0; i < it->second.size(); i++) {
                E(fmt(i == 0 ? "if (g_patches[%u]) {" : "} else if (g_patches[%u]) {", it->second[i].first));
                one(it->second[i].second);
            }
            E("} else {");
            one(w);
            E("}");
        } else {
            one(w);
        }
    }
    // Execution that runs off the end continues into the next function.
    const u32 last = rc_.dol.word(end_ - 4);
    const u32 op = bits(last, 0, 5);
    const bool ends = (op == 18 && !(last & 1)) ||
                      (op == 19 && (bits(last, 21, 30) == 16 || bits(last, 21, 30) == 528) && !(last & 1) &&
                       (bits(last, 6, 10) & 0b10100) == 0b10100);
    if (!ends) lines_.push_back("    " + rc_.callExpr(end_) + "; return;");
    std::vector<std::string> out;
    out.reserve(lines_.size());
    size_t li = 0;
    for (size_t i = 0; i < lines_.size(); i++) {
        if (li < labelLines.size() && labelLines[li].first == i) {
            const u32 a = labelLines[li++].second;
            if (labels_.count(a)) out.push_back(fmt("L_%08X:;", a));
            continue;
        }
        out.push_back(std::move(lines_[i]));
    }
    return out;
}

void FuncEmitter::insn(u32 w, u32 pc) {
    const u32 op = bits(w, 0, 5);
    const u32 d = bits(w, 6, 10);
    const u32 a = bits(w, 11, 15);
    const u32 b = bits(w, 16, 20);
    const s32 simm = sext(w & 0xFFFF, 16);
    const u32 uimm = w & 0xFFFF;
    const u32 usimm = u32(simm);

    if (op == 18) {  // b / bl / ba / bla
        const s32 li = sext26(w & 0x03FFFFFC);
        const u32 target = ((w >> 1) & 1) ? u32(li) : pc + u32(li);
        E(targetCode(target, w & 1));
        return;
    }
    if (op == 16) {  // bc
        const s32 bd = sext(w & 0xFFFC, 16);
        const u32 target = ((w >> 1) & 1) ? u32(bd) : pc + u32(bd);
        const std::string cond = branchCond(d, a);
        E(targetCode(target, w & 1, cond));
        return;
    }
    if (op == 19) {
        const u32 xo = bits(w, 21, 30);
        const bool lk = w & 1;
        if (xo == 16) {  // bclr
            const std::string cond = branchCond(d, a);
            if (lk) E("{ const u32 t = c.lr;" + fmt(" c.lr = 0x%08Xu; if (", pc + 4) + cond + ") c.call(t); }");
            else E(cond != "true" ? "if (" + cond + ") return;" : "return;");
            return;
        }
        if (xo == 528) {  // bcctr
            const std::string cond = branchCond(d | 0b00100, a);
            if (lk) {
                E(fmt("c.lr = 0x%08Xu; if (", pc + 4) + cond + ") c.call(c.ctr);");
            } else {
                auto it = rc_.jumpTables.find(pc);
                if (it != rc_.jumpTables.end() && !it->second.empty()) {
                    std::set<u32> targets(it->second.begin(), it->second.end());
                    std::string cases;
                    for (u32 t : targets) {
                        if (start_ <= t && t < end_) {
                            labels_.insert(t);
                            if (!cases.empty()) cases += ' ';
                            cases += fmt("case 0x%08Xu: goto L_%08X;", t, t);
                        }
                    }
                    E("if (" + cond + ") { switch (c.ctr) { " + cases + " default: c.call(c.ctr); return; } }");
                } else {
                    E("if (" + cond + ") { c.call(c.ctr); return; }");
                }
            }
            return;
        }
        const std::string ea = fmt("c.crbit(%u)", a), eb = fmt("c.crbit(%u)", b);
        std::string v;
        switch (xo) {
        case 257: v = "(" + ea + " & " + eb + ")"; break;
        case 449: v = "(" + ea + " | " + eb + ")"; break;
        case 193: v = "(" + ea + " ^ " + eb + ")"; break;
        case 225: v = "!(" + ea + " && " + eb + ")"; break;
        case 33: v = "!(" + ea + " || " + eb + ")"; break;
        case 289: v = "(" + ea + " == " + eb + ")"; break;
        case 129: v = "(" + ea + " && !" + eb + ")"; break;
        case 417: v = "(" + ea + " || !" + eb + ")"; break;
        default: break;
        }
        if (!v.empty()) {
            E(fmt("c.setCrbit(%u, ", d) + v + ");");
            return;
        }
        if (xo == 0) {  // mcrf
            E(fmt("c.cr[%u] = c.cr[%u];", d >> 2, a >> 2));
            return;
        }
        if (xo == 150 || xo == 50) {  // isync, rfi
            E("// isync/rfi");
            return;
        }
        throw Unsupported{fmt("op19 xo%u", xo)};
    }

    // --- integer arithmetic with immediates ---
    switch (op) {
    case 14: E(R(d) + " = " + ra0(a) + " + " + h8(usimm) + ";"); return;            // addi / li
    case 15: E(R(d) + " = " + ra0(a) + " + " + h8(usimm << 16) + ";"); return;      // addis / lis
    case 12:
    case 13:  // addic, addic.
        E("{ const u32 s = " + R(a) + "; " + R(d) + " = s + " + h8(usimm) + "; c.ca = " + R(d) + " < s; }");
        if (op == 13) E(rec(d));
        return;
    case 8:  // subfic
        E("{ const u32 s = " + R(a) + "; " + R(d) + " = " + h8(usimm) + " - s; c.ca = s <= " + h8(usimm) + "; }");
        return;
    case 7: E(R(d) + " = u32(s32(" + R(a) + fmt(") * s32(%d));", simm)); return;  // mulli
    case 10: E(fmt("c.cmpu(%u, ", d >> 2) + R(a) + fmt(", 0x%Xu);", uimm)); return;
    case 11: E(fmt("c.cmps(%u, s32(", d >> 2) + R(a) + fmt("), %d);", simm)); return;
    // logical immediates: d = rS, a = rA
    case 24: E(R(a) + " = " + R(d) + fmt(" | 0x%Xu;", uimm)); return;
    case 25: E(R(a) + " = " + R(d) + fmt(" | 0x%Xu;", uimm << 16)); return;
    case 26: E(R(a) + " = " + R(d) + fmt(" ^ 0x%Xu;", uimm)); return;
    case 27: E(R(a) + " = " + R(d) + fmt(" ^ 0x%Xu;", uimm << 16)); return;
    case 28: E(R(a) + " = " + R(d) + fmt(" & 0x%Xu;", uimm)); E(rec(a)); return;
    case 29: E(R(a) + " = " + R(d) + fmt(" & 0x%Xu;", uimm << 16)); E(rec(a)); return;
    default: break;
    }
    // rotates
    if (op == 21 || op == 20 || op == 23) {
        const std::string sh = op != 23 ? fmt("%u", b) : "(" + R(b) + " & 31)";
        const u32 m = mask(bits(w, 21, 25), bits(w, 26, 30));
        const std::string rot = "c.rotl(" + R(d) + ", " + sh + ")";
        if (op == 20) E(R(a) + " = (" + rot + " & " + h8(m) + ") | (" + R(a) + " & " + h8(~m) + ");");  // rlwimi
        else E(R(a) + " = " + rot + " & " + h8(m) + ";");
        if (w & 1) E(rec(a));
        return;
    }

    // --- loads / stores ---
    const std::string ea = "(" + ra0(a) + " + " + h8(usimm) + ")";
    const std::string uea = "(" + R(a) + " + " + h8(usimm) + ")";
    auto loadExpr = [](u32 o, const std::string& addr) -> std::string {
        switch (o) {
        case 32: return "rd32(" + addr + ")";
        case 34: return "rd8(" + addr + ")";
        case 40: return "rd16(" + addr + ")";
        default: return "u32(s16(rd16(" + addr + ")))";  // 42
        }
    };
    if (op == 32 || op == 34 || op == 40 || op == 42) {
        E(R(d) + " = " + loadExpr(op, ea) + ";");
        return;
    }
    if (op == 33 || op == 35 || op == 41 || op == 43) {  // update forms
        E("{ const u32 e = " + uea + "; " + R(d) + " = " + loadExpr(op - 1, "e") + "; " + R(a) + " = e; }");
        return;
    }
    auto storeFn = [](u32 o) { return o == 36 ? "wr32" : o == 38 ? "wr8" : "wr16"; };
    if (op == 36 || op == 38 || op == 44) {
        E(std::string(storeFn(op)) + "(" + ea + ", " + R(d) + ");");
        return;
    }
    if (op == 37 || op == 39 || op == 45) {
        E("{ const u32 e = " + uea + "; " + storeFn(op - 1) + "(e, " + R(d) + "); " + R(a) + " = e; }");
        return;
    }
    switch (op) {
    case 46: E("{ u32 e = " + ea + fmt("; for (int i = %u; i < 32; i++, e += 4) c.r[i] = rd32(e); }", d)); return;   // lmw
    case 47: E("{ u32 e = " + ea + fmt("; for (int i = %u; i < 32; i++, e += 4) wr32(e, c.r[i]); }", d)); return;  // stmw
    case 48: E(fmt("c.setSingle(%u, rdf32(", d) + ea + "));"); return;  // lfs
    case 49: E("{ const u32 e = " + uea + fmt("; c.setSingle(%u, rdf32(e)); ", d) + R(a) + " = e; }"); return;
    case 50: E(fmt("c.f[%u] = rdf64(", d) + ea + ");"); return;  // lfd
    case 51: E("{ const u32 e = " + uea + fmt("; c.f[%u] = rdf64(e); ", d) + R(a) + " = e; }"); return;
    case 52: E("wrf32(" + ea + fmt(", float(c.f[%u]));", d)); return;  // stfs
    case 53: E("{ const u32 e = " + uea + fmt("; wrf32(e, float(c.f[%u])); ", d) + R(a) + " = e; }"); return;
    case 54: E("wrf64(" + ea + fmt(", c.f[%u]);", d)); return;
    case 55: E("{ const u32 e = " + uea + fmt("; wrf64(e, c.f[%u]); ", d) + R(a) + " = e; }"); return;
    default: break;
    }
    if (op == 56 || op == 57 || op == 60 || op == 61) {  // psq_l / psq_lu / psq_st / psq_stu
        const u32 wbit = bits(w, 16, 16), i = bits(w, 17, 19);
        const u32 off = u32(sext(w & 0xFFF, 12));
        const std::string base = (op == 56 || op == 60) ? ra0(a) : R(a);
        const std::string e = "(" + base + " + " + h8(off) + ")";
        const bool upd = op == 57 || op == 61;
        const char* fn = op <= 57 ? "psqLoad" : "psqStore";
        E("{ const u32 e = " + e + fmt("; c.%s(%u, e, %u, %u);", fn, d, wbit, i) + (upd ? " " + R(a) + " = e;" : "") + " }");
        return;
    }
    if (op == 31) return op31(w, d, a, b);
    if (op == 59 || op == 63) return fp(w, op, d, a, b);
    if (op == 4) return ps(w, d, a, b);
    if (op == 3) {
        E("// twi");
        return;
    }
    if (op == 17) {
        E("// sc");
        return;
    }
    throw Unsupported{fmt("op%u", op)};
}

void FuncEmitter::op31(u32 w, u32 d, u32 a, u32 b) {
    const u32 xo10 = bits(w, 21, 30);
    const u32 xo9 = bits(w, 22, 30);
    const bool rc = w & 1;
    auto recd = [&](u32 reg) {
        if (rc) E(rec(reg));
    };
    if (xo10 == 0) {
        E(fmt("c.cmps(%u, s32(", d >> 2) + R(a) + "), s32(" + R(b) + "));");
        return;
    }
    if (xo10 == 32) {
        E(fmt("c.cmpu(%u, ", d >> 2) + R(a) + ", " + R(b) + ");");
        return;
    }
    {
        std::string e;
        switch (xo9) {
        case 266: e = R(a) + " + " + R(b); break;                                        // add
        case 40: e = R(b) + " - " + R(a); break;                                         // subf
        case 235: e = "u32(s32(" + R(a) + ") * s32(" + R(b) + "))"; break;               // mullw
        case 104: e = "u32(-s32(" + R(a) + "))"; break;                                  // neg
        case 75: e = "u32((s64(s32(" + R(a) + ")) * s64(s32(" + R(b) + "))) >> 32)"; break;  // mulhw
        case 11: e = "u32((u64(" + R(a) + ") * u64(" + R(b) + ")) >> 32)"; break;         // mulhwu
        case 491: e = "c.divw(" + R(a) + ", " + R(b) + ")"; break;                       // divw
        case 459: e = "c.divwu(" + R(a) + ", " + R(b) + ")"; break;                      // divwu
        default: break;
        }
        if (!e.empty()) {
            E(R(d) + " = " + e + ";");
            recd(d);
            return;
        }
    }
    switch (xo9) {
    case 10: E("{ const u32 s = " + R(a) + "; " + R(d) + " = s + " + R(b) + "; c.ca = " + R(d) + " < s; }"); recd(d); return;
    case 138: E("{ const u64 t = u64(" + R(a) + ") + " + R(b) + " + c.ca; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    case 202: E("{ const u64 t = u64(" + R(a) + ") + c.ca; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    case 234: E("{ const u64 t = u64(" + R(a) + ") + c.ca + 0xFFFFFFFFull; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    case 8: E("{ const u32 x = " + R(a) + ", y = " + R(b) + "; " + R(d) + " = y - x; c.ca = y >= x; }"); recd(d); return;
    case 136: E("{ const u64 t = u64(~" + R(a) + ") + " + R(b) + " + c.ca; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    case 200: E("{ const u64 t = u64(~" + R(a) + ") + c.ca; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    case 232: E("{ const u64 t = u64(~" + R(a) + ") + c.ca + 0xFFFFFFFFull; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    default: break;
    }
    // logical: d = rS, a = rA
    {
        const char* o = nullptr;
        switch (xo10) {
        case 28: o = "&"; break;
        case 444: o = "|"; break;
        case 316: o = "^"; break;
        case 60: o = "& ~"; break;
        case 412: o = "| ~"; break;
        default: break;
        }
        if (o) {
            E(R(a) + " = " + R(d) + " " + o + R(b) + ";");
            recd(a);
            return;
        }
    }
    switch (xo10) {
    case 124: E(R(a) + " = ~(" + R(d) + " | " + R(b) + ");"); recd(a); return;  // nor
    case 476: E(R(a) + " = ~(" + R(d) + " & " + R(b) + ");"); recd(a); return;  // nand
    case 284: E(R(a) + " = ~(" + R(d) + " ^ " + R(b) + ");"); recd(a); return;  // eqv
    case 24: E(R(a) + " = (" + R(b) + " & 0x20) ? 0u : (" + R(d) + " << (" + R(b) + " & 31));"); recd(a); return;  // slw
    case 536: E(R(a) + " = (" + R(b) + " & 0x20) ? 0u : (" + R(d) + " >> (" + R(b) + " & 31));"); recd(a); return;  // srw
    case 792: E(fmt("c.sraw(%u, ", a) + R(d) + ", " + R(b) + " & 0x3F);"); recd(a); return;  // sraw
    case 824: E(fmt("c.sraw(%u, ", a) + R(d) + fmt(", %u);", b)); recd(a); return;          // srawi
    case 26: E(R(a) + " = c.cntlzw(" + R(d) + ");"); recd(a); return;
    case 922: E(R(a) + " = u32(s32(s16(" + R(d) + ")));"); recd(a); return;  // extsh
    case 954: E(R(a) + " = u32(s32(s8(" + R(d) + ")));"); recd(a); return;   // extsb
    default: break;
    }
    // indexed loads/stores
    const std::string ea = "(" + ra0(a) + " + " + R(b) + ")";
    const std::string uea = "(" + R(a) + " + " + R(b) + ")";
    auto lx = [](u32 x, const std::string& addr) -> std::string {
        switch (x) {
        case 23: case 55: return "rd32(" + addr + ")";
        case 87: case 119: return "rd8(" + addr + ")";
        case 279: case 311: return "rd16(" + addr + ")";
        case 343: case 375: return "u32(s16(rd16(" + addr + ")))";
        case 534: return "rd32r(" + addr + ")";
        case 790: return "rd16r(" + addr + ")";
        default: return {};
        }
    };
    if (xo10 == 23 || xo10 == 87 || xo10 == 279 || xo10 == 343 || xo10 == 534 || xo10 == 790) {
        E(R(d) + " = " + lx(xo10, ea) + ";");
        return;
    }
    if (xo10 == 55 || xo10 == 119 || xo10 == 311 || xo10 == 375) {
        E("{ const u32 e = " + uea + "; " + R(d) + " = " + lx(xo10, "e") + "; " + R(a) + " = e; }");
        return;
    }
    {
        const char* fn = nullptr;
        switch (xo10) {
        case 151: fn = "wr32"; break;
        case 215: fn = "wr8"; break;
        case 407: fn = "wr16"; break;
        case 662: fn = "wr32r"; break;
        case 918: fn = "wr16r"; break;
        default: break;
        }
        if (fn) {
            E(std::string(fn) + "(" + ea + ", " + R(d) + ");");
            return;
        }
        switch (xo10) {
        case 183: fn = "wr32"; break;
        case 247: fn = "wr8"; break;
        case 439: fn = "wr16"; break;
        default: break;
        }
        if (fn) {
            E("{ const u32 e = " + uea + "; " + fn + "(e, " + R(d) + "); " + R(a) + " = e; }");
            return;
        }
    }
    switch (xo10) {
    case 535: E(fmt("c.setSingle(%u, rdf32(", d) + ea + "));"); return;  // lfsx
    case 567: E("{ const u32 e = " + uea + fmt("; c.setSingle(%u, rdf32(e)); ", d) + R(a) + " = e; }"); return;
    case 599: E(fmt("c.f[%u] = rdf64(", d) + ea + ");"); return;
    case 631: E("{ const u32 e = " + uea + fmt("; c.f[%u] = rdf64(e); ", d) + R(a) + " = e; }"); return;
    case 663: E("wrf32(" + ea + fmt(", float(c.f[%u]));", d)); return;
    case 695: E("{ const u32 e = " + uea + fmt("; wrf32(e, float(c.f[%u])); ", d) + R(a) + " = e; }"); return;
    case 727: E("wrf64(" + ea + fmt(", c.f[%u]);", d)); return;
    case 759: E("{ const u32 e = " + uea + fmt("; wrf64(e, c.f[%u]); ", d) + R(a) + " = e; }"); return;
    case 983: E("wr32(" + ea + fmt(", u32(bits64(c.f[%u])));", d)); return;  // stfiwx
    default: break;
    }
    // psq_lx / psq_stx
    {
        const u32 sub = bits(w, 25, 30);
        if (sub == 6 || sub == 7 || sub == 38 || sub == 39) {
            const u32 wbit = bits(w, 21, 21), i = bits(w, 22, 24);
            const std::string e = (sub == 6 || sub == 7) ? ea : uea;
            const bool upd = sub == 38 || sub == 39;
            const char* fn = (sub == 6 || sub == 38) ? "psqLoad" : "psqStore";
            E("{ const u32 e = " + e + fmt("; c.%s(%u, e, %u, %u);", fn, d, wbit, i) + (upd ? " " + R(a) + " = e;" : "") + " }");
            return;
        }
    }
    // special registers
    const u32 spr = bits(w, 16, 20) << 5 | bits(w, 11, 15);
    if (xo10 == 339) {  // mfspr
        if (spr == 8) E(R(d) + " = c.lr;");
        else if (spr == 9) E(R(d) + " = c.ctr;");
        else if (spr == 1) E(R(d) + " = c.getXer();");
        else if (spr >= 912 && spr <= 919) E(R(d) + fmt(" = c.gqr[%u];", spr - 912));
        else E(R(d) + fmt(" = c.mfspr(%u);", spr));
        return;
    }
    if (xo10 == 467) {  // mtspr
        if (spr == 8) E("c.lr = " + R(d) + ";");
        else if (spr == 9) E("c.ctr = " + R(d) + ";");
        else if (spr == 1) E("c.setXer(" + R(d) + ");");
        else if (spr >= 912 && spr <= 919) E(fmt("c.gqr[%u] = ", spr - 912) + R(d) + ";");
        else E(fmt("c.mtspr(%u, ", spr) + R(d) + ");");
        return;
    }
    if (xo10 == 19) {
        E(R(d) + " = c.getCr();");
        return;
    }
    if (xo10 == 144) {  // mtcrf
        E(fmt("c.mtcrf(0x%02X, ", bits(w, 12, 19)) + R(d) + ");");
        return;
    }
    if (xo10 == 371) {
        E(R(d) + fmt(" = c.mftb(%u);", spr));
        return;
    }
    if (xo10 == 83 || xo10 == 146 || xo10 == 595 || xo10 == 210 || xo10 == 659 || xo10 == 242) {
        if (xo10 == 83) E(R(d) + " = c.msr;");
        else if (xo10 == 146) E("c.msr = " + R(d) + ";");
        else E("// sr access");
        return;
    }
    switch (xo10) {
    case 86: case 54: case 470: case 278: case 246: case 982: case 598: case 854: case 1014: case 758:
        if (xo10 == 1014) E("c.dcbz(" + ea + ");");
        else E("// cache/sync");
        return;
    case 4: E("// tw"); return;
    case 512: E(fmt("c.cr[%u] = u8((c.so ? 8 : 0) | (c.ov ? 4 : 0) | (c.ca ? 2 : 0)); c.ca = 0;", d >> 2)); return;
    default: break;
    }
    throw Unsupported{fmt("op31 xo%u", xo10)};
}

void FuncEmitter::fp(u32 w, u32 op, u32 d, u32 a, u32 b) {
    const u32 cc = bits(w, 21, 25);
    const u32 xo5 = bits(w, 26, 30);
    const u32 xo10 = bits(w, 21, 30);
    const bool rc = w & 1;
    const bool single = op == 59;
    const std::string A = fmt("c.f[%u]", a), B = fmt("c.f[%u]", b), C = fmt("c.f[%u]", cc);
    auto res = [&](const std::string& expr) {
        if (single) E(fmt("c.setSingle(%u, float(", d) + expr + "));");
        else E(fmt("c.f[%u] = ", d) + expr + ";");
        if (rc) E("c.setCr1();");
    };
    std::string e;
    switch (xo5) {
    case 21: e = A + " + " + B; break;
    case 20: e = A + " - " + B; break;
    case 25: e = A + " * " + C; break;
    case 18: e = A + " / " + B; break;
    case 29: e = "fma_(" + A + ", " + C + ", " + B + ")"; break;
    case 28: e = "fma_(" + A + ", " + C + ", -" + B + ")"; break;
    case 31: e = "-fma_(" + A + ", " + C + ", " + B + ")"; break;
    case 30: e = "-fma_(" + A + ", " + C + ", -" + B + ")"; break;
    case 24: if (single) e = "1.0 / " + B; break;
    case 26: e = "1.0 / sqrt_(" + B + ")"; break;
    case 22: e = "sqrt_(" + B + ")"; break;
    default: break;
    }
    if (!e.empty()) {
        res(e);
        return;
    }
    if (xo5 == 23) {  // fsel
        res("(" + A + " >= 0.0) ? " + C + " : " + B);
        return;
    }
    if (op == 63) {
        if (xo10 == 0 || xo10 == 32) {
            E(fmt("c.fcmp(%u, ", d >> 2) + A + ", " + B + (xo10 == 32 ? ", true);" : ", false);"));
            return;
        }
        std::string s;
        switch (xo10) {
        case 72: s = B; break;
        case 40: s = "-" + B; break;
        case 264: s = "fabs_(" + B + ")"; break;
        case 136: s = "-fabs_(" + B + ")"; break;
        default: break;
        }
        if (!s.empty()) {
            E(fmt("c.f[%u] = ", d) + s + ";");
            if (rc) E("c.setCr1();");
            return;
        }
        if (xo10 == 12) {  // frsp
            E(fmt("c.setSingle(%u, float(", d) + B + "));");
            return;
        }
        if (xo10 == 14 || xo10 == 15) {  // fctiw / fctiwz
            E(fmt("c.f[%u] = c.fctiw(", d) + B + (xo10 == 15 ? ", 1);" : ", 0);"));
            return;
        }
        if (xo10 == 583) {  // mffs
            E(fmt("c.f[%u] = dbl(u64(c.fpscr));", d));
            return;
        }
        if (xo10 == 711) {  // mtfsf
            E("c.fpscr = u32(bits64(" + B + "));");
            return;
        }
        if (xo10 == 70 || xo10 == 38 || xo10 == 134 || xo10 == 64) {
            E("// fpscr bit op");
            return;
        }
    }
    throw Unsupported{fmt("fp op%u xo%u", op, xo10)};
}

void FuncEmitter::ps(u32 w, u32 d, u32 a, u32 b) {
    const u32 c_ = bits(w, 21, 25);
    const u32 xo5 = bits(w, 26, 30);
    const u32 xo10 = bits(w, 21, 30);
    const std::string A0 = fmt("c.f[%u]", a), A1 = fmt("c.ps1[%u]", a);
    const std::string B0 = fmt("c.f[%u]", b), B1 = fmt("c.ps1[%u]", b);
    const std::string C0 = fmt("c.f[%u]", c_), C1 = fmt("c.ps1[%u]", c_);
    auto pair = [&](const std::string& e0, const std::string& e1) {
        E("{ const double v0 = double(float(" + e0 + ")), v1 = double(float(" + e1 + fmt(")); c.f[%u] = v0; c.ps1[%u] = v1; }", d, d));
    };
    auto fma = [](const std::string& x, const std::string& y, const std::string& z) {
        return "fma_(" + x + ", " + y + ", " + z + ")";
    };
    std::string e0, e1;
    switch (xo5) {
    case 21: e0 = A0 + " + " + B0; e1 = A1 + " + " + B1; break;
    case 20: e0 = A0 + " - " + B0; e1 = A1 + " - " + B1; break;
    case 25: e0 = A0 + " * " + C0; e1 = A1 + " * " + C1; break;
    case 18: e0 = A0 + " / " + B0; e1 = A1 + " / " + B1; break;
    case 29: e0 = fma(A0, C0, B0); e1 = fma(A1, C1, B1); break;
    case 28: e0 = fma(A0, C0, "-" + B0); e1 = fma(A1, C1, "-" + B1); break;
    case 31: e0 = "-" + fma(A0, C0, B0); e1 = "-" + fma(A1, C1, B1); break;
    case 30: e0 = "-" + fma(A0, C0, "-" + B0); e1 = "-" + fma(A1, C1, "-" + B1); break;
    case 12: e0 = A0 + " * " + C0; e1 = A1 + " * " + C0; break;  // ps_muls0
    case 13: e0 = A0 + " * " + C1; e1 = A1 + " * " + C1; break;  // ps_muls1
    case 14: e0 = fma(A0, C0, B0); e1 = fma(A1, C0, B1); break;  // ps_madds0
    case 15: e0 = fma(A0, C1, B0); e1 = fma(A1, C1, B1); break;  // ps_madds1
    case 10: e0 = A0 + " + " + B1; e1 = C1; break;               // ps_sum0
    case 11: e0 = C0; e1 = A0 + " + " + B1; break;               // ps_sum1
    case 24: e0 = "1.0 / " + B0; e1 = "1.0 / " + B1; break;      // ps_res
    case 26: e0 = "1.0 / sqrt_(" + B0 + ")"; e1 = "1.0 / sqrt_(" + B1 + ")"; break;  // ps_rsqrte
    case 23: e0 = "(" + A0 + " >= 0.0) ? " + C0 + " : " + B0; e1 = "(" + A1 + " >= 0.0) ? " + C1 + " : " + B1; break;  // ps_sel
    default: break;
    }
    if (!e0.empty()) {
        pair(e0, e1);
        if (w & 1) E("c.setCr1();");
        return;
    }
    switch (xo10) {
    case 40: e0 = "-" + B0; e1 = "-" + B1; break;                               // ps_neg
    case 72: e0 = B0; e1 = B1; break;                                           // ps_mr
    case 136: e0 = "-fabs_(" + B0 + ")"; e1 = "-fabs_(" + B1 + ")"; break;      // ps_nabs
    case 264: e0 = "fabs_(" + B0 + ")"; e1 = "fabs_(" + B1 + ")"; break;        // ps_abs
    case 528: e0 = A0; e1 = B0; break;                                          // ps_merge00
    case 560: e0 = A0; e1 = B1; break;                                          // ps_merge01
    case 592: e0 = A1; e1 = B0; break;                                          // ps_merge10
    case 624: e0 = A1; e1 = B1; break;                                          // ps_merge11
    default: break;
    }
    if (!e0.empty()) {
        E("{ const double v0 = " + e0 + ", v1 = " + e1 + fmt("; c.f[%u] = v0; c.ps1[%u] = v1; }", d, d));
        return;
    }
    if (xo10 == 0 || xo10 == 32 || xo10 == 64 || xo10 == 96) {  // ps_cmpu0, ps_cmpo0, ps_cmpu1, ps_cmpo1
        const bool first = xo10 == 0 || xo10 == 32;
        E(fmt("c.fcmp(%u, ", d >> 2) + (first ? A0 : A1) + ", " + (first ? B0 : B1) +
          ((xo10 == 32 || xo10 == 96) ? ", true);" : ", false);"));
        return;
    }
    if (xo10 == 1014) {  // dcbz_l
        E(fmt("c.dcbz(c.r[%u] + c.r[%u]);", a, b));
        return;
    }
    const u32 sub6 = bits(w, 25, 30);
    if (sub6 == 6 || sub6 == 7 || sub6 == 38 || sub6 == 39) {  // psq_lx etc. encoded under op4
        const u32 wbit = bits(w, 21, 21), i = bits(w, 22, 24);
        const std::string e = (sub6 == 6 || sub6 == 7) ? "(" + ra0(a) + fmt(" + c.r[%u])", b) : fmt("(c.r[%u] + c.r[%u])", a, b);
        const bool upd = sub6 == 38 || sub6 == 39;
        const char* fn = (sub6 == 6 || sub6 == 38) ? "psqLoad" : "psqStore";
        E("{ const u32 e = " + e + fmt("; c.%s(%u, e, %u, %u);", fn, d, wbit, i) + (upd ? fmt(" c.r[%u] = e;", a) : "") + " }");
        return;
    }
    throw Unsupported{fmt("ps xo%u/%u", xo10, xo5)};
}

std::string chunkName(int i) { return fmt("funcs_%03d.cpp", i); }

void Recompiler::run(Output& out, const std::string& version) {
    std::vector<u32> addrs;
    for (auto& [a, f] : funcs) addrs.push_back(a);
    std::vector<u32> hooked;
    for (u32 a : addrs)
        if (!hle.count(a) && hooks.count(funcs[a].name)) hooked.push_back(a);

    std::string header = "#pragma once\n// Generated from the user's main.dol by the LM recompiler. Do not commit.\n"
                         "#include \"recomp/ctx.h\"\nnamespace recomp {\n";
    for (u32 a : addrs) header += "void " + fname(a) + "(Ctx& c);\n";
    for (u32 a : hooked) header += "void " + fname(a) + "_orig(Ctx& c);\n";
    header += "}\n";
    out.files.push_back({"funcs.h", std::move(header)});

    // Contiguous chunks of roughly equal instruction count.
    u64 total = 0;
    for (u32 a : addrs) total += funcs[a].size / 4 + 8;
    size_t idx = 0;
    u64 done = 0;
    for (int ch = 0; ch < kChunks; ch++) {
        const u64 goal = total * u64(ch + 1) / kChunks;
        std::string text = "// Generated from the user's main.dol by the LM recompiler. Do not commit.\n"
                           "#include \"funcs.h\"\nnamespace recomp {\n";
        while (idx < addrs.size() && (done < goal || ch == kChunks - 1)) {
            const u32 a = addrs[idx++];
            const Func& f = funcs[a];
            done += f.size / 4 + 8;
            text += "// " + f.name + "\n";
            text += "void " + fname(a) + "(Ctx& c) {\n";
            text += fmt("    c.enter(0x%08Xu);\n", a);
            if (hle.count(a)) {
                text += "    static const HleFn s = hleLookup(\"" + f.name + "\");\n";
                text += "    if (s) { s(c); } else { hleMissing(\"" + f.name + fmt("\", 0x%08Xu, c); }\n", a);
            } else {
                if (hooks.count(f.name)) {
                    // The translated body lives in f_X_orig so a hook can call it.
                    text += "    static const HleFn hook = hleLookup(\"" + f.name + "\");\n";
                    text += "    if (hook) { hook(c); return; }\n";
                    text += "    " + fname(a) + "_orig(c);\n}\n";
                    text += "void " + fname(a) + "_orig(Ctx& c) {\n";
                }
                for (auto& line : FuncEmitter(*this, a, f.size).translate()) {
                    text += line;
                    text += '\n';
                }
            }
            text += "}\n";
        }
        text += "}\n";
        out.files.push_back({chunkName(ch), std::move(text)});
    }

    std::string t = "// Generated from the user's main.dol by the LM recompiler. Do not commit.\n"
                    "#include \"funcs.h\"\nnamespace recomp {\nextern const FuncEntry kFuncTable[] = {\n";
    for (u32 a : addrs) t += fmt("    {0x%08Xu, &", a) + fname(a) + "},\n";
    t += "};\n";
    t += "extern const SymEntry kSymTable[] = {\n";
    for (u32 a : addrs) t += "    {\"" + funcs[a].name + fmt("\", 0x%08Xu},\n", a);
    t += "};\n";
    t += "extern const FuncEntry kOrigTable[] = {\n";
    for (u32 a : hooked) t += fmt("    {0x%08Xu, &", a) + fname(a) + "_orig},\n";
    if (hooked.empty()) t += "    {0, nullptr},\n";
    t += "};\n";
    t += "extern const GameModule kGameModule = {\n";
    t += "    kModuleAbiVersion, \"" + version + "\",\n";
    t += fmt("    kFuncTable, %zuu, kSymTable, %zuu, kOrigTable, %zuu,\n", addrs.size(), addrs.size(), hooked.size());
    t += "};\n}\n";
    out.files.push_back({"func_table.cpp", std::move(t)});
    out.functions = unsigned(addrs.size());
}

}  // namespace

std::vector<std::string> outputFileNames() {
    std::vector<std::string> names{"funcs.h"};
    for (int i = 0; i < kChunks; i++) names.push_back(chunkName(i));
    names.push_back("func_table.cpp");
    return names;
}

bool recompile(const Input& in, Output& out, std::string& error) {
    try {
        const Dol dol(in.dol);
        std::map<u32, Func> all;
        std::map<std::string, std::pair<u32, u32>> objs;
        loadSymbols(in.symbols, all, objs);
        std::map<u32, Func> funcs;
        for (auto& [a, f] : all)
            if (f.size > 0 && dol.has(a)) funcs[a] = f;
        if (funcs.empty()) {
            error = fmt("no functions in the symbol map (%zu bytes, %zu symbols, %zu DOL sections)", in.symbols.size(), all.size(), dol.sections.size());
            return false;
        }
        std::map<u32, std::pair<std::string, std::string>> vmap;
        const bool haveMap = !in.versionMap.empty();
        if (haveMap) {
            for (std::string_view line : splitLines(in.versionMap)) {
                line = trim(line);
                if (line.empty() || line[0] == '#') continue;
                const size_t s1 = line.find(' ');
                const size_t s2 = s1 == std::string_view::npos ? s1 : line.find(' ', s1 + 1);
                u32 addr;
                if (s2 == std::string_view::npos || !parseHex(line.substr(0, s1), addr)) continue;
                vmap[addr] = {std::string(line.substr(s1 + 1, s2 - s1 - 1)), std::string(trim(line.substr(s2 + 1)))};
            }
        }
        Recompiler rc(dol, std::move(funcs), objs, haveMap ? std::vector<TuRange>{} : loadHleRanges(in.splits),
                      haveMap ? &vmap : nullptr);
        // hooks.txt: "<symbol> [us] [pal]". A line applies to the US build unless it lists
        // only other versions, and to PAL only when tagged "pal" (hooks are written
        // against US code; a PAL tag means the hook was checked against the PAL build).
        // Japanese build: tag "jp" (checked against the Japanese code, like "pal").
        const bool pal = in.version == "GLMP01", jp = in.version == "GLMJ01";
        for (std::string_view line : splitLines(in.hooks)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string_view> words;
            size_t p = 0;
            while (p < line.size()) {
                size_t q = line.find_first_of(" \t", p);
                if (q == std::string_view::npos) q = line.size();
                if (q > p) words.push_back(line.substr(p, q - p));
                p = q + 1;
            }
            bool tagUs = false, tagPal = false, tagJp = false;
            const bool isPatch = words[0] == "patch";
            for (size_t i = isPatch ? 4 : 1; i < words.size(); i++) {
                if (words[i] == "us") tagUs = true;
                else if (words[i] == "pal") tagPal = true;
                else if (words[i] == "jp") tagJp = true;
            }
            const bool applies = pal ? tagPal : jp ? tagJp : (tagUs || (!tagPal && !tagJp));
            if (!applies) continue;
            if (isPatch) {  // "patch <id> <address> <instruction word> [us] [pal]"
                u32 id = 0, addr = 0, word = 0;
                if (words.size() >= 4 && parseHex(words[2], addr) && parseHex(words[3], word)) {
                    id = u32(std::strtoul(std::string(words[1]).c_str(), nullptr, 10));
                    rc.patches[addr].push_back({id, word});
                }
                continue;
            }
            rc.hooks.insert(std::string(words[0]));
        }
        // Every hook for this release must name one of its functions: a hook whose name the
        // symbol map (or, for the European game, the version map) does not give any function would
        // silently never run (a stale GLMP01_map.txt after tagging a hook `pal`).
        {
            std::unordered_set<std::string> names;
            for (const auto& [a, f] : rc.funcs) names.insert(f.name);
            std::string missing;
            for (const std::string& h : rc.hooks)
                if (!names.count(h)) missing += (missing.empty() ? "" : ", ") + h;
            if (!missing.empty()) {
                error = "hooks.txt names functions the " + in.version + " build does not have (" + missing +
                        "): re-run tools/recomp/gen_version_map.py";
                return false;
            }
        }
        // Patch addresses must lie inside a function of this release.
        for (const auto& [addr, list] : rc.patches) {
            auto it = rc.funcs.upper_bound(addr);
            if (it == rc.funcs.begin() || addr >= std::prev(it)->first + std::prev(it)->second.size) {
                error = fmt("hooks.txt patch at %08X is outside every %s function", addr, in.version.c_str());
                return false;
            }
        }
        rc.run(out, in.version);
        size_t nhle = rc.hle.size();
        out.summary = fmt("recomp %s: %u functions (%zu HLE) -> %d files; %zu jump tables, %zu address-taken interior entries",
                          in.version.c_str(), out.functions, nhle, kChunks, rc.jumpTables.size(), rc.addressEntries);
        for (auto& [k, v] : rc.unsupported) out.summary += fmt("\n  unsupported %s: %u", k.c_str(), v);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace lmrc
