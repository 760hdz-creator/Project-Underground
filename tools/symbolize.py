"""Map '+RVA' host addresses in a runtime log to symbols from out/gauntlet.map.

Usage: python -I tools/symbolize.py < log   (or pass a log file path)
"""
import bisect
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
syms = []
for line in open(os.path.join(ROOT, "out", "gauntlet.map"), encoding="latin-1"):
    m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})", line)
    if m:
        syms.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
syms.sort()
keys = [a for a, _ in syms]


def name(rva):
    i = bisect.bisect_right(keys, rva) - 1
    return f"{syms[i][1]}+{rva - syms[i][0]:X}" if i >= 0 else f"+{rva:X}"


src = open(sys.argv[1]) if len(sys.argv) > 1 else sys.stdin
for line in src:
    print(re.sub(r"\+([0-9A-F]{3,})\b", lambda m: name(int(m.group(1), 16)), line), end="")
