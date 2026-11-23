# Project Underground: STATE

Static recomp of Tony Hawk's Underground (Xbox OG, USA) to a native PC build.
Sibling of Project Gauntlet (G:\Project Gauntlet): its x86->C lifter, runtime and DSOUND mixer were ported here.

## Decisions (2026-11-22)
- Source: `Tony Hawk's Underground (USA).xiso.iso` (Xbox). Not committed. `game/` = junction to `extracted/`.
- Renderer: native Vulkan on Windows first (no D3D11, no DXVK). Linux later on the same Vulkan backend.
- Toolchain: llvm-mingw 20261104 (clang, pinned SHA-256) fetched by `tools/bootstrap.py` into `toolchain/`.
  No Visual Studio, no Windows SDK, no Vulkan SDK. Players' installer will fetch it too (~190 MB).
- Vulkan: vendored Vulkan-Headers v1.4.328 + volk (third_party/vulkan).
- Exe links statically (UCRT + kernel32 only): players need only a Vulkan driver.
- PS2 HD Remaster .rar = HD texture pack. Last step.

## Inputs (verified)
- default.xbe: XDK 5659 (XAPILIB D3D8 D3DX8 XGRAPHC DSOUND XBOXKRNL LIBC XNETS), entry 0x177858, thunk 0x207D60,
  image end 0x382480, stack 0x10000, title ID 4156002D, PE path c:\skate5\Build\XBox\Final\Skate5.exe.
- No PDB/map on the disc. Library functions found by byte signatures: `tools/oovpa_scan.py <OOVPADatabase dir> game/default.xbe`
  reads Cxbx-Reloaded XbSymbolDatabase (MIT) .inl files as data (never builds/runs its code; the auto-mode
  classifier blocked building it). DB clone: scratchpad xbsdb/src (re-clone github.com/Cxbx-Reloaded/XbSymbolDatabase).
- Sections: .text, D3D, D3DX, XGRPH, DSOUND, WMADEC, BINK* (Bink video, lifted), XNET, XPP, .rdata, .data, DOLBY, BINKDATA.

## Build / run
- `python -I tools/build.py lift lifted runtime` = merge out/missed_targets.txt into recomp/seeds_extra.txt, lift
  (recomp/lift.py, ~25 s), compile lifted (32 jobs ~15 s), build runtime -> out/underground.exe.
- `python -I tools/build.py oracle` -> out/oracle.dll; `python -I tools/oracle_test.py [--limit N]` (slow: hours for all leaves).
- Run: `./out/underground.exe` from repo root (game/default.xbe). Env: UG_KLOG, UG_DSLOG, UG_HOOKLOG (recomp/hooks.txt),
  UG_DUMP=<va>,<len> (dump at a null call), UG_PADS, UG_NOAUDIO, UG_WAVDUMP.
- Symbolize host crash offsets: `toolchain/.../bin/llvm-symbolizer --obj=out/underground.exe 0x140000000+off`.

## Done
- Lifter (from Gauntlet) with sections/MMIO ranges from the XBE header; added scalar SSE (THUG is /arch:SSE: ~65k
  sites), extra MMX ops, VC7 memcpy jump tables (neg index, sub+jb index, branch-reached jmp), cpuid (Pentium III)
  and a toggleable EFLAGS.ID (Bink's CPU check). 12927 functions, 96.0% .text coverage, 46 unimplemented sites
  (enter, segment movs, fnsave/frstor...).
- Build flags for lifted code: -O2 -fno-strict-aliasing -fwrapv -ffp-contract=off -fno-math-errno.
- Oracle DLL under mingw: fault recovery via RtlCaptureContext + VEH (no __try in mingw C). Partial run: 7 fails so far
  (00011980 mem, 19430/224EF/3DC24 esp off by 0x10000, 48D63, 6A330 mem, 78820 st0 nan vs inf) - not yet examined.
- Runtime (from Gauntlet): memory, loader, fibers, kernel, fs, nv2a. New: kernel2.c (38 exports THUG adds: Io*, mutants,
  perf counter, Mm*, Xc* crypto, Phy* no-link, NtQueryFullAttributesFile in fs.c), XboxKrnlVersion 1.0.5838.1,
  XeImageFileName etc., fake kernel PE header at 0x8001003C (XAPI INIT-section probe).
- HLE (recomp/hle.txt): XInput (5 functions; THUG polls XInputOpen, no XGetDevices) in hle.c; DSOUND C API
  (26 entry points incl. Lock/Unlock for Bink, multiple streams with callbacks) in dsound.c with Gauntlet's mixer.
- BOOT STATUS: XAPI init, saves on T:/U:, qb.prx scripts, sound banks, textures; Activision, Neversoft and intro Bink
  movies play through (no renderer: nothing visible; gpu_null.c). Runs 120 s without a fatal.
  nv2a.c warns: "[pb] INLINE_ARRAY not implemented", "[pb] DMA handle 9 base 0".

## Next
1. Vulkan renderer (runtime/src/gpu_vulkan.c) behind the kelvin.h gpu_* interface; window + swapchain; draw what nv2a.c
   decodes (Gauntlet's gpu_d3d11.c is the reference for state translation). Implement INLINE_ARRAY in nv2a.c.
2. Look at the oracle fails.
3. Then menus/gameplay, installer (llvm-mingw fetched on the player PC), Linux, HD textures.
- Repos: origin gitlab.com/octagram-labs-group/Project-Underground, forge 8gramlabs/Project-Underground (both public).
  Homepage card live (placeholder image landing/img/thug.jpg: replace with a screenshot later). release-sync REPOS updated.
