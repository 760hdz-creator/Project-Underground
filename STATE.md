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
- 2026-11-22 (night): GAMEPLAY REACHED. Story: intro cutscene -> Create-a-Skater -> NJ_01V cutscene -> New Jersey
  level, skater controllable, HUD, 60 fps, no fatal in 300 s (out/ingame_nj.png).
  - Cutscene stall fix: overlapped NtReadFile on async handles (no FILE_SYNCHRONOUS_IO_*, event or ApcContext given)
    returns STATUS_PENDING with the IOSB already filled; THUG's streamers (33940, 357F7) take a sync success as EOS.
    NtWaitForSingleObject(Ex) on a file handle returns at once.
  - DSOUND: IDirectSoundBuffer_Release frees the voice (game never AddRefs), slots reused.
  - New debug env: UG_FSLOG=1 (file reads), UG_PEEK="va,len;*ptr+off,len" (guest dump at first UG_WATCHDOG sample).
  - Scripted route to gameplay (menu input timing is flaky run to run, D-pad and stick alike, so the CAS exit is a
    repeating cycle; this LDOWN variant reached NJ twice in a row):
    UG_INPUT="$(python -c "
ev=[f'{t}:A' for t in range(6,31,3)]
for c in range(40,200,12): ev+=[f'{c}:B:0.3',f'{c+1.5}:B:0.3',f'{c+3}:B:0.3',f'{c+5}:LDOWN:0.3',f'{c+7}:LDOWN:0.3',f'{c+9}:A:0.3']
print(','.join(ev))")" UG_SHOT=1200 ./out/underground.exe   (NJ level by ~frame 6000; run 280 s)
  - FIXED 2026-11-23: magenta tint = final combiner (c0/c1 now SET_SPECULAR_FOG_FACTOR 0x1E20/24; fog alpha 1 when
    fog off). Fog function (FOG_MODE/PARAMS) added in nv2a.vert. build.py now tracks local #include mtimes.
  - OPEN (cosmetic): translucent dark-blue band/wedge across the sky at the top of the screen, intermittent, only with
    sky in view. A/B runs inconclusive (UG_SKIPMODE=2, UG_SKIPTEXMAT=1: the sky dome itself uses texture matrices, so
    skipping texmat draws removes the sky). Not texgen (no draw sets TEXGEN 0x3C0..). stage_tex divides by q per
    fragment. Left for later.
  - DONE 2026-11-23: shadow maps. Depth formats 0x2A/0x2C/0x2E/0x30 sample the host depth surface at that address
    (ZSurface.dview, depth-only view); shader stage mode 2 (3D projective) on a depth format = shadow stage (tmode 4).
    Compare is STORED op REF (func 0x1E6C, D3D order never/less/eq/le/gt/ne/ge/always), ref = r/q / zmax clamped to
    [0,1]. THUG: skater rendered into a 256x256 depth surface (035B7000 CAS, 03311000 levels), receivers multiply dst by
    0.75 + 0.25*shadow (skater self-shadow) or t1 + 0.6 (world sectors, only some sectors, e.g. alleys).
    Also fixed: depth clamp enabled (NV2A does not clip z; receivers were clipped), polygon offset implemented
    (fill enable 0x0338, factor 0x0384, units 0x0388; receivers use -2/-4), TF_DEPTH sorts after DXT (size path).
  - OPEN: one shot showed a hard-edged darker rectangle on a pool floor (receiver sector darkened as a whole: ref >= 1
    outside the light frustum?). Check with UG_DBGCOLOR=7 there. Debug: UG_DBGCOLOR=6 (r = fract(ref*16), g = stored
    depth < 1, b = ref > stored), 7 (uv, ref/4 of shadow draws; other draws white), UG_SHADOWNOZ=1 (no z test/cull on
    shadow draws). Draw log now prints all 4 stage modes, shader prog, z func and polygon offset.
  - FIXED: color/zeta surfaces grow only (Surface.aw/ah); a smaller clip (640x420 letterbox in CAS/cutscenes, was
    recreating + clearing both targets every frame) renders into the top-left of the allocation. Rect sampling of a
    render target uses the allocated size.
  - Debug env added: UG_SKIPFMT=<hex>, UG_SKIPMODE=<m>, UG_SKIPTEXMAT=1 (both drop draws in windows where
    ((frame+150)/300) is odd; shot N shows frame N-1), UG_DRAWLOG prints fog, texgen, texmat-stage formats, pos range.
- DONE 2026-11-23: fixed-function texgen. Gameplay draw survey (run_shadow4/run_texgen logs): lighting (0x0314) is
  never on, no texture has the cube-map bit, only texgen seen is 0x8512 reflection map (also 0x8511 normal map
  implemented). FF path computes eye pos (MV 0x480) and eye normal (IMV 0x580 = inverse-transpose 3x3, same row
  layout: n_eye.j = dot(imv[j], n), checked against dumped matrices), r = u - 2(n.u)n, then texture matrix. Other
  texgen modes and FF skinning (0x0328) log once. Most texgen draws have the stage disabled (stale state).
  q forced to 1 on generated stages (absent texcoord arrays come from k_cur, w may be 0). Not A/B'd visually.
  OPEN: texgen draws have LIGHT_CONTROL 0x294 = 00020001; if bit 16 is LOCALEYE (unverified), the viewer is
  infinite and u should be a constant eye vector, not normalize(eye pos). Sign of that vector unverified too.
- Oracle (all leaves): 2228 pass, 17 fail, 887 skip. Explained: ret imm16 sign (unicorn bug: 19430, 224EF, 3DC24,
  168D83, 181B68 are junk starts), rdtsc/cpuid stubs (173E80, 18A9D0, 1975F0, 182300). 2026-11-23, all explained:
  78820/1C5395 st0 "nan vs inf" was a harness bug (f80_to_float read x87 inf as NaN; fixed, pass); 1945D0, 1C5FA7,
  1F8A78 pass on rerun; 48D63 junk start (push esp; ... ret 4); 11980 memzero, uc stops mid rep stosd on a huge
  random count; 6A330 NaN-vs-NaN payload only (clang swaps commutative mulss operands). No lifter bug found.

## Next
1. Sky wedge (cosmetic); pool-floor rectangle (UG_DBGCOLOR=7). Neither showed in 16 NJ shots on 2026-11-23
   (run_texgen, run_dbg7); a quarter-pipe light/dark split at f14400 is not a shadow draw (baked lighting).
2. Renderer gaps (not seen in NJ so far: check other levels first): FF lighting, cube maps, eye/object-linear and
   sphere texgen, MSAA, widescreen, CPU-drawn loading bar (writes guest framebuffer RAM directly).
3. Lifter sites: fnsave/frstor/fnstenv/fldenv implemented 2026-11-23 (MSVC CRT pow error path 182F30 uses
   fnsave/frstor around its handler). Remaining 41 unimplemented sites are all junk decodes (enter with nonzero
   level, nonsense segment movs, ltr/sgdt/ljmp/int1/fstpnce): nothing to do.
4. INSTALLER (in progress 2026-11-23; port of G:\Project Gauntlet docs/installer.md, minus its VS/admin toolchain path).
   Slices, in order:
   a. tools/ug_build.py --root D [--iso img] [--version V]: extract -> D/game, xbe SHA-256 check, llvm-mingw into
      D/toolchain (bootstrap with a dir parameter), lift with committed seeds only -> D/build/lifted (must be
      byte-identical to out/lifted), compile + link with build.py's flags (import, never copy) -> D/underground.exe
      (-mwindows, -DUG_VERSION). Test dir G:\ug_test (delete by hand later); run from another cwd to the menus.
      DONE fc8825a: ~60 s into G:\ug_test (toolchain there = junction to the dev toolchain, so the download path
      was not re-tested); lift byte-identical (23 files); imports only system DLLs + UCRT api-ms-*; GUI subsystem;
      run from C:\ -> quick reboot (HalReturnToFirmware 2) -> relaunch -> Neversoft intro at f900, log in out\.
   b. main.c: ALREADY DONE (ported from Gauntlet earlier): cwd = exe dir when game\default.xbe missing; LOG -> out\underground.log without a console; settings.ini.
   c. DONE: payload 14.3 MB (67 tracked files); its embedded python ran ug_build into G:\ug_test2 (also delete by
      hand), lift byte-identical. VERSION = 0.1.0 (new file). Spec was:
      tools/package.py -> dist/payload.zip (SHIP allowlist, THUG FORBIDDEN list, embedded CPython + capstone at the
      dev version, licenses: Vulkan-Headers, volk, xxhash, CPython, capstone).
   d. installer/setup.cpp port built with llvm-mingw (-static, -municode, llvm-rc), neutral placeholder art (user
      art wanted; never Activision art), launcher + updater (forge releases/latest).
   e. Bump VERSION locally only. No tag/push/release until the user says push.
   Then: Linux port, PS2 HD textures.
- Not pushed since the initial commits (user rule: push only when asked).
- Repos: origin gitlab.com/octagram-labs-group/Project-Underground, forge 8gramlabs/Project-Underground (both public).
  Homepage card live (placeholder image landing/img/thug.jpg: replace with a screenshot later). release-sync REPOS updated.
