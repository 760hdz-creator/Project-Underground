# Project Underground: STATE

Static recomp of Tony Hawk's Underground (Xbox OG, USA XISO) to a native PC build.
Sibling of Project Gauntlet (G:\Project Gauntlet): reuse its x86->C lifter and NV2A push-buffer translation.

## Decisions (2026-11-22)
- Source: `Tony Hawk's Underground (USA).xiso.iso` (Xbox). Not committed.
- Renderer: native Vulkan on Windows first (no D3D11, no DXVK). Linux port later on the same Vulkan backend.
- Toolchain: llvm-mingw (clang) + portable CMake + Ninja, fetched by tools/bootstrap. No Visual Studio, no Windows SDK, no Vulkan SDK.
- Vulkan: vendored Vulkan-Headers + volk; glslang bundled for runtime NV2A shader -> SPIR-V.
- Exe statically links the C/C++ runtime: players need only a Vulkan 1.1 driver.
- PS2 HD Remaster .rar = HD texture pack. Ignore until the end (texture replacement, like Gauntlet).

## Plan
1. Bootstrap toolchain + repos (GitLab + forge).
2. Extract XISO, identify XDK version, find default.xbe, check for PDB/symbols.
3. Port Gauntlet lifter; make generated C clang-clean.
4. Kernel/XDK HLE runtime.
5. Vulkan NV2A backend.
6. Boot to menu, then gameplay.
7. Linux port.
8. HD textures from PS2 pack.

## Status
- Folder + git initialized. Nothing else yet.
