"""Differential test: lifted functions (out/oracle.dll) vs unicorn running the original bytes.

Usage: python -I tools/oracle_test.py [--trials N] [--only VA,VA] [--all-leaf] [--limit N]

For each selected function both engines start from the same memory image and
registers, with a return address pointing at a sentinel. We compare general
registers, x87 TOP / ST(0), and memory in the scratch, stack and .data ranges.
"""
import argparse
import ctypes
import os
import random
import struct
import sys

import unicorn
from unicorn import x86_const as U

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
from xbedis import Xbe  # noqa: E402

MEM_SIZE = 0x04000000
SCRATCH = (0x03000000, 0x00100000)
STACK_TOP = 0x03FF0000
STACK = (0x03F00000, 0x000F0000)
SENTINEL = 0x03FFF000
REGS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
UREGS = (U.UC_X86_REG_EAX, U.UC_X86_REG_ECX, U.UC_X86_REG_EDX, U.UC_X86_REG_EBX,
         U.UC_X86_REG_ESP, U.UC_X86_REG_EBP, U.UC_X86_REG_ESI, U.UC_X86_REG_EDI)


class X128(ctypes.Union):
    _fields_ = [("f", ctypes.c_float * 4), ("u", ctypes.c_uint32 * 4), ("q", ctypes.c_uint64 * 2)]


class Ctx(ctypes.Structure):
    _fields_ = [(r, ctypes.c_uint32) for r in REGS] + [
        ("df", ctypes.c_uint32), ("fs_base", ctypes.c_uint32),
        ("st", ctypes.c_double * 8), ("top", ctypes.c_uint32), ("fcw", ctypes.c_uint16), ("fsw", ctypes.c_uint16),
        ("mm", ctypes.c_uint64 * 8), ("xmm", X128 * 8), ("mxcsr", ctypes.c_uint32)]


def f80_to_float(v):
    if isinstance(v, tuple):
        mant, exp = v
    else:
        mant, exp = v & ((1 << 64) - 1), v >> 64
    sign = -1.0 if exp & 0x8000 else 1.0
    exp &= 0x7FFF
    if exp == 0 and mant == 0:
        return 0.0 * sign
    if exp == 0x7FFF:   # infinity: integer bit set, fraction zero; anything else is a NaN
        return sign * float("inf") if (mant & ((1 << 63) - 1)) == 0 else float("nan")
    return sign * mant * 2.0 ** (exp - 16383 - 63)


def close(a, b):
    if a != a and b != b:
        return True
    if a == b:
        return True
    return abs(a - b) <= 1e-5 * max(abs(a), abs(b), 1e-30)


def mem_diff(a, b, base):
    """Return list of differing 4-byte words, ignoring tiny float differences."""
    out = []
    if a == b:
        return out
    n = len(a)
    step = 4096
    for o in range(0, n, step):
        if a[o:o + step] == b[o:o + step]:
            continue
        for w in range(o, min(o + step, n), 4):
            x, y = a[w:w + 4], b[w:w + 4]
            if x != y:
                fx, fy = struct.unpack("<f", x)[0], struct.unpack("<f", y)[0]
                if close(fx, fy) and abs(fx) > 1e-30:
                    continue
                out.append((base + w, x.hex(), y.hex()))
                if len(out) > 8:
                    return out
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=4)
    ap.add_argument("--only")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--nonleaf", action="store_true", help="test non-leaf functions without fs: access")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()

    xbe = Xbe(os.path.join(ROOT, "extracted", "default.xbe"))
    image = bytearray(MEM_SIZE)
    for name, va, vsize, raw, rsize in xbe.sections:
        image[va:va + rsize] = xbe.data[raw:raw + rsize]
    data_sec = next(s for s in xbe.sections if s[0] == ".data")
    DATA = (data_sec[1], data_sec[2])
    regions = [SCRATCH, STACK, DATA]

    dll = ctypes.CDLL(os.path.join(ROOT, "out", "oracle.dll"))
    dll.oracle_init.restype = ctypes.c_void_p
    dll.oracle_run.argtypes = (ctypes.c_uint32, ctypes.POINTER(Ctx), ctypes.c_char_p)
    assert dll.oracle_ctx_size() == ctypes.sizeof(Ctx), (dll.oracle_ctx_size(), ctypes.sizeof(Ctx))
    gmem = dll.oracle_init()
    if args.v:
        print(f"oracle.dll base {dll._handle:#x}, g_mem {gmem:#x}")

    uc = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
    uc.mem_map(0, MEM_SIZE)

    funcs = []
    for line in open(os.path.join(ROOT, "out", "lifted", "functions.tsv"), encoding="utf-8"):
        va, n, calls, fs, ind, leaf, name = line.rstrip("\n").split("\t")
        funcs.append((int(va, 16), int(n), leaf == "1", name, fs == "1"))
    if args.only:
        want = {int(x, 16) for x in args.only.split(",")}
        sel = [f for f in funcs if f[0] in want]
    elif args.nonleaf:
        sel = [f for f in funcs if not f[2] and not f[4]]
    else:
        sel = [f for f in funcs if f[2]]
    if args.limit:
        sel = sel[:args.limit]

    rng = random.Random(args.seed)
    stats = {"pass": 0, "fail": 0, "skip": 0}
    failures = []
    for va, n, leaf, name, _fs in sel:
        verdict = "pass"
        detail = ""
        done = 0
        for attempt in range(args.trials * 4):
            if done >= args.trials:
                break
            # memory image + scratch data
            words = []
            for _ in range(SCRATCH[1] // 4 // 16):
                k = rng.randrange(3)
                if k == 0:
                    words.append(struct.pack("<f", rng.uniform(-10, 10)))
                elif k == 1:
                    words.append(struct.pack("<I", SCRATCH[0] + 0x40 * rng.randrange(1, 0x3000)))
                else:
                    words.append(struct.pack("<I", rng.randrange(8)))
            scratch = b"".join(words) * 16
            mem = bytearray(image)
            mem[SCRATCH[0]:SCRATCH[0] + SCRATCH[1]] = scratch[:SCRATCH[1]]
            esp = STACK_TOP
            args_ = []
            for i in range(12):
                k = rng.randrange(3)
                if k == 0:
                    args_.append(SCRATCH[0] + 0x40 * rng.randrange(1, 0x3000))
                elif k == 1:
                    args_.append(struct.unpack("<I", struct.pack("<f", rng.uniform(-10, 10)))[0])
                else:
                    args_.append(rng.randrange(16))
            struct.pack_into("<I", mem, esp, SENTINEL)
            for i, a in enumerate(args_):
                struct.pack_into("<I", mem, esp + 4 + 4 * i, a)
            regs = [SCRATCH[0] + 0x40 * rng.randrange(1, 0x3000) for _ in REGS]
            regs[4] = esp

            uc = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)  # reuse leaves stale state
            uc.mem_map(0, MEM_SIZE)
            uc.mem_write(0, bytes(mem))
            for r, v in zip(UREGS, regs):
                uc.reg_write(r, v)
            uc.reg_write(U.UC_X86_REG_EFLAGS, 0x202)
            uc.reg_write(U.UC_X86_REG_FPCW, 0x027F)
            uc.reg_write(U.UC_X86_REG_FPSW, 0)
            uc.reg_write(U.UC_X86_REG_FPTAG, 0xFFFF)
            try:
                uc.emu_start(va, SENTINEL, count=2_000_000)
            except unicorn.UcError as e:
                detail = f"unicorn: {e}"
                continue
            except OSError as e:  # unicorn crashed in-process (seen on code writes)
                detail = f"unicorn host error: {e}"
                uc = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
                uc.mem_map(0, MEM_SIZE)
                continue
            if uc.reg_read(U.UC_X86_REG_EIP) != SENTINEL:
                detail = "unicorn did not return"
                continue

            if os.environ.get("ORACLE_DUMP"):
                open(os.environ["ORACLE_DUMP"], "wb").write(bytes(mem) + struct.pack("<8I", *regs))
            ctypes.memmove(gmem, bytes(mem), MEM_SIZE)
            c = Ctx()
            for r, v in zip(REGS, regs):
                setattr(c, r, v)
            c.fcw = 0x027F
            err = ctypes.create_string_buffer(512)
            rc = dll.oracle_run(va, ctypes.byref(c), err)
            if rc != 0:
                if args.v:
                    gb = (ctypes.c_char * MEM_SIZE).from_address(gmem)
                    print("  debug: esp", hex(regs[4]), "guest args now",
                          [hex(struct.unpack_from("<I", gb, regs[4] + 4 * i)[0]) for i in range(4)],
                          "mem args", [hex(struct.unpack_from("<I", mem, regs[4] + 4 * i)[0]) for i in range(4)],
                          "attempt", attempt)
                msg = err.value.decode(errors="replace")
                if rc == -3 and "(guest 0x" in msg and int(msg.split("(guest 0x")[1].rstrip(")"), 16) >= MEM_SIZE:
                    detail = "lifted ran past mapped memory (input-induced)"
                    continue
                verdict, detail = "fail", f"lifted rc {rc}: {msg}"
                break

            diffs = []
            for r, ur in zip(REGS, UREGS):
                a, b = uc.reg_read(ur), getattr(c, r)
                if a != b:
                    diffs.append(f"{r} uc={a:08X} lifted={b:08X}")
            utop = (uc.reg_read(U.UC_X86_REG_FPSW) >> 11) & 7
            if utop != c.top:
                diffs.append(f"x87 top uc={utop} lifted={c.top}")
            elif utop != 0:
                u0 = f80_to_float(uc.reg_read(U.UC_X86_REG_FP0 + utop))
                l0 = c.st[c.top]
                if not close(u0, l0):
                    diffs.append(f"st0 uc={u0!r} lifted={l0!r}")
            gbuf = (ctypes.c_char * MEM_SIZE).from_address(gmem)
            for base, size in regions:
                d = mem_diff(bytes(uc.mem_read(base, size)), bytes(gbuf[base:base + size]), base)
                diffs += [f"mem {a:08X} uc={x} lifted={y}" for a, x, y in d]
            if diffs:
                verdict, detail = "fail", "; ".join(diffs[:6])
                break
            done += 1
        if verdict == "pass" and done == 0:
            verdict = "skip"
        elif verdict == "pass":
            detail = f"{done} trials"
        stats[verdict] += 1
        if verdict == "fail":
            failures.append((va, name, detail))
        if args.v or verdict == "fail":
            print(f"{verdict:4} {va:08X} {name:<40} {detail}")
    print(stats)


if __name__ == "__main__":
    main()
