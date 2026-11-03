#!/usr/bin/env python3
"""Build the Japanese (GLMJ01) function table, decomp/config/GLMJ01/symbols.txt (docs/SETUP.md,
"The Japanese game").

    python tools/recomp/gen_jp_symbols.py --dtk <path to dtk.exe> [--jp DIR] [--work DIR]

The decomp has tables for the USA and European releases only. The Japanese one comes from
decomp-toolkit (encounter/decomp-toolkit, MIT; a dev-only tool, not in the repo: download a
release binary), the tool the decomp's own tables were started from: `dtk dol split` analyses
the Japanese main.dol (control flow from the entry point, every call target, jump tables,
.ctors, its built-in SDK/runtime signatures) and writes functions and data objects.

On the USA DOL, dtk's analysis gives the same 10984 function starts and sizes as the curated
decomp table except four assembly/padding cases, where dtk ends a function early and makes the
rest a separate tiny function (Run, PSVECSquareMag, List_AddChannelTail,
countUniqueMaterials). Those are fixed here the same way for the Japanese table: a Japanese
function matched to a USA function that is longer by exactly the next Japanese function, whose
bytes equal the USA function's tail, absorbs it.

Then run tools/recomp/gen_version_map.py --target jp (names, SDK units, hooks, VA()).
Reads only the user's own dump; writes address/size/name tables only.
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import pal_diff as P  # noqa: E402

FUNC_RE = re.compile(r"(\S+) = (\.?\w+):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)(.*)")


def run_dtk(dtk, jp, work):
    dol = Path(jp) / "sys" / "main.dol"
    work.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(dol, work / "main.dol")
    import hashlib
    sha = hashlib.sha1(dol.read_bytes()).hexdigest()
    (work / "config").mkdir(exist_ok=True)
    for f in ("symbols.txt", "splits.txt"):
        (work / "config" / f).unlink(missing_ok=True)
    (work / "config.yml").write_text(
        f"object: main.dol\nhash: {sha}\nsymbols: config/symbols.txt\nsplits: config/splits.txt\n",
        encoding="utf-8", newline="\n")
    subprocess.run([str(dtk), "dol", "split", "config.yml", "out"], cwd=work, check=True)
    return work / "config" / "symbols.txt", work / "config" / "splits.txt"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dtk", required=True)
    ap.add_argument("--jp", default=str(P.DEF_JP))
    ap.add_argument("--us", default=str(P.DEF_US))
    ap.add_argument("--work", help="scratch folder for dtk (default: a temporary folder)")
    a = ap.parse_args()
    work = Path(a.work) if a.work else Path(tempfile.mkdtemp(prefix="lm_jp_dtk_"))
    sym, spl = run_dtk(a.dtk, a.jp, work)
    out = ROOT / "decomp" / "config" / "GLMJ01"
    out.mkdir(parents=True, exist_ok=True)
    (out / "symbols.txt").write_text(sym.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
    (out / "splits.txt").write_text(spl.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")

    # Merge dtk's split-off tails (see above), using the USA table as the reference.
    us = P.Version("us", a.us, "GLME01")
    jp = P.Version("jp", a.jp, "GLMJ01")
    recs, _, _ = P.match_functions(us, jp)
    absorb = {}  # JP function addr -> new size; removed tails
    removed = set()
    for r in recs:
        u, p = r["us"], r["pal"]
        if not u or not p or u["size"] <= p["size"]:
            continue
        k = u["size"] - p["size"]
        tail = jp.fat.get(p["addr"] + p["size"])
        if not tail or tail["size"] != k or k > 0x20 or (u["addr"] + p["size"]) in us.fat:
            continue
        if us.dol.read(u["addr"] + p["size"], k) != jp.dol.read(tail["addr"], k):
            continue
        # the rest must be the same code too (not a modified function that ends in a blr
        # followed by an unrelated empty function)
        if P.analyze(us, dict(u, size=p["size"]))[0] != P.analyze(jp, p)[0]:
            continue
        absorb[p["addr"]] = u["size"]
        removed.add(tail["addr"])
        print(f"merged {tail['name']} into {p['name']} ({u['name']}, size {u['size']:#x})")
    lines = []
    for ln in (out / "symbols.txt").read_text(encoding="utf-8").splitlines():
        m = FUNC_RE.match(ln)
        if m and int(m[3], 16) in removed:
            continue
        if m and int(m[3], 16) in absorb:
            ln = f"{m[1]} = {m[2]}:0x{m[3]}; // type:function size:0x{absorb[int(m[3], 16)]:X}{m[5]}"
        lines.append(ln)
    (out / "symbols.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{out / 'symbols.txt'}: {sum(1 for l in lines if FUNC_RE.match(l))} functions")


if __name__ == "__main__":
    main()
