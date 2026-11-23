"""Build Project Underground on this PC from the player's own disc image (what the installer runs).

usage: python -I tools/ug_build.py --root <install dir> [--iso <image.iso>] [--version V] [--jobs N]
  1. --iso: extract the XDVDFS image into <root>/game (without --iso, update mode keeps the data)
  2. check game/default.xbe is the supported USA release (SHA-256)
  3. llvm-mingw (pinned, SHA-256 checked, from its GitHub release) -> <root>/toolchain (once, ~190 MB download)
  4. recomp/lift.py with the committed seeds -> <root>/build/lifted/*.c
  5. compile + link with tools/build.py's flags (one copy of them) -> <root>/underground.exe (no console window)
Progress lines start with "## ", a failure with "ERROR: " (the installer shows both). Logs go to stdout.
Nothing from the disc leaves the player's PC. No Visual Studio, no Windows SDK, no admin rights.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)  # the build kit: recomp/, runtime/, third_party/, tools/
XBE_SHA256 = "979424bddf7aec098e429ea0a7255aef4b257411348e497a26558f13be9de29e"


def step(msg):
    print("## " + msg, flush=True)


def fail(msg):
    print("ERROR: " + msg, flush=True)
    sys.exit(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--iso")
    ap.add_argument("--version", default="0.0.0")
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    game, bdir = os.path.join(root, "game"), os.path.join(root, "build")
    jobs = a.jobs or max(1, min(os.cpu_count() or 2, 16))
    os.makedirs(root, exist_ok=True)

    if a.iso:
        step("Extracting the disc image")
        if os.path.isdir(game):
            shutil.rmtree(game)
        p = subprocess.run([sys.executable, "-I", os.path.join(HERE, "extract_xiso.py"), a.iso, game],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(p.stdout[-2000:], flush=True)
        if p.returncode:
            fail("Could not read the disc image. Use an Xbox (XISO or redump) image of Tony Hawk's Underground (USA).")
    xbe = os.path.join(game, "default.xbe")
    if not os.path.exists(xbe):
        fail("game\\default.xbe is missing: run a full install with your disc image.")
    step("Checking default.xbe")
    if sha256(xbe) != XBE_SHA256:
        fail("default.xbe is not the supported version (Tony Hawk's Underground, Xbox, USA).")

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
    if os.path.isdir(lifted):
        shutil.rmtree(lifted)
    build.lift_only(xbe, lifted)

    build.LIFTED = lifted
    build.RT_OBJ = os.path.join(bdir, "rt")
    step(f"Compiling with {jobs} jobs (1 to 3 minutes)")
    build.build_lifted(jobs)
    step("Linking underground.exe")
    exe = os.path.join(root, "underground.exe")
    build.build_runtime(jobs, exe=exe + ".new", defines=[f'-DUG_VERSION="{a.version}"'], gui=True)
    os.replace(exe + ".new", exe)
    os.makedirs(os.path.join(root, "out"), exist_ok=True)   # saves (out/hdd) and the log live here
    step("Done")


if __name__ == "__main__":
    main()
