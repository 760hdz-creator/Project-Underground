# Project Underground installer, launcher and updater (build on the player's PC)

Nothing from Activision/Neversoft ships. `ProjectUnderground-Setup-<ver>.exe` holds only our build kit and licensed
third-party parts; the game is lifted and compiled on the player's PC from their own disc image. Port of Project
Gauntlet's installer (G:\Project Gauntlet\docs\installer.md) without its Visual Studio / admin toolchain step.

## Pieces
- `tools/package.py` -> `dist/payload.zip` (~14 MB): git-tracked files from `SHIP` (lifter + tables, runtime source,
  vendored Vulkan-Headers + volk, ug_build.py, build.py, bootstrap.py, extract_xiso.py, xbedis.py, VERSION), embedded
  CPython 3.15.0 (python.org), capstone 5.0.7 (pinned: the dev lift's version), `licenses/`. Aborts on a `FORBIDDEN`
  match (xbe/iso/rar/bik/prx/xbx/pdb, game/, extracted/, out/, lifted/, toolchain/).
- `tools/ug_build.py --root D [--iso img] [--version V]`: extract -> `D\game`, SHA-256 of `default.xbe`
  (979424bd...), llvm-mingw 20261104 (pinned SHA-256, GitHub release) -> `D\toolchain` (~190 MB, first install only,
  no admin), lift with the committed seeds -> `D\build\lifted` (byte-identical to the dev lift), compile + link with
  tools/build.py's flags (imported, single copy) -> `D\underground.exe` (`-mwindows`, `-DUG_VERSION`). ~1 minute.
- `installer/setup.cpp`, built by `python -I installer\build.py` (runs package.py, windres, clang++ `-static
  -municode -mwindows`) -> `dist\ProjectUnderground-Setup-<VERSION>.exe` (~16 MB, imports system DLLs only).
  Unpacks the payload, runs `python\python.exe -I tools\ug_build.py`, progress from `## ` lines, errors from
  `ERROR: `, log `D\build\setup.log`. Then `launcher.exe` (copy of the setup), Desktop + Start menu shortcuts
  (`launcher.exe --launcher`), `HKCU\Software\ProjectUnderground` and the HKCU uninstall entry. Default folder
  `%LOCALAPPDATA%\Project Underground`.
  - Command line: `--update [--dir X]`, `--uninstall [--dir X]` (keeps `out\` = saves and log), `--iso <img> [--dir X]`,
    `--launcher [--dir X]`.
  - Art: neutral placeholders from `installer/art/*.py` (never game art; user art welcome).
- Launcher: Play (UG_NOUPDATE=1), update check, settings to `settings.ini [settings]` (UG_FULLSCREEN, UG_FPS=30 when
  60 fps is off, UG_VSYNC=0 when VSync is off, UG_MONITOR, UG_SCALE, UG_PADS; a default deletes the key; the game's
  main.c load_settings applies keys not already in the environment), Logs, Uninstall.
- Game updater (`runtime/src/updater.c`): installed builds GET forge `8gramlabs/Project-Underground` releases/latest;
  a newer tag asks, downloads `ProjectUnderground-Setup-*.exe`, runs `--update --dir <install>`. Off for dev builds,
  UG_INPUT runs, UG_NOUPDATE=1. Test overrides UG_UPDATE_REPO / UG_UPDATE_ASSET / UG_UPDATE_API.

## Linux
- Runtime on Linux: runtime/src/linux (win32.c Win32 subset on POSIX, sdl_plat.c window/keyboard/monitors on
  SDL3, pad_sdl.c gamepads; include/windows.h builds on DXVK Native's include/native/windows headers, build time
  only). gpu_vulkan.c takes its instance extensions and surface from SDL. volk dlopens libvulkan.so.1.
- `tools/build_linux.sh <dir> [--version V] [--lifted <dir>]`: libugrt.a + lib/libSDL3.so.0 (+ the game).
  Deps (built once by the TTT2 Linux port): ~/ttt2-deps/sdl3/install (UG_SDL3), DXVK Native source (UG_WINHDR).
- `tools/package_linux.sh` (WSL, from a clean clone so files are LF) -> dist/ProjectUnderground-Linux-<ver>.tar.gz
  (~4 MB): setup.sh, README-Linux.txt, kit/ (SHIP files, tools/pyrun.py, pylib/capstone 5.0.7 manylinux wheel,
  linux/libugrt.a + lib/ + include/ + gamecontrollerdb.txt + icon.png, licenses). FORBIDDEN check as on Windows.
- `ug_build.py` on Linux: lift through pyrun.py (byte-identical to the Windows lift apart from CRLF), compile with
  build.py's LIFT_FLAGS using the system clang/gcc, link libugrt.a + lib/*.so, rpath $ORIGIN/lib -> <root>/underground.
- `setup.sh --iso <img> [--dir D] [--play] [--no-gui]`, `--update --dir D`, `--uninstall --dir D` (keeps out/ and
  settings.ini). Default D = ~/.local/share/project-underground. Menu entry + icon; underground.sh sets UG_INSTALLED=1.
  Needs glibc >= 2.38, python3 >= 3.9, clang or gcc, libvulkan.so.1 (setup.sh checks each).
- Updater (updater.c Linux part): curl GETs forge releases/latest, asset ProjectUnderground-Linux*.tar.gz, zenity or
  kdialog asks, download + extract to /tmp, `setup.sh --update --dir <cwd> --play`, exit.
- Verified 2026-11-23 in WSL Ubuntu 24.04 (llvmpipe): install from the ISO in 72 s, launch, --update, --uninstall,
  and the updater loop against a local release JSON (UG_UPDATE_API, fake zenity): prompt, download, rebuild, relaunch.
  Scripted route reaches NJ gameplay. Not tested: a real GPU on Linux, real controllers, Wayland desktop sessions.

## Release steps (only when the user says push)
1. Bump `VERSION`; `git add` new tracked files (package.py stages from `git ls-files`).
2. `python -I installer\build.py`.
3. Test `dist\ProjectUnderground-Setup-<ver>.exe --iso <image> --dir <empty folder>`.
4. Tag `v<ver>`, push origin + forge, then `tea release create --login forge --repo 8gramlabs/Project-Underground
   --tag v<ver> --title v<ver> --note "<notes>" --asset dist/ProjectUnderground-Setup-<ver>.exe` and
   `glab release create v<ver> dist/ProjectUnderground-Setup-<ver>.exe`.
   Linux: `bash tools/package_linux.sh` in WSL from a fresh clone, attach dist/ProjectUnderground-Linux-<ver>.tar.gz
   to both releases too. Release gate: Windows + Linux + Android together.

## Verified (2026-11-23, dev box; toolchain junctioned, so the llvm-mingw download itself was not re-run)
- `--iso` into an empty folder (~53 s): all steps, launcher, shortcuts, registry. Installed game run from another
  cwd: first-boot quick reboot, relaunch, intro movies.
- `--update --dir` (~70 s): rebuild without extraction, launcher rewritten.
- `--uninstall --dir`: kit, game data, registry, shortcuts removed; `out\` kept; launcher deletes itself.
- Update check: "no release found" on the own repo; Gauntlet's repo as override -> "v0.2.1 available".

## Open
- Launcher clicks by hand (Play, Logs, settings writes, Update download), real llvm-mingw download, clean PC /
  Windows Sandbox. Setup and exe unsigned (SmartScreen may warn). Setup window opens at CW_USEDEFAULT.
