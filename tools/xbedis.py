"""Disassemble XBE code at a virtual address.

Usage: python -I tools/xbedis.py <default.xbe> <va hex> [count=40]
Stops after `count` instructions or at the first ret/int3 run.
"""
import struct
import sys

import capstone


class Xbe:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        u32 = lambda o: struct.unpack_from("<I", self.data, o)[0]
        self.base = u32(0x104)
        self.sections = []
        n, sec = u32(0x11C), u32(0x120) - self.base
        for i in range(n):
            _f, va, vsize, raw, rsize, name_va = struct.unpack_from("<6I", self.data, sec + i * 56)
            o = name_va - self.base
            name = self.data[o : self.data.index(b"\0", o)].decode()
            self.sections.append((name, va, vsize, raw, rsize))

    def read(self, va, n):
        # Bytes past the raw data up to the virtual size are zeros (as loaded).
        for _name, sva, vsize, raw, rsize in self.sections:
            if sva <= va < sva + max(rsize, vsize):
                n = min(n, sva + max(rsize, vsize) - va)
                o = va - sva
                got = self.data[raw + o : raw + min(o + n, rsize)] if o < rsize else b""
                return got + bytes(n - len(got))
        return b""

    def section_of(self, va):
        for name, sva, vsize, _raw, _rs in self.sections:
            if sva <= va < sva + vsize:
                return name
        return None


def main():
    xbe = Xbe(sys.argv[1])
    va = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for i, ins in enumerate(md.disasm(xbe.read(va, count * 16), va)):
        if i >= count:
            break
        print(f"{ins.address:08X}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")
        if ins.mnemonic == "int3":
            break


if __name__ == "__main__":
    main()
