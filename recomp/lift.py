"""Static x86 -> C lifter for default.xbe (Project Underground; from Project Gauntlet).

Usage: python -I recomp/lift.py <default.xbe> <out_dir> [--seeds match.tsv] [--per-file N]

Output in <out_dir>:
  funcs.h        prototypes
  f_NNN.c        lifted functions
  functable.c    sorted VA -> function table for indirect calls
  lift_report.txt unimplemented mnemonics, function count, jump tables
"""
import argparse
import collections
import os
import struct
import sys

import capstone
from capstone import x86_const as X

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from xbedis import Xbe  # noqa: E402

# Sections come from the XBE header. XBE section flags mark .rdata executable too, so classify by name.
NON_CODE = {".rdata", ".data", "DOLBY", "BINKDATA", "$$XTIMAGE", "$$XSIMAGE", ".XTLID", ".XPP&Dat", ".XID"}
NON_DATA = {"$$XTIMAGE", "$$XSIMAGE", ".XTLID"}
CODE_SECTIONS = ()   # filled in by configure(xbe)
DATA_SECTIONS = ()

R32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
R16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
R8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
R8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
CTYPE = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
STYPE = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
BITS = {1: 7, 2: 15, 4: 31}

COND = {
    "o": "of", "no": "!of", "b": "cf", "ae": "!cf", "e": "zf", "ne": "!zf",
    "be": "(cf|zf)", "a": "(!cf&&!zf)", "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf",
    "l": "(sf!=of)", "ge": "(sf==of)", "le": "(zf||sf!=of)", "g": "(!zf&&sf==of)",
}
COND_ALIAS = {"c": "b", "nae": "b", "nc": "ae", "nb": "ae", "z": "e", "nz": "ne", "na": "be",
              "nbe": "a", "pe": "p", "po": "np", "nge": "l", "nl": "ge", "ng": "le", "nle": "g"}


IO_RANGES = []         # D3D section (NV2A registers), DSOUND section (APU registers): from configure(xbe)
DSOUND_RANGE = (0, 0)
# DSOUND is lifted as-is until its API entry points are identified (no PDB on this disc). Set
# DSOUND_HLE = True once recomp/hle.txt covers the DSOUND C API; DSOUND_LIFT then lists the
# DSOUND-section functions that must stay lifted (CRT static initializers).
DSOUND_HLE = False
DSOUND_LIFT = set()
TEXT_BASE = 0x11000    # lowest code address; from configure(xbe)


def configure(xbe):
    """Derive section tables and MMIO ranges from the XBE header."""
    global CODE_SECTIONS, DATA_SECTIONS, IO_RANGES, DSOUND_RANGE, TEXT_BASE
    names = [n for n, va, vs, raw, rs in xbe.sections]
    CODE_SECTIONS = tuple(n for n in names if n not in NON_CODE)
    DATA_SECTIONS = tuple(n for n in names if n not in NON_DATA and n != ".text")
    sec = {n: (va, va + vs) for n, va, vs, raw, rs in xbe.sections}
    IO_RANGES = [sec[n] for n in ("D3D", "DSOUND") if n in sec]
    DSOUND_RANGE = sec.get("DSOUND", (0, 0))
    TEXT_BASE = sec[".text"][0]


HLE = {}  # va -> name: functions implemented by the runtime instead of lifted
for _line in open(os.path.join(HERE, "hle.txt"), encoding="utf-8"):
    _line = _line.split("#")[0].split()
    if _line:
        HLE[int(_line[0], 16)] = _line[1]

HOOKS = {}  # va -> name: lifted functions that call x86_hook(c, va) on entry (recomp/hooks.txt)
for _line in open(os.path.join(HERE, "hooks.txt"), encoding="utf-8"):
    _line = _line.split("#")[0].split()
    if _line:
        HOOKS[int(_line[0], 16)] = _line[1]


def cond(cc):
    return COND[COND_ALIAS.get(cc, cc)]


class Unsupported(Exception):
    pass


# --------------------------------------------------------------------------- discovery

class Program:
    def __init__(self, xbe):
        self.xbe = xbe
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.code = [(n, va, rs) for n, va, vs, raw, rs in xbe.sections if n in CODE_SECTIONS]
        self.decoded = {}

    def is_code(self, va):
        return any(sva <= va < sva + rs for _n, sva, rs in self.code)

    def decode(self, va):
        ins = self.decoded.get(va)
        if ins is None:
            buf = self.xbe.read(va, 16)
            ins = next(self.md.disasm(buf, va), None) if buf else None
            self.decoded[va] = ins
        return ins


def jump_table(prog, ins, insns):
    """Recover `jmp [reg*4 + table]`. Returns list of targets or None."""
    op = ins.operands[0]
    if op.type != X.X86_OP_MEM or op.mem.scale != 4 or op.mem.base != 0 or op.mem.index == 0:
        return None
    table = op.mem.disp & 0xFFFFFFFF
    idx_reg = ins.reg_name(op.mem.index)
    # Bound from a preceding `cmp idx, N` + `ja`, possibly through a byte table:
    # cmp r, N / ja def / [movzx r, byte [r + bytetab]] / jmp [r*4 + table]
    limit = None
    bytetab = None
    mask_bound = False
    negate = False
    via_jb = False
    cur = ins.address
    for _ in range(10):
        prev = None
        for back in range(1, 16):
            p_ = insns.get(cur - back)
            if p_ is not None and p_.address + p_.size == cur:
                if p_.mnemonic not in ("jmp", "ret"):   # no fall-through from an unconditional jump
                    prev = p_
                break
        if prev is None:
            # reached only by a branch (e.g. `cmp ecx, 8 / jb L / ... / L: neg ecx / jmp [...]`): follow the branch
            for p_ in insns.values():
                if p_ is not None and p_.mnemonic.startswith("j") and p_.mnemonic != "jmp" and p_.operands and                         p_.operands[0].type == X.X86_OP_IMM and (p_.operands[0].imm & 0xFFFFFFFF) == cur:
                    prev = p_
                    via_jb = p_.mnemonic in ("jb", "jc", "jnae")
                    break
        if prev is None:
            break
        cur = prev.address
        ops_ = prev.operands
        if prev.mnemonic == "movzx" and ops_[1].type == X.X86_OP_MEM and                 prev.reg_name(ops_[0].reg) == idx_reg and bytetab is None:
            r = ops_[1].mem.base or ops_[1].mem.index
            bytetab = (ops_[1].mem.disp & 0xFFFFFFFF, prev.reg_name(r))
        elif prev.mnemonic == "cmp" and len(ops_) == 2 and ops_[1].type == X.X86_OP_IMM and                 ops_[0].type == X.X86_OP_REG:
            creg = prev.reg_name(ops_[0].reg)
            if creg == idx_reg or (bytetab is not None and creg == bytetab[1]):
                limit = ops_[1].imm & 0xFFFFFFFF
                break
        elif prev.mnemonic == "and" and len(ops_) == 2 and ops_[1].type == X.X86_OP_IMM and \
                ops_[0].type == X.X86_OP_REG and prev.reg_name(ops_[0].reg) == idx_reg and bytetab is None and \
                (ops_[1].imm & 0xFFFFFFFF) < 256:
            limit = ops_[1].imm & 0xFFFFFFFF   # `and idx, mask` bounds the index (VC6 memcpy/memset tables)
            mask_bound = True
            break
        elif via_jb and prev.mnemonic == "sub" and len(ops_) == 2 and ops_[1].type == X.X86_OP_IMM and                 ops_[0].type == X.X86_OP_REG and prev.reg_name(ops_[0].reg) == idx_reg and bytetab is None:
            # `sub idx, N / jb L / ... L: jmp [idx*4 + table]`: idx is -N..-1 there (VC7 memcpy short tails)
            n = ops_[1].imm & 0xFFFFFFFF
            out = []
            for i in range(1, min(n, 64) + 1):
                b = prog.xbe.read((table - 4 * i) & 0xFFFFFFFF, 4)
                t = struct.unpack("<I", b)[0] if len(b) == 4 else 0
                if prog.is_code(t):
                    out.append(((-i) & 0xFFFFFFFF, t))
            return out or None
        elif prev.mnemonic == "neg" and ops_[0].type == X.X86_OP_REG and prev.reg_name(ops_[0].reg) == idx_reg                 and not negate and bytetab is None:
            negate = True   # `neg idx; jmp [idx*4 + table]`: entries below the table (VC7 memcpy tail copies)
        elif ops_ and ops_[0].type == X.X86_OP_REG and prev.reg_name(ops_[0].reg) == idx_reg and                 prev.mnemonic not in ("cmp", "test", "push") and bytetab is None:
            break  # index register redefined before any bound check
    if negate:
        if limit is None:
            return None
        out = []
        for i in range(limit + 1):
            b = prog.xbe.read((table - 4 * i) & 0xFFFFFFFF, 4)
            t = struct.unpack("<I", b)[0] if len(b) == 4 else 0
            if not prog.is_code(t):
                break
            out.append(((-i) & 0xFFFFFFFF, t))
        return out or None
    if bytetab is not None and limit is not None:
        raw = prog.xbe.read(bytetab[0], limit + 1)
        count = max(raw) + 1 if raw else 0
    elif limit is not None:
        count = limit + 1
    else:
        count = None  # unbounded (state machines): take entries that point near the jmp
    targets = []
    for i in range(count if count is not None else 256):
        b = prog.xbe.read(table + 4 * i, 4)
        if len(b) < 4:
            break
        t = struct.unpack("<I", b)[0]
        if count is None and not (ins.address - 0x10000 <= t <= ins.address + 0x10000 and plausible(prog, t)):
            break
        if not prog.is_code(t):
            if count is None or (limit is not None and i == count - 1 and bytetab is None):
                break   # `cmp idx, N / jb`: N itself is out of range
            if mask_bound:
                targets.append(None)   # masked index: unreachable slot (memcpy tables start with filler)
                continue
            return None
        targets.append(t)
    if not any(t is not None for t in targets):
        return None
    return [(i, t) for i, t in enumerate(targets) if t is not None]


class Function:
    def __init__(self, start):
        self.start = start
        self.insns = {}       # va -> capstone insn
        self.labels = set()   # va that need a label
        self.tables = {}      # va of jmp -> targets
        self.calls = set()
        self.tails = set()
        self.imms = set()


def discover(prog, start, starts):
    f = Function(start)
    work = [start]
    block = []
    while work:
        va = work.pop()
        block = []
        while va not in f.insns:
            ins = prog.decode(va)
            if ins is None:
                f.insns[va] = None  # invalid -> trap
                break
            f.insns[va] = ins
            block.append(ins)
            m = ins.mnemonic
            nxt = va + ins.size
            if m in ("push", "mov") and ins.operands[-1].type == X.X86_OP_IMM:
                imm = ins.operands[-1].imm & 0xFFFFFFFF
                if imm >= TEXT_BASE and prog.is_code(imm):
                    f.imms.add(imm)
            if m == "call":
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    f.calls.add(op.imm & 0xFFFFFFFF)
                va = nxt
                continue
            if m in ("ret", "retf", "hlt", "ud2", "int3", "iretd", "iret"):
                break
            if m == "jmp" or m.startswith("j") or m in ("loop", "loope", "loopne"):
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xFFFFFFFF
                    if t != start and t in starts:
                        f.tails.add(t)
                    elif prog.is_code(t):
                        f.labels.add(t)
                        work.append(t)
                    if m == "jmp":
                        break
                    va = nxt
                    continue
                if m == "jmp":
                    targets = jump_table(prog, ins, f.insns)
                    if targets:
                        f.tables[va] = targets
                        for _i, t in targets:
                            f.labels.add(t)
                            work.append(t)
                    break
            va = nxt
    return f


def find_seeds(prog):
    """Weak seeds: pointers in data that look like function starts."""
    seeds = set()
    for n, va, vs, raw, rs in prog.xbe.sections:
        if n not in DATA_SECTIONS:
            continue
        data = prog.xbe.read(va, rs)
        for o in range(0, len(data) - 3, 4):
            t = struct.unpack_from("<I", data, o)[0]
            if not prog.is_code(t) or t in seeds:
                continue
            p = prog.xbe.read(t - 1, 1)
            p3 = prog.xbe.read(t - 3, 1)
            boundary = (p and p[0] in (0xCC, 0x90, 0xC3)) or (p3 and p3[0] == 0xC2) or t % 16 == 0
            if not boundary:
                # member of a function pointer table (initializer lists, vtables)?
                nb = [struct.unpack_from("<I", data, q)[0] for q in (o - 4, o + 4) if 0 <= q <= len(data) - 4]
                boundary = any(prog.is_code(v) and v >= TEXT_BASE for v in nb)
            if boundary and plausible(prog, t):
                seeds.add(t)
    return seeds


def is_junk(f):
    for ins in f.insns.values():
        if ins is None or ins.mnemonic in JUNK or ins.mnemonic.startswith(("bnd ", "notrack ", "v")):
            return True
    return False


JUNK = {"aas", "aaa", "daa", "das", "aam", "aad", "in", "out", "insb", "insd", "insw", "outsb", "outsd", "outsw",
        "hlt", "into", "bound", "arpl", "les", "lds", "salc", "iretd", "retf", "ljmp", "lcall", "int1", "sldt",
        "lldt", "clts", "xlatb", "sysret", "syscall", "getsec", "wbinvd", "invd", "cli", "sti"}


def plausible(prog, va):
    """True if `va` decodes like real code for a few instructions."""
    for _ in range(12):
        ins = prog.decode(va)
        if ins is None or ins.mnemonic in JUNK or ins.mnemonic.startswith("bnd ") or ins.mnemonic.startswith("v"):
            return False
        if ins.mnemonic in ("ret", "jmp"):
            return True
        va += ins.size
    return True


# --------------------------------------------------------------------------- emission

class Emitter:
    def __init__(self, prog, func, starts):
        self.prog = prog
        self.f = func
        self.starts = starts
        self.out = []
        self.unimpl = collections.Counter()
        # XDK D3D library code touches NV2A registers: route its memory accesses through the MMIO check.
        self.io = any(lo <= func.start < hi for lo, hi in IO_RANGES)

    # -- operands --
    def addr(self, ins, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg_read(ins.reg_name(m.base)))
        if m.index:
            idx = self.reg_read(ins.reg_name(m.index))
            parts.append(f"{idx}*{m.scale}" if m.scale != 1 else idx)
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xFFFFFFFF:X}u")
        if m.segment and ins.reg_name(m.segment) == "fs":
            parts.append("c->fs_base")
        elif m.segment and ins.reg_name(m.segment) not in ("ds", "ss", "es", "cs"):
            raise Unsupported(f"segment {ins.reg_name(m.segment)}")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def stack_based(self, ins, op):
        b = op.mem.base
        return b and ins.reg_name(b) == "esp" and not op.mem.index

    def reg_read(self, r):
        if r in R32:
            return r
        if r in R16:
            return f"(uint16_t){R16[r]}"
        if r in R8L:
            return f"(uint8_t){R8L[r]}"
        if r in R8H:
            return f"(uint8_t)({R8H[r]}>>8)"
        raise Unsupported(f"reg {r}")

    def reg_write(self, r, v):
        if r in R32:
            return f"{r} = (uint32_t)({v});"
        if r in R16:
            b = R16[r]
            return f"{b} = ({b} & 0xFFFF0000u) | (uint16_t)({v});"
        if r in R8L:
            b = R8L[r]
            return f"{b} = ({b} & 0xFFFFFF00u) | (uint8_t)({v});"
        if r in R8H:
            b = R8H[r]
            return f"{b} = ({b} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({v}) << 8);"
        raise Unsupported(f"reg {r}")

    def read(self, ins, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_read(ins.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return f"({CTYPE[size]})0x{op.imm & MASK[size]:X}u"
        if op.type == X.X86_OP_MEM:
            x = "X" if self.io and not self.stack_based(ins, op) else ""
            return f"RD{size * 8}{x}({self.addr(ins, op)})"
        raise Unsupported("operand")

    def write(self, ins, op, v, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_write(ins.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            x = "X" if self.io and not self.stack_based(ins, op) else ""
            return f"WR{size * 8}{x}({self.addr(ins, op)}, ({CTYPE[size]})({v}));"
        raise Unsupported("write operand")

    # -- flag helpers (all emitted; clang removes the dead ones) --
    def flags_logic(self, r, size):
        return f"cf = 0; of = 0; zf = ({r}) == 0; sf = ({r}) >> {BITS[size]}; pf = PARITY({r});"

    def flags_szp(self, r, size):
        return f"zf = ({r}) == 0; sf = ({r}) >> {BITS[size]}; pf = PARITY({r});"

    def emit(self, s):
        self.out.append("    " + s)

    def sync_out(self):
        return "c->eax=eax; c->ecx=ecx; c->edx=edx; c->ebx=ebx; c->esp=esp; c->ebp=ebp; c->esi=esi; c->edi=edi;"

    def sync_in(self):
        return "eax=c->eax; ecx=c->ecx; edx=c->edx; ebx=c->ebx; esp=c->esp; ebp=c->ebp; esi=c->esi; edi=c->edi;"

    def call_target(self, t, ret_va):
        if t in self.starts:
            call = f"f_{t:08X}(c);"
        else:
            call = f"x86_call_indirect(c, 0x{t:X}u);"
        return f"esp -= 4; WR32(esp, 0x{ret_va:X}u); {self.sync_out()} {call} {self.sync_in()}"

    def tail(self, t):
        # Jump to another function: run it with our frame, then return.
        if t in self.starts:
            return f"{self.sync_out()} f_{t:08X}(c); return;"
        return f"{self.sync_out()} x86_call_indirect(c, 0x{t:X}u); return;"

    # -- function --
    def run(self):
        f = self.f
        name = f"f_{f.start:08X}"
        if f.start in HLE or (DSOUND_HLE and DSOUND_RANGE[0] <= f.start < DSOUND_RANGE[1] and f.start not in DSOUND_LIFT):
            # Replaced by a host implementation (runtime dispatches on the VA); see recomp/hle.txt.
            # DSOUND is HLE'd at the API level: a DSOUND function the runtime does not implement
            # fails loudly instead of running the library on fake objects.
            return (f"/* HLE: {HLE.get(f.start, 'DSOUND internal')} */\n"
                    f"void {name}(X86Context *c) {{ x86_hle(c, 0x{f.start:X}u); }}")
        self.out.append(f"void {name}(X86Context *c) {{")
        if os.environ.get("UG_LIFT_FNCOUNT") == "1":   # debug lifts only: per-function call counts (runtime ug_fncount)
            self.emit(f"ug_fncount(0x{f.start:X}u);")
        if f.start in HOOKS:
            self.emit(f"x86_hook(c, 0x{f.start:X}u);  /* {HOOKS[f.start]} */")
        self.emit("uint32_t eax=c->eax, ecx=c->ecx, edx=c->edx, ebx=c->ebx, esp=c->esp, ebp=c->ebp, esi=c->esi, edi=c->edi;")
        self.emit("uint8_t cf=0, zf=0, sf=0, of=0, pf=0; (void)pf;")
        order = sorted(f.insns)
        if order[0] != f.start:
            self.emit(f"goto L_{f.start:08X};")
        prev_end = None
        for va in order:
            ins = f.insns[va]
            if prev_end is not None and prev_end != va:
                # Gap: previous instruction falls through into undiscovered bytes? No: gaps only
                # follow terminators, so nothing to do.
                pass
            self.out.append(f"L_{va:08X}:;")
            if ins is None:
                self.emit(f"{self.sync_out()} x86_unimpl(c, 0x{va:X}u, \"invalid opcode\"); return;")
                prev_end = None
                continue
            try:
                self.insn(ins)
            except Unsupported as e:
                key = ins.mnemonic
                self.unimpl[key] += 1
                text = f"{ins.mnemonic} {ins.op_str}".replace('"', "'")
                self.emit(f"{self.sync_out()} x86_unimpl(c, 0x{va:X}u, \"{text} ({e})\"); return;")
            nxt = va + ins.size
            prev_end = nxt
            # If the next instruction is not part of this function but control falls through,
            # jump/tail there explicitly.
            if not self.terminates(ins) and nxt not in f.insns:
                if nxt in self.starts:
                    self.emit(self.tail(nxt))
                else:
                    self.emit(f"{self.sync_out()} x86_unimpl(c, 0x{nxt:X}u, \"fell off decoded code\"); return;")
        self.out.append("}")
        return "\n".join(self.out)

    @staticmethod
    def terminates(ins):
        return ins.mnemonic in ("ret", "retf", "jmp", "hlt", "ud2", "int3", "iretd", "iret")

    def goto(self, t):
        if t in self.f.insns:
            spin = "x86_spin(); " if t <= self.cur_va and self.spin_loop(t, self.cur_va) else ""
            return f"{spin}goto L_{t:08X};"
        return self.tail(t)

    def spin_loop(self, lo, hi):
        """Short backward loop that only polls memory (no calls, no stores) and reads a global: a spin-wait on a
        value another thread or an interrupt changes. Cooperative scheduling must yield there."""
        body = [i for va, i in self.f.insns.items() if lo <= va <= hi]
        if not body or len(body) > 24 or any(i is None for i in body):
            return False
        polls = False
        for i in body:
            if i.mnemonic in ("call", "ret") or i.mnemonic.startswith(("rep", "stos", "movs")):
                return False
            if i.mnemonic == "pause":
                polls = True
            for k, op in enumerate(i.operands):
                if op.type != X.X86_OP_MEM:
                    continue
                if k == 0 and len(i.operands) > 1 and i.mnemonic not in ("cmp", "test"):
                    return False   # a store: copy/fill loop, not a poll
                if op.mem.base == 0 and op.mem.index == 0:
                    polls = True
        return polls

    # -- instructions --
    def insn(self, ins):
        self.cur_va = ins.address
        m = ins.mnemonic
        ops = ins.operands
        va = ins.address
        nxt = va + ins.size
        prefix = ""
        if m.startswith(("rep ", "repe ", "repne ", "lock ")):
            prefix, m = m.split(" ", 1)
            if prefix == "lock":
                prefix = ""

        h = getattr(self, "i_" + m, None)
        if h is not None:
            return h(ins, ops, nxt) if not prefix else h(ins, ops, nxt, prefix)
        if m.startswith("j") and (m[1:] in COND or m[1:] in COND_ALIAS):
            t = ops[0].imm & 0xFFFFFFFF
            self.emit(f"if ({cond(m[1:])}) {{ {self.goto(t)} }}")
            return
        if m.startswith("set") and (m[3:] in COND or m[3:] in COND_ALIAS):
            self.emit(self.write(ins, ops[0], f"{cond(m[3:])} ? 1 : 0", 1))
            return
        if m.startswith("cmov") and (m[4:] in COND or m[4:] in COND_ALIAS):
            self.emit(f"if ({cond(m[4:])}) {{ {self.write(ins, ops[0], self.read(ins, ops[1]))} }}")
            return
        if m.startswith("f"):
            return self.x87(ins, m, ops)
        return self.simd(ins, m, ops)

    # ---- MMX / SSE ----
    MMX_BIN = {"paddb", "paddw", "paddd", "psubb", "psubw", "psubd", "paddsw", "psubsw", "paddusw", "pmullw",
               "pcmpeqb", "pcmpeqw", "pcmpgtb", "pcmpgtw", "pcmpgtd", "pavgb", "pand", "pandn", "por", "pxor",
               "pmaddwd", "packssdw", "packuswb", "punpcklbw", "punpckhbw", "punpcklwd", "punpckhwd",
               "punpckldq", "punpckhdq", "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad",
               "psubusw", "psubusb", "paddusb", "paddsb", "psubsb", "pmulhw", "pmulhuw", "pminub", "pmaxub",
               "pminsw", "pmaxsw", "pavgw", "packsswb", "psadbw"}
    SSE_SCALAR = {"addss", "subss", "mulss", "divss", "minss", "maxss", "sqrtss", "rcpss", "rsqrtss"}
    SSE_BIN = {"addps", "subps", "mulps", "divps", "minps", "maxps", "andps", "orps", "xorps", "andnps",
               "unpcklps", "unpckhps", "movlhps", "movhlps", "sqrtps", "rcpps", "rsqrtps"}

    def vreg(self, ins, op):
        n = ins.reg_name(op.reg)
        if n.startswith("mm"):
            return "mm", int(n[2:])
        if n.startswith("xmm"):
            return "xmm", int(n[3:])
        return "gpr", n

    def src64(self, ins, op):
        if op.type == X.X86_OP_IMM:
            return f"(uint64_t)0x{op.imm & 0xFF:X}u"
        if op.type == X.X86_OP_MEM:
            return f"RD64({self.addr(ins, op)})"
        kind, r = self.vreg(ins, op)
        if kind == "mm":
            return f"c->mm[{r}]"
        if kind == "xmm":
            return f"c->xmm[{r}].q[0]"
        raise Unsupported("mmx src")

    def srcf32(self, ins, op):
        if op.type == X.X86_OP_MEM:
            return f"RDF32({self.addr(ins, op)})"
        return f"c->xmm[{self.vreg(ins, op)[1]}].f[0]"

    def src128(self, ins, op):
        if op.type == X.X86_OP_MEM:
            return f"({{ X128 t_; memcpy(&t_, g_mem + {self.addr(ins, op)}, 16); t_; }})"
        kind, r = self.vreg(ins, op)
        if kind != "xmm":
            raise Unsupported("sse src")
        return f"c->xmm[{r}]"

    def simd(self, ins, m, ops):
        e = self.emit
        if m in self.MMX_BIN:
            kind, d = self.vreg(ins, ops[0])
            if kind != "mm":
                raise Unsupported("sse2 integer")
            e(f"c->mm[{d}] = mmx_{m}(c->mm[{d}], {self.src64(ins, ops[1])});")
            return
        if m in self.SSE_BIN:
            kind, d = self.vreg(ins, ops[0])
            e(f"sse_{m}(&c->xmm[{d}], {self.src128(ins, ops[1])});")
            return
        if m in self.SSE_SCALAR:
            # scalar single: lane 0 only, lanes 1..3 of the destination unchanged
            d = self.vreg(ins, ops[0])[1]
            src = self.srcf32(ins, ops[1])
            a = f"c->xmm[{d}].f[0]"
            expr = {"addss": f"{a} + {src}", "subss": f"{a} - {src}", "mulss": f"{a} * {src}",
                    "divss": f"{a} / {src}", "minss": f"x86_minf({a}, {src})", "maxss": f"x86_maxf({a}, {src})",
                    "sqrtss": f"sqrtf({src})", "rcpss": f"1.0f / {src}", "rsqrtss": f"1.0f / sqrtf({src})"}[m]
            e(f"{a} = {expr};")
            return
        if m in ("comiss", "ucomiss"):
            d = self.vreg(ins, ops[0])[1]
            e(f"{{ float a_ = c->xmm[{d}].f[0], b_ = {self.srcf32(ins, ops[1])}; "
              f"if (a_ != a_ || b_ != b_) {{ zf = pf = cf = 1; }} else {{ zf = a_ == b_; pf = 0; cf = a_ < b_; }} of = sf = 0; }}")
            return
        if m == "cvtsi2ss":
            d = self.vreg(ins, ops[0])[1]
            e(f"c->xmm[{d}].f[0] = (float)(int32_t){self.read(ins, ops[1], 4)};")
            return
        if m in ("cmpss", "cmpps") or (m.startswith("cmp") and m.endswith(("ss", "ps")) and len(m) > 5):
            raise Unsupported("sse compare")
        if m == "movntq":
            e(f"WR64({self.addr(ins, ops[0])}, c->mm[{self.vreg(ins, ops[1])[1]}]);")
            return
        if m == "shufps":
            kind, d = self.vreg(ins, ops[0])
            e(f"sse_shufps(&c->xmm[{d}], {self.src128(ins, ops[1])}, {ops[2].imm & 0xFF}u);")
            return
        if m == "movd":
            if ops[0].type == X.X86_OP_REG and self.vreg(ins, ops[0])[0] in ("mm", "xmm"):
                kind, d = self.vreg(ins, ops[0])
                v = self.read(ins, ops[1], 4)
                e(f"c->mm[{d}] = (uint32_t)({v});" if kind == "mm" else f"c->xmm[{d}] = (X128){{0}}; c->xmm[{d}].u[0] = {v};")
            else:
                kind, s = self.vreg(ins, ops[1])
                v = f"(uint32_t)c->mm[{s}]" if kind == "mm" else f"c->xmm[{s}].u[0]"
                e(self.write(ins, ops[0], v, 4))
            return
        if m == "movq":
            if ops[0].type == X.X86_OP_MEM:
                e(f"WR64({self.addr(ins, ops[0])}, {self.src64(ins, ops[1])});")
            else:
                kind, d = self.vreg(ins, ops[0])
                if kind == "mm":
                    e(f"c->mm[{d}] = {self.src64(ins, ops[1])};")
                else:
                    e(f"{{ uint64_t t_ = {self.src64(ins, ops[1])}; c->xmm[{d}].q[0] = t_; c->xmm[{d}].q[1] = 0; }}")
            return
        if m in ("movaps", "movups", "movntps"):
            if ops[0].type == X.X86_OP_MEM:
                kind, s = self.vreg(ins, ops[1])
                e(f"memcpy(g_mem + {self.addr(ins, ops[0])}, &c->xmm[{s}], 16);")
            else:
                kind, d = self.vreg(ins, ops[0])
                e(f"c->xmm[{d}] = {self.src128(ins, ops[1])};")
            return
        if m == "movss":
            if ops[0].type == X.X86_OP_MEM:
                kind, s = self.vreg(ins, ops[1])
                e(f"WR32({self.addr(ins, ops[0])}, c->xmm[{s}].u[0]);")
            else:
                kind, d = self.vreg(ins, ops[0])
                if ops[1].type == X.X86_OP_MEM:
                    e(f"c->xmm[{d}] = (X128){{0}}; c->xmm[{d}].u[0] = RD32({self.addr(ins, ops[1])});")
                else:
                    e(f"c->xmm[{d}].u[0] = c->xmm[{self.vreg(ins, ops[1])[1]}].u[0];")
            return
        if m in ("movlps", "movhps"):
            lane = 0 if m == "movlps" else 1
            if ops[0].type == X.X86_OP_MEM:
                e(f"WR64({self.addr(ins, ops[0])}, c->xmm[{self.vreg(ins, ops[1])[1]}].q[{lane}]);")
            else:
                e(f"c->xmm[{self.vreg(ins, ops[0])[1]}].q[{lane}] = RD64({self.addr(ins, ops[1])});")
            return
        if m in ("cvttss2si", "cvtss2si"):
            src = f"RDF32({self.addr(ins, ops[1])})" if ops[1].type == X.X86_OP_MEM else \
                f"c->xmm[{self.vreg(ins, ops[1])[1]}].f[0]"
            fn = "x86_cvtf2i_trunc" if m == "cvttss2si" else "x86_cvtf2i_near"   # x86 result out of range (ARM saturates)
            e(self.write(ins, ops[0], f"{fn}({src})", 4))
            return
        if m in ("cvtps2pi", "cvttps2pi"):
            # two packed floats (xmm low qword or m64) -> two int32 in an MMX register
            kind, d = self.vreg(ins, ops[0])
            src = self.src64(ins, ops[1])
            fn = "x86_cvtf2i_trunc" if m == "cvttps2pi" else "x86_cvtf2i_near"
            e(f"{{ uint64_t t_ = {src}; float a_, b_; uint32_t lo_ = (uint32_t)t_, hi_ = (uint32_t)(t_ >> 32); "
              f"memcpy(&a_, &lo_, 4); memcpy(&b_, &hi_, 4); "
              f"c->mm[{d}] = (uint64_t){fn}(a_) | ((uint64_t){fn}(b_) << 32); }}")
            return
        if m == "cvtpi2ps":
            kind, d = self.vreg(ins, ops[0])
            e(f"{{ uint64_t t_ = {self.src64(ins, ops[1])}; c->xmm[{d}].f[0] = (float)(int32_t)t_; "
              f"c->xmm[{d}].f[1] = (float)(int32_t)(t_ >> 32); }}")
            return
        if m == "pshufw":
            kind, d = self.vreg(ins, ops[0])
            imm = ops[2].imm & 0xFF
            e(f"{{ uint64_t t_ = {self.src64(ins, ops[1])}; c->mm[{d}] = "
              + " | ".join(f"(((t_ >> {16 * ((imm >> (2 * i)) & 3)}) & 0xFFFFull) << {16 * i})" for i in range(4))
              + "; }")
            return
        if m == "xlatb":
            e("eax = (eax & 0xFFFFFF00u) | RD8(ebx + (uint8_t)eax);")
            return
        if m in ("wbinvd", "invd"):
            return
        raise Unsupported("mnemonic")

    # data movement
    def i_mov(self, ins, ops, nxt):
        if ops[0].type == X.X86_OP_REG and ins.reg_name(ops[0].reg) in ("ds", "es", "fs", "gs", "ss", "cs"):
            raise Unsupported("segment load")
        if ops[1].type == X.X86_OP_REG and ins.reg_name(ops[1].reg) in ("ds", "es", "fs", "gs", "ss", "cs"):
            raise Unsupported("segment read")
        self.emit(self.write(ins, ops[0], self.read(ins, ops[1], ops[0].size)))

    def i_movzx(self, ins, ops, nxt):
        self.emit(self.write(ins, ops[0], self.read(ins, ops[1])))

    def i_movsx(self, ins, ops, nxt):
        s = ops[1].size
        self.emit(self.write(ins, ops[0], f"({STYPE[ops[0].size]})({STYPE[s]}){self.read(ins, ops[1])}"))

    def i_lea(self, ins, ops, nxt):
        self.emit(self.write(ins, ops[0], self.addr(ins, ops[1])))

    def i_xchg(self, ins, ops, nxt):
        s = ops[0].size
        self.emit(f"{{ {CTYPE[s]} t0 = {self.read(ins, ops[0])}, t1 = {self.read(ins, ops[1])}; "
                  f"{self.write(ins, ops[0], 't1')} {self.write(ins, ops[1], 't0')} }}")

    def i_push(self, ins, ops, nxt):
        s = ops[0].size if ops[0].type != X.X86_OP_IMM else 4
        if ops[0].type == X.X86_OP_REG and ins.reg_name(ops[0].reg) in ("ds", "es", "fs", "gs", "ss", "cs"):
            self.emit("esp -= 4; WR32(esp, 0);")
            return
        v = self.read(ins, ops[0], s)
        if ops[0].type == X.X86_OP_IMM:
            v = f"(uint32_t)0x{ops[0].imm & 0xFFFFFFFF:X}u"
        self.emit(f"{{ uint32_t t = {v}; esp -= {s}; WR{s * 8}(esp, ({CTYPE[s]})t); }}")

    def i_pop(self, ins, ops, nxt):
        s = ops[0].size
        if ops[0].type == X.X86_OP_REG and ins.reg_name(ops[0].reg) in ("ds", "es", "fs", "gs", "ss", "cs"):
            self.emit("esp += 4;")
            return
        # pop [esp+x] uses esp after the increment
        self.emit(f"{{ {CTYPE[s]} t = RD{s * 8}(esp); esp += {s}; {self.write(ins, ops[0], 't')} }}")

    def i_pushal(self, ins, ops, nxt):
        self.emit("{ uint32_t t = esp; esp -= 4; WR32(esp, eax); esp -= 4; WR32(esp, ecx); esp -= 4; WR32(esp, edx); "
                  "esp -= 4; WR32(esp, ebx); esp -= 4; WR32(esp, t); esp -= 4; WR32(esp, ebp); esp -= 4; WR32(esp, esi); "
                  "esp -= 4; WR32(esp, edi); }")

    def i_popal(self, ins, ops, nxt):
        self.emit("edi = RD32(esp); esi = RD32(esp+4); ebp = RD32(esp+8); ebx = RD32(esp+16); edx = RD32(esp+20); "
                  "ecx = RD32(esp+24); eax = RD32(esp+28); esp += 32;")

    def i_pushfd(self, ins, ops, nxt):
        # ID (bit 21) is toggleable, as on a CPU with CPUID (code tests that before using cpuid)
        self.emit("esp -= 4; WR32(esp, 0x202u | cf | (pf<<2) | (zf<<6) | (sf<<7) | (c->df<<10) | (of<<11) | x86_eflags_id);")

    def i_popfd(self, ins, ops, nxt):
        self.emit("{ uint32_t t = RD32(esp); esp += 4; cf = t & 1; pf = (t>>2)&1; zf = (t>>6)&1; sf = (t>>7)&1; "
                  "c->df = (t>>10)&1; of = (t>>11)&1; x86_eflags_id = t & 0x200000u; }")

    def i_lahf(self, ins, ops, nxt):
        self.emit("eax = (eax & 0xFFFF00FFu) | ((uint32_t)(0x02 | cf | (pf<<2) | (zf<<6) | (sf<<7)) << 8);")

    def i_sahf(self, ins, ops, nxt):
        self.emit("{ uint32_t t = eax >> 8; cf = t & 1; pf = (t>>2)&1; zf = (t>>6)&1; sf = (t>>7)&1; }")

    def i_cdq(self, ins, ops, nxt):
        self.emit("edx = (uint32_t)((int32_t)eax >> 31);")

    def i_cwde(self, ins, ops, nxt):
        self.emit("eax = (uint32_t)(int32_t)(int16_t)eax;")

    def i_cwd(self, ins, ops, nxt):
        self.emit("edx = (edx & 0xFFFF0000u) | (uint16_t)((int16_t)eax >> 15);")

    def i_cbw(self, ins, ops, nxt):
        self.emit("eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;")

    def i_bswap(self, ins, ops, nxt):
        r = ins.reg_name(ops[0].reg)
        self.emit(f"{r} = __builtin_bswap32({r});")

    def i_nop(self, ins, ops, nxt):
        pass

    i_wait = i_nop
    i_prefetch = i_nop
    i_prefetchnta = i_nop
    i_prefetcht0 = i_nop
    i_sfence = i_nop
    i_emms = i_nop
    i_femms = i_nop

    def i_clc(self, ins, ops, nxt):
        self.emit("cf = 0;")

    def i_stc(self, ins, ops, nxt):
        self.emit("cf = 1;")

    def i_cmc(self, ins, ops, nxt):
        self.emit("cf = !cf;")

    def i_cld(self, ins, ops, nxt):
        self.emit("c->df = 0;")

    def i_std(self, ins, ops, nxt):
        self.emit("c->df = 1;")

    def i_cli(self, ins, ops, nxt):
        pass

    i_sti = i_cli

    # arithmetic
    def arith(self, ins, ops, kind, carry_in=None):
        s = ops[0].size
        t, st = CTYPE[s], STYPE[s]
        a = self.read(ins, ops[0])
        b = self.read(ins, ops[1], s)
        w = kind not in ("cmp",)
        sub = kind in ("sub", "cmp", "sbb")
        cin = "cf" if carry_in else "0"
        code = [f"{{ {t} a = {a}, b = {b}, ci = {cin};"]
        if sub:
            code.append(f"{t} r = ({t})(a - b - ci); cf = ci ? (a <= b) : (a < b); of = ((a ^ b) & (a ^ r)) >> {BITS[s]};")
        else:
            code.append(f"{t} r = ({t})(a + b + ci); cf = ci ? (r <= a) : (r < a); of = (~(a ^ b) & (a ^ r)) >> {BITS[s]};")
        code.append(self.flags_szp("r", s))
        if w:
            code.append(self.write(ins, ops[0], "r"))
        code.append("}")
        self.emit(" ".join(code))

    def i_add(self, ins, ops, nxt):
        self.arith(ins, ops, "add")

    def i_adc(self, ins, ops, nxt):
        self.arith(ins, ops, "add", True)

    def i_sub(self, ins, ops, nxt):
        if ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
            self.emit(self.write(ins, ops[0], "0") + " cf = 0; of = 0; zf = 1; sf = 0; pf = 1;")
            return
        self.arith(ins, ops, "sub")

    def i_sbb(self, ins, ops, nxt):
        self.arith(ins, ops, "sbb", True)

    def i_cmp(self, ins, ops, nxt):
        self.arith(ins, ops, "cmp")

    def logic(self, ins, ops, op, write=True):
        s = ops[0].size
        t = CTYPE[s]
        if op == "^" and ops[1].type == X.X86_OP_REG and ops[0].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
            self.emit(self.write(ins, ops[0], "0") + " cf = 0; of = 0; zf = 1; sf = 0; pf = 1;")
            return
        code = f"{{ {t} r = ({t})({self.read(ins, ops[0])} {op} {self.read(ins, ops[1], s)}); {self.flags_logic('r', s)}"
        if write:
            code += " " + self.write(ins, ops[0], "r")
        self.emit(code + " }")

    def i_and(self, ins, ops, nxt):
        self.logic(ins, ops, "&")

    def i_or(self, ins, ops, nxt):
        self.logic(ins, ops, "|")

    def i_xor(self, ins, ops, nxt):
        self.logic(ins, ops, "^")

    def i_test(self, ins, ops, nxt):
        self.logic(ins, ops, "&", write=False)

    def i_not(self, ins, ops, nxt):
        s = ops[0].size
        self.emit(self.write(ins, ops[0], f"~{self.read(ins, ops[0])}"))

    def i_neg(self, ins, ops, nxt):
        s = ops[0].size
        t = CTYPE[s]
        self.emit(f"{{ {t} a = {self.read(ins, ops[0])}, r = ({t})(0 - a); cf = a != 0; "
                  f"of = (a & r) >> {BITS[s]}; {self.flags_szp('r', s)} {self.write(ins, ops[0], 'r')} }}")

    def incdec(self, ins, ops, d):
        s = ops[0].size
        t = CTYPE[s]
        lim = (1 << BITS[s]) if d > 0 else (1 << BITS[s])
        ofc = f"r == 0x{lim:X}u" if d > 0 else f"a == 0x{lim:X}u"
        self.emit(f"{{ {t} a = {self.read(ins, ops[0])}, r = ({t})(a {'+' if d > 0 else '-'} 1); of = {ofc}; "
                  f"{self.flags_szp('r', s)} {self.write(ins, ops[0], 'r')} }}")

    def i_inc(self, ins, ops, nxt):
        self.incdec(ins, ops, 1)

    def i_dec(self, ins, ops, nxt):
        self.incdec(ins, ops, -1)

    def i_imul(self, ins, ops, nxt):
        if len(ops) == 1:
            s = ops[0].size
            if s == 4:
                self.emit(f"{{ int64_t r = (int64_t)(int32_t)eax * (int32_t){self.read(ins, ops[0])}; eax = (uint32_t)r; "
                          f"edx = (uint32_t)(r >> 32); cf = of = (r != (int32_t)r); }}")
            elif s == 2:
                self.emit(f"{{ int32_t r = (int32_t)(int16_t)eax * (int16_t){self.read(ins, ops[0])}; "
                          f"eax = (eax & 0xFFFF0000u) | (uint16_t)r; edx = (edx & 0xFFFF0000u) | (uint16_t)(r >> 16); "
                          f"cf = of = (r != (int16_t)r); }}")
            else:
                self.emit(f"{{ int16_t r = (int16_t)((int8_t)eax * (int8_t){self.read(ins, ops[0])}); "
                          f"eax = (eax & 0xFFFF0000u) | (uint16_t)r; cf = of = (r != (int8_t)r); }}")
            return
        s = ops[0].size
        a = self.read(ins, ops[1] if len(ops) == 3 else ops[0])
        b = self.read(ins, ops[2] if len(ops) == 3 else ops[1], s)
        st = STYPE[s]
        self.emit(f"{{ int64_t r = (int64_t)({st}){a} * ({st}){b}; cf = of = (r != ({st})r); "
                  f"{self.write(ins, ops[0], 'r')} }}")

    def i_mul(self, ins, ops, nxt):
        s = ops[0].size
        if s == 4:
            self.emit(f"{{ uint64_t r = (uint64_t)eax * {self.read(ins, ops[0])}; eax = (uint32_t)r; edx = (uint32_t)(r >> 32); "
                      f"cf = of = edx != 0; }}")
        elif s == 2:
            self.emit(f"{{ uint32_t r = (uint32_t)(uint16_t)eax * {self.read(ins, ops[0])}; eax = (eax & 0xFFFF0000u) | (uint16_t)r; "
                      f"edx = (edx & 0xFFFF0000u) | (uint16_t)(r >> 16); cf = of = (r >> 16) != 0; }}")
        else:
            self.emit(f"{{ uint16_t r = (uint16_t)((uint8_t)eax * {self.read(ins, ops[0])}); eax = (eax & 0xFFFF0000u) | r; "
                      f"cf = of = (r >> 8) != 0; }}")

    def i_div(self, ins, ops, nxt, signed=False):
        s = ops[0].size
        d = self.read(ins, ops[0])
        if s == 4:
            if signed:
                self.emit(f"{{ int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); int32_t dv = (int32_t){d}; "
                          f"eax = (uint32_t)(int32_t)(n / dv); edx = (uint32_t)(int32_t)(n % dv); }}")
            else:
                self.emit(f"{{ uint64_t n = ((uint64_t)edx << 32) | eax; uint32_t dv = {d}; "
                          f"eax = (uint32_t)(n / dv); edx = (uint32_t)(n % dv); }}")
        elif s == 2:
            ty = "int32_t" if signed else "uint32_t"
            dt = "int16_t" if signed else "uint16_t"
            self.emit(f"{{ {ty} n = ({ty})(((edx & 0xFFFF) << 16) | (eax & 0xFFFF)); {dt} dv = ({dt}){d}; "
                      f"eax = (eax & 0xFFFF0000u) | (uint16_t)(n / dv); edx = (edx & 0xFFFF0000u) | (uint16_t)(n % dv); }}")
        else:
            ty = "int16_t" if signed else "uint16_t"
            dt = "int8_t" if signed else "uint8_t"
            self.emit(f"{{ {ty} n = ({ty})(uint16_t)eax; {dt} dv = ({dt}){d}; "
                      f"eax = (eax & 0xFFFF0000u) | (uint8_t)(n / dv) | ((uint32_t)(uint8_t)(n % dv) << 8); }}")

    def i_idiv(self, ins, ops, nxt):
        self.i_div(ins, ops, nxt, signed=True)

    def shift(self, ins, ops, kind):
        s = ops[0].size
        t, st = CTYPE[s], STYPE[s]
        cnt = "1" if len(ops) == 1 else self.read(ins, ops[1], 1)
        bits = s * 8
        a = self.read(ins, ops[0])
        if kind == "shl":
            body = (f"r = ({t})((uint32_t)a << n); cf = n <= {bits} ? ((uint32_t)a >> ({bits} - n)) & 1 : 0; "
                    f"of = (r >> {BITS[s]}) ^ cf;")
        elif kind == "shr":
            body = f"r = ({t})((uint32_t)a >> n); cf = ((uint32_t)a >> (n - 1)) & 1; of = (a >> {BITS[s]}) & 1;"
        elif kind == "sar":
            body = f"r = ({t})(({st})a >> (n > {bits - 1} ? {bits - 1} : n)); cf = (({st})a >> (n > {bits} ? {bits - 1} : n - 1)) & 1; of = 0;"
        elif kind == "rol":
            body = f"unsigned k = n % {bits}; r = ({t})((a << k) | (a >> (({bits} - k) % {bits}))); cf = r & 1; of = (r >> {BITS[s]}) ^ cf;"
        elif kind == "ror":
            body = f"unsigned k = n % {bits}; r = ({t})((a >> k) | (a << (({bits} - k) % {bits}))); cf = (r >> {BITS[s]}) & 1; of = ((r >> {BITS[s]}) ^ (r >> {BITS[s] - 1})) & 1;"
        elif kind in ("rcl", "rcr"):
            loop = ("for (unsigned i = 0; i < n; i++) { uint8_t hi = (r >> %d) & 1; r = (%s)((r << 1) | cf); cf = hi; }"
                    % (BITS[s], t)) if kind == "rcl" else \
                   ("for (unsigned i = 0; i < n; i++) { uint8_t lo = r & 1; r = (%s)((r >> 1) | ((%s)cf << %d)); cf = lo; }"
                    % (t, t, BITS[s]))
            body = f"r = a; {loop} of = 0;"
        else:
            raise Unsupported(kind)
        flags = "" if kind in ("rol", "ror", "rcl", "rcr") else self.flags_szp("r", s)
        self.emit(f"{{ {t} a = {a}, r; unsigned n = ({cnt}) & 31; if (n) {{ {body} {flags} "
                  f"{self.write(ins, ops[0], 'r')} }} }}")

    def i_shl(self, ins, ops, nxt):
        self.shift(ins, ops, "shl")

    i_sal = i_shl

    def i_shr(self, ins, ops, nxt):
        self.shift(ins, ops, "shr")

    def i_sar(self, ins, ops, nxt):
        self.shift(ins, ops, "sar")

    def i_rol(self, ins, ops, nxt):
        self.shift(ins, ops, "rol")

    def i_ror(self, ins, ops, nxt):
        self.shift(ins, ops, "ror")

    def i_rcl(self, ins, ops, nxt):
        self.shift(ins, ops, "rcl")

    def i_rcr(self, ins, ops, nxt):
        self.shift(ins, ops, "rcr")

    def i_shld(self, ins, ops, nxt, right=False):
        if ops[0].size != 4:
            raise Unsupported("16-bit shld")
        a, b = self.read(ins, ops[0]), self.read(ins, ops[1])
        n = self.read(ins, ops[2], 1)
        if right:
            body = "r = (a >> n) | (b << (32 - n)); cf = (a >> (n - 1)) & 1;"
        else:
            body = "r = (a << n) | (b >> (32 - n)); cf = (a >> (32 - n)) & 1;"
        self.emit(f"{{ uint32_t a = {a}, b = {b}, r; unsigned n = ({n}) & 31; if (n) {{ {body} of = 0; "
                  f"{self.flags_szp('r', 4)} {self.write(ins, ops[0], 'r')} }} }}")

    def i_shrd(self, ins, ops, nxt):
        self.i_shld(ins, ops, nxt, right=True)

    def i_bsf(self, ins, ops, nxt):
        self.emit(f"{{ uint32_t v = {self.read(ins, ops[1])}; zf = v == 0; if (v) {{ {self.write(ins, ops[0], '__builtin_ctz(v)')} }} }}")

    def i_bsr(self, ins, ops, nxt):
        self.emit(f"{{ uint32_t v = {self.read(ins, ops[1])}; zf = v == 0; if (v) {{ {self.write(ins, ops[0], '31 - __builtin_clz(v)')} }} }}")

    def i_bt(self, ins, ops, nxt):
        self.emit(f"cf = ({self.read(ins, ops[0])} >> ({self.read(ins, ops[1])} & {ops[0].size * 8 - 1})) & 1;")

    def i_xadd(self, ins, ops, nxt):
        s = ops[0].size
        t = CTYPE[s]
        self.emit(f"{{ {t} a = {self.read(ins, ops[0])}, b = {self.read(ins, ops[1])}, r = ({t})(a + b); cf = r < a; "
                  f"of = (~(a ^ b) & (a ^ r)) >> {BITS[s]}; {self.flags_szp('r', s)} {self.write(ins, ops[1], 'a')} "
                  f"{self.write(ins, ops[0], 'r')} }}")

    def i_cmpxchg(self, ins, ops, nxt):
        s = ops[0].size
        t = CTYPE[s]
        acc = {1: "al", 2: "ax", 4: "eax"}[s]
        self.emit(f"{{ {t} d = {self.read(ins, ops[0])}, a = {self.reg_read(acc)}; zf = d == a; "
                  f"if (zf) {{ {self.write(ins, ops[0], self.read(ins, ops[1]))} }} else {{ {self.reg_write(acc, 'd')} }} }}")

    # control flow
    def i_jmp(self, ins, ops, nxt):
        op = ops[0]
        if op.type == X.X86_OP_IMM:
            self.emit(self.goto(op.imm & 0xFFFFFFFF))
            return
        targets = self.f.tables.get(ins.address)
        idx_expr = None
        if targets:
            idx_expr = self.reg_read(ins.reg_name(op.mem.index))
            cases = " ".join(f"case 0x{i:X}u: goto L_{t:08X};" for i, t in targets)
            self.emit(f"switch ({idx_expr}) {{ {cases} default: break; }}")
        self.emit(f"{{ uint32_t t = {self.read(ins, op, 4)}; {self.sync_out()} x86_call_indirect(c, t); return; }}")

    def i_call(self, ins, ops, nxt):
        op = ops[0]
        if op.type == X.X86_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t == nxt:  # call $+5 / pop reg  (get EIP)
                self.emit(f"esp -= 4; WR32(esp, 0x{nxt:X}u);")
                return
            self.emit(self.call_target(t, nxt))
            return
        self.emit(f"{{ uint32_t t = {self.read(ins, op, 4)}; esp -= 4; WR32(esp, 0x{nxt:X}u); {self.sync_out()} "
                  f"x86_call_indirect(c, t); {self.sync_in()} }}")

    def i_ret(self, ins, ops, nxt):
        n = ops[0].imm if ops else 0
        self.emit(f"esp += {4 + n}; {self.sync_out()} return;")

    def i_leave(self, ins, ops, nxt):
        self.emit("esp = ebp; ebp = RD32(esp); esp += 4;")

    def i_enter(self, ins, ops, nxt):
        size, level = ops[0].imm, ops[1].imm
        if level:
            raise Unsupported("enter level")
        self.emit(f"esp -= 4; WR32(esp, ebp); ebp = esp; esp -= {size};")

    def loopish(self, ins, ops, condexpr):
        t = ops[0].imm & 0xFFFFFFFF
        self.emit(f"ecx -= 1; if (ecx != 0{condexpr}) {{ {self.goto(t)} }}")

    def i_loop(self, ins, ops, nxt):
        self.loopish(ins, ops, "")

    def i_loope(self, ins, ops, nxt):
        self.loopish(ins, ops, " && zf")

    def i_loopne(self, ins, ops, nxt):
        self.loopish(ins, ops, " && !zf")

    def i_jecxz(self, ins, ops, nxt):
        self.emit(f"if (ecx == 0) {{ {self.goto(ops[0].imm & 0xFFFFFFFF)} }}")

    def i_int3(self, ins, ops, nxt):
        self.emit(f"{self.sync_out()} x86_int(c, 0x{ins.address:X}u, 3); return;")

    def i_int(self, ins, ops, nxt):
        self.emit(f"{self.sync_out()} x86_int(c, 0x{ins.address:X}u, {ops[0].imm}); {self.sync_in()}")

    def i_hlt(self, ins, ops, nxt):
        self.emit(f"{self.sync_out()} x86_unimpl(c, 0x{ins.address:X}u, \"hlt\"); return;")

    i_ud2 = i_hlt

    def i_cpuid(self, ins, ops, nxt):
        self.emit("x86_cpuid(&eax, &ebx, &ecx, &edx);")

    def i_rdtsc(self, ins, ops, nxt):
        self.emit("{ uint64_t t = x86_rdtsc(c); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }")

    def i_in(self, ins, ops, nxt):
        s = ops[0].size
        port = self.read(ins, ops[1], 2) if ops[1].type == X.X86_OP_REG else f"0x{ops[1].imm:X}u"
        self.emit(self.write(ins, ops[0], f"x86_in(c, {port}, {s})"))

    def i_out(self, ins, ops, nxt):
        s = ops[1].size
        port = self.read(ins, ops[0], 2) if ops[0].type == X.X86_OP_REG else f"0x{ops[0].imm:X}u"
        self.emit(f"x86_out(c, {port}, {self.read(ins, ops[1])}, {s});")

    # string ops
    def string(self, ins, kind, size, prefix=""):
        t = CTYPE[size]
        step = f"(c->df ? -{size} : {size})"
        if kind == "movs":
            body = f"WR{size*8}(edi, RD{size*8}(esi)); esi += {step}; edi += {step};"
        elif kind == "stos":
            body = f"WR{size*8}(edi, ({t})eax); edi += {step};"
        elif kind == "lods":
            body = self.reg_write({1: "al", 2: "ax", 4: "eax"}[size], f"RD{size*8}(esi)") + f" esi += {step};"
        elif kind in ("cmps", "scas"):
            a = f"RD{size*8}(esi)" if kind == "cmps" else f"({t})eax"
            b = f"RD{size*8}(edi)"
            adv = f"esi += {step}; edi += {step};" if kind == "cmps" else f"edi += {step};"
            body = (f"{{ {t} a = {a}, b = {b}, r = ({t})(a - b); cf = a < b; of = ((a ^ b) & (a ^ r)) >> {BITS[size]}; "
                    f"{self.flags_szp('r', size)} {adv} }}")
        else:
            raise Unsupported(kind)
        if not prefix:
            self.emit(body)
        elif kind in ("cmps", "scas"):
            stop = "!zf" if prefix in ("repe", "rep") else "zf"
            self.emit(f"while (ecx) {{ {body} ecx--; if ({stop}) break; }}")
        elif kind == "movs" and size == 4:
            self.emit("if (!c->df && ecx && (edi <= esi || edi >= esi + ecx * 4)) { memmove(g_mem + edi, g_mem + esi, (size_t)ecx * 4); esi += ecx * 4; edi += ecx * 4; ecx = 0; }")
            self.emit(f"while (ecx) {{ {body} ecx--; }}")
        else:
            self.emit(f"while (ecx) {{ {body} ecx--; }}")

    def _str(kind, size):
        def h(self, ins, ops, nxt, prefix=""):
            self.string(ins, kind, size, prefix)
        return h

    i_movsb, i_movsw, i_movsd = _str("movs", 1), _str("movs", 2), _str("movs", 4)
    i_stosb, i_stosw, i_stosd = _str("stos", 1), _str("stos", 2), _str("stos", 4)
    i_lodsb, i_lodsw, i_lodsd = _str("lods", 1), _str("lods", 2), _str("lods", 4)
    i_cmpsb, i_cmpsw, i_cmpsd = _str("cmps", 1), _str("cmps", 2), _str("cmps", 4)
    i_scasb, i_scasw, i_scasd = _str("scas", 1), _str("scas", 2), _str("scas", 4)

    # ---- x87 ----
    def fsrc(self, ins, op):
        """Value of a memory or st(i) operand as double."""
        if op.type == X.X86_OP_REG:
            return f"ST({self.sti(ins, op)})"
        a = self.addr(ins, op)
        return {4: f"(double)RDF32({a})", 8: f"RDF64({a})", 10: f"x87_load80({a})"}[op.size]

    def sti(self, ins, op):
        name = ins.reg_name(op.reg)
        if not name.startswith("st"):
            raise Unsupported("x87 reg")
        return int(name[3]) if "(" in name else 0

    def fstore(self, ins, op, v):
        if op.type == X.X86_OP_REG:
            return f"ST({self.sti(ins, op)}) = {v};"
        a = self.addr(ins, op)
        return {4: f"WRF32({a}, (float)({v}));", 8: f"WRF64({a}, {v});", 10: f"x87_store80({a}, {v});"}[op.size]

    def iread(self, ins, op):
        a = self.addr(ins, op)
        return {2: f"(double)(int16_t)RD16({a})", 4: f"(double)(int32_t)RD32({a})", 8: f"(double)(int64_t)RD64({a})"}[op.size]

    def x87(self, ins, m, ops):
        e = self.emit
        n = len(ops)
        arith = {"add": "+", "sub": "-", "subr": "-r", "mul": "*", "div": "/", "divr": "/r"}
        if m in ("fld",):
            e(f"{{ double v = {self.fsrc(ins, ops[0])}; FPUSH(c, v); }}")
        elif m == "fild":
            e(f"FPUSH(c, {self.iread(ins, ops[0])});")
        elif m in ("fst", "fstp"):
            e(self.fstore(ins, ops[0], "ST(0)") + (" FPOP(c);" if m == "fstp" else ""))
        elif m in ("fist", "fistp", "fisttp"):
            sz = ops[0].size
            a = self.addr(ins, ops[0])
            if m == "fisttp":
                conv = {2: f"WR16({a}, (uint16_t)FISTT16(ST(0)));", 4: f"WR32({a}, (uint32_t)FISTT32(ST(0)));",
                        8: f"WR64({a}, (uint64_t)FISTT64(ST(0)));"}[sz]
            else:
                conv = {2: f"WR16({a}, (uint16_t)FIST16(c, ST(0)));", 4: f"WR32({a}, (uint32_t)FIST32(c, ST(0)));",
                        8: f"WR64({a}, (uint64_t)FIST64(c, ST(0)));"}[sz]
            e(conv + (" FPOP(c);" if m != "fist" else ""))
        elif m == "fxch":
            i = max((self.sti(ins, o) for o in ops), default=1) or 1
            e(f"{{ double t = ST(0); ST(0) = ST({i}); ST({i}) = t; }}")
        elif m in ("fldz", "fld1", "fldpi", "fldl2e", "fldln2", "fldlg2", "fldl2t"):
            k = {"fldz": "0.0", "fld1": "1.0", "fldpi": "3.14159265358979323846", "fldl2e": "1.44269504088896340736",
                 "fldln2": "0.693147180559945309417", "fldlg2": "0.301029995663981195214",
                 "fldl2t": "3.32192809488736234787"}[m]
            e(f"FPUSH(c, {k});")
        elif m in ("fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr",
                   "faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp"):
            pop = m.endswith("p")
            base = m[1:-1] if pop else m[1:]
            op = arith[base]
            rev = op.endswith("r")
            op = op[0]
            if n == 0:
                d, s_ = "ST(1)", "ST(0)"
                pop = True
            elif n == 1 and pop:
                d, s_ = f"ST({self.sti(ins, ops[0])})", "ST(0)"
            elif n == 1:
                d, s_ = "ST(0)", self.fsrc(ins, ops[0])
            else:
                d, s_ = f"ST({self.sti(ins, ops[0])})", f"ST({self.sti(ins, ops[1])})"
            expr = f"({s_}) {op} ({d})" if rev else f"({d}) {op} ({s_})"
            e(f"{d} = {expr};" + (" FPOP(c);" if pop else ""))
        elif m in ("fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr"):
            op = arith[m[2:]]
            rev = op.endswith("r")
            op = op[0]
            v = self.iread(ins, ops[0])
            e(f"ST(0) = {v} {op} ST(0);" if rev else f"ST(0) = ST(0) {op} {v};")
        elif m in ("fcom", "fcomp", "fucom", "fucomp", "fcompp", "fucompp", "ficom", "ficomp", "ftst"):
            if m == "ftst":
                src = "0.0"
            elif m.startswith("fi"):
                src = self.iread(ins, ops[0])
            elif n:
                src = self.fsrc(ins, ops[0])
            else:
                src = "ST(1)"
            pops = 2 if m.endswith("pp") else 1 if m.endswith("p") else 0
            e(f"FCOM(c, ST(0), {src});" + " FPOP(c);" * pops)
        elif m in ("fcomi", "fcomip", "fucomi", "fucomip", "fcompi", "fucompi"):
            i = self.sti(ins, ops[1]) if n == 2 else self.sti(ins, ops[0]) if n else 1
            e(f"FCOMI(ST(0), ST({i}));" + (" FPOP(c);" if "p" in m[4:] else ""))
        elif m in ("fnstsw", "fstsw"):
            e(self.write(ins, ops[0], "FSTSW(c)", 2))
        elif m in ("fnstcw", "fstcw"):
            e(self.write(ins, ops[0], "c->fcw", 2))
        elif m == "fldcw":
            e(f"c->fcw = {self.read(ins, ops[0], 2)};")
        elif m in ("fchs", "fabs", "fsqrt", "fsin", "fcos", "frndint"):
            f = {"fchs": "-ST(0)", "fabs": "fabs(ST(0))", "fsqrt": "sqrt(ST(0))", "fsin": "sin(ST(0))",
                 "fcos": "cos(ST(0))", "frndint": "FROUND(c, ST(0))"}[m]
            e(f"ST(0) = {f};")
        elif m == "fsincos":
            e("{ double v = ST(0); ST(0) = sin(v); FPUSH(c, cos(v)); }")
        elif m == "fptan":
            e("ST(0) = tan(ST(0)); FPUSH(c, 1.0);")
        elif m == "fpatan":
            e("ST(1) = atan2(ST(1), ST(0)); FPOP(c);")
        elif m == "fyl2x":
            e("ST(1) = ST(1) * log2(ST(0)); FPOP(c);")
        elif m == "f2xm1":
            e("ST(0) = exp2(ST(0)) - 1.0;")
        elif m == "fscale":
            e("ST(0) = ldexp(ST(0), (int)trunc(ST(1)));")
        elif m in ("fprem", "fprem1"):
            fn = "fmod" if m == "fprem" else "remainder"
            e(f"ST(0) = {fn}(ST(0), ST(1)); c->fsw &= ~0x0400;")
        elif m == "fxam":
            e("{ double v = ST(0); uint16_t s = c->fsw & ~0x4700; if (signbit(v)) s |= 0x0200; "
              "if (isnan(v)) s |= 0x0100; else if (isinf(v)) s |= 0x0500; else if (v == 0) s |= 0x4000; else s |= 0x0400; c->fsw = s; }")
        elif m in ("ffree", "ffreep"):
            if m == "ffreep":
                e("FPOP(c);")
        elif m in ("fnsave", "fsave", "fnstenv", "fstenv"):
            # 32-bit protected-mode image: FCW, FSW (with TOP), tag word (all valid: the lifter keeps no empty
            # tags), IP/DP zeroed; fnsave adds ST(0..7) as 80-bit values at +28 and then reinitializes the FPU
            # (MSVC CRT pow's error path saves/restores around its handler: 183040/18304C)
            e(f"{{ uint32_t a = {self.addr(ins, ops[0])}; WR32(a, c->fcw); WR32(a + 4, FSTSW(c)); WR32(a + 8, 0); "
              "for (int i = 12; i < 28; i += 4) WR32(a + i, 0);")
            if "save" in m:
                e("  for (int i = 0; i < 8; i++) x87_store80(a + 28 + i * 10, ST(i));")
                e("  c->top = 0; c->fcw = 0x037F; c->fsw = 0; }")
            else:
                e("  c->fcw |= 0x3F; }")   # fnstenv masks all exceptions after storing
        elif m in ("frstor", "fldenv"):
            e(f"{{ uint32_t a = {self.addr(ins, ops[0])}; uint16_t s = RD16(a + 4); c->fcw = RD16(a); "
              "c->top = (s >> 11) & 7; c->fsw = s & ~0x3800;")
            if m == "frstor":
                e("  for (int i = 0; i < 8; i++) ST(i) = x87_load80(a + 28 + i * 10);")
            e("}")
        elif m in ("fnclex", "fclex", "fninit", "finit"):
            if "init" in m:
                e("c->top = 0; c->fcw = 0x037F; c->fsw = 0;")
            else:
                e("c->fsw &= 0x7F00;")
        elif m.startswith("fcmov"):
            cc = {"fcmovb": "cf", "fcmove": "zf", "fcmovbe": "(cf|zf)", "fcmovu": "pf", "fcmovnb": "!cf",
                  "fcmovne": "!zf", "fcmovnbe": "(!cf&&!zf)", "fcmovnu": "!pf"}[m]
            e(f"if ({cc}) ST(0) = ST({self.sti(ins, ops[1])});")
        else:
            raise Unsupported("x87")


# --------------------------------------------------------------------------- driver

# Host-side tunables patched into specific lifted functions: (va, old C text, new C text, expected count).
SOURCE_PATCHES = []


def source_patch(va, code):
    for pva, old, new, n in SOURCE_PATCHES:
        if pva == va:
            if code.count(old) != n:
                raise SystemExit(f"source patch {pva:X}: expected {n} x {old!r}, found {code.count(old)}")
            code = code.replace(old, new)
    return code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xbe")
    ap.add_argument("out")
    ap.add_argument("--seeds", help="match_pdb.py output; exact/moved rows become seeds")
    ap.add_argument("--per-file", type=int, default=600)
    args = ap.parse_args()

    xbe = Xbe(args.xbe)
    u32 = lambda o: struct.unpack_from("<I", xbe.data, o)[0]
    entry = u32(0x128) ^ 0xA8FC57AB
    configure(xbe)
    prog = Program(xbe)

    extra = set()
    if args.seeds:
        for line in open(args.seeds, encoding="utf-8"):
            va, _size, _name, _mod, status = line.rstrip("\n").split("\t")
            if status in ("exact", "moved"):
                extra.add(int(va, 16))
    extra_file = os.path.join(HERE, "seeds_extra.txt")  # missed indirect targets (oracle / runtime logs)
    if os.path.exists(extra_file):
        for line in open(extra_file):
            line = line.split("#")[0].strip()
            if line:
                extra.add(int(line, 16))
    strong = {entry} | {s for s in extra if prog.is_code(s)}
    weak = find_seeds(prog) - strong
    starts = strong | weak
    print(f"seeds: {len(strong)} strong, {len(weak)} weak")

    funcs = {}
    rejected = set()
    work = [(s, True) for s in sorted(strong)] + [(s, False) for s in sorted(weak)]
    work.reverse()  # strong first
    while work:
        s, is_strong = work.pop()
        if s in funcs or (s in rejected and not (is_strong or s in strong)):
            continue
        is_strong = is_strong or s in strong  # a strong seed reached first as a weak immediate stays strong
        rejected.discard(s)
        f = discover(prog, s, starts)
        if not is_strong and is_junk(f):
            rejected.add(s)
            continue
        funcs[s] = f
        for t in f.calls | f.tails:
            if t not in funcs and prog.is_code(t):
                starts.add(t)
                work.append((t, True))
        for t in f.imms:
            if t not in funcs and t not in rejected and plausible(prog, t):
                starts.add(t)
                work.append((t, False))
    starts -= rejected
    print(f"rejected weak seeds: {len(rejected)}")
    # Second pass: jumps to starts discovered later become tail calls.
    for s in list(funcs):
        funcs[s] = discover(prog, s, starts)
    print(f"functions: {len(funcs)}, instructions: {sum(len(f.insns) for f in funcs.values())}")

    os.makedirs(args.out, exist_ok=True)
    unimpl = collections.Counter()
    order = sorted(funcs)
    with open(os.path.join(args.out, "funcs.h"), "w") as h:
        h.write('#pragma once\n#include "x86rt.h"\n')
        for s in order:
            h.write(f"void f_{s:08X}(X86Context *c);\n")
    nfiles = 0
    for i in range(0, len(order), args.per_file):
        with open(os.path.join(args.out, f"f_{nfiles:03}.c"), "w") as o:
            o.write('#include "funcs.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n'
                    '#pragma clang diagnostic ignored "-Wunused-variable"\n\n')
            for s in order[i : i + args.per_file]:
                em = Emitter(prog, funcs[s], starts)
                o.write(source_patch(s, em.run()))
                o.write("\n\n")
                unimpl.update(em.unimpl)
        nfiles += 1
    with open(os.path.join(args.out, "functable.c"), "w") as o:
        o.write('#include "funcs.h"\n\nconst struct { uint32_t va; GuestFunc fn; } g_functable[] = {\n')
        for s in order:
            o.write(f"    {{0x{s:08X}u, f_{s:08X}}},\n")
        o.write(f"}};\nconst uint32_t g_functable_count = {len(order)};\n")
    names = {}
    if args.seeds:
        for line in open(args.seeds, encoding="utf-8"):
            va, _size, name, _mod, status = line.rstrip("\n").split("\t")
            if status in ("exact", "moved"):
                names[int(va, 16)] = name
    with open(os.path.join(args.out, "functions.tsv"), "w", encoding="utf-8") as o:
        for s_ in order:
            f = funcs[s_]
            ins_ = [i for i in f.insns.values() if i is not None]
            has_fs = any("fs:" in i.op_str for i in ins_)
            indirect = any(i.mnemonic in ("call", "jmp") and i.operands and i.operands[0].type != X.X86_OP_IMM
                           and i.address not in f.tables for i in ins_)
            leaf = not f.calls and not f.tails and not indirect and not has_fs and None not in f.insns.values()
            o.write(f"{s_:08X}\t{len(ins_)}\t{len(f.calls)}\t{int(has_fs)}\t{int(indirect)}\t{int(leaf)}\t"
                    f"{names.get(s_, '')}\n")
    with open(os.path.join(args.out, "lift_report.txt"), "w") as o:
        o.write(f"functions {len(funcs)}\nfiles {nfiles}\n")
        o.write(f"jump tables {sum(len(f.tables) for f in funcs.values())}\n")
        o.write("unimplemented:\n")
        for k, v in unimpl.most_common():
            o.write(f"  {k} {v}\n")
    covered = set()
    for f in funcs.values():
        for va, ins in f.insns.items():
            if ins is not None:
                covered.update(range(va, va + ins.size))
    tva, tsize = next((va, rs) for n, va, rs in prog.code if n == ".text")
    hit = sum(1 for a in covered if tva <= a < tva + tsize)
    gaps, a = [], tva
    while a < tva + tsize:
        if a in covered:
            a += 1
            continue
        b = a
        while b < tva + tsize and b not in covered:
            b += 1
        raw = prog.xbe.read(a, b - a)
        if raw.strip(bytes([0xCC, 0x90])):
            gaps.append((b - a, a))
        a = b
    gaps.sort(reverse=True)
    with open(os.path.join(args.out, "lift_report.txt"), "a") as o:
        o.write(f"text coverage {hit}/{tsize} ({100 * hit / tsize:.1f}%)\nlargest gaps:\n")
        for size, va in gaps[:200]:
            o.write(f"  {va:08X} {size:6X} {prog.xbe.read(va, 8).hex()}\n")
    print(f"text coverage {100 * hit / tsize:.1f}%, {len(gaps)} non-padding gaps")
    print(f"files: {nfiles}, unimplemented sites: {sum(unimpl.values())}")
    print("top unimplemented:", unimpl.most_common(25))


if __name__ == "__main__":
    main()
