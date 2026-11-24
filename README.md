<div align="center">

<img src="docs/images/icon.png" width="128" alt="Project Underground icon">

# Project Underground

**Tony Hawk's Underground (Xbox) rebuilt as a native game for Windows, Linux and Android.**

[![Windows](https://img.shields.io/badge/Windows-playable-0078D6?logo=windows&logoColor=white)](#status)
[![Linux](https://img.shields.io/badge/Linux-playable-FCC624?logo=linux&logoColor=black)](#status)
[![Android](https://img.shields.io/badge/Android-in_testing-3DDC84?logo=android&logoColor=white)](#status)
[![Vulkan](https://img.shields.io/badge/renderer-Vulkan-A41E22?logo=vulkan&logoColor=white)](#how-it-works)
[![60 fps](https://img.shields.io/badge/60_fps-default-success)](#features)
[![Release](https://img.shields.io/badge/first_release-coming_soon-lightgrey)](#status)

A static recompilation by 8gram Labs: the original x86 game code is translated to C and compiled natively for
64-bit Windows, Linux and Android phones, with its own Vulkan renderer, higher resolutions, 60 fps and a launcher.

![Project Underground](docs/images/banner.jpg)

</div>

> No game code or data ships with this project. The installer builds the game **on your PC from your own
> Tony Hawk's Underground (Xbox, USA) disc image**.

> [!IMPORTANT]
> **No download yet.** The first release ships when the Windows, Linux and Android builds are all ready
> together. Windows and Linux are playable today. The Android build still has to be tested on a real phone.
> Developers can [build it from source](#building-from-source-developers) now.

<div align="center">

![New Jersey](docs/images/gameplay_nj.jpg)

![Grinding in New Jersey](docs/images/gameplay_grind.jpg)

</div>

## Features

- **Native builds, no emulator.** Every function of `default.xbe` is lifted to C and compiled for the machine it
  runs on: 64-bit Windows, Linux (x86-64) and Android (arm64).
- **Own Vulkan renderer.** The Xbox GPU's (NV2A) push buffer goes straight to Vulkan on every platform: vertex
  programs and register combiners run as shader interpreters, with shadow maps, fog and texture generation. No
  Direct3D, no DXVK.
- **High resolution.** The game renders at a multiple of the Xbox's 480 lines: 2x (960 lines) by default, up to 6x.
- **60 fps.** Frames are paced at a steady 60 fps (the launcher can switch to 30), with VSync on or off. The game
  times its logic from real clocks, so it runs at the right speed.
- **Widescreen.** The game's own 16:9 mode is on by default (the Xbox widescreen setting): a wider view of the
  level. The original 4:3 is one click away.
- **Window settings.** Windowed or borderless fullscreen (F11 or Alt+Enter), any monitor, window size presets,
  keep-aspect or stretch, and sharp output at any Windows display scaling.
- **Launcher.** Play, display settings (fullscreen, monitor, window size, widescreen, picture fit, render scale,
  60 fps, VSync, players), updates, logs, the Android build and uninstall, all on one screen.
- **Builds on your PC, no Visual Studio.** The setup fetches its own compiler (llvm-mingw, about 190 MB, once) and
  builds the game in about a minute. No admin rights needed.
- **Linux.** The same renderer on Vulkan, SDL3 for the window, sound and controllers, and a `setup.sh` installer
  with menu entry, updates and uninstall.
- **Android.** Your PC turns your installed game into an APK for your phone (arm64, Android 13+, Vulkan 1.3), with
  on-screen touch controls.
- **Almost any controller.** Xbox, PlayStation 3/4/5, Switch Pro and Joy-Con, Steam Controller and Steam Deck,
  8BitDo and thousands more through SDL3 and the community controller database, on Windows, Linux and Android. Up
  to 4 players, plus the keyboard. See [Controls](#controls).
- **HD texture packs (optional).** It loads PCSX2-format replacement packs made for the PS2 version, such as the
  community *HD Remaster* pack. Palettized textures are matched by their exact PS2 hashes; compressed (DXT) ones by
  picture (see [HD textures](#hd-textures)). F10 in the game switches them on and off.

## HD textures

![Original and HD texture pack, same frame](docs/images/hd_compare.jpg)

Optional. Unpack a PCSX2 replacement pack for the PS2 version (*Tony Hawk's Underground (USA) [SLUS-20731]*), then
pick its folder, for example `...\SLUS-20731\replacements`, in the launcher (**Settings**, **HD texture pack**).
Linux: `./setup.sh --hd <folder>` or `UG_HDTEX=<folder>` in `settings.ini`. The pack is used where it is and is not
copied. **F10** in the game switches it on and off, so you can compare.

How it works: the pack names its files by PS2 texture hashes. The Xbox version keeps two kinds of texture:
- **Palettized (8-bit)** ones have the same indices and palette as on the PS2, so the PS2 hashes are rebuilt exactly
  (as in Project Gauntlet). In New Jersey 79 of 83 find their file.
- **Compressed (DXT)** ones, most of the level art, were re-encoded for the Xbox, so their PS2 hashes can't be
  rebuilt. Each is shrunk to a 16-pixel thumbnail and compared with thumbnails of the whole pack; a pack texture is
  used only when it is clearly the same picture, and flat or ambiguous textures keep the original. In New Jersey
  about 600 of 870 detailed textures find their HD version.

The pack's textures have up to 4x the resolution: road surfaces, grass, wood and walls are visibly sharper up close.
Anisotropic filtering (16x, `UG_ANISO`) keeps that detail on surfaces seen at an angle.

## The installer and launcher

| Setup | Launcher | Launcher settings |
|---|---|---|
| ![Setup](docs/images/installer.jpg) | ![Launcher](docs/images/launcher.jpg) | ![Launcher settings](docs/images/launcher_settings.jpg) |

The skateboards are the buttons. Setup: choose your disc image and install folder, then **Install**. Updates
rebuild the game in place and keep your saves and settings.

## Status

| Area | State |
|---|---|
| Boot, Activision/Neversoft/intro movies (Bink), title menus | Working |
| Story mode: intro cutscene, Create-a-Skater, New Jersey level | Working, skater controllable, HUD, 60 fps |
| Saves, loading, cutscene streaming | Working |
| Sound, music, movies | Working (DirectSound mixed in software) |
| Renderer: textures, combiners, fog, shadow maps, texgen, depth clamp, polygon offset | Working |
| Widescreen 16:9 (the game's own mode), window size, monitor, fullscreen | Working |
| Other levels | Not checked yet: renderer gaps (FF lighting, cube maps, some texgen modes) may show up |
| Installer, launcher, updates, uninstall (Windows) | Install, update and uninstall tested |
| Linux | Boots to New Jersey gameplay; install, update, uninstall and the updater tested (Ubuntu 24.04, WSL) |
| Android | APK builds and signs on Windows and Linux; not yet run on a phone |
| HD texture packs (PS2 PCSX2 packs, matched by picture) | Working (optional; F10 toggles) |

Known gaps:
- A translucent dark band sometimes crosses the sky (cosmetic).
- Only the first levels have been played so far; later levels may need renderer work.
- Not yet tested on a clean PC, on a real Linux GPU, or with real controllers on Linux.

## Controls

| Xbox pad | Keyboard |
|---|---|
| D-pad | Arrow keys |
| Left stick / right stick | W A S D / I J K L |
| A / B / X / Y | Z / X / C / V |
| Black / White | F / G |
| LT / RT | Q / E |
| START / BACK | Enter / Backspace |
| L3 + R3 (both sticks pressed): in-game menu | Esc or F1: in-game menu |
| | F11 or Alt+Enter: fullscreen, F10: HD textures on/off |

In-game menu: pauses the game; fullscreen, frame rate (30/60), camera direction (right stick: up is up and left is left by default; "Xbox" = the original inverted camera), HD textures on/off, and Quit
(press twice). Changes are saved to the launcher's settings. Once a story or career is going it also has Cheats:
Unlock everything (the game's own unlock: levels, skaters, movies, cheat codes), Always special, Perfect rails,
Perfect manuals, Moon gravity and Perfect skitch.

Name entry (the on-screen keyboard, e.g. save names): just type. While the on-screen keyboard is open, letters, digits,
Backspace and Enter go into the text; the arrow keys still move over the on-screen keys.

Controllers: Xbox, PlayStation, Switch Pro / Joy-Con, Steam, 8BitDo and most other USB and Bluetooth pads work
(buttons go by position, so the bottom face button is A). SDL3 (`SDL3.dll` next to the game on Windows) and the
community [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) (`gamecontrollerdb.txt`) cover a
few thousand pads; a pad that is still unknown gets a generic layout, and your own SDL mapping lines can go in
`controllers.txt` in the game folder (never touched by updates). Set the number of players in the launcher.

## How it works

- `recomp/lift.py` translates the x86 code of `default.xbe` to C: 12,927 functions (96% of `.text`), including
  the SSE and x87 code. A differential oracle compares lifted functions with an emulator run on the original bytes
  (2,228 pass; every failure is explained and none is a lifter bug).
- `runtime/` holds the Xbox kernel, file system, threads and fibers, XInput and DirectSound HLE, and the NV2A
  push-buffer to Vulkan renderer (`runtime/src/gpu_vulkan.c`, shaders in `runtime/shaders/`).
- `installer/` holds the setup and launcher (one Win32 exe built with llvm-mingw), and `tools/ug_build.py` runs the
  on-PC build: extract, check the disc, translate, compile, link.
- Linux and Android use the same runtime through a small Win32 compatibility layer (`runtime/src/linux/`) and SDL3.
  `tools/ug_android.py` builds the APK on the player's PC from a template APK that holds no game code.

See [docs/installer.md](docs/installer.md) for the installer, launcher, Linux and Android details.

## Building from source (developers)

Requirements: Python 3 with `capstone` (`pip install capstone==5.0.7`) and your disc image. No Visual Studio, no Windows SDK, no Vulkan SDK: the compiler
(llvm-mingw) is fetched for you.

```bat
python -I tools\ug_build.py --root <build folder> --iso "<path to your disc image>"
<build folder>\underground.exe
```

This is the same build the installer runs. `python -I installer\build.py` packages
`dist\ProjectUnderground-Setup-<version>.exe`. Linux: `tools/build_linux.sh` and `tools/package_linux.sh`.

## Legal

Tony Hawk's Underground is a trademark of its owners. This project is not affiliated with or endorsed by them. It
contains no game code, data or assets. You need your own copy of the game.
