"""Find XDK library functions in an XBE by byte signatures (dev tool, reads data only).

Reads the OOVPA signature tables of Cxbx-Reloaded's XbSymbolDatabase (MIT) as text and matches them
against the XBE's sections. No code from that project is built or run.

Usage: python -I tools/oovpa_scan.py <XbSymbolDatabase/src/OOVPADatabase> <default.xbe> [--xdk 5659] [--lib DSound,...]
Prints: va  symbol  version  section  [AMBIGUOUS n]
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbedis import Xbe  # noqa: E402

# library -> XBE sections to search
LIB_SECTIONS = {
    "DSound": ["DSOUND"],
    "Xapi": [".text", "XPP"],
    "XNet": ["XNET"],
    "D3D8": ["D3D"],
    "XGraphic": ["XGRPH"],
}

HEADER = re.compile(r"OOVPA_SIG_HEADER_(?:NO_XREF|XREF)(?:_DETECT|_EXTEND)?\(\s*(\w+)\s*,\s*(\d+)")
NUM = r"(0x[0-9A-Fa-f]+|\d+)"


def parse_inl(path):
    """-> list of (name, version, [(offset, byte or None for xref)])"""
    text = open(path, encoding="utf-8", errors="replace").read()
    text = re.sub(r"//[^\n]*", "", text)
    out = []
    for m in HEADER.finditer(text):
        name, ver = m.group(1), int(m.group(2))
        start = text.find("OOVPA_SIG_MATCH(", m.end())
        if start < 0:
            continue
        depth, i = 0, start + len("OOVPA_SIG_MATCH")
        while i < len(text):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = text[start:i]
        pairs = []
        xrefs = []
        for e in re.finditer(r"XREF_ENTRY\(\s*" + NUM + r"\s*,\s*XREF_(\w+)\s*\)", body):
            off = int(e.group(1), 0)
            xrefs.append((off, e.group(2)))
        for e in re.finditer(r"OV_MATCH\(([^)]*)\)", body):
            vals = [int(v, 0) for v in re.findall(NUM, e.group(1))]
            pairs += [(vals[0] + k, b) for k, b in enumerate(vals[1:])]
        for e in re.finditer(r"\{\s*" + NUM + r"\s*,\s*" + NUM + r"\s*\}", body):
            pairs.append((int(e.group(1), 0), int(e.group(2), 0)))
        if pairs:
            out.append((name, ver, pairs, xrefs))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("db")
    ap.add_argument("xbe")
    ap.add_argument("--xdk", type=int, default=5659)
    ap.add_argument("--lib", default=",".join(LIB_SECTIONS))
    a = ap.parse_args()
    xbe = Xbe(a.xbe)
    secs = {n: (va, xbe.read(va, rs)) for n, va, vs, raw, rs in xbe.sections}

    def u32(va):
        b = xbe.read(va, 4)
        return int.from_bytes(b, "little") if len(b) == 4 else None

    # symbol -> (version, lib, xrefs, [(va, section)])  candidates from fixed bytes only
    cand = {}
    for lib in a.lib.split(","):
        d = os.path.join(a.db, lib)
        sigs = {}
        for f in sorted(os.listdir(d)):
            if f.endswith(".inl"):
                for name, ver, pairs, xrefs in parse_inl(os.path.join(d, f)):
                    if ver <= a.xdk:
                        sigs.setdefault(name, []).append((ver, pairs, xrefs))
        for name in sorted(sigs):
            for ver, pairs, xrefs in sorted(sigs[name], key=lambda v: -v[0]):
                fixed = [(o, b) for o, b in pairs if b is not None]
                if len(fixed) < 5:
                    continue
                hits = []
                for sn in LIB_SECTIONS.get(lib, []):
                    if sn not in secs:
                        continue
                    base, data = secs[sn]
                    o0, b0 = fixed[0]
                    n = len(data)
                    pos = data.find(bytes([b0]), o0)
                    while pos >= 0:
                        st = pos - o0
                        if all(0 <= st + o < n and data[st + o] == b for o, b in fixed):
                            hits.append((base + st, sn))
                        pos = data.find(bytes([b0]), pos + 1)
                if hits:
                    cand[name] = (ver, lib, xrefs, hits)
                    break

    # resolve XREFs: a call at +off must land on the referenced symbol once that is unique
    def target(va, off):
        r = u32(va + off)
        return None if r is None else (va + off + 4 + r) & 0xFFFFFFFF

    changed = True
    while changed:
        changed = False
        for name, (ver, lib, xrefs, hits) in cand.items():
            if len(hits) <= 1:
                continue
            keep = []
            for va, sn in hits:
                ok = True
                for off, ref in xrefs:
                    rc = cand.get(ref)
                    if rc and len(rc[3]) == 1 and target(va, off) != rc[3][0][0]:
                        ok = False
                    elif rc and len(rc[3]) > 1 and target(va, off) not in {h[0] for h in rc[3]}:
                        ok = False
                if ok:
                    keep.append((va, sn))
            if keep and len(keep) < len(hits):
                cand[name] = (ver, lib, xrefs, keep)
                changed = True
    # a unique address claimed by several names: report all, marked
    owners = {}
    for name, (ver, lib, xrefs, hits) in cand.items():
        if len(hits) == 1:
            owners.setdefault(hits[0][0], []).append(name)
    for name, (ver, lib, xrefs, hits) in sorted(cand.items(), key=lambda kv: kv[1][3][0][0]):
        tag = "" if len(hits) == 1 else f"	AMBIGUOUS {len(hits)}"
        if len(hits) == 1 and len(owners[hits[0][0]]) > 1:
            tag = "	SHARED " + ",".join(n for n in owners[hits[0][0]] if n != name)
        for va, sn in hits[:4]:
            print(f"{va:08X}	{name}	{ver}	{lib}:{sn}{tag}")


if __name__ == "__main__":
    main()
