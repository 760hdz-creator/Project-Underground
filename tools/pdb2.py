"""Minimal reader for VC6-era "program database 2.00" (JG / MSF 2.0) PDB files.

Usage:
  python -I tools/pdb2.py <file.pdb> info          stream list + PDB info + DBI header
  python -I tools/pdb2.py <file.pdb> publics       public symbols: seg:off name
  python -I tools/pdb2.py <file.pdb> stream <n> <out>  dump raw stream
"""
import struct
import sys

MAGIC = b"Microsoft C/C++ program database 2.00\r\n\x1aJG\0\0"


class Pdb2:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if not d.startswith(MAGIC):
            raise SystemExit("not a PDB 2.00 file")
        o = len(MAGIC)
        self.page_size, _free_page, _num_pages = struct.unpack_from("<IHH", d, o)
        o += 8
        root_size, _ = struct.unpack_from("<II", d, o)
        o += 8
        root_pages = struct.unpack_from(f"<{self._npages(root_size)}H", d, o)
        root = self._read(root_pages, root_size)
        nstreams = struct.unpack_from("<H", root, 0)[0]
        o = 4
        sizes = []
        for _ in range(nstreams):
            size = struct.unpack_from("<I", root, o)[0]
            sizes.append(0 if size == 0xFFFFFFFF else size)
            o += 8
        self.streams = []
        for size in sizes:
            n = self._npages(size)
            pages = struct.unpack_from(f"<{n}H", root, o)
            o += 2 * n
            self.streams.append((pages, size))

    def _npages(self, size):
        return (size + self.page_size - 1) // self.page_size

    def _read(self, pages, size):
        ps = self.page_size
        return b"".join(self.data[p * ps : (p + 1) * ps] for p in pages)[:size]

    def stream(self, i):
        return self._read(*self.streams[i])


def dbi_header(dbi):
    # NewDBI header (VC 4.1+): verSig=-1, verHdr, age, then stream indices.
    sig, ver, age, gs, _ver2, ps, _pdbver, symrec = struct.unpack_from("<iIIHHHHH", dbi, 0)
    return {"sig": sig, "ver": ver, "age": age, "globals": gs, "publics": ps, "symrec": symrec}


def iter_symbols(buf):
    o = 0
    while o + 4 <= len(buf):
        reclen, rectype = struct.unpack_from("<HH", buf, o)
        if reclen < 2:
            break
        n = o + 2 + reclen
        if rectype in (0x0400, 0x0401, 0x0403):
            # VC6 S_PROCREF/S_DATAREF/S_LPROCREF: name is a pascal string
            # outside reclen, then the record is padded to 4 bytes.
            n += 1 + buf[n]
            n = (n + 3) & ~3
        yield rectype, buf[o + 4 : n]
        o = n


def modules(pdb):
    """Yield (name, objfile, stream, cbSyms) from the DBI module list."""
    dbi = pdb.stream(3)
    cb_modi = struct.unpack_from("<i", dbi, 24)[0]
    o, end = 64, 64 + cb_modi
    while o < end:
        sn, cb_syms = struct.unpack_from("<HI", dbi, o + 34)
        a = dbi.index(b"\0", o + 64)
        b = dbi.index(b"\0", a + 1)
        yield dbi[o + 64 : a].decode("latin-1"), dbi[a + 1 : b].decode("latin-1"), sn, cb_syms
        o = (b + 1 + 3) & ~3


def procs(pdb):
    """Yield (seg, off, size, name, module, objfile) for S_GPROC32_ST/S_LPROC32_ST."""
    for mod, obj, sn, cb_syms in modules(pdb):
        if sn == 0xFFFF or not cb_syms:
            continue
        buf = pdb.stream(sn)[4:cb_syms]  # skip CV signature
        for rectype, body in iter_symbols(buf):
            if rectype in (0x100A, 0x100B):
                size, = struct.unpack_from("<I", body, 12)
                off, seg = struct.unpack_from("<IH", body, 28)
                yield seg, off, size, pascal(body, 35), mod, obj


def pascal(b, o):
    n = b[o]
    return b[o + 1 : o + 1 + n].decode("latin-1")


def main():
    pdb = Pdb2(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "info":
        print(f"page size {pdb.page_size}, {len(pdb.streams)} streams")
        for i, (_, size) in enumerate(pdb.streams):
            if size:
                print(f"  stream {i:4}: {size}")
        ver, sig, age = struct.unpack_from("<III", pdb.stream(1), 0)
        print(f"PDB info: version {ver} signature 0x{sig:08X} age {age}")
        print("DBI:", dbi_header(pdb.stream(3)))
    elif cmd == "publics":
        h = dbi_header(pdb.stream(3))
        types = {}
        for rectype, body in iter_symbols(pdb.stream(h["symrec"])):
            types[rectype] = types.get(rectype, 0) + 1
            if rectype == 0x1009:  # S_PUB32_ST: typind u32, off u32, seg u16, name
                _typ, off, seg = struct.unpack_from("<IIH", body, 0)
                print(f"{seg:04X}:{off:08X} {pascal(body, 10)}")
        print("record types:", {hex(k): v for k, v in types.items()}, file=sys.stderr)
    elif cmd == "procs":
        for seg, off, size, name, mod, obj in procs(pdb):
            print(f"{seg:04X}:{off:08X} {size:6X} {name}\t{mod}\t{obj}")
    elif cmd == "modules":
        for mod, obj, sn, cb in modules(pdb):
            print(f"{sn:5} {cb:8} {mod}\t{obj}")
    elif cmd == "stream":
        open(sys.argv[4], "wb").write(pdb.stream(int(sys.argv[3])))


if __name__ == "__main__":
    main()
