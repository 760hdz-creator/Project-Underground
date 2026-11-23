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

- 2026-11-22 (later): native Vulkan renderer works. Bink intro, title menus, difficulty select render correctly
  (screens checked with UG_SHOT). See runtime/src/gpu_vulkan.c header comment for the design.
  - Shaders: runtime/shaders/nv2a.vert (NV2A vertex program interpreter + fixed-function composite path) and nv2a.frag
    (register combiner interpreter). `python -I tools/shaders.py` -> runtime/src/shaders_spv.h (committed; needs
    `python -I tools/bootstrap.py --dev` for glslang 16.6.0, user approved the download 2026-11-22).
  - Pre-transformed vertices with rhw = 0 give oPos.w = inf: treated as w = 1 (else nothing draws).
  - XDK 5659 swap does not name the front buffer: flip presents the last render target.
  - Flip pacing: UG_FPS (default 60) via sched_sleep_until in nv2a.c 0x012C. Menus run 60 fps, 205 draws, 30k verts.
  - Texture cache: in-place re-upload for rewritten textures (Bink), eviction after 600 idle frames.
  - Debug env: UG_SHOT=N (out/shot_<frame>.ppm), UG_DRAWLOG=<frame>, UG_DBGCOLOR=1..5, UG_HEADLESS=1,
    UG_PBSTATS=<s>, UG_WAITDUMP=<s>, UG_INPUT="<s>:<BUTTON>[:<dur>],..." (A B X Y START BACK UP DOWN LEFT RIGHT
    LT RT BLACK WHITE LS RS LUP LDOWN LLEFT LRIGHT), UG_WATCHDOG=<s> (60 host stack samples, 50 ms apart).
    NOTE: UG_NVLOG logs only the first write of each register, whatever its value.
  - Lifter: polling loops (short, call-free, store-free, reading a global) get x86_spin() on the back-edge (49 sites).
  - Memory: RAM also mapped at 0xF0000000 (write-combined view D3D's LockRect returns; loading-bar timer callback).
- CURRENT BLOCKER (story mode): after Difficulty -> the game opens data/cutscenes/Intro_01.cut.Xbx and then loops in
  the cutscene loader 106EE0: { call 33660 (stream update), call 31C40 (ready?), call 174090 (wait one vblank) }.
  31C40 -> 34760 -> 358C0: the stream object (ptr at [0x24E7DC] / [0x24E7E0]) is "ready" when [+0xA0] == 1 and
  [+0x98] >= [+0x9C] + 0x2000 (bytes buffered). [+0x98] is written at 337E5, 33BD3, 35768, 35850: find which path
  should advance it (async file read completion? DSOUND stream callback? NtReadFile returns at once, no APCs seen).
  The vblank callback (173E90, via D3D's ISR) does run. Repro:
  `UG_INPUT=$(python -c "print(','.join(f'{t}:A' for t in range(6,240,3)))") ./out/underground.exe`.
- Oracle (all leaves): 2228 pass, 17 fail, 887 skip. Explained: ret imm16 sign (unicorn bug: 19430, 224EF, 3DC24,
  168D83, 181B68 are junk starts), rdtsc/cpuid stubs (173E80, 18A9D0, 1975F0, 182300). Unexamined: 11980, 48D63,
  6A330, 78820/1C5395 (st0 nan vs inf), 1945D0 (x87 top), 1C5FA7, 1F8A78.

## Next
1. Fix the Intro_01 cutscene stream stall (above), reach gameplay.
2. Renderer gaps: shader stage modes other than 2D/pass-through (mode 2 seen), cube maps, fixed-function lighting
   (0x0314 = 1 seen), fog, MSAA, widescreen, CPU-drawn loading bar (writes guest framebuffer RAM directly).
3. Oracle fails above. Unimplemented lifter sites (46: enter, segment movs, fnsave/frstor).
4. Installer (llvm-mingw fetched on the player PC, build like Gauntlet's gt_build.py), Linux port, PS2 HD textures.
- Not pushed since the initial commits (user rule: push only when asked).
- Repos: origin gitlab.com/octagram-labs-group/Project-Underground, forge 8gramlabs/Project-Underground (both public).
  Homepage card live (placeholder image landing/img/thug.jpg: replace with a screenshot later). release-sync REPOS updated.
