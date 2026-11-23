"""Stage the installer payload: dist/payload.zip (embedded in ProjectUnderground-Setup.exe by installer/build.bat).

usage: python -I tools/package.py
Holds only our build kit (git-tracked files from SHIP) + third party: the embedded CPython (python.org), the
capstone Python package (BSD, from this PC's site-packages, same version the dev lift uses) and the vendored
Vulkan-Headers + volk. No game code or data: the player's PC lifts and builds the game from their own disc
(tools/ug_build.py). Aborts if anything matching FORBIDDEN is staged.
"""
import fnmatch
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

import capstone

ROOT = Path(__file__).resolve().parent.parent
PY_VER = "3.15.0"
PY_URL = f"https://www.python.org/ftp/python/{PY_VER}/python-{PY_VER}-embed-amd64.zip"
CAPSTONE_VER = "5.0.7"   # the dev lift's version: a different decoder could change the lifted code
SHIP = ["VERSION", "recomp/lift.py", "recomp/hle.txt", "recomp/hooks.txt", "recomp/seeds_extra.txt",
        "recomp/include/*", "runtime/src/*", "third_party/vulkan/*", "tools/ug_build.py", "tools/build.py",
        "tools/bootstrap.py", "tools/extract_xiso.py", "tools/xbedis.py", "tools/ug_android.py"]
# Android kit (tools/build_android.sh in WSL: android-kit/ = libugrt.a, include/, template.apk, licenses/; no game code)
ANDROID_KIT = Path(__import__("os").environ.get("UG_ANDROID_KIT", ROOT / "dist" / "_cache" / "android-kit"))
FORBIDDEN = ["*.xbe", "*.iso", "*.rar", "*.bik", "*.prx", "*.xbx", "*.pdb", "game/*", "extracted/*", "out/*",
             "*/lifted/*", "toolchain/*"]


def main():
    if capstone.__version__ != CAPSTONE_VER:
        sys.exit(f"capstone {capstone.__version__} installed, the kit pins {CAPSTONE_VER}")
    out = ROOT / "dist" / "payload"
    if out.exists():
        shutil.rmtree(out)
    tracked = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    files = [f for f in tracked if any(fnmatch.fnmatch(f, p) for p in SHIP)]
    for f in files:
        (out / f).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / f, out / f)
    cache = ROOT / "dist" / "_cache" / Path(PY_URL).name
    if not cache.exists():
        cache.parent.mkdir(parents=True, exist_ok=True)
        sibling = Path(r"G:\Project Gauntlet\dist\_cache") / cache.name
        if sibling.exists():
            shutil.copy2(sibling, cache)
        else:
            print("downloading " + PY_URL)
            urllib.request.urlretrieve(PY_URL, cache)
    with zipfile.ZipFile(cache) as z:
        z.extractall(out / "python")
    cs = Path(capstone.__file__).parent
    shutil.copytree(cs, out / "python" / "capstone", ignore=shutil.ignore_patterns("__pycache__", "include", "*.a"))
    lic_dir = out / "licenses"
    lic_dir.mkdir(exist_ok=True)
    for lic in list(cs.parent.glob("capstone-*.dist-info/LICENSE*")) + list(cs.parent.glob("capstone-*.dist-info/licenses/*")):
        shutil.copy2(lic, lic_dir / ("capstone_" + lic.name))
    shutil.copy2(out / "python" / "LICENSE.txt", lic_dir / "python_LICENSE.txt")
    for lic in ("LICENSE-Vulkan-Headers.md", "LICENSE-volk.md"):
        shutil.copy2(ROOT / "third_party" / "vulkan" / lic, lic_dir / lic)
    if not (ANDROID_KIT / "template.apk").exists():
        sys.exit(f"Android kit missing at {ANDROID_KIT} (tools/build_android.sh <dir> in WSL, then copy <dir>/android-kit)")
    shutil.copytree(ANDROID_KIT, out / "android", ignore=shutil.ignore_patterns("licenses"))
    for lic in (ANDROID_KIT / "licenses").glob("*"):
        shutil.copy2(lic, lic_dir / ("android_" + lic.name))
    with zipfile.ZipFile(ANDROID_KIT / "template.apk") as z:   # the template must hold no game code
        if any(n.endswith("libmain.so") for n in z.namelist()):
            sys.exit("android template.apk holds game code")
    # xxHash (BSD-2-Clause): the license is the header comment of runtime/src/xxhash.h
    head = (ROOT / "runtime" / "src" / "xxhash.h").read_text(encoding="utf-8", errors="replace").split("*/")[0] + "*/\n"
    (lic_dir / "xxhash_LICENSE.txt").write_text(head, encoding="utf-8")
    bad = [p for p in out.rglob("*") if p.is_file() and
           any(fnmatch.fnmatch(p.relative_to(out).as_posix().lower(), f.lower()) for f in FORBIDDEN)]
    if bad:
        sys.exit("FORBIDDEN files staged: " + ", ".join(map(str, bad[:10])))
    zpath = ROOT / "dist" / "payload.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(out.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(out).as_posix())
    print(f"staged {len(files)} tracked files + python {PY_VER} + capstone {capstone.__version__} -> {zpath} "
          f"({zpath.stat().st_size / 2**20:.1f} MB)")


if __name__ == "__main__":
    main()
