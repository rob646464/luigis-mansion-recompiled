#!/usr/bin/env python3
"""Diff the PAL (GLMP01) build of Luigi's Mansion against the US (GLME01) build.

Works only on the user's own dumps; prints analysis, never writes game data into the repo.

    python tools/pal_diff.py code    [--tsv out.tsv] [--all]      # match functions, list differences
    python tools/pal_diff.py show    <US addr|name|-> [--pal ADDR] [--side pal]  # side-by-side disassembly
    python tools/pal_diff.py var     <pal|us> <ADDR> [--size N]   # map a variable to the other version + uses
    python tools/pal_diff.py xref    <pal|us> <ADDR> [--exact]    # code referencing an address / its object
    python tools/pal_diff.py data                                 # same-shaped data objects whose bytes differ
    python tools/pal_diff.py files   [--deep]                     # disc files that differ (and archive members)
    python tools/pal_diff.py arcdiff <A.szp> <B.szp> [--path RE]  # archive members / JMP rows that differ
    python tools/pal_diff.py strings <regex>                      # strings in both DOLs

See docs/HIDDEN_MANSION.md for what these found.

Defaults point at the dump folders next to the repo; override with --us / --pal (disc roots
containing sys/main.dol and files/).

Matching: every function (from decomp/config/*/symbols.txt) is reduced to a "loose"
fingerprint (opcodes and registers; branch displacements and 16-bit immediates masked).
The PAL DOL was built with a compiler that schedules some instructions differently, so the
address-ordered lists are aligned (difflib) on the *multiset* of loose tokens; moved
functions are then paired by identical fingerprint and the rest by similarity between the
same matched neighbours.  Pairs are compared "strictly": immediates directly, address
operands (lis/@l pairs, r2/r13 small data, registers derived from them) by the *value*
they load from read-only data (.rodata/.sdata2) or as a wildcard for writable data, and
bl targets through the function map.  Pairs that differ only in struct field offsets are
reported as "layout".
"""
import argparse
import bisect
import difflib
import hashlib
import re
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

# The user's dumps sit next to the repo (or next to the main checkout, for worktrees).
DUMPS = next((p for p in [ROOT, *ROOT.parents] if (p / "Luigis Mansion US").exists()), ROOT)
DEF_US = DUMPS / "Luigis Mansion US"
DEF_PAL = DUMPS / "Lugis Mansion PAL"
DEF_JP = DUMPS / "Luigis Mansion JP"

SYM_RE = re.compile(r"(\S+) = (\.?\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")

# D-form opcodes whose low 16 bits are an immediate/displacement.
DFORM = set(range(7, 16)) | {24, 25, 26, 27, 28, 29} | set(range(32, 56))
LOADS = {32: 4, 33: 4, 34: 1, 35: 1, 40: 2, 41: 2, 42: 2, 43: 2, 48: 4, 49: 4, 50: 8, 51: 8}
PSQ = {56, 57, 60, 61}
LOGICAL = {24, 25, 26, 27, 28, 29}  # rS,rA swapped: result in rA


class Dol:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        d = self.data
        offs = struct.unpack(">18I", d[0:72])
        addrs = struct.unpack(">18I", d[72:144])
        sizes = struct.unpack(">18I", d[144:216])
        self.secs = sorted((a, o, s, i) for i, (o, a, s) in enumerate(zip(offs, addrs, sizes)) if s)
        self.bss = struct.unpack(">II", d[216:224])

    def read(self, addr, n):
        for a, o, s, _ in self.secs:
            if a <= addr and addr + n <= a + s:
                return self.data[o + addr - a:o + addr - a + n]
        return None

    def word(self, addr):
        return struct.unpack(">I", self.read(addr, 4))[0]


class Version:
    def __init__(self, tag, disc, cfg):
        self.tag = tag
        self.disc = Path(disc)
        self.dol = Dol(self.disc / "sys" / "main.dol")
        self.syms = []
        for line in (ROOT / "decomp/config" / cfg / "symbols.txt").read_text().splitlines():
            m = SYM_RE.match(line)
            if m:
                self.syms.append(dict(name=m[1], sec=m[2], addr=int(m[3], 16), type=m[4],
                                      size=int(m[5], 16) if m[5] else 0))
        self.funcs = sorted((s for s in self.syms if s["type"] == "function" and s["size"]
                             and s["sec"] in (".text", ".init")), key=lambda s: s["addr"])
        self.objs = sorted((s for s in self.syms if s["type"] == "object" and s["size"]
                            and s["sec"] in (".data", ".rodata", ".sdata", ".sdata2")),
                           key=lambda s: s["addr"])
        self.by_name = {s["name"]: s for s in self.syms}
        self.fstarts = [f["addr"] for f in self.funcs]
        self.fat = {f["addr"]: f for f in self.funcs}
        self.ro = [(s["addr"], s["addr"] + s["size"]) for s in self._section_ranges(".rodata", ".sdata2")]
        self.sda2, self.sda = self._sda_bases()
        self.splits = self._splits(cfg)

    def _section_ranges(self, *names):
        out = []
        for n in names:
            ss = [s for s in self.syms if s["sec"] == n]
            if ss:
                lo = min(s["addr"] for s in ss)
                hi = max(s["addr"] + s["size"] for s in ss)
                out.append(dict(addr=lo, size=hi - lo))
        return out

    def _sda_bases(self):
        f = self.by_name["__init_registers"]
        regs = {}
        for a in range(f["addr"], f["addr"] + f["size"], 4):
            w = self.dol.word(a)
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:
                regs[rd] = imm << 16
            elif op == 24 and rd == ra and ra in regs:  # ori rA, rS, imm (rS==rA)
                regs[ra] |= imm
        return regs.get(2), regs.get(13)

    def _splits(self, cfg):
        out = []
        cur = None
        for line in (ROOT / "decomp/config" / cfg / "splits.txt").read_text().splitlines():
            if line and not line[0].isspace() and line.endswith(":"):
                cur = line[:-1]
            m = re.match(r"\s+\.text\s+start:0x(\w+) end:0x(\w+)", line)
            if m and cur:
                out.append((int(m[1], 16), int(m[2], 16), cur))
        out.sort()
        return out

    def tu(self, addr):
        i = bisect.bisect_right([s[0] for s in self.splits], addr) - 1
        if i >= 0 and addr < self.splits[i][1]:
            return self.splits[i][2]
        return ""

    def is_ro(self, addr):
        return any(lo <= addr < hi for lo, hi in self.ro)

    def func_at(self, addr):
        i = bisect.bisect_right(self.fstarts, addr) - 1
        if i >= 0 and addr < self.funcs[i]["addr"] + self.funcs[i]["size"]:
            return self.funcs[i]
        return None

    def lookup(self, key):
        if key in self.by_name:
            return self.by_name[key]
        a = int(key, 16)
        return self.fat.get(a) or self.func_at(a)


def walk(v, f):
    """Yield (addr, word, ea) for each instruction; ea is the effective address of a D-form
    operand whose base register holds a known address (lis/@l pairs, r2/r13 small data,
    and registers derived from those by addi/ori), else None."""
    known = {}
    if v.sda2 is not None:
        known[2] = v.sda2
    if v.sda is not None:
        known[13] = v.sda
    for a in range(f["addr"], f["addr"] + f["size"], 4):
        w = v.dol.word(a)
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        simm = (w & 0xFFFF) - ((w & 0x8000) << 1)
        ea = None
        if op == 15 and ra == 0:  # lis
            ea = (w & 0xFFFF) << 16
            known[rd] = ea
            yield a, w, ea if 0x80000000 <= ea < 0x81800000 else None
            continue
        if op in PSQ and ra in known:
            ea = (known[ra] + ((w & 0xFFF) - ((w & 0x800) << 1))) & 0xFFFFFFFF
        elif op in DFORM and op not in LOGICAL and ra in known and op not in (7, 8, 10, 11, 12, 13):
            ea = (known[ra] + simm) & 0xFFFFFFFF
        elif op == 24 and rd in known:  # ori rA, rS, lo
            ea = known[rd] | (w & 0xFFFF)
        if ea is not None and not 0x80000000 <= ea < 0x81800000:
            ea = None
        yield a, w, ea
        # register tracking (straight-line approximation)
        if op == 14 and ea is not None:
            known[rd] = ea
        elif op == 24 and ea is not None:
            known[ra] = ea
        elif op in LOGICAL or op in (20, 21, 23, 30):
            known.pop(ra, None)
        elif op in (7, 8, 12, 13, 14, 15) or op in range(32, 36) or op in range(40, 46):
            if rd not in (2, 13):
                known.pop(rd, None)
        elif op == 31:
            dst = ra if _xo_writes_ra(w) else rd
            if dst not in (2, 13):
                known.pop(dst, None)
        if op in (33, 35, 41, 43, 45, 37, 39, 47, 49, 51, 53, 55):  # update forms write rA
            known.pop(ra, None)


def _xo_writes_ra(w):
    xo = (w >> 1) & 0x3FF
    # logical/shift/sign-extend ops in opcode 31 write rA
    return xo in (24, 26, 28, 60, 124, 284, 316, 412, 444, 476, 536, 792, 824, 922, 954)


def analyze(v, f):
    """Return (loose tokens, strict tokens, call targets, data refs) for one function."""
    loose, strict, calls, refs = [], [], [], []
    for a, w, ea in walk(v, f):
        op = w >> 26
        if ea is not None and op != 15:
            refs.append((len(loose), ea))
        if op == 18:
            t = w & 0xFC000003
            loose.append(t)
            if w & 1:  # bl
                tgt = (w & 0x03FFFFFC) - ((w & 0x02000000) << 1) + (0 if w & 2 else a)
                calls.append((len(strict), tgt & 0xFFFFFFFF))
                strict.append(("bl",))
            else:
                strict.append(t)
            continue
        if op == 16:
            loose.append(w & 0xFFFF0003)
            strict.append(w & 0xFFFF0003)
            continue
        if op in DFORM or op in PSQ:
            mask = 0xFFFFF000 if op in PSQ else 0xFFFF0000
            loose.append(w & mask)
            if ea is None:
                strict.append(w)
            elif op in LOADS and v.is_ro(ea):
                strict.append((w & mask, "val", v.dol.read(ea, LOADS[op])))
            else:
                strict.append((w & mask, "addr"))
            continue
        loose.append(w)
        strict.append(w)
    return loose, strict, calls, refs


def fhash(tokens):
    return hashlib.blake2b(repr(tokens).encode(), digest_size=10).digest()


def _ratio(a, b):
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()


def match_functions(us, pal):
    ua = [analyze(us, f) for f in us.funcs]
    pa = [analyze(pal, f) for f in pal.funcs]
    # The PAL build was made with a different compiler patch level that schedules some
    # instructions differently, so alignment uses the *multiset* of loose tokens.
    uh = [fhash(sorted(x[0])) for x in ua]
    ph = [fhash(sorted(x[0])) for x in pa]
    sm = difflib.SequenceMatcher(None, uh, ph, autojunk=False)
    pairs = {}  # ui -> (pi, kind)
    un_u, un_p = [], []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            for k in range(i2 - i1):
                pairs[i1 + k] = (j1 + k, "same")
            continue
        # pair up inside replace blocks by similarity, preserving order
        used = set()
        j = j1
        for i in range(i1, i2):
            best, bi = 0.0, None
            for jj in range(j, min(j2, j + 8)):
                if uh[i] == ph[jj]:
                    best, bi = 1.0, jj
                    break
                r = _ratio(ua[i][0], pa[jj][0])
                if r > best:
                    best, bi = r, jj
            if bi is not None and best >= 0.6:
                pairs[i] = (bi, "same" if best == 1.0 else "modified")
                used.add(bi)
                j = bi + 1
            else:
                un_u.append(i)
        un_p += [jj for jj in range(j1, j2) if jj not in used]
    # second pass: functions that moved (identical fingerprint elsewhere), then by similarity
    by_hash = {}
    for jj in un_p:
        by_hash.setdefault(ph[jj], []).append(jj)
    rest_u = []
    for i in un_u:
        c = by_hash.get(uh[i])
        if c:
            pairs[i] = (c.pop(0), "same")
        else:
            rest_u.append(i)
    rest_p = [jj for lst in by_hash.values() for jj in lst]
    taken = set()
    for i in rest_u:
        su = us.funcs[i]["size"]
        if su < 0x20:
            continue
        best, bi = 0.0, None
        for jj in rest_p:
            sp = pal.funcs[jj]["size"]
            if jj in taken or not 0.75 * su <= sp <= 1.33 * su:
                continue
            r = _ratio(ua[i][0], pa[jj][0])
            if r > best:
                best, bi = r, jj
        if bi is not None and best >= 0.7:
            pairs[i] = (bi, "modified")
            taken.add(bi)
    # third pass: an unmatched US function and an unmatched PAL function that sit between the
    # same matched neighbours are treated as a rewrite of each other ("modified")
    matched_p = {jj for jj, _ in pairs.values()}
    for i in range(len(us.funcs)):
        if i in pairs or us.funcs[i]["size"] < 0x20:
            continue
        lo = max((k for k in range(i - 1, max(-1, i - 40), -1) if k in pairs), default=None)
        hi = min((k for k in range(i + 1, min(len(us.funcs), i + 40)) if k in pairs), default=None)
        if lo is None or hi is None:
            continue
        cands = [jj for jj in range(pairs[lo][0] + 1, pairs[hi][0]) if jj not in matched_p]
        best, bi = 0.0, None
        for jj in cands:
            r = _ratio(ua[i][0], pa[jj][0])
            if r > best:
                best, bi = r, jj
        if bi is not None and (best >= 0.35 or (len(cands) == 1 and best >= 0.2)):
            pairs[i] = (bi, "modified")
            matched_p.add(bi)
    matched_pal_addr = {pal.funcs[jj]["addr"] for jj in matched_p}
    u2p = {us.funcs[i]["addr"]: pal.funcs[jj]["addr"] for i, (jj, _) in pairs.items()}
    dmap = {}  # US data address -> Counter(PAL data address)
    out = []
    for i in range(len(us.funcs)):
        if i not in pairs:
            out.append(dict(us=us.funcs[i], pal=None, kind="us-only", diffs=[]))
            continue
        j, kind = pairs[i]
        rec = dict(us=us.funcs[i], pal=pal.funcs[j], kind=kind, diffs=[])
        (ul, us_s, uc, ur), (pl, ps_s, pc, pr) = ua[i], pa[j]
        if ul == pl:  # same instruction order: pair up the data addresses they use
            for (k1, e1), (k2, e2) in zip(ur, pr):
                if k1 == k2:
                    dmap.setdefault(e1, Counter())[e2] += 1
        if kind == "same":
            if ul == pl:
                for k, (x, y) in enumerate(zip(us_s, ps_s)):
                    if x != y:
                        rec["diffs"].append((k * 4, x, y))
            else:  # rescheduled: compare as multisets
                cu, cp = Counter(us_s), Counter(ps_s)
                for t in sorted((cu - cp).elements(), key=repr):
                    rec["diffs"].append((us_s.index(t) * 4, t, None))
                for t in sorted((cp - cu).elements(), key=repr):
                    rec["diffs"].append((ps_s.index(t) * 4, None, t))
        # calls compared through the function map (unmapped callees are ignored)
        mu = Counter(u2p[t] for _, t in uc if t in u2p)
        mp = Counter(t for _, t in pc if t in matched_pal_addr)
        for t in (mu - mp).elements():
            rec["diffs"].append((0, ("call", t), None))
        for t in (mp - mu).elements():
            rec["diffs"].append((0, None, ("call", t)))
        if kind == "same" and rec["diffs"]:
            rec["kind"] = "layout" if all(map(_is_field_offset_diff, rec["diffs"])) else "const"
        out.append(rec)
    p2u = {v: k for k, v in u2p.items()}
    for j in range(len(pal.funcs)):
        if j not in matched_p:
            out.append(dict(us=None, pal=pal.funcs[j], kind="pal-only", diffs=[]))
    # order by PAL address; US-only functions go after the PAL copy of their predecessor
    us_sorted = sorted(u2p)
    def pos(r):
        if r["pal"]:
            return (r["pal"]["addr"], 0)
        k = bisect.bisect_left(us_sorted, r["us"]["addr"]) - 1
        return (u2p[us_sorted[k]] if k >= 0 else 0, 1)
    out.sort(key=pos)
    return out, u2p, dmap


def _is_field(t):
    # a load/store/addi through a non-stack register: a struct field offset
    return isinstance(t, int) and ((t >> 26) in range(32, 56) or (t >> 26) == 14) and ((t >> 16) & 31) not in (0, 1)


def _is_field_offset_diff(d):
    """True when a diff is only a struct field offset change (the PAL build added members to
    some classes, shifting later fields), not a behavioural change."""
    _, x, y = d
    return all(t is None or _is_field(t) for t in (x, y))


def fmt_tok(t):
    if t is None:
        return "(none)"
    if isinstance(t, int):
        return f"{t:08X}"
    if t[0] == "call":
        return f"bl {t[1]:08X}"
    if len(t) == 3 and t[1] == "val":
        b = t[2]
        if len(b) == 4:
            return f"{t[0]:08X} [{struct.unpack('>I', b)[0]:#x} / {struct.unpack('>f', b)[0]:g}f]"
        if len(b) == 8:
            return f"{t[0]:08X} [{struct.unpack('>d', b)[0]:g}d]"
        return f"{t[0]:08X} [{b.hex()}]"
    return repr(t)


def describe(diff):
    off, x, y = diff
    if isinstance(x, int) and isinstance(y, int) and (x & 0xFFFF0000) == (y & 0xFFFF0000):
        sx = (x & 0xFFFF) - ((x & 0x8000) << 1)
        sy = (y & 0xFFFF) - ((y & 0x8000) << 1)
        return f"+0x{off:X}: imm {sx} -> {sy}"
    return f"+0x{off:X}: {fmt_tok(x)} -> {fmt_tok(y)}"


def name(f):
    return f["name"] if f else "-"


def cmd_code(a, us, pal):
    recs, _, _ = match_functions(us, pal)
    counts = {}
    for r in recs:
        counts[r["kind"]] = counts.get(r["kind"], 0) + 1
    print(f"# functions US={len(us.funcs)} PAL={len(pal.funcs)}  " +
          "  ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    if a.tsv:
        with open(a.tsv, "w", encoding="utf-8") as fh:
            fh.write("kind\tus_addr\tus_name\tpal_addr\tpal_name\tus_tu\n")
            for r in recs:
                u, p = r["us"], r["pal"]
                fh.write(f"{r['kind']}\t{u['addr']:08X}\t{u['name']}\t" if u else f"{r['kind']}\t-\t-\t")
                fh.write(f"{p['addr']:08X}\t{p['name']}\t" if p else "-\t-\t")
                fh.write((us.tu(u["addr"]) if u else "") + "\n")
    last_us = 0
    for r in recs:
        if r["us"]:
            last_us = r["us"]["addr"]
        if r["kind"] == "same":
            continue
        u, p = r["us"], r["pal"]
        if not a.all and r["kind"] in ("us-only", "pal-only") and (u or p)["size"] <= 8:
            continue
        tu = us.tu(u["addr"] if u else last_us)
        print(f"{r['kind']:9} US {u['addr'] if u else 0:08X} {name(u)[:60]:60} "
              f"PAL {p['addr'] if p else 0:08X} size {u['size'] if u else 0:#x}/{p['size'] if p else 0:#x}  {tu}")
        for d in r["diffs"][:12]:
            print("            " + describe(d))


def disasm(v, f, fnames=None, dnames=None):
    """Disassemble one function. fnames/dnames: optional addr -> label maps for call targets
    and data addresses (used to show PAL code with US names)."""
    try:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_PS)
    except Exception:
        md = None
    fnames = fnames or {}
    dnames = dnames or {}
    lines = []
    for a, w, ea in walk(v, f):
        txt = None
        if md:
            ins = list(md.disasm(struct.pack(">I", w), a))
            if ins:
                txt = f"{ins[0].mnemonic} {ins[0].op_str}"
        if txt is None:
            txt = f".word 0x{w:08x}"
        op = w >> 26
        if op == 18 and w & 1:
            tgt = ((w & 0x03FFFFFC) - ((w & 0x02000000) << 1) + a) & 0xFFFFFFFF
            g = fnames.get(tgt) or (v.fat[tgt]["name"] if tgt in v.fat else None)
            if g:
                txt += f"  ; {g}"
        elif ea is not None and op != 15:
            note = dnames.get(ea) or _dname(v, ea)
            b = v.dol.read(ea, LOADS[op]) if op in LOADS else None
            if b is not None:  # constant, or the initial value of initialized writable data
                eq = " = " if v.is_ro(ea) else " (init) = "
                if op in (48, 49):
                    note += f"{eq}{struct.unpack('>f', b)[0]:g}f"
                elif op in (50, 51):
                    note += f"{eq}{struct.unpack('>d', b)[0]:g}"
                elif len(b) == 4:
                    note += f"{eq}{struct.unpack('>I', b)[0]:#x}"
                else:
                    note += f"{eq}{int.from_bytes(b, 'big'):#x}"
            elif op == 14:
                st = v.dol.read(ea, 48)
                m = re.match(rb"[\x20-\x7e]{3,}\x00", st or b"")
                if m:
                    note += f' "{m.group(0)[:-1].decode()}"'
            txt += f"  ; {note}"
        lines.append(f"{a:08X} {txt}")
    return lines


def _dname(v, ea):
    if not hasattr(v, "_dstarts"):
        v._dobjs = sorted((s for s in v.syms if s["type"] == "object"), key=lambda s: s["addr"])
        v._dstarts = [s["addr"] for s in v._dobjs]
    i = bisect.bisect_right(v._dstarts, ea) - 1
    if i >= 0:
        o = v._dobjs[i]
        if ea < o["addr"] + max(o["size"], 1):
            return o["name"] + (f"+{ea - o['addr']:#x}" if ea != o["addr"] else "")
    return f"{ea:08X}"


def _pal_names(us, pal, u2p, dmap):
    fn = {pa: us.fat[ua]["name"] for ua, pa in u2p.items() if not us.fat[ua]["name"].startswith("fn_")}
    fn.update({pa: f"{us.fat[ua]['name']}(US)" for ua, pa in u2p.items() if us.fat[ua]["name"].startswith("fn_")})
    dn = {}
    for ue, c in dmap.items():
        pe = c.most_common(1)[0][0]
        dn.setdefault(pe, _dname(us, ue) + "(US)")
    return fn, dn


def cmd_show(a, us, pal):
    _, u2p, dmap = match_functions(us, pal)
    p2u = {v: k for k, v in u2p.items()}
    uf = None if a.func == "-" else us.lookup(a.func)
    if a.pal:
        pf = pal.lookup(a.pal)
        if uf is None and pf["addr"] in p2u:
            uf = us.fat[p2u[pf["addr"]]]
    else:
        pf = pal.fat.get(u2p.get(uf["addr"]))
    fn, dn = _pal_names(us, pal, u2p, dmap)
    if uf:
        print(f"US  {uf['name']} @ {uf['addr']:08X} size {uf['size']:#x}  [{us.tu(uf['addr'])}]")
    print(f"PAL {name(pf)} @ {pf['addr'] if pf else 0:08X} size {pf['size'] if pf else 0:#x}")
    ul = disasm(us, uf) if uf else []
    pl = disasm(pal, pf, fn, dn) if pf else []
    if a.side == "pal":
        print("\n".join(pl))
        return
    strip = lambda s: re.sub(r"0x[0-9a-f]{4,}|-?\b\d+\b|; .*", "#", s[9:])
    sm = difflib.SequenceMatcher(None, [strip(x) for x in ul], [strip(x) for x in pl], autojunk=False)
    w = a.width
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        n = max(i2 - i1, j2 - j1)
        for k in range(n):
            l = ul[i1 + k] if i1 + k < i2 else ""
            r = pl[j1 + k] if j1 + k < j2 else ""
            mark = " " if tag == "equal" and strip(l) == strip(r) else "|"
            print(f"{l[:w]:{w}} {mark} {r}")


def cmd_data(a, us, pal):
    # .sdata2 constant pools are merged per translation unit and shift whenever a TU gains a
    # literal, so they align poorly by shape; `code` already compares the literals each
    # instruction loads. Skip them unless asked.
    secs = {".data", ".rodata", ".sdata"} | ({".sdata2"} if a.sdata2 else set())
    us.objs = [o for o in us.objs if o["sec"] in secs]
    pal.objs = [o for o in pal.objs if o["sec"] in secs]

    def sig(v, o):
        b = v.dol.read(o["addr"], o["size"])
        if b is None:
            return None, (o["sec"], o["size"])
        # mask words that look like pointers into MEM1
        ws = bytearray(b)
        for k in range(0, len(ws) - 3, 4):
            if 0x80000000 <= struct.unpack_from(">I", ws, k)[0] < 0x81800000:
                ws[k:k + 4] = b"PTR!"
        return bytes(ws), (o["sec"], o["size"])
    us_s = [sig(us, o) for o in us.objs]
    pal_s = [sig(pal, o) for o in pal.objs]
    sm = difflib.SequenceMatcher(None, [x[1] for x in us_s], [x[1] for x in pal_s], autojunk=False)
    n = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != "equal":
            if a.all:
                print(f"{tag:8} US {[hex(o['addr']) for o in us.objs[i1:i2]][:6]} PAL {[hex(o['addr']) for o in pal.objs[j1:j2]][:6]}")
            continue
        for k in range(i2 - i1):
            uo, po = us.objs[i1 + k], pal.objs[j1 + k]
            ub, pb = us_s[i1 + k][0], pal_s[j1 + k][0]
            if ub is None or pb is None or ub == pb:
                continue
            n += 1
            print(f"{uo['sec']:8} US {uo['addr']:08X} {uo['name'][:40]:40} PAL {po['addr']:08X} size {uo['size']:#x}")
            if uo["size"] <= a.bytes:
                print(f"           US  {us.dol.read(uo['addr'], uo['size']).hex()}")
                print(f"           PAL {pal.dol.read(po['addr'], po['size']).hex()}")
            else:
                diffs = [k2 for k2 in range(0, uo["size"], 4) if ub[k2:k2 + 4] != pb[k2:k2 + 4]]
                print(f"           {len(diffs)} words differ, first at +{diffs[0]:#x}" if diffs else "")
    print(f"# {n} same-shape data objects differ (pointers masked)")


def cmd_files(a, us, pal):
    import rarc
    ur, pr = us.disc / "files", pal.disc / "files"
    uf = {p.relative_to(ur).as_posix() for p in ur.rglob("*") if p.is_file() and "CVS" not in p.parts}
    pf = {p.relative_to(pr).as_posix() for p in pr.rglob("*") if p.is_file() and "CVS" not in p.parts}
    for f in sorted(uf - pf):
        print(f"us-only   {f}")
    for f in sorted(pf - uf):
        print(f"pal-only  {f}")
    for f in sorted(uf & pf):
        bu, bp = (ur / f).read_bytes(), (pr / f).read_bytes()
        if bu == bp:
            continue
        print(f"differs   {f}  ({len(bu)} -> {len(bp)} bytes)")
        if a.deep and (f.endswith(".szp") or f.endswith(".arc")):
            try:
                iu = dict(rarc.rarc_files(rarc.decompress(bu)))
                ip = dict(rarc.rarc_files(rarc.decompress(bp)))
            except Exception as e:  # not an archive
                print(f"            (not RARC: {e})")
                continue
            for k in sorted(set(iu) | set(ip)):
                if k not in ip:
                    print(f"            us-only  {k}")
                elif k not in iu:
                    print(f"            pal-only {k}")
                elif iu[k] != ip[k]:
                    print(f"            differs  {k} ({len(iu[k])} -> {len(ip[k])})")


def parse_jmp(d, names):
    """Parse a JMP parameter table into (field names, list of row dicts)."""
    n, nf, data_off, entry_size = struct.unpack_from(">iiiI", d, 0)
    fields = []
    for i in range(nf):
        h, mask, off, shift, typ = struct.unpack_from(">IIHBB", d, 0x10 + i * 12)
        fields.append((names.get(h, f"#{h:07x}"), mask, off, shift, typ))
    rows = []
    for e in range(n):
        base = data_off + e * entry_size
        row = {}
        for nm, mask, off, shift, typ in fields:
            raw = d[base + off:base + off + 4]
            if typ == 1:
                v = d[base + off:base + off + 32].split(b"\0")[0].decode("shift_jis", "replace")
            elif typ == 2:
                v = round(struct.unpack(">f", raw)[0], 3)
            else:
                v = (struct.unpack(">I", raw)[0] & mask) >> shift
            row[nm] = v
        rows.append(row)
    return [f[0] for f in fields], rows


def jmp_names(dol_bytes):
    import jmp_dump
    return {jmp_dump.name_hash(x): x.decode() for x in jmp_dump.dol_strings(dol_bytes)}


def diff_jmp(da, db, names, label, limit=40, out=print):
    fa, ra = parse_jmp(da, names)
    fb, rb = parse_jmp(db, names)
    if fa != fb:
        out(f"    fields differ: -{sorted(set(fa) - set(fb))} +{sorted(set(fb) - set(fa))}")
    common = [f for f in fa if f in fb]
    key = lambda r: tuple(r.get(f) for f in common)
    sm = difflib.SequenceMatcher(None, [key(r) for r in ra], [key(r) for r in rb], autojunk=False)
    shown = 0
    ops = sm.get_opcodes()
    if all(t == "equal" for t, *_ in ops):
        out(f"    (all {len(ra)} rows have identical values; only the byte layout differs)")
        return
    nchg = sum(max(i2 - i1, j2 - j1) for t, i1, i2, j1, j2 in ops if t != "equal")
    out(f"    {len(ra)} -> {len(rb)} rows, {nchg} changed/added/removed")
    for tag, i1, i2, j1, j2 in ops:
        if tag == "equal":
            continue
        n = max(i2 - i1, j2 - j1)
        for k in range(n):
            x = ra[i1 + k] if i1 + k < i2 else None
            y = rb[j1 + k] if j1 + k < j2 else None
            if shown >= limit:
                out(f"    ... (more; {len(ra)} -> {len(rb)} rows)")
                return
            shown += 1
            tagname = x.get("name") if x else y.get("name")
            if x and y:
                ch = {f: (x[f], y[f]) for f in common if x[f] != y[f]}
                out(f"    row {i1 + k}->{j1 + k} {tagname!s:14} " + ", ".join(f"{f}: {u}->{v}" for f, (u, v) in ch.items()))
            elif x:
                out(f"    row {i1 + k} removed: " + ", ".join(f"{f}={x[f]}" for f in common[:10]))
            else:
                out(f"    row {j1 + k} added:   " + ", ".join(f"{f}={y[f]}" for f in common[:10]))


def cmd_arcdiff(a, us, pal):
    import rarc
    fa, fb = Path(a.a), Path(a.b)
    for p_ in (fa, fb):
        if not p_.exists():
            raise SystemExit(f"missing {p_}")
    ia = dict(rarc.rarc_files(rarc.decompress(fa.read_bytes())))
    ib = dict(rarc.rarc_files(rarc.decompress(fb.read_bytes())))
    names = jmp_names(us.dol.data)
    names.update(jmp_names(pal.dol.data))
    for k in sorted(set(ia) | set(ib)):
        if a.path and not re.search(a.path, k):
            continue
        if k not in ib:
            print(f"only in A  {k}")
        elif k not in ia:
            print(f"only in B  {k}")
        elif ia[k] != ib[k]:
            print(f"differs    {k} ({len(ia[k])} -> {len(ib[k])})")
            if k.startswith("jmp/") and a.rows:
                diff_jmp(ia[k], ib[k], names, k, a.rows)


def cmd_strings(a, us, pal):
    rx = re.compile(a.regex.encode())
    for v in (us, pal):
        for addr, off, size, idx in v.dol.secs:
            seg = v.dol.data[off:off + size]
            for m in re.finditer(rb"[\x20-\x7e]{2,80}(?=\x00)", seg):
                if rx.search(m.group(0)):
                    print(f"{v.tag} {addr + m.start():08X} {m.group(0).decode()}")


def cmd_xref(a, us, pal):
    v = pal if a.version == "pal" else us
    target = int(a.addr, 16)
    obj = next((o for o in v.syms if o["type"] == "object" and o["addr"] <= target < o["addr"] + max(o["size"], 1)), None)
    lo, hi = (obj["addr"], obj["addr"] + obj["size"]) if obj and not a.exact else (target, target + 1)
    if obj:
        print(f"# {target:08X} is in {obj['name']} {obj['sec']} [{obj['addr']:08X}, +{obj['size']:#x})")
    for f in v.funcs:
        for ad, w, ea in walk(v, f):
            if w >> 26 == 18 and w & 1:
                tgt = ((w & 0x03FFFFFC) - ((w & 0x02000000) << 1) + ad) & 0xFFFFFFFF
                if tgt == target:
                    print(f"{ad:08X} {f['name']} (call)")
            elif ea is not None and lo <= ea < hi and (w >> 26 != 15):
                print(f"{ad:08X} {f['name']}  ea={ea:08X}")


def _xrefs(v, lo, hi):
    out = []
    for f in v.funcs:
        for ad, w, ea in walk(v, f):
            if ea is not None and lo <= ea < hi and (w >> 26 != 15):
                out.append((ad, f, ea, w))
    return out


def _insn(w):
    try:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_PS)
        i = next(md.disasm(struct.pack(">I", w), 0), None)
        if i:
            return f"{i.mnemonic} {i.op_str}"
    except ImportError:
        pass
    return f".word {w:08x}"


def cmd_field(a, us, pal):
    """Loads/stores/addi with a given displacement through a non-stack, non-small-data base
    register (struct field uses), optionally only in functions whose name matches --in."""
    v = pal if a.version == "pal" else us
    off = int(a.offset, 0)
    names = {}
    if v is pal:
        _, u2p, _ = match_functions(us, pal)
        names = {pa: us.fat[ua]["name"] + "(US)" for ua, pa in u2p.items()}
    for f in v.funcs:
        for ad, w, ea in walk(v, f):
            op, ra = w >> 26, (w >> 16) & 31
            if (op in range(32, 56) or op == 14) and ra not in (0, 1, 2, 13) and (w & 0xFFFF) == (off & 0xFFFF):
                print(f"{ad:08X} {_insn(w):28} {names.get(f['addr'], f['name'])}")


def cmd_var(a, us, pal):
    """Map a data address between versions (via matched functions) and list who uses it."""
    recs, u2p, dmap = match_functions(us, pal)
    p2u = {v: k for k, v in u2p.items()}
    t = int(a.addr, 16)
    size = int(a.size, 0)
    if a.version == "pal":
        pal_addr = t
        cands = Counter()
        for ue, c in dmap.items():
            for pe, n in c.items():
                if pe == t:
                    cands[ue] += n
        us_addr = cands.most_common(1)[0][0] if cands else None
    else:
        us_addr = t
        c = dmap.get(t)
        pal_addr = c.most_common(1)[0][0] if c else None
    print(f"US  {us_addr:08X}" if us_addr is not None else "US  (no mapping found)",
          f"   PAL {pal_addr:08X}" if pal_addr is not None else "   PAL (no mapping found)")
    for v, addr, nm in ((us, us_addr, lambda f: f["name"]),
                        (pal, pal_addr, lambda f: (us.fat[p2u[f["addr"]]]["name"] + " [US " + f"{p2u[f['addr']]:08X}]")
                         if f["addr"] in p2u else f["name"] + " [PAL-only]")):
        if addr is None:
            continue
        print(f"--- {v.tag} references to {addr:08X}..+{size:#x}")
        for ad, f, ea, w in _xrefs(v, addr, addr + size):
            print(f"  {ad:08X} {_insn(w):28} {nm(f)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--us", default=str(DEF_US))
    ap.add_argument("--pal", dest="pal_disc", default=None)
    ap.add_argument("--jp", action="store_true",
                    help="compare US with the Japanese GLMJ01 instead of PAL (the 'pal' side is then the "
                         "Japanese game; needs decomp/config/GLMJ01 from branch japan-support)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("code")
    c.add_argument("--tsv")
    c.add_argument("--all", action="store_true", help="also list tiny unmatched functions")
    s = sub.add_parser("show")
    s.add_argument("func")
    s.add_argument("--pal")
    s.add_argument("--side", choices=["both", "pal"], default="both")
    s.add_argument("--width", type=int, default=70)
    d = sub.add_parser("data")
    d.add_argument("--bytes", type=int, default=32)
    d.add_argument("--all", action="store_true", help="also list unaligned runs")
    d.add_argument("--sdata2", action="store_true", help="include .sdata2 constant pools")
    f = sub.add_parser("files")
    f.add_argument("--deep", action="store_true", help="also diff inside .szp/.arc archives")
    ad = sub.add_parser("arcdiff", help="diff two RARC archives (.szp/.arc); JMP tables row by row")
    ad.add_argument("a")
    ad.add_argument("b")
    ad.add_argument("--path", help="regex filter on member paths")
    ad.add_argument("--rows", type=int, default=40, help="max changed JMP rows to print per table (0: none)")
    st = sub.add_parser("strings")
    st.add_argument("regex")
    x = sub.add_parser("xref")
    x.add_argument("version", choices=["us", "pal"])
    x.add_argument("addr")
    x.add_argument("--exact", action="store_true", help="only the exact address, not its whole object")
    fl = sub.add_parser("field", help="struct-field uses of a displacement")
    fl.add_argument("version", choices=["us", "pal"])
    fl.add_argument("offset")
    va = sub.add_parser("var")
    va.add_argument("version", choices=["us", "pal"])
    va.add_argument("addr")
    va.add_argument("--size", default="4")
    a = ap.parse_args()
    us = Version("US ", a.us, "GLME01")
    if a.jp:
        pal = Version("JP ", a.pal_disc or str(DEF_JP), "GLMJ01")
    else:
        pal = Version("PAL", a.pal_disc or str(DEF_PAL), "GLMP01_00")
    {"code": cmd_code, "show": cmd_show, "data": cmd_data, "files": cmd_files,
     "strings": cmd_strings, "xref": cmd_xref, "var": cmd_var, "field": cmd_field, "arcdiff": cmd_arcdiff}[a.cmd](a, us, pal)


if __name__ == "__main__":
    main()
