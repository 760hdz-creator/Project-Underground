"""Build Project Underground on this PC from the player's own disc image (what the installer runs).

usage: python -I tools/ug_build.py --root <install dir> [--iso <image.iso>] [--version V] [--jobs N]
       ... --progress [--game <image or extracted folder>]   (a launcher's setup step: see class Progress)
  1. --iso: extract the XDVDFS image into <root>/game (without --iso, update mode keeps the data)
  2. check game/default.xbe is the supported USA release (SHA-256)
  3. llvm-mingw (pinned, SHA-256 checked, from its GitHub release) -> <root>/toolchain (once, ~190 MB download)
  4. recomp/lift.py with the committed seeds -> <root>/build/lifted/*.c
  5. compile + link with tools/build.py's flags (one copy of them) -> <root>/underground.exe (no console window)
Linux (the Linux package): no llvm-mingw; steps 3-5 are recomp/lift.py through tools/pyrun.py (the kit's capstone in
pylib/), then the system clang (or gcc) compiles the lifted code with build.py's LIFT_FLAGS and links it with the
prebuilt runtime (linux/libugrt.a + linux/lib/libSDL3.so.0) -> <root>/underground, rpath $ORIGIN/lib.
Progress lines start with "## ", a failure with "ERROR: " (the installer shows both). Logs go to stdout.
Nothing from the disc leaves the player's PC. No Visual Studio, no Windows SDK, no admin rights.
"""
import argparse
import concurrent.futures
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)  # the build kit: recomp/, runtime/, third_party/, tools/
XBE_SHA256 = "979424bddf7aec098e429ea0a7255aef4b257411348e497a26558f13be9de29e"
WRONG = "This is another game or version. Project Underground needs Tony Hawk's Underground for the original Xbox, USA."


def step(msg):
    print("## " + msg, flush=True)


def fail(msg):
    print("ERROR: " + msg, flush=True)
    sys.exit(1)


def retry(what, f):
    """f(), tried for 20 s: OneDrive, virus scanners and indexers hold files for a moment."""
    for i in range(40):
        try:
            return f()
        except PermissionError as e:
            if i == 39:
                fail(f"{what}: {e.strerror or e}: {e.filename}")
            time.sleep(0.5)


def rmtree(path):
    """Delete a folder. OneDrive marks its folders read-only, and Windows does not remove a read-only folder."""
    def once():
        if not os.path.isdir(path):
            return
        for d, dirs, files in os.walk(path):
            for p in [d] + [os.path.join(d, f) for f in files]:
                if not os.path.islink(p) and not os.stat(p).st_mode & stat.S_IWRITE:
                    os.chmod(p, os.stat(p).st_mode | stat.S_IWRITE)
        shutil.rmtree(path)
    retry("Could not delete an old folder (close programs that use it and try again)", once)


class Progress:
    """--progress: a launcher runs the build without a window. stdout gets "progress 0.4 Text" lines and, last, what went
    wrong; everything else goes to <root>/build/setup.log."""
    AT = {"Extracting": .02, "Copying": .02, "Checking": .25, "Getting": .27, "downloading": .28, "extracting": .34,
          "Translating": .4, "Finding": .52, "Compiling": .54, "Linking": .95, "Done": 1}

    def __init__(self, log):
        self.out, self.buf, self.err = sys.stdout, "", False
        os.makedirs(os.path.dirname(log), exist_ok=True)
        self.log = open(log, "w", encoding="utf-8", buffering=1)

    def write(self, s):
        self.buf += s
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            self.log.write(line + "\n")
            if line.startswith("## "):
                self.say(f"progress {self.AT.get(line[3:].split(' ')[0], .3)} {line[3:]}")
            elif line.startswith("compiled ") and line.endswith(")"):
                a, b = line[line.rindex("(") + 1:-1].split("/")
                if int(b) > 2:
                    self.say(f"progress {.54 + .4 * int(a) / int(b):.3f} Compiling ({a} of {b})")
            elif line.startswith("ERROR: ") or self.err:
                self.err = True
                self.say(line.replace("ERROR: ", "", 1))
        return len(s)

    def say(self, line):
        self.out.write(line + "\n")
        self.out.flush()

    def flush(self):
        self.log.flush()

    def isatty(self):
        return False


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def linux_cc():
    """clang preferred (what the Windows build uses), gcc as fallback; CC overrides."""
    for cc in [os.environ.get("CC"), "clang", "gcc", "cc"]:
        if cc and shutil.which(cc):
            return shutil.which(cc)
    fail("No C compiler found. Install clang (or gcc) with your package manager, e.g. sudo apt install clang")


def build_linux(root, lifted, jobs):
    sys.path.insert(0, HERE)
    import build  # noqa: E402  (LIFT_FLAGS: the one copy of the lifted-code flags)
    rt = os.path.join(KIT, "linux")
    if not os.path.exists(os.path.join(rt, "libugrt.a")):
        fail("linux/libugrt.a is missing from the build kit (use the Linux package).")
    step("Finding the C compiler")
    cc = linux_cc()
    print("compiler: " + cc, flush=True)
    flags = [f for f in build.LIFT_FLAGS if f != "-c" and not f.startswith("-I")] + ["-I" + os.path.join(rt, "include")]
    items = [(os.path.join(lifted, f), os.path.join(lifted, f[:-2] + ".o"))
             for f in sorted(os.listdir(lifted)) if f.endswith(".c")]
    step(f"Compiling {len(items)} files with {jobs} jobs (1 to 5 minutes)")

    def one(it):
        return it[0], run([cc] + flags + ["-c", it[0], "-o", it[1]])
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for i, (src, (code, out)) in enumerate(ex.map(one, items), 1):
            if code:
                fail(f"compile failed: {os.path.basename(src)}\n{out[-3000:]}")
            print(f"compiled {os.path.basename(src)} ({i}/{len(items)})", flush=True)
    step("Linking underground")
    lib = os.path.join(root, "lib")
    rmtree(lib)
    shutil.copytree(os.path.join(rt, "lib"), lib)
    if os.path.exists(os.path.join(rt, "gamecontrollerdb.txt")):
        shutil.copy2(os.path.join(rt, "gamecontrollerdb.txt"), root)   # controller mappings (pad_sdl.c)
    libs = [os.path.join(lib, f) for f in sorted(os.listdir(lib)) if ".so" in f]
    exe = os.path.join(root, "underground")
    code, out = run([cc, "-o", exe + ".new"] + [it[1] for it in items] + [os.path.join(rt, "libugrt.a")] + libs +
                    ["-lm", "-lpthread", "-ldl", "-Wl,--disable-new-dtags", "-Wl,-rpath,$ORIGIN/lib"])
    if code:
        fail("link failed:\n" + out[-3000:])
    os.replace(exe + ".new", exe)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--iso")
    ap.add_argument("--game", help="like --iso; also a folder with the extracted disc (default.xbe)")
    ap.add_argument("--progress", action="store_true")
    ap.add_argument("--version", default="0.0.0")
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    game, bdir = os.path.join(root, "game"), os.path.join(root, "build")
    jobs = a.jobs or max(1, min(os.cpu_count() or 2, 16))
    os.makedirs(root, exist_ok=True)
    ready = os.path.join(bdir, "version.txt")   # written once the build has finished
    disc_ok = os.path.join(bdir, "disc.txt")    # ... once the whole disc is in <root>/game
    if a.progress:
        sys.stdout = Progress(os.path.join(bdir, "setup.log"))
        exe = os.path.join(root, "underground.exe" if os.name == "nt" else "underground")
        if all(map(os.path.exists, (ready, disc_ok, exe, os.path.join(game, "default.xbe")))):
            with open(ready) as f:
                if f.read().strip() == a.version:
                    sys.stdout.say("progress 1 Already built")
                    return
    if os.path.exists(ready):
        os.remove(ready)
    if a.game and a.progress and os.path.exists(disc_ok):
        a.game = None   # an update: the disc of the first setup is still there
    py = [sys.executable, "-I"]
    if os.name != "nt":   # Linux: the kit's capstone (pylib/) goes on sys.path; -I ignores PYTHONPATH
        py.append(os.path.join(HERE, "pyrun.py"))

    if a.game and os.path.isdir(a.game):
        src = os.path.abspath(a.game)
        if not os.path.exists(os.path.join(src, "default.xbe")):
            fail("There is no default.xbe in that folder. Choose the extracted disc of Tony Hawk's Underground (Xbox, USA).")
        if sha256(os.path.join(src, "default.xbe")) != XBE_SHA256:
            fail(WRONG)
        if src != os.path.abspath(game):
            step("Copying the game folder")
            if os.path.exists(disc_ok):
                os.remove(disc_ok)
            rmtree(game)
            shutil.copytree(src, game)
    elif a.game:
        a.iso = a.game
    if a.iso:
        if os.path.exists(disc_ok):
            os.remove(disc_ok)
        step("Extracting the disc image")
        rmtree(game)
        code, out = run(py + [os.path.join(HERE, "extract_xiso.py"), a.iso, game])
        print(out[-2000:], flush=True)
        if code:
            fail("Could not read the disc image. Use an Xbox (XISO or redump) image of Tony Hawk's Underground (USA).")
    xbe = os.path.join(game, "default.xbe")
    if not os.path.exists(xbe):
        fail("game\\default.xbe is missing: run a full install with your disc image.")
    step("Checking default.xbe")
    if sha256(xbe) != XBE_SHA256:
        fail(WRONG)
    if a.iso or a.game:
        os.makedirs(bdir, exist_ok=True)
        with open(disc_ok, "w") as f:
            f.write(XBE_SHA256 + "\n")

    if os.name != "nt":
        lifted = os.path.join(bdir, "lifted")
        step("Translating the game code (about 30 seconds)")
        rmtree(lifted)
        code, out = run(py + [os.path.join(KIT, "recomp", "lift.py"), xbe, lifted])
        print(out[-600:], flush=True)
        if code:
            fail("code translation failed:\n" + out[-2000:])
        build_linux(root, lifted, jobs)
        os.makedirs(os.path.join(root, "out"), exist_ok=True)
        with open(os.path.join(bdir, "version.txt"), "w") as f:
            f.write(a.version + "\n")
        step("Done")
        return

    # build.py reads the toolchain location when it is imported
    os.environ["UG_TOOLCHAIN_DIR"] = os.path.join(root, "toolchain")
    sys.path.insert(0, HERE)
    import bootstrap  # noqa: E402
    step("Getting the compiler (llvm-mingw, first install only: ~190 MB)")
    bootstrap.main()
    import build  # noqa: E402

    os.makedirs(bdir, exist_ok=True)
    lifted = os.path.join(bdir, "lifted")
    step("Translating the game code (about 30 seconds)")
    rmtree(lifted)
    build.lift_only(xbe, lifted)

    build.LIFTED = lifted
    build.RT_OBJ = os.path.join(bdir, "rt")
    rmtree(build.RT_OBJ)   # never reuse objects of an earlier version: build.py compares file dates only
    step(f"Compiling with {jobs} jobs (1 to 3 minutes)")
    build.build_lifted(jobs)
    step("Linking underground.exe")
    exe = os.path.join(root, "underground.exe")
    build.build_runtime(jobs, exe=exe + ".new", defines=[f'-DUG_VERSION="{a.version}"'], gui=True)
    retry("Could not replace underground.exe. Close the game and try again", lambda: os.replace(exe + ".new", exe))
    # SDL3.dll + gamecontrollerdb.txt: every controller SDL knows (runtime/src/pad_sdl.c; without them, XInput only)
    for f in ("SDL3.dll", "gamecontrollerdb.txt"):
        src = os.path.join(KIT, "sdl3", f)
        if os.path.exists(src):
            retry("Could not replace " + f, lambda: shutil.copy2(src, os.path.join(root, f)))
    os.makedirs(os.path.join(root, "out"), exist_ok=True)   # saves (out/hdd) and the log live here
    with open(ready, "w") as f:
        f.write(a.version + "\n")
    step("Done")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:   # the installer shows the ERROR line; the log keeps the details
        traceback.print_exc()
        fail(f"{type(e).__name__}: {e}")
