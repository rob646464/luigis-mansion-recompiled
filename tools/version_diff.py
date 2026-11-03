#!/usr/bin/env python3
"""Compare the data of the USA (GLME01), Japanese (GLMJ01) and European (GLMP01) releases of
Luigi's Mansion: disc files, JMP tables row by row, event scripts, music sequences, main.dol functions. Companion to
tools/pal_diff.py (code: `pal_diff.py [--jp] code|show|var|xref`); findings in
docs/VERSION_DIFFERENCES.md.

Works only on the user's own dumps (sys/ + files/ folders next to the repo); prints analysis,
never writes game data into the repository.

    python tools/version_diff.py files                    # three-way table of disc files
    python tools/version_diff.py arc <path in files/> A B  # archive members; JMP tables by row name
    python tools/version_diff.py events A B [--diff]      # event scripts (text/eventNN.txt) that differ
    python tools/version_diff.py event <NN> A B           # unified diff of one event script
    python tools/version_diff.py msg <NN> A B             # message table of one event (A, B side by side)
    python tools/version_diff.py seqs A B                 # music sequences (JaiInit.aaf / JaiArcS.arc)
    python tools/version_diff.py fnsum jp|pal [--range LO-HI]  # changed main.dol functions vs US, summarized

`--jp` / `fnsum jp` need the Japanese function table decomp/config/GLMJ01 (branch japan-support).

A and B are us, jp or pal. JMP rows are compared as a multiset (order ignored); a removed and
an added row with the same name or most fields equal are shown as one change, so inserted rows
do not show as long runs of changes. Tables without a name field and equal row counts (polygon
tables) are compared by index.
"""
import argparse
import difflib
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import pal_diff as P  # noqa: E402
import rarc  # noqa: E402

DISCS = {"us": P.DEF_US, "jp": P.DEF_JP, "pal": P.DEF_PAL}
ENC = {"us": "latin-1", "pal": "latin-1", "jp": "shift_jis"}


def files_of(v):
    r = DISCS[v] / "files"
    return {p.relative_to(r).as_posix(): p for p in r.rglob("*") if p.is_file() and "CVS" not in p.parts}


def load_arc(path):
    return dict(rarc.rarc_files(rarc.decompress(Path(path).read_bytes())))


_names = None


def names():
    global _names
    if _names is None:
        _names = {}
        for v in DISCS.values():
            dol = v / "sys" / "main.dol"
            if dol.exists():
                _names.update(P.jmp_names(dol.read_bytes()))
    return _names


def cmd_files(a):
    fs = {v: files_of(v) for v in DISCS if (DISCS[v] / "files").exists()}
    allf = sorted(set().union(*[set(f) for f in fs.values()]))
    vs = list(fs)
    print("# " + " ".join(f"{v:>4}" for v in vs) + "  file   (letters: same content -> same letter, '-' missing)")
    for f in allf:
        ids, cls = [], {}
        for v in vs:
            p = fs[v].get(f)
            if p is None:
                ids.append("-")
                continue
            b = p.read_bytes()
            if b not in cls:
                cls[b] = "ABC"[len(cls)]
            ids.append(cls[b])
        if a.all or len(set(ids)) > 1:
            print("  " + " ".join(f"{i:>4}" for i in ids) + f"  {f}")


def diff_table(da, db, label, limit):
    fa, ra = P.parse_jmp(da, names())
    fb, rb = P.parse_jmp(db, names())
    if fa != fb:
        print(f"    fields: -{sorted(set(fa) - set(fb))} +{sorted(set(fb) - set(fa))}")
    common = [f for f in fa if f in fb]
    # Multiset difference of whole rows (order ignored), then pair each removed row with the
    # added row sharing most field values (at least half): reported as a change.
    if "name" not in common and len(ra) == len(rb):  # rows tied to their index (polygons)
        ch = [(i, x, y) for i, (x, y) in enumerate(zip(ra, rb)) if x != y]
        print(f"    {len(ra)} rows by index, {len(ch)} differ")
        summ = Counter(tuple((f, x[f], y[f]) for f in common if x[f] != y[f]) for i, x, y in ch)
        for k, n in summ.most_common(limit):
            print(f"    {n:6} x " + ", ".join(f"{f}: {u}->{v}" for f, u, v in k))
        return
    if "name" in common and "dmd_name" not in common and len({r["name"] for r in ra}) == 1:  # roominfo: tag rows by index
        for i, r in enumerate(ra):
            r["name"] = f"{r['name']}[{i}]"
        for i, r in enumerate(rb):
            r["name"] = f"{r['name']}[{i}]"
    tup = lambda r: tuple(r.get(f) for f in common)
    ca, cb = Counter(tup(r) for r in ra), Counter(tup(r) for r in rb)
    gone = list((ca - cb).elements())
    new = list((cb - ca).elements())
    lines = []
    lab = lambda t: " ".join(str(t[common.index(f)]) for f in ("name", "dmd_name", "room_no", "other", "room")
                             if f in common and t[common.index(f)] not in (None, "(null)"))
    ni = common.index("name") if "name" in common else None
    for g in gone:
        best, score = None, len(common) // 2 - 1
        for i, n in enumerate(new):
            s = sum(1 for x, y in zip(g, n) if x == y)
            if ni is not None and g[ni] == n[ni] and len({r["name"] for r in ra}) > 1:
                s += len(common)  # same unique-ish name: prefer it over field similarity
            if s > score:
                best, score = i, s
        if best is None:
            lines.append(f"    - [{lab(g)}] " + ", ".join(f"{f}={v}" for f, v in zip(common, g)))
        else:
            n = new.pop(best)
            lines.append(f"    ~ [{lab(g)}] " + ", ".join(f"{f}: {x}->{y}" for f, x, y in zip(common, g, n) if x != y))
    for n in new:
        lines.append(f"    + [{lab(n)}] " + ", ".join(f"{f}={v}" for f, v in zip(common, n)))
    if not lines:
        print(f"    ({len(ra)} rows, the same rows; only their order or byte layout differs)")
        return
    print(f"    {len(ra)} -> {len(rb)} rows, {len(lines)} differences")
    for ln in lines[:limit]:
        print(ln)
    if len(lines) > limit:
        print(f"    ... {len(lines) - limit} more")


def cmd_arc(a):
    ia = load_arc(DISCS[a.a] / "files" / a.path)
    ib = load_arc(DISCS[a.b] / "files" / a.path)
    for k in sorted(set(ia) | set(ib)):
        if k not in ib:
            print(f"only {a.a:3} {k}")
        elif k not in ia:
            print(f"only {a.b:3} {k}")
        elif ia[k] != ib[k]:
            print(f"differs  {k} ({len(ia[k])} -> {len(ib[k])})")
            if k.startswith("jmp/"):
                diff_table(ia[k], ib[k], k, a.rows)


def script(v, nn):
    p = DISCS[v] / "files" / "Event" / f"event{nn}.szp"
    if not p.exists():
        return None
    arc = load_arc(p)
    t = arc.get(f"text/event{nn}.txt")
    return t.decode("cp932", "replace") if t is not None else None  # scripts: Japanese comments in every release


def messages(v, nn):
    arc = load_arc(DISCS[v] / "files" / "Event" / f"event{nn}.szp")
    for k, d in arc.items():
        if k.startswith("message/") and (v != "pal" or "/english/" in k):
            return d.decode(ENC[v], "replace")
    return ""


def norm(s):
    return [ln.rstrip() for ln in s.replace("\r", "").split("\n")]


def cmd_events(a):
    nums = sorted({p.stem[5:] for v in (a.a, a.b) for p in (DISCS[v] / "files" / "Event").glob("event*.szp")})
    for nn in nums:
        sa, sb = script(a.a, nn), script(a.b, nn)
        if sa is None and sb is None:
            continue
        if sa is None or sb is None:
            print(f"event{nn}: only in {a.a if sb is None else a.b}")
            continue
        if norm(sa) == norm(sb):
            continue
        d = [ln for ln in difflib.unified_diff(norm(sa), norm(sb), lineterm="", n=0) if ln[:1] in "+-" and ln[:3] not in ("+++", "---")]
        print(f"event{nn}: {len(d)} changed lines")
        if a.diff:
            for ln in d:
                print("    " + ln)


def cmd_event(a):
    sa, sb = script(a.a, a.nn), script(a.b, a.nn)
    for ln in difflib.unified_diff(norm(sa), norm(sb), f"{a.a}/event{a.nn}", f"{a.b}/event{a.nn}", lineterm="", n=a.context):
        print(ln)


def cmd_msg(a):
    ma, mb = norm(messages(a.a, a.nn)), norm(messages(a.b, a.nn))
    for i in range(max(len(ma), len(mb))):
        x = ma[i] if i < len(ma) else ""
        y = mb[i] if i < len(mb) else ""
        print(f"{i:3} {x[:70]:70} | {y[:70]}")


def sequences(v):
    """BARC list of /AudioRes/JaiInit.aaf: [(index, name, bytes)] from JaiArcS.arc."""
    import struct
    r = DISCS[v] / "files" / "AudioRes"
    aaf, arc = (r / "JaiInit.aaf").read_bytes(), (r / "Seqs" / "JaiArcS.arc").read_bytes()
    i = aaf.find(b"BARC----")
    n = struct.unpack_from(">I", aaf, i + 0xC)[0]
    out = []
    for k in range(n):
        e = i + 0x20 + 0x20 * k
        name = aaf[e:e + 14].split(bytes(1))[0].decode("latin-1")
        off, size = struct.unpack_from(">II", aaf, e + 0x18)
        out.append((k, name, arc[off:off + size]))
    return out


def cmd_seqs(a):
    sa, sb = sequences(a.a), sequences(a.b)
    print(f"# {len(sa)} sequences in {a.a}, {len(sb)} in {a.b}")
    nb = {}
    for k, name, d in sb:
        nb.setdefault(name, (k, d))
    seen = set()
    done = set()
    for k, name, d in sa:
        if name in done:
            continue
        done.add(name)
        if name not in nb:
            print(f"  {k:3} {name:16} only in {a.a} ({len(d)} bytes)")
            continue
        seen.add(name)
        kb, db = nb[name]
        if d != db or k != kb:
            idx = f"index {k} -> {kb}" if k != kb else f"index {k}"
            print(f"  {k:3} {name:16} {idx}: " + ("same data" if d == db else f"{len(d)} -> {len(db)} bytes"))
    for k, name, d in sb:
        if name not in seen and name not in {x[1] for x in sa}:
            print(f"  {k:3} {name:16} only in {a.b} ({len(d)} bytes)")


def _features(v, f, cname):
    """Call targets (by name), li / cmpwi immediates and read-only constants of one function."""
    import struct
    out = Counter()
    _, strict, calls, _ = P.analyze(v, f)
    for _, t in calls:
        out[("call", cname(t))] += 1
    for t in strict:
        if isinstance(t, int):
            op, ra, imm = t >> 26, (t >> 16) & 31, t & 0xFFFF
            simm = imm - ((imm & 0x8000) << 1)
            if op == 14 and ra == 0:
                out[("li", simm)] += 1
            elif op == 11:
                out[("cmpwi", simm)] += 1
            elif op == 10:
                out[("cmplwi", imm)] += 1
        elif isinstance(t, tuple) and len(t) == 3 and t[1] == "val":
            b = t[2]
            out[("const", round(struct.unpack(">f", b)[0], 4) if len(b) == 4 else
                 round(struct.unpack(">d", b)[0], 4) if len(b) == 8 else b.hex())] += 1
    return out


def cmd_fnsum(a):
    """Semantic summary of every changed function between US and another release."""
    us = P.Version("US ", P.DEF_US, "GLME01")
    other = P.Version(a.other.upper(), DISCS[a.other], "GLMJ01" if a.other == "jp" else "GLMP01_00")
    recs, u2p, _ = P.match_functions(us, other)
    p2u = {v: k for k, v in u2p.items()}
    uname = lambda t: us.fat[t]["name"] if t in us.fat else f"{t:08X}"
    oname = lambda t: (uname(p2u[t]) if t in p2u else f"{a.other}:{t:08X}")
    for r in recs:
        if r["kind"] in ("same", "layout"):
            continue
        u, o = r["us"], r["pal"]
        if a.range:
            lo, hi = (int(x, 16) for x in a.range.split("-"))
            if not (u and lo <= u["addr"] < hi):
                continue
        hdr = f"{r['kind']:8} US {u['addr'] if u else 0:08X} {(u or {}).get('name', '-')[:40]:40} {a.other.upper()} {o['addr'] if o else 0:08X} size {u['size'] if u else 0:#x}/{o['size'] if o else 0:#x}"
        if not u or not o:
            print(hdr)
            continue
        fu, fo = _features(us, u, uname), _features(other, o, oname)
        minus, plus = fu - fo, fo - fu
        print(hdr)
        if minus:
            print("      US only: " + ", ".join(f"{k[0]} {k[1]}" + (f" x{n}" if n > 1 else "") for k, n in sorted(minus.items(), key=repr)))
        if plus:
            print(f"      {a.other.upper()} only: " + ", ".join(f"{k[0]} {k[1]}" + (f" x{n}" if n > 1 else "") for k, n in sorted(plus.items(), key=repr)))


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("files")
    f.add_argument("--all", action="store_true")
    r = sub.add_parser("arc")
    r.add_argument("path")
    r.add_argument("a", choices=DISCS)
    r.add_argument("b", choices=DISCS)
    r.add_argument("--rows", type=int, default=60)
    e = sub.add_parser("events")
    e.add_argument("a", choices=DISCS)
    e.add_argument("b", choices=DISCS)
    e.add_argument("--diff", action="store_true")
    e1 = sub.add_parser("event")
    e1.add_argument("nn")
    e1.add_argument("a", choices=DISCS)
    e1.add_argument("b", choices=DISCS)
    e1.add_argument("--context", type=int, default=2)
    m = sub.add_parser("msg")
    m.add_argument("nn")
    m.add_argument("a", choices=DISCS)
    m.add_argument("b", choices=DISCS)
    sq = sub.add_parser("seqs", help="music/sound sequences (JaiInit.aaf BARC list) that differ")
    sq.add_argument("a", choices=DISCS)
    sq.add_argument("b", choices=DISCS)
    fs = sub.add_parser("fnsum", help="changed functions: calls, li/cmpwi immediates, constants that differ")
    fs.add_argument("other", choices=["jp", "pal"])
    fs.add_argument("--range", help="US address range, e.g. 80040000-80050000")
    a = ap.parse_args()
    {"seqs": cmd_seqs, "fnsum": cmd_fnsum, "files": cmd_files, "arc": cmd_arc, "events": cmd_events, "event": cmd_event, "msg": cmd_msg}[a.cmd](a)


if __name__ == "__main__":
    main()
