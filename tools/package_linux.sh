#!/usr/bin/env bash
# Linux package: dist/ProjectUnderground-Linux-x64-<VERSION>.tar.gz (run on Linux / WSL from a checkout of this repo).
#
#   tools/package_linux.sh [out dir]      (default: dist/)
#
# Holds our build kit (git-tracked files, as tools/package.py ships for Windows), the prebuilt runtime
# (tools/build_linux.sh: libugrt.a + libSDL3.so.0), capstone 5.0.7 (BSD, manylinux wheel from PyPI),
# SDL_GameControllerDB and the licenses. No game code or data: setup.sh lifts and compiles the game on the
# player's PC from their own disc. Aborts if anything FORBIDDEN is staged.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:-$root/dist}
ver=$(tr -d '\r\n' < "$root/VERSION")
name=ProjectUnderground-Linux-x64-$ver
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
stage=$work/$name
kit=$stage/kit
mkdir -p "$kit/linux" "$kit/licenses" "$kit/pylib" "$out"

# runtime + SDL3, stamped with the version (the game's updater compares it)
bash "$root/tools/build_linux.sh" "$work/rt" --version "$ver"
cp "$work/rt/libugrt.a" "$kit/linux/"
cp -r "$work/rt/lib" "$work/rt/include" "$kit/linux/"
strip --strip-unneeded "$kit/linux/lib/"*.so* 2>/dev/null || true
cp "$root/installer/res/icon.png" "$kit/linux/icon.png"
# Android kit (no game code; setup.sh --android builds the APK from it): tools/build_android.sh stage 1
akit=${UG_ANDROID_KIT:-}
if [[ -z $akit ]]; then bash "$root/tools/build_android.sh" "$work/android" --version "$ver" >/dev/null; akit=$work/android/android-kit; fi
mkdir -p "$kit/android"
cp -r "$akit/libugrt.a" "$akit/include" "$akit/template.apk" "$kit/android/"
for f in "$akit/licenses/"*; do cp "$f" "$kit/licenses/android_$(basename "$f")"; done
unzip -l "$kit/android/template.apk" | grep -q libmain.so && { echo "android template.apk holds game code" >&2; exit 1; }

# kit files (git-tracked): what tools/ug_build.py needs on Linux
ship=(VERSION recomp/lift.py recomp/hle.txt recomp/hooks.txt recomp/seeds_extra.txt 'recomp/include/*'
      tools/ug_build.py tools/ug_android.py tools/build.py tools/bootstrap.py tools/pyrun.py tools/extract_xiso.py
      tools/xbedis.py)
cd "$root"
for pat in "${ship[@]}"; do
    git ls-files -- "$pat" | while read -r f; do mkdir -p "$kit/$(dirname "$f")"; tr -d '\r' < "$f" > "$kit/$f"; done
done
cp "$root/installer/linux/setup.sh" "$stage/setup.sh"
cp "$root/installer/linux/README-Linux.txt" "$stage/README-Linux.txt"
tr -d '\r' < "$root/installer/linux/ProjectUnderground" > "$stage/ProjectUnderground"   # RecompLauncher starts this
tr -d '\r' < "$root/recomplauncher.json" > "$stage/recomplauncher.json"
chmod +x "$stage/setup.sh" "$stage/ProjectUnderground"

# capstone 5.0.7 (manylinux wheel: pure Python + its own libcapstone.so), the version the dev lift uses
cache=${UG_CACHE:-$HOME/.cache/project-underground}
mkdir -p "$cache"
whl=$cache/capstone-5.0.7-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl
whl_sha=${UG_CAPSTONE_SHA:-}
[[ -f $whl ]] || curl -sSfL -o "$whl" "https://files.pythonhosted.org/packages/py3/c/capstone/$(basename "$whl")"
[[ -z $whl_sha ]] || echo "$whl_sha  $whl" | sha256sum -c - >/dev/null
python3 -m zipfile -e "$whl" "$kit/pylib"
rm -rf "$kit/pylib/capstone/include" "$kit/pylib/capstone/lib/"*.a
for f in "$kit/pylib"/capstone-*.dist-info/LICENSE* "$kit/pylib"/capstone-*.dist-info/licenses/*; do
    [[ -f $f ]] && cp "$f" "$kit/licenses/capstone_$(basename "$f")"
done

# SDL_GameControllerDB (zlib): mappings for controllers SDL has none for (pinned commit)
gcdb_rev=4e87e3c6c2574f49e1cdf8526b6f829b61e5ccb3
for f in gamecontrollerdb.txt LICENSE; do
    [[ -f $cache/gcdb-$gcdb_rev-$f ]] || curl -sSfL -o "$cache/gcdb-$gcdb_rev-$f" \
        "https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/$gcdb_rev/$f"
done
cp "$cache/gcdb-$gcdb_rev-gamecontrollerdb.txt" "$kit/linux/gamecontrollerdb.txt"
cp "$cache/gcdb-$gcdb_rev-LICENSE" "$kit/licenses/SDL_GameControllerDB_LICENSE.txt"

# licenses of the shipped parts
cp "${UG_SDL3_SRC:-$HOME/ttt2-deps/sdl3/src}/LICENSE.txt" "$kit/licenses/SDL3_LICENSE.txt"
cp "$root/third_party/vulkan/LICENSE-Vulkan-Headers.md" "$root/third_party/vulkan/LICENSE-volk.md" "$kit/licenses/"
# xxHash (BSD-2-Clause): the license is the header comment of runtime/src/xxhash.h
awk '{print} /\*\//{exit}' "$root/runtime/src/xxhash.h" | tr -d '\r' > "$kit/licenses/xxhash_LICENSE.txt"
cat > "$kit/licenses/README.txt" <<'EOF'
Third-party parts of the Linux package:
- SDL3 3.4 (zlib license, https://libsdl.org): lib/libSDL3.so.0
- volk and Vulkan-Headers (MIT / Apache 2.0, https://github.com/zeux/volk, https://github.com/KhronosGroup/Vulkan-Headers):
  in libugrt.a
- xxHash (BSD 2-clause, https://github.com/Cyan4973/xxHash): in libugrt.a
- SDL_GameControllerDB (zlib license, https://github.com/mdqinc/SDL_GameControllerDB): gamecontrollerdb.txt
- capstone 5.0.7 (BSD, https://www.capstone-engine.org): pylib/capstone
EOF

# nothing from the disc
bad=$(cd "$stage" && find . -type f \( -iname '*.xbe' -o -iname '*.iso' -o -iname '*.rar' -o -iname '*.bik' -o -iname '*.prx' \
      -o -iname '*.xbx' -o -iname '*.pdb' -o -path '*/lifted/*' -o -path './kit/game/*' \) | head)
[[ -z $bad ]] || { echo "FORBIDDEN files staged: $bad" >&2; exit 1; }

tar -C "$work" -czf "$out/$name.tar.gz" "$name"
echo "package: $out/$name.tar.gz ($(du -h "$out/$name.tar.gz" | cut -f1))"
