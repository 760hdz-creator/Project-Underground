"""Build dist/ProjectUnderground-Setup-<VERSION>.exe with llvm-mingw (no Visual Studio, no Windows SDK).

1. python -I tools/package.py -> dist/payload.zip (stages from git ls-files: `git add` new tracked files first)
2. windres (llvm-mingw) setup.rc -> setup.res (COFF): icon, background art, payload, manifest
3. clang++ setup.cpp setup.res, static (no libc++ / libunwind / winpthread DLLs), GUI subsystem, wWinMain

usage: python -I installer/build.py [--skip-package] [--toolchain <llvm-mingw dir>]
"""
import argparse
import glob
import os
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LIBS = ["user32", "gdi32", "comctl32", "comdlg32", "shell32", "ole32", "uuid", "winhttp", "windowscodecs",
        "msimg32", "advapi32", "uxtheme", "gdiplus"]


def run(cmd, cwd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def find_toolchain(arg):
    if arg:
        return arg
    hits = sorted(glob.glob(os.path.join(ROOT, "toolchain", "llvm-mingw-*-ucrt-x86_64")))
    if not hits:
        sys.exit("llvm-mingw not found under toolchain/ (pass --toolchain)")
    return hits[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-package", action="store_true", help="reuse dist/payload.zip")
    ap.add_argument("--toolchain")
    a = ap.parse_args()
    bin_dir = os.path.join(find_toolchain(a.toolchain), "bin")
    with open(os.path.join(ROOT, "VERSION")) as f:
        ver = f.read().strip()
    dist = os.path.join(ROOT, "dist")
    os.makedirs(dist, exist_ok=True)
    if not a.skip_package:
        run([sys.executable, "-I", os.path.join(ROOT, "tools", "package.py")], ROOT)
    if not os.path.exists(os.path.join(dist, "payload.zip")):
        sys.exit("dist/payload.zip is missing")
    res = os.path.join(HERE, "setup.res")
    run([os.path.join(bin_dir, "x86_64-w64-mingw32-windres.exe"), "-O", "coff", "-i", "setup.rc", "-o", res], HERE)
    exe = os.path.join(dist, f"ProjectUnderground-Setup-{ver}.exe")
    run([os.path.join(bin_dir, "x86_64-w64-mingw32-clang++.exe"), "-O2", "-std=c++20", "-static", "-municode",
         "-mwindows", "-DUNICODE", "-D_UNICODE", f'-DUG_SETUP_VERSION="{ver}"', "-Wall", "-Wno-missing-braces",
         "setup.cpp", res, "-o", exe, "-s"] + ["-l" + lib for lib in LIBS], HERE)
    os.remove(res)
    # the portable zip (RecompLauncher): the same program and recomplauncher.json
    zname = os.path.join(dist, f"ProjectUnderground-{ver}-Windows-x64.zip")
    with zipfile.ZipFile(zname, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(exe, "ProjectUnderground.exe")
        z.write(os.path.join(ROOT, "recomplauncher.json"), "recomplauncher.json")
    print(f"Built {zname} ({os.path.getsize(zname) / 1e6:.1f} MB)")
    print(f"Built {exe} ({os.path.getsize(exe) / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
