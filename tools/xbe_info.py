"""Dump an Xbox XBE header: sections, entry, kernel imports, XDK library versions.

Usage: python -I tools/xbe_info.py <default.xbe>
"""
import struct
import sys

ENTRY_KEYS = {"retail": 0xA8FC57AB, "debug": 0x94859D4B}
THUNK_KEYS = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152}


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"XBEH":
        raise SystemExit("not an XBE")
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]
    base = u32(0x104)

    def at(va):
        return va - base

    def cstr(va):
        o = at(va)
        return data[o : data.index(b"\0", o)].decode("latin-1")

    print(f"base        0x{base:08X}")
    print(f"image size  0x{u32(0x10C):08X}")
    print(f"timestamp   0x{u32(0x114):08X}")
    print(f"init flags  0x{u32(0x124):08X}")
    print(f"stack       0x{u32(0x130):08X}")
    print(f"TLS addr    0x{u32(0x12C):08X}")
    print(f"debug path  {cstr(u32(0x14C))}")
    print(f"debug file  {cstr(u32(0x150))}")

    num_sec, sec_va = u32(0x11C), u32(0x120)
    sections = []
    for i in range(num_sec):
        o = at(sec_va) + i * 56
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<6I", data, o)
        sections.append((cstr(name_va), va, vsize, raw, rsize, flags))

    def read_va(va, n):
        for name, sva, vsize, raw, rsize, _ in sections:
            if sva <= va < sva + rsize:
                return data[raw + va - sva : raw + va - sva + n]
        return None

    # Pick the key set that puts entry and thunk inside the image.
    for kind in ("retail", "debug"):
        entry = u32(0x128) ^ ENTRY_KEYS[kind]
        thunk = u32(0x158) ^ THUNK_KEYS[kind]
        if base <= entry < base + u32(0x10C) and base <= thunk < base + u32(0x10C):
            break
    print(f"kind        {kind}")
    print(f"entry       0x{entry:08X}")
    print(f"kernel thunk 0x{thunk:08X}")

    print(f"\n{len(sections)} sections:")
    for name, va, vsize, raw, rsize, flags in sections:
        print(f"  {name:<10} va 0x{va:08X} vsize 0x{vsize:07X} raw 0x{raw:07X} rsize 0x{rsize:07X} flags 0x{flags:X}")

    print("\nlibraries:")
    nlib, lib_va = u32(0x160), u32(0x164)
    for i in range(nlib):
        o = at(lib_va) + i * 16
        name = data[o : o + 8].rstrip(b"\0").decode()
        major, minor, build, flags = struct.unpack_from("<4H", data, o + 8)
        print(f"  {name:<8} {major}.{minor}.{build} flags 0x{flags:X}")

    imports = []
    va = thunk
    while True:
        b = read_va(va, 4)
        v = struct.unpack("<I", b)[0]
        if v == 0:
            break
        imports.append(v & 0x7FFFFFFF)
        va += 4
    print(f"\n{len(imports)} kernel imports (ordinals):")
    print("  " + " ".join(str(x) for x in imports))


if __name__ == "__main__":
    main()
