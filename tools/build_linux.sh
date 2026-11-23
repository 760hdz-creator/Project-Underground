#!/usr/bin/env bash
# Linux build: the runtime library (runtime/src + runtime/src/linux + volk) and, with --lifted, the game.
#
#   tools/build_linux.sh <build dir> [--version X.Y.Z] [--lifted <dir with f_*.c> [--exe <path>]]
#
# <build dir>/libugrt.a   the runtime (prebuilt into the Linux package; players only compile the lifted code)
# <build dir>/lib/        libSDL3.so.0 (window, input, audio): the game's rpath
# <build dir>/include/    recomp/include copy (what the lifted code needs)
#
# Vulkan: volk (third_party/vulkan) loads libvulkan.so.1 at run time; nothing Vulkan is linked.
# Build-time only dependencies (built once by the TTT2 Linux port's tools/build_linux.sh; override if elsewhere):
#   UG_SDL3=<prefix with lib/libSDL3.so.0 and include/SDL3>      default ~/ttt2-deps/sdl3/install
#   UG_WINHDR=<DXVK Native source>: include/native/windows (Win32 base types under linux/include/windows.h)
#                                                                default ~/.cache/ttt2-platform/shared-dxvk/source-*
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${1:?usage: build_linux.sh <build dir> [--version X] [--lifted dir [--exe path]]}
shift
version="" lifted="" exe=""
while [[ $# -gt 0 ]]; do
    case $1 in
    --version) version=$2; shift 2 ;;
    --lifted) lifted=$2; shift 2 ;;
    --exe) exe=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
sdl=${UG_SDL3:-$HOME/ttt2-deps/sdl3/install}
winhdr=${UG_WINHDR:-$(ls -d "$HOME"/.cache/ttt2-platform/shared-dxvk/source-*/ 2>/dev/null | head -1)}
cc=${CC:-clang}
jobs=${UG_JOBS:-$(nproc)}

mkdir -p "$build/rt" "$build/lib" "$build/include"
src=$root/runtime/src
vk=$root/third_party/vulkan
inc=(-I"$src/linux/include" -I"$winhdr/include/native/windows" -I"$sdl/include" -I"$root/recomp/include" -I"$src"
     -I"$vk" -I"$vk/include")
# Missing shim declarations must not compile: an implicit int return truncates a 64-bit pointer.
cflags=(-O2 -g -std=gnu11 -D_GNU_SOURCE -fno-strict-aliasing -fPIC -Wno-everything
        -Werror=implicit-function-declaration -Werror=int-conversion -Werror=incompatible-pointer-types)
[[ -n $version ]] && cflags+=(-DUG_VERSION="\"$version\"")

objs=()
for f in "$src"/*.c "$src"/linux/*.c "$vk/volk.c"; do
    o=$build/rt/$(basename "${f%.c}").o
    objs+=("$o")
    rm -f "$o"
    "$cc" "${cflags[@]}" "${inc[@]}" -c "$f" -o "$o" &
    while [[ $(jobs -r | wc -l) -ge $jobs ]]; do wait -n || true; done
done
wait || true
for o in "${objs[@]}"; do [[ -f $o ]] || { echo "compile failed: $o" >&2; exit 1; }; done
rm -f "$build/libugrt.a"
ar rcs "$build/libugrt.a" "${objs[@]}"
cp -L "$sdl/lib/libSDL3.so.0" "$build/lib/"
cp "$root/recomp/include/"*.h "$build/include/"
echo "runtime: $build/libugrt.a"

if [[ -n $lifted ]]; then
    exe=${exe:-$build/underground}
    mkdir -p "$build/lifted-obj"
    lobjs=()
    # same flags as tools/build.py LIFT_FLAGS: the lifted code must behave as on Windows
    lflags=(-O2 -fno-strict-aliasing -fwrapv -ffp-contract=off -fno-math-errno -w -I"$build/include")
    for f in "$lifted"/*.c; do
        o=$build/lifted-obj/$(basename "${f%.c}").o
        lobjs+=("$o")
        if [[ ! -f $o || $f -nt $o ]]; then
            rm -f "$o"
            "$cc" "${lflags[@]}" -c "$f" -o "$o" &
        fi
        while [[ $(jobs -r | wc -l) -ge $jobs ]]; do wait -n || true; done
    done
    wait || true
    for o in "${lobjs[@]}"; do [[ -f $o ]] || { echo "compile failed: $o" >&2; exit 1; }; done
    "$cc" -o "$exe" "${lobjs[@]}" "$build/libugrt.a" "$build/lib/libSDL3.so.0" -lm -lpthread -ldl \
        -Wl,--disable-new-dtags -Wl,-rpath,'$ORIGIN/lib' -Wl,-rpath,"$build/lib"
    echo "game: $exe"
fi
