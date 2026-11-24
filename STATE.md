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
RESUME HERE (paused 2026-11-24 ~00:30, user's request):
A. PINK HOUSE (NJ, user screenshot): a pink house wall shows a diagonal split, an untextured flat-pink part and
   sky through the wall (also in our A/B shot 12300 of the HD run: skater standing in front of it). Tool ready
   (4cd0dc8): F9 or UG_CAPTURE=<frame> -> out/capture.log (every draw of that frame, "[draw] #n ...") + shot N
   (normal) + shot N+1 (UG_DBGCOLOR=10: draw index = r + 256*g). Asked the user to run
   `cd "G:\Project Underground"; .\out\underground.exe`, stand there, press F9. Then: read the pixel id on the
   wall, find "#id" in capture.log, see what is wrong (texture/stage state, vertex attrs, clip). The scripted
   NJ route does not land in the same spot every run.
   SESSION 2026-11-24 (pink house, NOT FIXED, cause not isolated):
   - Excluded: position garbage (attr 0 never hit the bad-float path), dropped draws (0 "buffers full" / "pool
     exhausted" in 20 logs), strips split over several DRAW_ARRAYS (0 cases in 3 NJ runs), wrong winding in the level
     data (NJ.scn: 13958 triangles agree with their normals, 363 oppose, all of the large strongly opposing ones are
     two small props), front face state (0x900, cull 1/404 on the walls looked at).
   - The game culls per mesh (occluders): partly drawn sectors are normal; the 4 checked were hidden behind nearer
     geometry. Best remaining lead: a mesh judged occluded when it is visible. Next: capture the house itself, then
     compare with NJ.scn (which mesh is missing), then look at the occlusion code.
   - Never captured with F9: the holes were only in out/cmp_12300.jpg, and probably in out/badattr/r2 shots 08400/09600
     (mission spot, skater at the gate). The route is random: 1 run in ~10 got there. Trigger captures on content
     (pink siding covering > 12 % of the screen), not on a frame number. Camera override (UG_CAM, removed) is useless
     for this: the game culls with its real camera.
   - FIXED (proven, but not shown to be this bug): valid texcoords with a denormal component were replaced by the
     previous vertex's (fetch_vertex). NaN / >1e30 texcoords in the level data (a flat porch trim) still are.
   - Tools in out/badattr (git-ignored): prx.py (unpack), sector.py / allsec.py (NJ.scn parser: sectors, meshes, index
     lists), missing.py / partial.py (on-screen meshes not submitted in a capture), crop.py (overlay on the shot).
B. Also seen (debug run, NJ pool by a brick wall): the wall's brick pattern drawn on the ground in front of it.
   Unknown whether it is a bug or the game's effect.
   2026-11-24: in out/pink/shot.png the fence next to the wall shows on the ground too: looks like the game's own
   planar reflection in the wet floor (not proven).
   GRAPHICS REQUEST (user, 2026-11-24): supersampling, anti-aliasing, ambient occlusion, anisotropic filtering,
   unlimited draw distance. NOTHING OF IT WRITTEN YET. State and plan:
   - Anisotropic: already in (UG_ANISO, 16x default). Missing: a launcher / in-game menu entry.
   - Supersampling: UG_SCALE (1..8, default 2; launcher combo exists). Missing: submit_and_present downsamples with ONE
     linear blit, which skips texels above 2x the window: step down by halves first.
   - AA (FXAA) + ambient occlusion (from ZSurface.dview): one fullscreen pass at the world -> HUD boundary (after the
     last perspective world draw, before the first HUD draw) so the HUD stays sharp. Find that boundary first
     (perspective = comp row 3 not 0,0,0,x; beware letterbox / cutscene frames and render-to-texture). Needs a colour
     copy image, a new pipeline (touch_pipeline() is the template) and new shaders (python -I tools/shaders.py).
     Near plane 2, far about 32000 (measured from the composite matrices): use them to linearize depth.
     All three off by env; settings in the in-game menu and the launcher.
   - Draw distance: the far plane is not the limit (NJ is about +-16000 units, far about 32000) and fog was off in
     every captured NJ draw. Camera near/far = camera object +0xA8 / +0xAC (camera list [0x229C28],
     SetClippingDistances 0x11C240, defaults 12 / 12000; in NJ the matrices show about 2 / 32000).
     Suspend component: +0x1C / +0x20 suspend distance and its square, +0x24 squared camera distance, +0x30 suspended
     (IsSuspended 0x4CB70, 6 callers), +0x34..+0x40 lod_dist1..4; init 0x4C960; its manager writes +0x24 / +0x30 at
     0x4CF78..0x4CFF0: READ THAT NEXT. Model LODs: LODDistance read at 0x96F63 (last LOD forced to 3.4e38).
     TRIED AND REVERTED: hook on 0x4CB80 (distance > lod_dist2): never called in NJ, its only caller 0x9C460 skips an
     update, it does not hide anything. Raising the suspend distance itself also wakes far AI: gameplay change, avoid.
     First measure which objects disappear with distance; nothing proves yet that anything visible is cut in NJ.
   - Gotcha (again): a backslash-n inside a bash heredoc became a real newline in C strings and broke three builds;
     write such patches with the Write tool, and never run the exe unless the build printed "linked".
C. README pictures (user asked): hd_compare.jpg REPLACED (static UG_SHOTAB pair 09600/09601 of a NJ run with the
   HD pack, box on the ramp: graffiti only in HD; scratchpad hdcompose.py, hdpairs.py). TODO: all 15 maps
   (nj ny fl sd hi vc sj ru hn ph se sc sc2 dj vn; not testlevel, not sp = cut/empty): out/levels2/sweep.sh
   (UG_LEVEL redirect, UG_SHOT=600 from 4200; ~65 min). Do NOT run anything else meanwhile (shared out/shot_*,
   route timing). TaskStop does not kill the bash loop: kill bash + timeout + underground.exe by PID. Then pick
   the best widescreen shot per map (resize to 16:9), README gallery (docs/images/maps/*.jpg), commit, push.
D. Not pushed: 4cd0dc8 (F9 capture) and the new hd_compare.jpg.
1. Sky wedge (cosmetic); pool-floor rectangle (UG_DBGCOLOR=7). Neither showed in 16 NJ shots on 2026-11-23
   (run_texgen, run_dbg7); a quarter-pipe light/dark split at f14400 is not a shadow draw (baked lighting).
2. Renderer gaps (not seen in NJ so far: check other levels first): FF lighting, cube maps, eye/object-linear and
   sphere texgen, MSAA. Widescreen done. CPU-drawn loading bar: not worth it, a level load takes < 0.3 s here
   (NJ: one 292 ms frame between f3000 and f3300), the loading screen is barely on screen.
0. STATUS 2026-11-23 (end of late session): user todo 2-5 done and committed (c3b9de0..287f2c5), PUSHED to both remotes
   2026-11-23 (no tag/release). Linux build of all of it (WSL ~/ug-linux, lifted copied from out/lifted incl. *.h to ~/ug-lifted2)
   verified: menu opens/fullscreen on-off/toggles/closes (out/linux/menu_test.sh), typing "Kid42" in Save Name
   (out/linux/type_test.sh, UG_TYPE). Waiting on the user: todo 1 (where the black wedge still shows: screenshot),
   by-ear check of cutscene speech sync, phone test (adb), push/release decision.
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
      Game-side updater DONE 73cca5c (runtime/src/updater.c from Gauntlet): ug_test build logged "no release
      found" (own repo) and, with UG_UPDATE_REPO=8gramlabs/Project-Gauntlet UG_UPDATE_ASSET=ProjectGauntlet-Setup,
      "v0.2.1 available (installed 0.1.0)" (prompt shown; download/run step untested).
   d. DONE (setup.cpp ported by a subagent, reviewed): `python -I installer\build.py` -> dist\ProjectUnderground-
      Setup-0.1.0.exe (15.8 MB, system DLLs only). Launcher settings: UG_FULLSCREEN, UG_FPS (30 when unticked),
      UG_VSYNC (default on; unticked = 0, matches gpu_vulkan.c), UG_MONITOR, UG_SCALE, UG_PADS. Neutral placeholder
      art from installer/art/*.py (user art welcome). `--iso ... --dir G:\ug_inst` test OK (~53 s).
      CLEANUP of that test (by hand): rmdir junction G:\ug_inst\toolchain FIRST, then delete G:\ug_inst, G:\ug_test,
      G:\ug_test2 (same junction rule); HKCU\Software\ProjectUnderground, HKCU\...\Uninstall\ProjectUnderground;
      shortcuts Desktop (OneDrive) and Start Menu "Project Underground.lnk".
      --update and --uninstall TESTED OK later (see docs/installer.md "Verified"); uninstall removed the registry
      keys and shortcuts itself; only G:\ug_inst\out remains. Still delete by hand: G:\ug_inst, and G:\ug_test +
      G:\ug_test2 (rmdir their toolchain junctions FIRST).
      UNTESTED: launcher clicks (Play/Logs/Update), settings.ini writes, real llvm-mingw download, clean PC / Sandbox.
   USER RULE 2026-11-23: release NO installer until all ports (Windows, Linux, Android) are ready. "push" = git push
   to both remotes only, no tag/release, until then.
   e. Bump VERSION locally only. No tag/push/release until the user says push.
   Then: Linux port (WSL Ubuntu-24.04, same Vulkan backend; Gauntlet docs/installer.md "Linux" + tools/
   build_linux.sh, package_linux.sh as models), Android port (Gauntlet tools/build_android.sh, gt_android.py,
   android/ as models; Vulkan), then the first release of all three together. PS2 HD textures last.
   NOTE: main.c already has #ifndef _WIN32 / __ANDROID__ branches from the Gauntlet port (sdl_plat.c referenced).
   LINUX PORT (2026-11-23): runtime/src/linux (win32.c, sdl_plat.c, pad_sdl.c, include/windows.h from Gauntlet;
   windows.h sits on DXVK Native's include/native/windows headers, build time only). gpu_vulkan.c: SDL instance
   extensions + SDL_Vulkan_CreateSurface on Linux. `bash tools/build_linux.sh ~/ug-linux --lifted ~/ug-lifted`
   in WSL (libugrt.a + lib/libSDL3.so.0; lifted flags = build.py LIFT_FLAGS; runtime -Werror=implicit-function-
   declaration etc.). Test data on ext4: ~/ug-run/game (copy of extracted/), ~/ug-run/underground -> ~/ug-linux.
   RESULT: boots, reboot relaunch, Bink intro, scripted route reaches NJ gameplay (out/linux/route.sh; shots in
   out/linux). WSL GPU = llvmpipe (~9 fps in 3D, 30 in Bink): correctness only. pgrep name after relaunch: "exe".
   LINUX PACKAGE DONE (docs/installer.md "Linux"): package_linux.sh -> ProjectUnderground-Linux-0.1.0.tar.gz
   (3.7 MB; build in WSL from a fresh clone: `git clone "/mnt/g/Project Underground" ~/ug-src`). setup.sh install
   (72 s), launch, --update, --uninstall and the in-game updater loop all verified in WSL. Lift on Linux matches the
   Windows lift (CRLF only). WSL test dirs: ~/ug-run, ~/ug-linux, ~/ug-lifted, ~/ug-lift-test, ~/ug-inst, ~/ug-src,
   ~/ug-dist (safe to delete). Not tested: real Linux GPU, real pads, Wayland desktop.
   Linux checks also passed: UG_PADTEST 35/35, SDL audio (non-silent UG_WAVDUMP), settings.ini keys, fullscreen on
   Wayland + X11 (3440x1440), process name kept after the reboot relaunch.
   ANDROID PORT (2026-11-23, built, NOT YET RUN ON A PHONE: no device on adb this session):
   - tools/build_android.sh <dir> [--lifted d] (WSL; NDK ~/Android/android-ndk-r27c, deps ~/.cache/ttt2-android:
     SDL 3.4.18 + android-project): libugrt.a, android-kit (template.apk, no game code), debug APK with --lifted.
     Dev APK: ~/uga/build/underground-0.1.0-android-arm64-debug.apk (game files copied in by hand, Gauntlet's way:
     run-as into Android/data/com.eightgramlabs.underground/files/game).
   - android/: UndergroundActivity, RestartActivity (HalReturnToFirmware relaunch), Vulkan 1.3 required.
   - Fixed from the Gauntlet copy: aarch64 fiber asm labels (were gt_*: link would fail), game/default.xbe.
   - gpu_vulkan.c: DXT CPU decode when no BC (UG_NOBC=1 tested on Windows, NJ correct), depth format must be
     sampleable, SUBOPTIMAL recreate only on real change, surface recreate on foreground / SURFACE_LOST,
     touch overlay (runtime/shaders/touch.*; UG_TOUCHDEMO=1 checked on Windows).
   - Player pipeline: tools/ug_android.py (LIFT_FLAGS + -fsigned-char); launcher "Android..." button; Linux
     setup.sh --android. Linux path verified (3.4 GB APK, apksigner v2 OK, zipalign OK). Windows path: run
     with the payload python against G:\ug_test: OK (NDK in G:\ug_test\android\ndk, APK 3.4 GB there: delete with
     the other test dirs). Installer rebuilt with the kit (20.1 MB); the launcher shows "Android...".
   - Windows payload needs dist\_cache\android-kit (copy of WSL ~/uga/build/android-kit).
   PAUSED (user away from the Galaxy S26 Ultra, 2026-11-23): resume when they are home. Meanwhile work on Next 1-2
   (renderer gaps in other levels). Phone test (connect it on adb): install the player APK, first-start unpack (3.2 GB), relaunch via
   RestartActivity, intro, menus, touch controls, background/resume, swapchain transform (log line
   "transform X"), fps. Then the release (user says push).
- INSTALLER ART (2026-11-23, user's art): background.jpg + icon (installer/art/src). Borderless skateboard-button UI
  in setup.cpp (docs/installer.md "UI"); previews checked (setup, launcher, settings panel) and a full --iso install
  ran OK with it. Game window icon = underground.ico. TODO before release: rebuild the Android kit in WSL
  (tools/build_android.sh) so template.apk carries the new ic_launcher.png, copy to dist\_cache\android-kit, and
  the Linux package (icon.png changed). Launcher hover/clicks by hand still untested.
- DISPLAY (2026-11-23): UG_WIDESCREEN (default on) sets the XC_VIDEO widescreen bit 0x00010000: THUG renders 16:9
  anamorphic with its own HUD layout (NJ gameplay checked, 60 fps); present at s->w/s->h * 4/3. UG_ASPECT=stretch,
  UG_WINDOW=WxH (physical px), window centred on its monitor, per-monitor DPI aware (process + thread context; the
  creation-time WM_DPICHANGED is ignored). Launcher: Window size / Screen / Picture combos.
  FPS: the game does not cap itself (UG_FPS=0 -> 240 fps = monitor vsync); all clocks (rdtsc, QPC, KeTickCount) are
  real-time. 60 fps paced = native rate. Above 60 untested (logic speed unverified at high rates).
  LEAK FIXED 2026-11-23 (user asked): history rewritten (filter-branch swapped the installer.jpg blob showing a
  C:\Users\<name> path for the clean one), force-pushed both remotes (GitLab main unprotected for force push, then
  re-protected), GitLab housekeeping prune + forge `git gc --prune=now` in the container: old commit/blob 404 on both.
- LEVEL CHEAT + ALL-LEVELS SWEEP (2026-11-23): ChangeLevel 0xF1D60 hook (recomp/hooks.txt, hle.c change_level_hook),
  UG_LEVEL=<code>, launcher "Level (test cheat)". out/levels/sweep.sh -> out/levels/<lv>/ (shots, run.log),
  out/levels/all_levels.png. All 17 load without a fatal: 14 good at 57-60 fps; SC sky shows hard seams between panels
  (sky draws 177-192: opaque, zmask 0, cap textures 128x64 DXT1 + side 256x128; decoded with UG_TEXDUMP: separate
  cloud photos that do not match -> most likely the original art; no hardware reference to confirm). SP: SPscn.prx
  never loads (empty level, cut content). testlevel: "problem with the disc" (Green_Sky.tex.Xbx not on the disc).
  Debug added: UG_TEXDUMP=<fmt hex | low 4 digits> (raw texture bytes to out/tex_<addr>_<fmt>.bin), draw log prints
  tex0 offset/address.
- HD TEXTURES (2026-11-23): PS2 PCSX2 pack (SLUS-20731 HD Remaster .rar, 8518 DXT5 .dds) unpacked to hdpack/
  (git-ignored). I8 textures: exact PS2 TEX0 hash as in Gauntlet (NJ 79/83). DXT: PS2 GS hashes can't be rebuilt
  -> picture matching (runtime/src/hdtex.c,
  docs/installer.md "HD texture packs"): UG_HDTEX=<folder>, F10 toggle, UG_SHOTAB A/B shots, UG_HDLOG. NJ: 597
  matched / 272 skipped, 60 fps, A/B correct (after the UG_SHOTAB off-by-one fix 9782935: earlier A/B pairs were HD vs HD); visibly sharper roads/grass/wood. UG_ANISO (16x default) added. Launcher "HD texture pack" row,
  Linux setup.sh --hd. Not done: Android (pack not in the APK), other levels' A/B.
- SESSION 2026-11-23 (late), all LOCAL commits (not pushed; push only when the user says):
  e44b9cd audio streams (RtlNtStatusToDosError table, ADPCM streams, packet completion), HD worker thread, SDL3
  controllers on Windows (pad_sdl.c shared, SDL3.dll + gamecontrollerdb in payload); b0710f8 / aaec64a shadow wedge
  (float attrs that read past the vertex array -> previous vertex's value, nv2a.c fetch_vertex); 3c3af88 / 447c430
  skateboard progress bar (GDI+, % on the deck); 8c04593 SAVES FIXED (fs.c: FILE_CREATE on a dir just made returned
  COLLISION -> XCreateSaveGame failed), UG_INVERTY/X + launcher "Camera (right stick)", XInput pad navigation in
  setup/launcher (untested with a real pad).
  Debug added: UG_HITCHLOG, UG_SHADOWSKIP, UG_DBGCOLOR=8/9, UG_SHOTRANGE, UG_STREAMLOG, UG_FSLOG creates/writes,
  draw log attr formats. Save route script: out/save (UG_INPUT ... 52:DOWN 56:A ... see session; works WITHOUT
  UG_KLOG/heavy logging: timing shifts otherwise).
  USER TODO LIST (2026-11-23, in this order):
  1. Black-line wedge "still visible": the window-scene wedge is fixed in aaec64a; ask the user where they still see
     it (screenshot) - may be other receivers / other scenes.
  2. DONE 2026-11-23 (late): voice "off by a little". Measured with UG_AVSYNCLOG=1 (hooks 106C10 cutscene start,
     31C60 start-preloaded-stream; dsound logs first mixed sample + waveOut queue): the cutscene (106E50) runs on its
     own clock and starts the speech stream the same ms as frame 0, first sample mixed +6..8 ms; the lag was the
     output queue, 80 ms (4 x 20 ms). Now 10 ms buffers, UG_AUDIOBUFS (default 4 = 38 ms; 3 underruns constantly
     through waveOut; Android 6), mixer thread TIME_CRITICAL, underruns logged. NJ route 290 s: 0 underruns.
     Linux SDL path: same queue, builds; not run (WSL audio). User should confirm by ear. If still late: WASAPI
     exclusive/low-latency path (~10-20 ms).
  3. DONE 2026-11-23 (late): keyboard typing. THUG keeps the PS2 USB-keyboard path: create_onscreen_keyboard
     (scripts/engine/menu/keyboard.qb) calls StartKeyboardHandler; its task 0xFA660 polls GetKeyboardInput 0x8BE20
     (Xbox stub returning 0, now HLE in hle.c/hle.txt) and runs handle_keyboard_input per char (0x20..0x7E, 0x19
     enter, 0x18 backspace; 0x14..0x17 = arrows for menus at 0xF9A60). Other tasks poll it too: only the handler
     task (return address 0xFA688) gets chars, one per call (a per-flip snapshot lost chars). Typing mode (letters
     stop driving the pad) = handler polled in the last second. Windows WM_CHAR (UG_KBDLOG=1 logs them), Linux SDL
     text input. Verified: UG_TYPE "Kid42" + Enter -> SaveMeta "Name=Kid42"; real keystrokes (SendKeys from
     scratchpad sendkeys.ps1, game launched from bash) BS BS x z x -> "Custom Skatxzx", keyboard stayed open (x is
     B = Cancel on the pad map: suppressed). Skater name in Edit Skater > Information: not tested.
     UG_TYPE="<s>:<text>,..." scripted typing ('<' backspace, '|' enter). Route: A 6..30 step 3, 52:DOWN, 56:A,
     61:A (Create New), then the keyboard is up at ~61 s. qb.prx unpacker: scratchpad prx.py (LZSS, N=4096 F=18).
  4. DONE 2026-11-23 (late): in-game menu, runtime/src/menu.c. F1 / Esc / L3+R3 (Esc no longer quits: Quit is a
     menu item, press twice). Modal loop at the NV2A flip (nv2a.c 0x012C): guest threads are fibers on one host
     thread, so the whole game pauses; sched_pause_add leaves the paused time out of sched_now_100ns (all guest
     clocks derive from it); dsound g_audio_hold outputs silence without advancing voices/streams. Renderer:
     gpu_menu_present re-presents last_present with the panel copied (opaque, centered, BGRA swap) into the
     swapchain image: no new shaders; same Vulkan path on Linux/Android, BUT Android has no way to open it (no
     Esc/F1, touch has no L3+R3) and its pad cache (android_pad_poll in sched_run, main stack) is frozen while the
     menu loop runs: phone list item "in-game menu" (needs a touch button + pad poll). Font: runtime/src/font8x8_basic.h
     (public domain). Items: Resume, Fullscreen, Frame rate 30/60 (g_fps, nv2a.c), Camera left/right + up/down (PC default = both flipped vs Xbox; UG_INVERTX/Y=0 = Xbox), HD (only with a
     pack; session only), Quit. Writes settings.ini (UG_FULLSCREEN, UG_FPS, UG_INVERTX, UG_INVERTY) only if the file
     exists. Close waits for all keys/buttons released. Debug: UG_INPUT "<s>:MENU", UG_MENUKEYS=down,right,ok,back
     (other words wait 300 ms), UG_MENUSHOT=1 -> out/menu.ppm. Verified (Windows): navigation + toggle, 10 s pause
     in NJ gameplay resumes seamlessly (shots 7420-7480), 0 audio underruns; fullscreen on/off from the menu
     (toggle posted to the window thread, WM_APP+1: swapchain 3440x1440 then back to 1706x960); settings.ini
     round trip (launcher-style file: UG_FPS removed, UG_INVERTY=1 added, [launcher] kept). Desktop SDL pads work
     while paused (get_live calls UpdateGamepads). Only arrows autorepeat in the menu. Not tested: Linux run
     (builds), real pad L3+R3, an actual install. NEVER screen-capture the desktop
     for tests (a DPI-wrong CopyFromScreen grabbed other windows; deleted): use UG_MENUSHOT / UG_SHOT only.
  5. DONE 2026-11-23 (late): CHEATS in the in-game menu (shown once a career exists). Cheat flags = career global
     flags: bitset at [[0x2FCD68]+0x20] + 0x584 (SetGlobalFlag 0x1298B0 -> 0x113820), set directly while paused:
     CHEAT_ON_n = 184 + n (n 1..15): always special ON_2 186, perfect rail ON_3 187, stats-13 ON_4 188, perfect
     manual ON_7 191, moon ON_9 193, sim ON_10 194, perfect skitch ON_12 196, bighead ON_13 197, slomo ON_15 199
     (cheats.qb aliases, global_flags.qb numbers). "Unlock everything" runs the game's own script
     unlock_all_cheats via RunScript-by-checksum 0x1F4B0(crc, 0, 0, 0) from the next XInputGetState (guest thread).
     Verified in NJ: moon toggle sets its flag, unlock ran (flag 120 set), no fatal; after it "Ollie 20 feet"
     (no baseline run). The game's own cheats menu (create_real_cheats_menu) toggles these five with toggle_cheat
     + nullscript callbacks, i.e. a plain flag flip like ours; only scripts call the C++ getter 0x113880, so the
     C++ side reads the bitset some other way (not traced).
     Script tools (scratchpad): prx.py (unpack .prx), qbscript.py <xbe> <qb> <script...> (decompile a script),
     qbctx.py. Unpacked qb in scratchpad/qb (re-run prx.py if the scratchpad is gone).
  6. Controller support in the launcher: done 8c04593 (needs a real-pad test).
- Pushed to both remotes 2026-11-23 (all commits up to 14d0e3a incl. HD textures, audio, typing, menu, cheats).
  No release until all three ports are ready.
- Repos: origin gitlab.com/octagram-labs-group/Project-Underground, forge 8gramlabs/Project-Underground (both public).
  Homepage card live (placeholder image landing/img/thug.jpg: replace with a screenshot later). release-sync REPOS updated.
