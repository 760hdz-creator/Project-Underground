"""Dev build driver: compile lifted code + runtime with the portable llvm-mingw toolchain.

Usage: python -I tools/build.py [lifted] [oracle] [runtime] [--jobs N]
  lifted   compile out/lifted/*.c -> out/lifted/*.o
  oracle   link out/oracle.dll (lifted objects + runtime/oracle/oracle.c) for tools/oracle_test.py
No Visual Studio or Windows SDK: llvm-mingw ships clang, lld and the mingw-w64 headers/libraries.
"""
import argparse, concurrent.futures, glob, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import bootstrap  # noqa: E402

BIN = os.path.join(bootstrap.llvm_mingw_dir(), "bin")
CC = os.path.join(BIN, "clang.exe" if os.name == "nt" else "clang")
INC = os.path.join(ROOT, "recomp", "include")
OUT = os.path.join(ROOT, "out")
LIFTED = os.path.join(OUT, "lifted")

# Lifted code type-puns guest memory and relies on x86 integer/float semantics:
#   -fno-strict-aliasing  (clang-cl defaulted to this; the GNU driver does not)
#   -fwrapv               signed overflow wraps like the guest
#   -ffp-contract=off     no FMA fusion: float results must match the guest bit for bit
LIFT_FLAGS = ["-O2", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-fno-math-errno", "-w", "-c", "-I" + INC]


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def newest_input(src):
    """mtime of src and of the local headers it #includes directly (e.g. shaders_spv.h)."""
    t = os.path.getmtime(src)
    d = os.path.dirname(src)
    with open(src, encoding="latin-1") as f:
        for line in f:
            m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
            if m and os.path.exists(os.path.join(d, m.group(1))):
                t = max(t, os.path.getmtime(os.path.join(d, m.group(1))))
    return t


def compile_all(items, jobs):
    def one(it):
        src, obj, flags = it
        if os.path.exists(obj) and os.path.getmtime(obj) >= newest_input(src):
            return src, (0, "")
        return src, run([CC] + flags + [src, "-o", obj])
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for i, (src, (code, out)) in enumerate(ex.map(one, items), 1):
            if code:
                bad += 1
                print(f"FAIL {os.path.basename(src)}\n{out[-3000:]}", flush=True)
            else:
                print(f"compiled {os.path.basename(src)} ({i}/{len(items)})", flush=True)
    if bad:
        sys.exit(f"ERROR: {bad} file(s) failed")


def lift(jobs):
    """Merge runtime misses (out/missed_targets.txt) into recomp/seeds_extra.txt, then re-lift."""
    seeds = os.path.join(ROOT, "recomp", "seeds_extra.txt")
    missed = os.path.join(OUT, "missed_targets.txt")
    have = {l.split("#")[0].strip().upper() for l in open(seeds)} if os.path.exists(seeds) else set()
    if os.path.exists(missed):
        new = sorted({l.strip().upper() for l in open(missed) if l.strip()} - have)
        with open(seeds, "a") as f:
            for va in new:
                f.write(va + "\n")
        os.remove(missed)
        print(f"seeds: +{len(new)} from missed_targets.txt")
    lift_only(os.path.join(ROOT, "game", "default.xbe"), LIFTED)


def lift_only(xbe, out_dir):
    """recomp/lift.py with the committed seeds only (ug_build.py calls this directly)."""
    code, out = run([sys.executable, "-I", os.path.join(ROOT, "recomp", "lift.py"), xbe, out_dir])
    print(out[-600:])
    if code:
        sys.exit("ERROR: lift failed")


def build_lifted(jobs):
    items = [(f, f[:-2] + ".o", LIFT_FLAGS) for f in sorted(glob.glob(os.path.join(LIFTED, "*.c")))]
    compile_all(items, jobs)


def build_oracle(jobs):
    obj = os.path.join(OUT, "oracle.o")
    compile_all([(os.path.join(ROOT, "runtime", "oracle", "oracle.c"), obj, ["-O2", "-c", "-I" + INC])], 1)
    objs = sorted(glob.glob(os.path.join(LIFTED, "*.o")))
    code, out = run([CC, "-shared", "-static", "-o", os.path.join(OUT, "oracle.dll"), obj] + objs)
    if code:
        sys.exit("ERROR: oracle link failed\n" + out[-3000:])
    print("linked out/oracle.dll")


RT_SRC = os.path.join(ROOT, "runtime", "src")
RT_OBJ = os.path.join(OUT, "rt")
RT_LIBS = ["-lwinmm", "-luser32", "-lgdi32", "-lshlwapi", "-lshell32", "-ladvapi32", "-lole32", "-lwinhttp"]


def build_runtime(jobs, exe=None, defines=(), gui=False):
    """Compile the runtime and link it with LIFTED/*.o. ug_build.py (player build) passes its own exe path,
    -DUG_VERSION and gui=True (no console window); RT_OBJ/LIFTED are module globals it repoints."""
    os.makedirs(RT_OBJ, exist_ok=True)
    flags = list(defines) + ["-O2", "-g", "-c", "-I" + INC, "-I" + RT_SRC, "-I" + os.path.join(ROOT, "third_party", "vulkan"),
             "-I" + os.path.join(ROOT, "third_party", "vulkan", "include"), "-D_CRT_SECURE_NO_WARNINGS", "-DVK_USE_PLATFORM_WIN32_KHR",
             "-Wall", "-Wno-unused-function", "-Wno-missing-braces"]
    srcs = sorted(glob.glob(os.path.join(RT_SRC, "*.c"))) + [os.path.join(ROOT, "third_party", "vulkan", "volk.c")]
    compile_all([(f, os.path.join(RT_OBJ, os.path.basename(f)[:-2] + ".o"), flags) for f in srcs], jobs)
    objs = [os.path.join(RT_OBJ, os.path.basename(f)[:-2] + ".o") for f in srcs]
    objs += sorted(glob.glob(os.path.join(LIFTED, "*.o")))
    exe = exe or os.path.join(OUT, "underground.exe")
    res = os.path.join(RT_OBJ, "underground.res.o")
    if os.path.exists(res):
        os.remove(res)
    code, out = run([os.path.join(BIN, "x86_64-w64-mingw32-windres.exe"), "-O", "coff", "-I", os.path.join(ROOT, "runtime"),
                     "-i", os.path.join(ROOT, "runtime", "underground.rc"), "-o", res])
    if code or not os.path.exists(res):   # the game works without its icon
        print("warning: no icon: " + out[-500:])
    else:
        objs.append(res)
    code, out = run([CC, "-static", "-g", "-o", exe] + (["-mwindows"] if gui else []) + objs + RT_LIBS)
    if code:
        sys.exit("ERROR: link failed\n" + out[-4000:])
    print("linked " + exe)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", nargs="*", default=["lifted"])
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    bootstrap.main()
    for w in a.what:
        {"lift": lift, "lifted": build_lifted, "oracle": build_oracle, "runtime": build_runtime}[w](a.jobs)


if __name__ == "__main__":
    main()
