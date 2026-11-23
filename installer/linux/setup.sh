#!/bin/sh
# Project Underground for Linux: install, update or uninstall (the Linux counterpart of ProjectUnderground-Setup.exe).
# Nothing from Activision ships: the game is translated and compiled on this PC from your own disc image.
#
#   ./setup.sh [--iso <image.iso>] [--dir <folder>] [--play]
#   ./setup.sh --update --dir <folder> [--play]     rebuild with this package's kit (no disc needed)
#   ./setup.sh --uninstall [--dir <folder>]         remove the game (keeps <folder>/out: saves, settings, log)
#   ./setup.sh --android [--dir <folder>]           build the Android APK from the installed game (downloads the
#                                                   Android NDK once, about 700 MB) -> <folder>/ProjectUnderground-*-Android.apk
#
# Default folder: ${XDG_DATA_HOME:-~/.local/share}/project-underground. Needs glibc 2.38+, python3 (3.9+), clang or gcc, and a
# Vulkan driver (libvulkan.so.1). With zenity installed and no terminal, it asks with dialogs and shows progress.
set -eu
here=$(cd -- "$(dirname -- "$0")" && pwd)
version=$(cat "$here/kit/VERSION" 2>/dev/null || echo 0.0.0)
mode=install iso="" dir="" play=0 nogui=0
while [ $# -gt 0 ]; do
    case $1 in
    --iso) iso=$2; shift 2 ;;
    --dir) dir=$2; shift 2 ;;
    --update) mode=update; shift ;;
    --uninstall) mode=uninstall; shift ;;
    --play) play=1; shift ;;
    --no-gui) nogui=1; shift ;;
    --android) mode=android; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done
data=${XDG_DATA_HOME:-$HOME/.local/share}
dir=${dir:-$data/project-underground}
gui=0
if [ $nogui = 0 ] && [ ! -t 0 ] && [ ! -t 1 ] && command -v zenity >/dev/null 2>&1 && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then gui=1; fi
title="Project Underground $version"

say() { if [ $gui = 1 ]; then zenity --info --title "$title" --width 420 --text "$1" 2>/dev/null || true; else echo "$1"; fi; }
die() {
    if [ $gui = 1 ]; then zenity --error --title "$title" --width 480 --text "$1" 2>/dev/null || true; fi
    echo "setup: $1" >&2
    exit 1
}

desktop_entry() {   # .desktop file + icon (the menu entry starts underground.sh)
    apps=$data/applications icons=$data/icons/hicolor/256x256/apps
    mkdir -p "$apps" "$icons"
    cp "$here/kit/linux/icon.png" "$icons/project-underground.png"
    cat > "$apps/project-underground.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Project Underground
Comment=Tony Hawk's Underground (recompiled)
Exec="$dir/underground.sh"
Path=$dir
Icon=project-underground
Terminal=false
Categories=Game;
Actions=uninstall;

[Desktop Action uninstall]
Name=Uninstall
Exec="$dir/kit/setup.sh" --uninstall --dir "$dir"
EOF
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" 2>/dev/null || true
}

if [ $mode = android ]; then
    [ -f "$dir/kit/tools/ug_android.py" ] || die "No Project Underground install in $dir (install it first)."
    if [ $gui = 1 ]; then
        zenity --question --title "$title" --width 480 --text "Build Project Underground for Android phones (arm64, Android 13+, Vulkan 1.3)?\n\nYour installed game is compiled for Android on this PC and packed with your game files into one APK (about 3.5 GB; the phone needs about 7 GB free). The first build downloads Google's Android NDK (about 700 MB; Android SDK license: developer.android.com/studio/terms)." 2>/dev/null || exit 0
    else
        echo "Building the Android APK. The first build downloads Google's Android NDK (about 700 MB; Android SDK license: https://developer.android.com/studio/terms)."
    fi
    mkdir -p "$dir/build"
    python3 -I "$dir/kit/tools/ug_android.py" --root "$dir" --accept-ndk-license 2>&1 | tee "$dir/build/android.log" | grep --line-buffered -E "^(## |ERROR|APK: )" || true
    apk=$(sed -n "s/^APK: //p" "$dir/build/android.log" | tail -1)
    [ -n "$apk" ] && [ -f "$apk" ] || die "The Android build failed. $(grep -m1 -A8 "^ERROR: " "$dir/build/android.log" | cut -c1-300)"
    say "Android APK ready: $apk\nCopy it to your phone and open it there to install."
    exit 0
fi
if [ $mode = uninstall ]; then
    [ -f "$dir/underground.sh" ] || die "No Project Underground install in $dir"
    if [ $gui = 1 ]; then
        zenity --question --title "$title" --width 420 --text "Remove Project Underground from $dir?\n\nSaves, settings and the log in $dir/out are kept." 2>/dev/null || exit 0
    fi
    entry=$data/applications/project-underground.desktop
    if [ -f "$entry" ] && grep -qF "Path=$dir" "$entry"; then   # only the menu entry that belongs to this folder
        rm -f "$entry" "$data/icons/hicolor/256x256/apps/project-underground.png"
    fi
    rm -rf "$dir/kit" "$dir/game" "$dir/build" "$dir/lib" "$dir/android/ndk" "$dir/underground" "$dir/underground.sh" "$dir/README-Linux.txt" \
        "$dir/gamecontrollerdb.txt"
    rmdir "$dir" 2>/dev/null || true
    say "Project Underground was removed. Your saves are still in $dir/out."
    exit 0
fi

# ---- checks ----
command -v python3 >/dev/null 2>&1 || die "python3 is missing. Install it with your package manager (for example: sudo apt install python3)."
python3 -c 'import sys; sys.exit(sys.version_info < (3, 9))' || die "python3 3.9 or newer is needed."
# the prebuilt libraries (SDL3, the runtime) need glibc 2.38+
glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')
case $glibc in
2.*) [ "${glibc#2.}" -ge 38 ] 2>/dev/null || die "This Linux is too old for Project Underground: it has glibc $glibc, 2.38 or newer is needed (Ubuntu 24.04, Debian 13, Fedora 39, Linux Mint 22, Arch, SteamOS 3.6 or newer)." ;;
*) [ -n "$glibc" ] || die "Project Underground needs a glibc-based Linux (glibc 2.38 or newer)." ;;
esac
cc_ok=0
for c in ${CC:-} clang gcc cc; do command -v "$c" >/dev/null 2>&1 && cc_ok=1 && break; done
[ $cc_ok = 1 ] || die "No C compiler found. Install clang (for example: sudo apt install clang, sudo dnf install clang, sudo pacman -S clang)."
if ! { ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; } && ! ls /usr/lib*/libvulkan.so.1 /usr/lib/*/libvulkan.so.1 >/dev/null 2>&1; then
    die "The Vulkan loader (libvulkan.so.1) is missing. Install your GPU's Vulkan driver (for example: sudo apt install libvulkan1 mesa-vulkan-drivers)."
fi

if [ $mode = install ] && [ -z "$iso" ] && [ -f "$dir/game/default.xbe" ]; then
    # an existing install: update it (keeps the extracted disc) unless a new image is given
    mode=update
fi
if [ $mode = install ] && [ -z "$iso" ]; then
    if [ $gui = 1 ]; then
        iso=$(zenity --file-selection --title "Select your Tony Hawk's Underground (Xbox, USA) disc image" \
              --file-filter "Disc images | *.iso *.ISO *.xiso" 2>/dev/null) || exit 0
    else
        die "Give your disc image: ./setup.sh --iso <Tony Hawk's Underground (USA).iso> [--dir <folder>]"
    fi
fi
[ -z "$iso" ] || [ -f "$iso" ] || die "Disc image not found: $iso"
if [ $mode = update ] && [ ! -f "$dir/game/default.xbe" ]; then die "No installed game data in $dir: run a full install with --iso."; fi
[ -z "$iso" ] || iso=$(cd -- "$(dirname -- "$iso")" && pwd)/$(basename -- "$iso")

# ---- kit + build ----
mkdir -p "$dir"
dir=$(cd -- "$dir" && pwd)
if [ "$here" != "$dir/kit" ]; then
    rm -rf "$dir/kit.new"
    cp -R "$here/kit" "$dir/kit.new"
    cp "$here/setup.sh" "$dir/kit.new/setup.sh"
    rm -rf "$dir/kit"
    mv "$dir/kit.new" "$dir/kit"
    cp "$here/README-Linux.txt" "$dir/" 2>/dev/null || true
fi
mkdir -p "$dir/build" "$dir/out"
log=$dir/build/setup.log
set -- python3 -I "$dir/kit/tools/ug_build.py" --root "$dir" --version "$version"
[ -z "$iso" ] || set -- "$@" --iso "$iso"
status=0
if [ $gui = 1 ]; then
    # the build writes the log on its own; the dialog only follows it (closing the dialog never stops the build)
    : > "$log"
    "$@" > "$log" 2>&1 < /dev/null &
    pid=$!
    ( tail -n +1 -f --pid=$pid "$log" 2>/dev/null | { p=0; while IFS= read -r line; do
        case $line in
        "## Done"*) echo 100 ;;
        "## "*) [ $p -lt 40 ] && p=$((p + 6)); echo $p; echo "# ${line#\#\# }" ;;
        "compiled "*) n=${line##*(}; n=${n%)}; echo $(( 40 + 55 * ${n%/*} / ${n#*/} )) ;;
        esac
      done; } ) | zenity --progress --auto-close --no-cancel --title "$title" --width 460 --text "Starting" 2>/dev/null || true
    wait $pid || true
    grep -q "^## Done" "$log" || status=1
else
    "$@" 2>&1 | tee "$log" | grep --line-buffered -E "^(## |ERROR|compiled .*0/)" || true
    grep -q "^## Done" "$log" || status=1
fi
if [ $status != 0 ]; then
    err=$(grep -m1 -A12 "^ERROR: " "$log" | cut -c1-300 || true)
    die "The build failed. ${err:-See $log}"
fi

# ---- launcher, settings, menu entry ----
cat > "$dir/underground.sh" <<'EOF'
#!/bin/sh
# Starts Project Underground from its folder; the log goes to out/underground.log. Settings: settings.ini.
cd -- "$(dirname -- "$0")" || exit 1
mkdir -p out
export UG_INSTALLED=1
exec ./underground "$@" 2>out/underground.log
EOF
chmod +x "$dir/underground.sh" "$dir/kit/setup.sh"
if [ ! -f "$dir/settings.ini" ]; then
    cat > "$dir/settings.ini" <<'EOF'
; Project Underground settings (same keys as the Windows launcher). Remove the ; to use a line.
[settings]
; UG_FULLSCREEN=1        ; start in borderless fullscreen (F11 or Alt+Enter toggles)
; UG_FPS=30              ; frame rate cap (default: 60)
; UG_VSYNC=0             ; no vsync
; UG_SCALE=4             ; render scale 1..8 (default: 2)
; UG_MONITOR=0           ; display number
; UG_PADS=2              ; gamepads in the game (1..4)
; UG_NOUPDATE=1          ; no update check at start
EOF
fi
desktop_entry
if [ $play = 1 ]; then
    nohup "$dir/underground.sh" >/dev/null 2>&1 &
elif [ $mode = install ]; then
    say "Project Underground is installed in $dir.\nStart it from your applications menu or with $dir/underground.sh"
else
    say "Project Underground was updated to $version."
fi
