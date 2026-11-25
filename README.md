<div align="center">

<img src="docs/images/icon.png" width="128" alt="Project Underground icon">

# Project Underground

**Tony Hawk's Underground (Xbox) rebuilt as a native game for Windows, Linux and Android.**

[![Windows](https://img.shields.io/badge/Windows-playable-0078D6?logo=windows&logoColor=white)](#status)
[![Linux](https://img.shields.io/badge/Linux-playable-FCC624?logo=linux&logoColor=black)](#status)
[![Android](https://img.shields.io/badge/Android-in_testing-3DDC84?logo=android&logoColor=white)](#status)
[![Vulkan](https://img.shields.io/badge/renderer-Vulkan-A41E22?logo=vulkan&logoColor=white)](#how-it-works)
[![60 fps](https://img.shields.io/badge/60_fps-default-success)](#features)
[![Release](https://img.shields.io/badge/release-Windows_%2B_Linux-success)](#download)

A static recompilation by 8gram Labs: the original x86 game code is translated to C and compiled natively for
64-bit Windows, Linux and Android phones, with its own Vulkan renderer, higher resolutions, 60 fps and a launcher.

![Project Underground](docs/images/banner.jpg)

</div>

> No game code or data ships with this project. The installer builds the game **on your PC from your own
> Tony Hawk's Underground (Xbox, USA) disc image**.

## Download

| | File |
|---|---|
| **Windows 10/11** (64-bit, Vulkan 1.3 GPU) | `ProjectUnderground-Setup-<version>.exe` |
| **Linux** (x86-64, glibc 2.38+, Vulkan 1.3 GPU) | `ProjectUnderground-Linux-<version>.tar.gz` |

Get them from the releases on [git.8gramlabs.com](https://git.8gramlabs.com/8gramlabs/Project-Underground/releases) or
[GitLab](https://gitlab.com/octagram-labs-group/Project-Underground/-/releases). You need your own disc image. The launcher
(Windows) and the game (Linux) offer new versions.

> [!WARNING]
> **Antivirus and "Windows protected your PC" warnings.** `ProjectUnderground-Setup.exe` is not code-signed (signing
> certificates cost money), so Windows SmartScreen and some antivirus programs may warn about it or flag it on heuristics. It
> also builds a brand-new `underground.exe` on your PC that no antivirus has seen before, which some engines dislike. These are
> false positives: nothing in it is malicious, and all of the source code is in this repository, so you can
> [build it yourself](#building-from-source-developers).
>
> - **SmartScreen ("Windows protected your PC"):** click **More info**, then **Run anyway**.
> - **Your antivirus quarantines `underground.exe` after the build:** restore it and allow the install folder, then run the
>   setup again and choose the update. Don't turn your antivirus off completely.
> - Only download from the official [Forge](https://git.8gramlabs.com/8gramlabs/Project-Underground/releases) or
>   [GitLab](https://gitlab.com/octagram-labs-group/Project-Underground/-/releases) release pages.

> [!NOTE]
> **Online play is being worked on.** For now only local two-player split screen is available.

> [!NOTE]
> **Android is in testing.** The game runs on a first phone, but the APK that the launcher builds has not been tested on one
> yet.

<div align="center">

![New Jersey](docs/images/gameplay_nj.jpg)

![Nosegrind](docs/images/gameplay_grind.jpg)

![Wall ride in Moscow](docs/images/gameplay_wall.jpg)

</div>

## Features

- **Native builds, no emulator.** Every function of `default.xbe` is lifted to C and compiled for the machine it
  runs on: 64-bit Windows, Linux (x86-64) and Android (arm64).
- **Own Vulkan renderer.** The Xbox GPU's (NV2A) push buffer goes straight to Vulkan on every platform: vertex
  programs and register combiners run as shader interpreters, with shadow maps, lighting, fog and texture
  generation. No Direct3D, no DXVK.
- **High resolution and supersampling.** The game renders at a multiple of the Xbox's 480 lines: 2x (960 lines) by
  default, up to 8x, scaled down cleanly to the window. Changes live in the in-game menu.
- **Optional graphics.** Anti-aliasing and ambient occlusion (the HUD stays sharp), anisotropic texture filtering,
  and a maximum draw distance that stops people, cars and props from vanishing.
- **60 fps.** Frames are paced at a steady 60 fps (the launcher can switch to 30), with VSync on or off. The game
  steps by real time, so it plays at the same speed at 30 fps or when a PC cannot hold 60.
- **Widescreen.** The game's own 16:9 mode is on by default (the Xbox widescreen setting): a wider view of the
  level. The original 4:3 is one click away.
- **Window settings.** Windowed or borderless fullscreen (F11 or Alt+Enter), any monitor, window size presets,
  keep-aspect or stretch, and sharp output at any Windows display scaling.
- **Launcher.** Play, display settings (fullscreen, monitor, window size, widescreen, picture fit, supersampling,
  graphics options, 60 fps, VSync, players), updates, logs, the Android build and uninstall, all on one screen.
  The setup and launcher windows can be resized from any edge or corner.
- **Builds on your PC, no Visual Studio.** The setup fetches its own compiler (llvm-mingw, about 190 MB, once) and
  builds the game in about a minute. No admin rights needed.
- **Linux.** The same renderer on Vulkan, SDL3 for the window, sound and controllers, and a `setup.sh` installer
  with menu entry, updates and uninstall.
- **Android.** Your PC turns your installed game into an APK for your phone (arm64, Android 13+, Vulkan 1.3), with
  on-screen touch controls.
- **Two-player split screen**, local. Online play is being worked on.
- **Almost any controller.** Xbox, PlayStation 3/4/5, Switch Pro and Joy-Con, Steam Controller and Steam Deck,
  8BitDo and thousands more through SDL3 and the community controller database, on Windows, Linux and Android. Up
  to 2 players, plus the keyboard. See [Controls](#controls).
- **HD texture packs (optional).** It loads PCSX2-format replacement packs made for the PS2 version, such as the
  community *HD Remaster* pack. Palettized textures are matched by their exact PS2 hashes; compressed (DXT) ones by
  picture (see [HD textures](#hd-textures)). F10 in the game switches them on and off.

## Maps

Every skating level runs. Pictures: 3x supersampling, anti-aliasing, ambient occlusion and maximum draw distance
on, original textures. Bonus levels are shown by the game's own short codes.

|  |  |  |
|---|---|---|
| ![New Jersey](docs/images/maps/nj.jpg)<br>New Jersey | ![Manhattan](docs/images/maps/ny.jpg)<br>Manhattan | ![Tampa](docs/images/maps/fl.jpg)<br>Tampa |
| ![San Diego](docs/images/maps/sd.jpg)<br>San Diego | ![Hawaii](docs/images/maps/hi.jpg)<br>Hawaii | ![Vancouver](docs/images/maps/vc.jpg)<br>Vancouver |
| ![Slam City Jam](docs/images/maps/sj.jpg)<br>Slam City Jam | ![Moscow](docs/images/maps/ru.jpg)<br>Moscow | ![HN](docs/images/maps/hn.jpg)<br>HN |
| ![PH](docs/images/maps/ph.jpg)<br>PH | ![SE](docs/images/maps/se.jpg)<br>SE | ![SC](docs/images/maps/sc.jpg)<br>SC |
| ![SC2](docs/images/maps/sc2.jpg)<br>SC2 | ![DJ](docs/images/maps/dj.jpg)<br>DJ | ![VN](docs/images/maps/vn.jpg)<br>VN |

Two players, split screen:

![Split screen](docs/images/split_screen.jpg)

## Android

![New Jersey on a phone](docs/images/android_nj.jpg)

![Moscow on a phone](docs/images/android_ru.jpg)

![Hawaii on a phone](docs/images/android_hi.jpg)

A Galaxy S26 Ultra, 3x supersampling, with the on-screen controls (they hide when a controller is connected). The orange
button opens the in-game menu: tap a row to change it, tap outside to close it.

## HD textures

**Where to get a pack:** the *HD Remaster* pack is in the Internet Archive's
[PCSX2 HD texture packs](https://archive.org/download/pcsx2-hd-texture-packs) collection. Direct download (573 MB):
[Tony Hawk's Underground HD Remaster.rar](https://dn721905.ca.archive.org/0/items/pcsx2-hd-texture-packs/Tony%20Hawk%27s%20Underground%20%28USA%29%20%5BSLUS-20731%5D%20HD%20Remaster.rar).
It is made by the community, not by this project. The launcher has the same link: **Settings**, **GET IT**.

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
| Renderer: textures, combiners, fog, lighting, skater shadows, texgen, depth clamp, polygon offset | Working |
| Widescreen 16:9 (the game's own mode), window size, monitor, fullscreen | Working |
| Other levels | All 15 load and play in free skate; story goals beyond New Jersey are not checked |
| Installer, launcher, updates, uninstall (Windows) | Install, update and uninstall tested |
| Linux | Boots to New Jersey gameplay; install, update, uninstall and the updater tested (Ubuntu 24.04, WSL) |
| Android | APK builds and signs on Windows and Linux; one phone (Galaxy S26 Ultra): story start, free skate, saving and loading, touch controls, in-game menu. Not yet tested there: the player-built APK, controllers, long sessions |
| HD texture packs (PS2 PCSX2 packs, matched by picture) | Working (optional; F10 toggles) |

Known gaps:
- The story has been played only through its first level; the other levels were checked in free skate.
- Online play is not available yet (local two-player split screen only).
- Two-player split screen draws no skater shadows.
- Not yet tested on a PC without any developer tools, on a real Linux GPU, or with real controllers on Linux.

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
