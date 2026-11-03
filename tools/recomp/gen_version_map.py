#!/usr/bin/env python3
"""Generate the European (GLMP01) or Japanese (GLMJ01) version map used by the recompiler
(docs/SETUP.md, "Version-aware code").

    python tools/recomp/gen_version_map.py [--target pal|jp|all] [--us DIR] [--pal DIR] [--jp DIR]

    --target pal (default): tools/recomp/GLMP01_map.txt + src/recomp/version_addrs.inc
    --target jp:            tools/recomp/GLMJ01_map.txt + src/recomp/version_addrs_jp.inc
    --target all:           both

The Japanese function table (decomp/config/GLMJ01/symbols.txt) comes from decomp-toolkit's
analysis of the Japanese main.dol (`dtk dol split`, MIT, dev-only tool; docs/SETUP.md
"The Japanese game"). Below, "PAL" stands for the target release.

The PAL decomp config has no translation-unit splits and fewer named functions, but the
runtime finds SDK functions (HLE) and hooks by their US names. This matches every PAL
function to its US counterpart and writes

    <PAL address> <canonical name> <translation unit | - | hook>

for each PAL function that belongs to an SDK unit (HLE'd or recompiled by TU), carries a
PAL-tagged hook, or is unnamed in the PAL map but named in the US one.

Names come from, in order: the PAL map itself; the US function tools/pal_diff.py pairs it
with; and call-site agreement (a matched caller pair whose `bl` sequences line up names
the PAL callee after the US one). The last step names the SDK functions the PAL release
rewrote (PADInit, PADRead, ...), which the similarity matcher cannot pair.

The output is address/name metadata only (like decomp/config/*/symbols.txt); it is
committed and embedded in the executable. Re-run it after tagging a hook `pal` in
tools/recomp/hooks.txt. Needs the user's own US + PAL dumps (dev machines only).
"""
import argparse
import difflib
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import pal_diff as P  # noqa: E402

SDK_PREFIXES = ("os/", "gx/", "vi/", "pad/", "dvd/", "ar/", "card/", "ai/", "dsp/", "db/", "si/",
                "exi/", "TRK_MINNOW_DOLPHIN/", "OdemuExi2/", "amcstubs/", "odenotstub/")


# SDK functions the PAL release reorganised, identified by hand from their callers and
# bodies (tools/pal_diff.py show - --pal ADDR --side pal). These override the matcher.
OVERRIDES = {"pal": {
    0x801E1E7C: ("OSGetLanguage", "os/OSRtc.c"),           # returns SRAM byte 0x12
    0x801EBDF8: ("VISetPostRetraceCallback", "vi/vi.c"),   # US body, PostCB slot
    0x801EBE3C: ("VISetPreRetraceCallback", "vi/vi.c"),    # second callback slot (new in PAL)
    0x801EE0AC: ("PADReset", "pad/Pad.c"),                 # JUTGamePad::read error path
    0x801EE86C: ("PADSetSamplingRate", "pad/Pad.c"),       # tail-calls SISetSamplingRate
    0x801EE88C: ("PADControlMotor", "pad/Pad.c"),          # JUTGamePad::CRumble callers
}, "jp": {}}


def unnamed(n):
    return not n or n == "-" or n.startswith(("fn_", "lbl_"))


def calls(v, f):
    out = []
    for a in range(f["addr"], f["addr"] + f["size"], 4):
        w = v.dol.word(a)
        if w >> 26 == 18 and (w & 3) == 1:  # bl
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            out.append((a + li) & 0xFFFFFFFF)
    return out


# Per target release: decomp config, hooks.txt tag, prefix of functions only it has, outputs.
TARGETS = {
    "pal": dict(cfg="GLMP01_00", tag="pal", fnprefix="palfn_", name="European", id="GLMP01",
                out="GLMP01_map.txt", addrs="version_addrs.inc"),
    "jp": dict(cfg="GLMJ01", tag="jp", fnprefix="jpfn_", name="Japanese", id="GLMJ01",
               out="GLMJ01_map.txt", addrs="version_addrs_jp.inc"),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", choices=["pal", "jp", "all"], default="pal")
    ap.add_argument("--us", default=str(P.DEF_US))
    ap.add_argument("--pal", default=str(P.DEF_PAL))
    ap.add_argument("--jp", default=str(P.DEF_JP))
    ap.add_argument("--out", help="map file (default tools/recomp/<id>_map.txt)")
    ap.add_argument("--addrs", help="address table (default src/recomp/version_addrs[_jp].inc)")
    a = ap.parse_args()
    us = P.Version("us", a.us, "GLME01")
    failed = []
    for t in (["pal", "jp"] if a.target == "all" else [a.target]):
        err = gen_target(a, us, t)
        if err:
            failed.append(err)
    if failed:
        sys.exit("\n".join(failed))


def gen_target(a, us, target):
    T = TARGETS[target]
    out = a.out or str(ROOT / "tools" / "recomp" / T["out"])
    addrs = a.addrs or str(ROOT / "src" / "recomp" / T["addrs"])
    pal = P.Version(target, a.pal if target == "pal" else a.jp, T["cfg"])
    recs, _, dmap = P.match_functions(us, pal)
    hooked = set()

    # PAL address -> [name, TU, kind]
    info = {}
    us_to_pal = {}
    for r in recs:
        u, p = r["us"], r["pal"]
        if not p:
            continue
        name = p["name"] if not unnamed(p["name"]) or not u else u["name"]
        info[p["addr"]] = [name, us.tu(u["addr"]) if u else "", r["kind"]]
        if u:
            us_to_pal.setdefault(u["name"], p["addr"])

    # Call-site agreement: matched callers with equally long bl lists name their callees.
    votes = defaultdict(Counter)
    for r in recs:
        u, p = r["us"], r["pal"]
        if not u or not p or r["kind"] == "pal-only":
            continue
        cu, cp = calls(us, u), calls(pal, p)
        if len(cu) != len(cp):
            continue
        for tu_, tp in zip(cu, cp):
            fu, fp = us.fat.get(tu_), pal.fat.get(tp)
            if fu and fp and unnamed(info.get(tp, ["-"])[0]) and not unnamed(fu["name"]):
                votes[tp][(fu["name"], us.tu(tu_))] += 1
    for tp, c in votes.items():
        (name, tu), n = c.most_common(1)[0]
        if n * 2 > sum(c.values()) and tp in info:
            info[tp][0] = name
            if not info[tp][1]:
                info[tp][1] = tu
            us_to_pal.setdefault(name, tp)

    # The SDK is linked as one contiguous block; a pairing outside it is a false match by
    # the similarity matcher. Unmatched PAL functions inside it take the preceding TU.
    sdk = [addr for addr, (n, tu, kind) in info.items() if kind == "same" and tu.startswith(SDK_PREFIXES)]
    lo, hi = min(sdk), max(sdk)
    last_tu = ""
    for addr in sorted(info):
        e = info[addr]
        if e[1].startswith(SDK_PREFIXES) and not lo <= addr <= hi:
            e[0], e[1] = pal.fat[addr]["name"], ""
        if not e[1] and e[2] == "pal-only" and lo <= addr <= hi:
            e[1] = last_tu
        if e[1]:
            last_tu = e[1]

    for addr, (name, tu) in OVERRIDES.get(target, {}).items():
        info[addr][0], info[addr][1] = name, tu
        us_to_pal[name] = addr

    lines = ["# Generated by tools/recomp/gen_version_map.py from tools/pal_diff.py matching. Do not edit.",
             f"# <{T['name']} ({T['id']}) address> <canonical name> <translation unit | - | hook>"]
    named = Counter(n for addr, (n, tu, kind) in info.items() if not tu.startswith(SDK_PREFIXES))
    for addr in sorted(info):
        name, tu, kind = info[addr]
        if tu.startswith(SDK_PREFIXES):
            lines.append(f"{addr:08X} {name} {tu}")
        elif not unnamed(name) and unnamed(pal.fat[addr]["name"]) and named[name] == 1:
            lines.append(f"{addr:08X} {name} -")  # (ambiguous names are left out)

    # Hooks tagged for the target ("<symbol> ... pal|jp" in hooks.txt) must have a counterpart
    # there. Functions only the target has are written palfn_/jpfn_<address>.
    missing = []
    fn_re = re.compile(re.escape(T["fnprefix"]) + r"([0-9A-F]{8})")
    for line in (ROOT / "tools" / "recomp" / "hooks.txt").read_text(encoding="utf-8").splitlines():
        words = line.split("#")[0].split()
        if len(words) < 2 or T["tag"] not in words[1:] or words[0] == "patch":
            continue  # (patch lines carry their own address)
        m = fn_re.fullmatch(words[0])
        addr = int(m[1], 16) if m and int(m[1], 16) in pal.fat else us_to_pal.get(words[0])
        if addr is None:
            missing.append(words[0])
        elif not info.get(addr, ["", ""])[1].startswith(SDK_PREFIXES):
            lines.append(f"{addr:08X} {words[0]} hook")
            hooked.add(addr)

    # Game functions the runtime calls by their USA name (guestSymbol / GU_SYM / callOriginal
    # in src/): give their PAL counterparts the same name.
    unmatched = []
    for nm in sorted(referenced_names()):
        addr = us_to_pal.get(nm)
        if addr is None or addr in hooked:
            if addr is None and nm in us.by_name and nm not in pal.by_name:
                unmatched.append(nm)
            continue
        line = f"{addr:08X} {nm} -"
        if info.get(addr, ["", ""])[1].startswith(SDK_PREFIXES) or pal.fat[addr]["name"] == nm or line in lines:
            continue
        lines.append(line)
    Path(out).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    left = [l for l in lines[2:] if l.split()[1].startswith("fn_") and l.split()[2] not in ("-", "hook")]
    print(f"{out}: {len(lines) - 2} entries; {len(left)} SDK functions still unnamed")
    for l in left:
        print("  " + l)
    if unmatched:
        print(f"functions the runtime names that have no {T['name']} counterpart (features using them must "
              "stay off there): " + ", ".join(unmatched))

    # USA addresses the runtime uses through VA(...) (src/recomp/version.h): their equivalents.
    bad = write_address_table(us, pal, recs, dmap, addrs, target)
    errs = []
    if missing:
        errs.append(f"hooks tagged {T['tag']} without a {T['name']} match: {', '.join(missing)}")
    if bad:
        errs.append(f"some VA() addresses have no {T['name']} equivalent: drop them from that path or add a fallback")
    return "; ".join(errs)

SRC_DIRS = [ROOT / "src" / "recomp", ROOT / "src" / "port"]


def sources():
    for d in SRC_DIRS:
        for p in sorted(d.glob("*")):
            if p.suffix in (".cpp", ".h", ".inc"):
                yield p, p.read_text(encoding="utf-8", errors="replace")


def referenced_names():
    rx = re.compile(r'(?:guestSymbol|GU_SYM)\(\s*"(\w+)"|callOriginal\(\s*\w+\s*,\s*"(\w+)"')
    out = set()
    for _, text in sources():
        for m in rx.finditer(text):
            out.add(m[1] or m[2])
    return out


VA_RE = re.compile(r"\bVA\(\s*0x([0-9A-Fa-f]{8})u?\s*\)")


def _align(a, b, i):
    """Index in b of element i of a, from the matching blocks of the two sequences."""
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    for x, y, n in sm.get_matching_blocks():
        if x <= i < x + n:
            return y + i - x
    return None


# Data the matcher cannot place (every reference is an r13 access among several alike in the
# function), identified by hand with tools/pal_diff.py show. USA address -> (PAL address, note).
DATA_OVERRIDES = {"pal": {
    # The health meter's argument: fn_8003E900 (identical) loads it for fn_800417F8,
    # lwz r3, 0x56c(r13) / lwz r3, 0x578(r13).
    0x804D84CC: (0x804C5B98, "lbl_804D84CC: health meter object, by hand (fn_8003E900)"),
    # Health shown by the meter and its damage-flash value, s16 pairs in fn_800417F8:
    # lha 0x530/0x532(r13) / lha 0x540/0x542(r13) (the matcher picked a neighbour for both).
    0x804D8490: (0x804C5B60, "lbl_804D8490: health meter value, by hand (fn_800417F8)"),
    0x804D8492: (0x804C5B62, "lbl_804D8492: health meter flash value, by hand (fn_800417F8)"),
}, "jp": {}}


def map_address(us, pal, u2p, dmap, addr, depth=0):
    """USA address -> (PAL address, how), or (None, why not)."""
    if addr in DATA_OVERRIDES[pal.tag]:
        return DATA_OVERRIDES[pal.tag][addr]
    f = us.func_at(addr)
    if f is not None or addr in us.fat:  # code: same instruction in the matched function
        f = f or us.fat[addr]
        p = u2p.get(f["addr"])
        if p is None:
            return None, f"{f['name']} has no PAL counterpart"
        pf = pal.fat[p]
        off = addr - f["addr"]
        if off == 0:
            return p, f"start of {f['name']}"
        if off == f["size"]:
            return p + pf["size"], f"end of {f['name']}"
        lu, lp = P.analyze(us, f)[0], P.analyze(pal, pf)[0]
        j = off // 4 if lu == lp else _align(lu, lp, off // 4)
        if j is None:
            return None, f"{f['name']}+{off:#x} is in code PAL changed"
        return p + 4 * j + off % 4, f"{f['name']}+{off:#x}"
    c = dmap.get(addr)
    if c:
        pe, n = c.most_common(1)[0]
        if n * 2 > sum(c.values()):
            return pe, f"{_dname(us, addr)}: {n} matching reference(s)"
    # Code that the PAL build reordered: the same instruction (opcode and registers) in the
    # matched PAL function, when every referencing function agrees on one PAL address.
    cands = Counter()
    for ad, uf, ea, w in P._xrefs(us, addr, addr + 1):
        p = u2p.get(uf["addr"])
        if p is None:
            continue
        hits = {pea for pa, pw, pea in P.walk(pal, pal.fat[p])
                if pea is not None and (pw >> 26) != 15 and (pw & 0xFFFF0000) == (w & 0xFFFF0000)
                and pal.is_ro(pea) == us.is_ro(addr) and not pal.func_at(pea)}
        if len(hits) == 1:
            cands[hits.pop()] += 1
    if len(cands) == 1 and not us.is_ro(addr):
        pe, n = cands.most_common(1)[0]
        return pe, f"{_dname(us, addr)}: same instruction in {n} matched function(s)"
    obj = next((o for o in us.syms if o["type"] == "object" and o["addr"] <= addr < o["addr"] + max(o["size"], 1)), None)
    if obj:
        # Another address of the same object that code references: the object moved as a whole.
        deltas = Counter()
        for ue, cc in dmap.items():
            if obj["addr"] <= ue < obj["addr"] + obj["size"]:
                deltas[cc.most_common(1)[0][0] - ue] += 1
        if deltas:
            d, n = deltas.most_common(1)[0]
            if n * 2 > sum(deltas.values()):
                return addr + d, f"{obj['name']}+{addr - obj['addr']:#x} (object moved by {d:+#x})"
        # A literal: the same value loaded by the matched PAL function.
        if us.is_ro(addr):
            cands = set()
            for ad, uf, ea, w in P._xrefs(us, addr, addr + 1):
                n = P.LOADS.get(w >> 26)
                p = u2p.get(uf["addr"])
                if not n or p is None:
                    continue
                want = us.dol.read(addr, n)
                for pa, pw, pea in P.walk(pal, pal.fat[p]):
                    if pea is not None and P.LOADS.get(pw >> 26) == n and pal.is_ro(pea) and pal.dol.read(pea, n) == want:
                        cands.add(pea)
            if len(cands) == 1:
                return cands.pop(), f"literal {us.dol.read(addr, 4).hex()} of the matched function"
            if cands:
                return None, f"literal is ambiguous in PAL ({', '.join(f'{x:08X}' for x in sorted(cands))})"
        # Something a pointer table points at: read the pointer in the PAL copy of the table.
        word = addr.to_bytes(4, "big")
        for sa, off, size, _ in us.dol.secs if depth < 2 else []:
            seg = us.dol.data[off:off + size]
            k = seg.find(word)
            while k >= 0:
                if k % 4 == 0:
                    pa, how = (map_address(us, pal, u2p, dmap, sa + k, depth + 1)
                               if sa + k != addr and depth < 2 else (None, ""))
                    if pa is not None and pal.dol.read(pa, 4):
                        return int.from_bytes(pal.dol.read(pa, 4), "big"), f"pointed at by {_dname(us, sa + k)} ({how})"
                k = seg.find(word, k + 1)
    return None, "no PAL reference found"


def _dname(v, ea):
    return P._dname(v, ea)


def write_address_table(us, pal, recs, dmap, out, target):
    u2p = {r["us"]["addr"]: r["pal"]["addr"] for r in recs if r["us"] and r["pal"]}
    wanted = {}
    for path, text in sources():
        for m in VA_RE.finditer(text):
            wanted.setdefault(int(m[1], 16), path.name)
    rows, bad = [], []
    for addr in sorted(wanted):
        pa, how = map_address(us, pal, u2p, dmap, addr)
        if pa is None:
            bad.append(f"{addr:08X} ({wanted[addr]}): {how}")
            continue
        rows.append(f"    {{0x{addr:08X}u, 0x{pa:08X}u}},  // {how}")
    T = TARGETS[target]
    text = ("// Generated by tools/recomp/gen_version_map.py. Do not edit.\n"
            f"// USA (GLME01) address -> {T['name']} ({T['id']}) address for every VA(...) in src/ (version.h).\n"
            + "\n".join(rows) + "\n")
    Path(out).write_text(text, encoding="utf-8", newline="\n")
    print(f"{out}: {len(rows)} addresses")
    for b in bad:
        print("  not mapped: " + b)
    return bad


if __name__ == "__main__":
    main()
