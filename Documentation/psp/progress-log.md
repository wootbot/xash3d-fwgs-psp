# PSP port progress log

Chronological notes: what was tried, measured numbers, what worked and what didn't.
Newest entries at the end. Plan and open work: [PLAN.md](../../PLAN.md). Search this file
(grep) for a topic instead of reading it whole; append new entries at the end.

- 2026-11-17: Fresh upstream fork created; old fork and hlsdk moved to `reference/`. Plan written.
- 2026-11-17: Phase 0 tooling (Docker build, stub audit, hlsdk build). Phase 1: upstream builds and
  links for PSP with `ref_null` (`--psp=prx,660,NULL`), EBOOT.PBP produced, 0 unresolved stubs in
  engine and game PRXs. Not run on hardware yet. Static memory measured (see Phase 1).
- 2026-11-17: **Milestone 1 on hardware.** Fixes needed: `CreateInterface` export for static
  filesystem_stdio, `XASH_REDUCE_FD` revived (9 file handles), nested sound lock deadlock.
- 2026-11-17: Running from `host0:` (game root = `build-psp`, PC `valve` data): first frame 13.8 s
  (stick: 35 s), `c0a0` loads in 4.7 s (stick: ~23 s). Sound audible. Tram ride through
  `c0a0a`/`c0a0b`/`c0a0c` transitions works after: device-root `chdir` fix, net_graph without font
  fix, `SAVE_HEAPSIZE` 2 MB on PSP (4 MB didn't fit). Engine heap on `c0a0c`: 24.3 MB allocated.
  ~~Open: ~25 s after "Verifying and uploading resources" before spawning~~ fixed, see below.
- 2026-11-17: Phase 2: `ref/gu` ported to REF_API 20 and linked (`--psp=prx,660,HW`, now the
  `docker-build.sh` default), 0 unresolved stubs. Static memory with `ref_gu`: text 4.98 MB, BSS
  15.8 MB (+2.4 MB over `ref_null`: 1 MB GU display list, 276 KB texture table, 296 KB studio
  state after capping MAXSTUDIOVERTS at 2048, 245 KB decal pool, 196 KB lightmap blocklights).
- 2026-11-17: **First frames with `ref_gu` on hardware.** Menu renders (first frame 5–6 s from
  host0:). Fixed: invisible menu buttons (8-bit BMP palettes have alpha 0), 1024x1024 button atlas
  (→ mainui text buttons on PSP), HD menu background (105 tiles, 6 MB → classic 12 tiles, menu
  texture RAM 6.71 → 1.29 MB). `c0a0` loads and runs scripted sequences but memory is exhausted:
  engine pools 23.5 MB (client edicts 4.8 MB, Zone Engine 4.6 MB, c0a0.bsp 3.9 MB, SoundLib 2.6 MB),
  textures 4.5 MB RAM + 1.5 MB VRAM, BSS 15.8 MB; even a PNG screenshot fails. max_edicts default
  now 900 (−0.9 MB, not yet measured). Next: texture compression (32-bit images → CLUT/DXT,
  fonts → 4-bit alpha CLUT), sound cache, Zone Engine.
  Debug aids: `+vid_autoshot N`, `+sys_cmdfile cmd.txt` (console commands from the PC via host0:),
  `gu_dumptexture <n>`; quit with `quit` via the cmdfile instead of PSPLink `reset`.
- 2026-11-17: **World renders in `c0a0`** (screenshots via the cmdfile work in game now). The engine
  was built with `-T debug` (`-O0`): 4 fps, ~200 ms per server frame (40 fps with the server
  paused); `docker-build.sh` now defaults to `-T fast` like the old port: 36–52 fps in the tram,
  xash.prx 6.2 → 4.8 MB, `c0a0` spawn 26 → 13 s. `timerefresh`: 196 fps (5 ms per frame).
  Textures: almost everything was already T8; mainui stb fonts → T4 alpha CLUT (640 → 160 KB),
  opaque 32-bit images → NeuQuant palette instead of RGB332, CLUT format per texture
  (5650/5551/4444 instead of always 4444), the shared 332 palette (lightmaps) no longer tops out
  at 14/15 red/green and 12/15 blue (darker, tinted lighting). c0a0 now: 735 textures, 3.1 MB RAM
  + 1.56 MB VRAM (incl. palettes); models 1.5 MB, map 0.65 MB, menu background tiles kept in game
  0.45 MB, sky 0.38 MB. Engine pools 22.3 MB (client edicts 4.78 → 3.92 MB with 900 edicts).
  Partition 2 has 0.68 MB free besides our heap (the 3 MB threshold minus game PRXs).
- 2026-11-17: `psp_meminfo` added. Headroom in `c0a0`: 5.5 MB heap + 0.68 MB kernel memory.
- 2026-11-17: **Map load stall fixed** (upstream bug, not PSP-specific): the server starts its
  netchan sequence at a random number up to BIT( 27 ) and `CL_ParseClientData` marked every
  "dropped" frame from 1 up to it: 15-36 s of soft-float compares on the PSP after `ucmd->begin()`
  (also the 10 s freeze when the c0a0 welder sparks fired). `c0a0` now: `map` -> connected 13 s
  (client connected 7 s after client start); remaining load time is `svc_resourcelist` (6.5 s:
  model/sound precache + renderer setup over host0:) and a 2.1 s loading-screen frame.
  Filesystem: PSP dirs count as case insensitive, so missing `custom/`/`valve_downloads/` are no
  longer rescanned on every lookup.
- 2026-11-17: Phase 3 started: on-demand Wi-Fi bring-up (`net_psp.c`). Modules load and apctl starts
  joining; the AP join failed (`0x80410106`) on the user's main router. With a 2.4 GHz SSID for the
  PSP ("<ssid>", profile 2) it connects in a few seconds and UDP works both ways (getchallenge
  reaches the PC, a connectionless `print` from the PC shows up in the PSP console). The PC
  couldn't ARP the PSP at first (stale state; fine after relaunching). `psp-ld -r` of ref_gu
  sometimes failed with "bad value" (fixed later: duplicate object names, see below).
- 2026-11-17: Receive fixed (per-frame yield for the net threads), clean quit with Wi-Fi up.
  **First GoldSrc connection**: the PSP joins ReHLDS (LAN, crossfire) over `connect ... gs`, gets
  serverdata and resources, loads the map, then runs out of engine heap before spawning.
- 2026-11-17: **Milestone 4: the PSP plays on ReHLDS over GoldSrc protocol** (crossfire, LAN).
  OOM fix: client packet-entity ring 64 -> 16 frames (5.3 -> 1.3 MB, client-local, protocol
  untouched), `MAX_STATIC_ENTITIES` 256 like the old port (the client allocated 3096 x 3 KB on the
  first static entity; server array 1 MB -> 87 KB), console scrollback 1.4 MB -> 224 KB. Out of
  memory now dumps the pool list and big allocations. Crossfire in game: heap 22.7 MB in use,
  10.6 MB largest free block (was 28.9 / 4.4); c0a0: 26.0 / 7.0 MB. Kernel: 65 KB free with the
  Wi-Fi modules loaded (they stay loaded until quit). Remaining big pools in MP: client edicts
  2.7 MB, temp entities 1.5 MB (`max_tempents` 500), SoundLib 2.2 MB, server edicts/baselines
  1 MB (allocated even as a pure client).
- 2026-11-17: **Steam tickets through steam-broker work on the PSP** (LAN server, `sv_lan 1`).
- 2026-11-17: **Heap fragmentation on level changes** (c0a0c: 5.8 MB free, 1.5 MB largest block, the
  1.55 MB surface array didn't fit). Fix: P5 RAM (`vmem_psp.c`, 4 MB volatile partition, one kernel
  block per allocation) behind a new `MEM_VOLATILE` pool flag in `zone.c`; `host.volatilepool` holds
  the 2 MB save/restore buffers and, on PSP, the model file buffer while a model loads
  (`FS_LoadFileToPool`). **The whole c0a0 tram ride runs** (c0a0 -> c0a0e, 4.9 MB largest block
  at the end). P5 contents don't survive suspend; only short-lived buffers go there.
  Broker no longer retries (and fails) every 5 s while Wi-Fi is down in singleplayer.
  The flaky `psp-ld -r` "bad value": `ref_gu` builds ref/common sources itself and waf gave them
  the same object names as the `ref_common` target, so two tasks wrote the same files (`idx = 7`).
- 2026-11-18: **Combat and load-time profiling over the Wi-Fi debug console** (HLDS test server,
  c1a3a, ~18 grunts in view). Studio models: one display list block per mesh and precomputed vertex
  colors (mesh submit 7.9 -> 4.6 ms/frame), a direct vertex lighting path without local lights
  (3.4 -> 2.0 ms), live entity lights collected once per frame instead of walking all 128 for each
  model with software double compares (1.2-1.9 ms -> ~0). Studio total ~19.7 -> ~16 ms/frame, idle
  frame ~32 -> ~27 ms. FPS counter on by default, screen kept on (`scePowerTick`).
  **Memory stick lookups were the load-time bottleneck**: a stat costs ~15 ms, a file found across
  the search paths ~45 ms, a missing one ~65 ms, and every console line with `-log` was its own
  write (~2 ms, sometimes ~130 ms), as was the watchdog heartbeat every 5 s. A directory listing
  cache in the filesystem (names and sizes, dropped on any write), a buffered `engine.log` and the
  heartbeat off the stick: **connect to c1a3a 66 s -> 23 s**, a sound loaded in game 85 -> 35 ms.
  Left in the 23 s: world load 3.2 s, model precache 3.5 s, CL_RegisterResources 2.8 s, serverdata
  2.7 s. Out of memory while loading a map (brokenpath) now drops to the menu instead of a fatal
  error (uncommitted, needs the earlier soft-fail work cleaned up first).
- 2026-11-18: **Firefight benchmark on blastcorpse1** (the user's map: walk forward from the spawn
  into a big fight; `tools/dbg/combat_test.sh`). Started at ~9-10 fps (~100-110 ms/frame), now
  **~14 fps (~72 ms)**. Movement prediction re-ran every unacknowledged command each frame (15 runs,
  10 ms): results are now reused until the next packet (7 runs, 4.5 ms), and player traces skip box
  physents out of reach (brush models can't be culled by their bounds: clip brushes). Sound mixed at
  22 kHz with the hardware SRC channel resampling (mixing 13 -> 5-7 ms); sentence words stay loaded
  (they were re-read from the stick, ~35 ms each, every time a soldier talked: svc_sound was up to
  25% of the frame). Netchan_TransmitBits cleared its 192 KB send buffer for every packet (1.2 ms);
  entity interpolation in float. Out of memory in game is now a Host_Error back to the menu
  (committed; host.status was never HOST_FRAME on PSP, which hid it). Maps whose entity lump was
  stripped by the download server are no longer downloaded again on every connect.
  Left per frame in the fight: studio ~25 ms for ~55 models (about half in the client DLL's bone
  setup, which does double math: hlsdk), world 6, temp entities 4 (client DLL, double math),
  server messages 4-7, prediction 3.5 + redo 3, sound 3-4.
- 2026-11-19: **Firefight ~13 -> ~16.5-19 fps** (blastcorpse1, 60 s windows: 75 -> 60 ms per frame
  with ~59 models, 53 ms with ~47). Client DLL (hlsdk `psp-hl`, committed there): bone setup sin/cos/
  asin on the VFPU (matrix 5 only, ref_gu keeps state in 6/7), per-model math in float, light
  transforms copied instead of recomputed: bones 4.4 -> 3.0 ms; pm_shared untouched (prediction
  must match the server). Engine: entity lighting 4.3 -> 1.6 ms (a dead CL_TraceLine in the sky
  test, float dlight compares, PSP sky-test cache per entity); particles without software doubles
  (2-3 -> 0.4 ms); **func_water warp 8.6 -> 1.7 ms** (16 software double ops per vertex, found with
  new brush-model timers: brush entities were 11 ms of untimed render time); translucent entity
  sort with precomputed keys (2.3 -> 0.4 ms). Static scan for hot software-double code:
  `psp-objdump -d xash | awk` counting `jal __*df*` per function. `psp_update <file>` pushes any
  game file (client PRX). Left per frame: studio ~17-19 (engine mesh submit ~11, client ~7),
  world 6-7, brush 5, prediction ~6.5, server messages 3.5-5, sound ~3.
- 2026-11-19 (later): pmove physents built once per packet instead of up to 4x a frame (-0.8 ms,
  checked against full rebuilds). **Memory is the studio bottleneck, not math**: with the GE
  drawing, the CPU's memory accesses run at about half speed (a mesh loop takes 1.6 ms with the GE
  idle vs 3.0 ms while it draws; 6 uncached stores per vertex alone cost 160 ns). Tried and
  reverted: vertices written through the cached alias (worse: thrashes the 16 KB dcache), indexed
  studio strips (each record is shared by only ~1.5 strip vertices: no gain), 16-byte packed
  vertices (loop -1 ms, but the packing pass costs 2.4 ms). Texture filtering doesn't change CPU
  times. In-game VRAM is full; all model textures (2.1 MB), lightmaps and most sprites are in RAM.
  Real fix would be GE skinning + GE lighting, which can't match GoldSrc's wrap lighting exactly.
  Other measured costs: TriBoxInPVS for temp entities 7.6 us/call (1.2 ms), PM_TestPlayerPosition
  ~160 us/call (brush entities can't be bounds-culled, clip hulls stick out).
- 2026-11-19 (evening): **Only 2 MB of VRAM were mapped**: `ka.prx` was never on the stick, so the
  unlock silently failed (1.2 MB left for textures after the frame buffers). Now falls back to
  KUBridge `kuKernelCall` on `sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x5BAA5439)`
  (sceGeEdramSetSize): **4 MB on 6.20 PRO**, `psp_meminfo` says which path worked. Menu images
  go to RAM. `gu_texture_maxsize` caps world/model/sprite textures, `gu_texture_vram 0` keeps
  everything in RAM. In-game texture memory at cap 256: ~6.9 MB + 0.55 MB palettes (studio 2.1,
  sprites 1.6, world ~1.7, lightmaps 0.3), VRAM full, ~3.5 MB still in RAM; at cap 16 all of it
  fits with ~1 MB VRAM to spare. **Measured, same 25-model scene before the blastcorpse1 fight:
  cap 256 27.3-29.5 ms, cap 64 27.0-28.4, cap 16 all VRAM 26.6-26.8, cap 16 all RAM 27.5-27.6.**
  VRAM vs RAM is ~0.8 ms (3%), downscaling ~1 ms more: textures aren't the bottleneck, default
  stays 256. Textures are already swizzled T8/T4 (lightmaps and rows < 16 bytes excepted);
  DXT1 would only halve sizes that don't matter. Palettes (512 B each, ~1000) are as big as
  the textures at cap 16.
  **Wi-Fi stopped working** after a TEMP-profiling client PRX (+24 KB BSS) was pushed: only
  ~32 KB of user memory were left once Wi-Fi was up, the inet module failed with 0x800208d9.
  Heap threshold 3 MB -> 3.25 MB (290 KB free in the menu now).
- 2026-11-19 (night): **Studio models skinned and lit on the GE** (`r_studio_ge`, default on):
  static per-model batches of <= 8 bones, bone matrices relative to the eye, lighting as a second
  pass from a 32-texel ramp indexed by the GE's texture matrix on the skinned normal (exact GoldSrc
  wrap lambert + gamma; screenshots match the CPU path to ~2.5%, less than two idle frames differ).
  Batches and state are CALLed sub-lists. **blastcorpse1 before the fight (25 models): engine studio
  9.7 -> 4.3 ms, frame 26.4 -> ~23.5 ms.** In the fight the gain is smaller (st_points ~10.5 -> ~7.5)
  because models lit by muzzle flashes (local lights) take the CPU path: exact local lights need the
  LinearGamma/ScreenGamma math per vertex; GE point lights would only approximate them. Writing
  per-frame vertices to VRAM instead of the RAM display list: ~0.5 ms. Left in the GE path: skin
  state blocks ~0.6 ms (rebuilt per frame), bone uploads ~0.4 ms, ramp + light block ~0.4 ms.
  `tools/dbg/shots.sh` / `fetch.sh` (with retries) take and fetch screenshots over the debug console.
- 2026-11-19 (late): **Syscall per draw removed**: in a GU_DIRECT list every `sceGuGetMemory` and
  `sceGuDrawArray` moves the GE stall address (a syscall). World/brush polygons, water, sky, sprites,
  particles, decals, triapi and 2D now use no-stall variants and update the stall every 16 draws:
  blastcorpse1 fight world 5.5 -> 4.0 ms, func_water 1.9 -> 1.4 ms (~330 polys, ~1,330 vertices),
  brush solids 0.6 -> 0.4, HUD/sprites ~0.5 less. GE skin state blocks are persistent per texture
  (were rebuilt ~70 times a frame, ~0.7 ms); the per-entity sky test cache is keyed by address
  (temp entities all have index 0) with a 32 unit radius. Fight windows now ~41-50 ms per frame
  (was ~50-55) with 47-62 models. **Bug fixed**: GE-skinned models were modulated by the last
  `sceGuColor` (the batches have no vertex color): the view model flickered red/gold after blood
  sprites and shells. Measured and left alone: mixer (fixed-point already; ~25 channels in a
  fight, ~3.7 ms), temp entity PVS test (~5 us/call, an early-exit walk didn't help), only ~1
  submodel per frame takes the CPU path for local lights. Left in the fight: studio ~13-15
  (engine draws ~6, light sampling ~1.5, client DLL ~5), world 4, brush 3.8, temp entities ~4.5,
  prediction ~4, sound ~4.
- 2026-11-19 (night): **Chrome on the GE**: one chrome mesh (gun parts, gloves, grunt goggles;
  chrome textures are also flat shaded) sent its whole submodel through the CPU path, 7-19
  submodels a frame in the fight including the view model. The skin pass now generates chrome
  texcoords from the skinned normals (texture matrix per batch, one view direction per batch) and
  flat shading reads the light ramp at a fixed point: view model 1.4 -> 0.5 ms, StudioDrawPoints
  ~6.5 -> ~4.5 ms. **Player traces cull brush entities** by the box HLDS links them with (origin +
  mins/maxs): a trace ~100 -> ~50 us, prediction ~2.5 -> ~1.9 ms per frame. **Temp entity PVS
  clusters are kept** while the box doesn't move (~70% of shells/gibs lie still): ~6 us -> ~1 us a
  test on a hit. Client DLL (hlsdk `psp-hl`, commit `psp: client: reuse the bones of unchanged
  models`): bone setups with the same inputs as the entity's last one (~20%, corpses) copy the
  cached bones, ~101 -> ~83 us per model. Each change was checked with a temporary recompute
  against the old path (0 differences). Measured and left alone: func_water is ~330 visible
  64-unit polygons (~1.7 us warp + ~2.4 us copy to the list each; none culled), studio bbox
  culling ~5-8 us a model, the mixer loops are ~25 cycles per sample and channel already.
  **Sound**: CL_ExtraUpdate topped the mix up three times a frame, ~5 paint calls a frame with a
  ~70 us setup per channel and call; extra updates are skipped while half the mixahead is left
  (1.3 calls a frame, mixing ~4.5 -> ~3 ms). With the profiler timers compiled out the firefight
  now runs at **25-30 fps** (~37 ms a frame; the timers add ~15%). World in the fight: BSP walk
  1.4, texture chains 1.9, lightmaps 0.9 ms. Tried and dropped: the child bone matrices on the
  VFPU (the whole bone loop is ~0.7 ms a frame, saved ~0.15). Uncached display list stores are
  faster than cached ones (306 vs 488 us per 64 KB, line fills), so the list stays uncached.
- 2026-11-20: **Network yield** 1 ms -> 200 us a frame: the stack's threads need ~0.6 ms of it in
  a firefight, the rest was idle (same packets per frame). Host frame outside the client: events
  (net yield, debug console, pad) ~0.8 ms, everything else < 0.1 ms. Note: a push with
  `dbgcon.py update` once came back at the menu without the debug console answering;
  relaunching from the XMB fixed it.
- 2026-11-20 (later): **Benchmark**: `timedemo` of a recorded firefight is repeatable to ~2%
  (`tools/dbg/timedemo.sh`, demo `fight1` on the stick; it includes ~8 ms a frame of demo reads
  from the stick, constant between builds). With it: the engine at `-O2` instead of `-Ofast`
  (540 KB smaller) made no difference, so code size isn't the lever. **Sky test** answers are
  also kept per 16 unit map cell (shells and gibs land near each other): sky traces 0.8 -> 0.4 ms.
  **GE skin blocks** are checked once per texture and frame (~220 meshes share ~40 textures):
  0.7 -> 0.4 ms. Client DLL (`client: psp: save the bones only for draws that set up a parent`):
  0.5 ms -> ~0. Measured and left alone: the studio bbox test's ~10 us a model are cache misses on
  model data bone setup reads anyway; batching each water surface into one indexed draw saved
  nothing (the cost is the per-vertex warp and walking the polygons, not draw commands); temp
  entities in a long fight are ~200-260 blood drip sprites (GoldSrc spawns 5-32 per hit, 1-3 s
  life), ~3.5-4.5 ms a frame of client loop work dominated by cache misses on the ~3 KB structs;
  the mixer is all 16-bit mono (half same rate, half resampled), ~35 cycles per sample and
  channel, ~3 ms at 30 channels: a VFPU version could save ~1 ms in heavy fights. The stick's
  `valve/gameinfo.txt` (generated by an older build) sets `max_tempents 500`; the PSP default is
  300 (deleting the file regenerates it).
- 2026-11-20 (evening): **Measuring.** Each TEMP profiler timer is a ~0.6 us syscall: with all
  sub-timers compiled out the timedemo goes 16.3 -> 17.5 fps. The profiler's 5 s report wrote
  three lines to `watchdog.log` (~130 ms per stick append): a ~0.35 s hitch every 5 s, now sent
  over the debug console instead. The timedemo clock starts at the demo's second frame, so it
  includes the level load (resource list + server info, ~20% of the run): fine for A/B, but
  gameplay fps is higher than it reports, and the menu stays drawn under a demo started from it.
  Ablations (`r_abl` bits, timedemo ms/frame saved): all studio models 14.4, studio draw points
  8.8, temp entity update 3.4 + their entities in the render list 9.1 (sprites themselves 1.6:
  the rest are studio temp entities, ~25 MP5 shells at ~200 us each), world 4.9, brush entities
  4.5, model lighting 3.1, mixing 0.7. Drawing every model twice in a row (cache warm) makes the
  second draw only ~20% cheaper; sorting opaque entities by model gained nothing. A stage
  sampler (TEMP `psp_samples start` / `psp_samples`: a thread counts the main thread's stage
  stack every ~1 ms, no timers in the measured code) showed the same spread: studio ~18% of
  the frame, prediction ~6.5%, world ~7.5%, brush ~6%, sound channel loop ~4.4%. A PSP froze
  during the first timedemo after adding per-bone and mixer stage marks (watchdog silent, then
  no ping): cause not found, the stage stack writes are now bounds checked against negative
  depths as well.
- 2026-11-20 (night): **Performance target set** (see the section at the top: under 60 fps is a
  bug, ~100 fps in the fight1 scene). The two PSP power-offs during timedemos were most likely
  the battery (15 years old, loose charger, found at 30% after the second), not the code.
  `r_studio_precull` (commit `054d4177`) culls studio models before the client renderer: client
  bbox calls 60-96 -> ~40 a frame, fps within noise. Tried and reverted: a cache of the floor
  light trace per model position (traces 15-22 -> 1-7 a frame, no fps change). Floor of the
  timedemo with all 3D work skipped (`r_abl 252`): 37.6 fps, but ~6 s of the 14 s are the level
  load, so ~15 ms a frame, of which ~7.5 ms are timedemo/debug artifacts (other threads: the
  debug console ~3.7 ms, demo reads from the stick ~2.4 ms, the menu under the demo ~1.5 ms).
  The network yield length (20 vs 200 us) changes nothing. CPU path for all studio models
  (`r_studio_ge 0`): 4.5 ms a frame slower. Water: 328 polygons, 1330 vertices a frame.
- 2026-11-20 (night, later): **Where a model's time goes** (engine renderer, fight1, per draw):
  soldier ~650 us = transform+cull 20, animation decode 114, bone matrix loop 74 (~2.5 us per
  bone for ~250 scalar FPU operations), save bones/attachments/events 56, lighting 38, GE
  output 150. A 1-bone prop costs 3-6 us per stage when five of the same model are drawn in a
  row and 14-20 us when it's drawn once: props and shells are dominated by cache misses (cold
  model data and code, ~0.3-0.4 us each), soldiers by scalar FPU arithmetic. Done: VFPU
  sin/cos/slerp in the engine (`1c6ed0c0`: soldier bones ~400 -> ~250 us), the engine studio
  renderer as the PSP default (`fc439acf`, now faster than the client DLL's). Tried and
  reverted: a cache of decoded animation frames (768 KB, soldier bones 252 -> 231 us, no fps
  change: the decode isn't the cost), the floor light trace cache, water batching. The debug
  console restart now leaves Wi-Fi first (`d536d059`) and the listener follows the PSP to a
  new address (`691f1ae9`). fight1 in game: 25.9 fps, 1% low 14.5.
  Next, by expected gain: the bone matrix pipeline on the VFPU; a rigid-model path (props,
  shells, gibs: cached pose and light, models drawn grouped so their data stays in the cache);
  then a PSP model format generated offline (reduced bones, pre-baked quantized animation, GE
  command lists), which removes both the arithmetic and most of the scattered data.
- 2026-11-20 (night, later still): Bone matrices on the VFPU (`67cf0eac`: a 3x4 concat with
  `vmmul`, self-tested against the C version on first use; without the distort/hologram
  effects the light transforms are a copy of the bone transforms): soldier matrix loop 74 ->
  39 us. **Static water** (`b58bddfb`, `r_water_static 1`): water surfaces are built once into
  GE triangle lists with a scrolling texture instead of the warp; only polygons that need
  clipping are still warped (fight1: ~310 of ~328 a frame from the lists), 26.3 -> 26.65 fps.
  Pitfall found on the way: a cvar registered lazily behind its own `value` check never gets
  registered, so setting it does nothing and an A/B measures the same code twice. Register
  cvars in an init or new-map hook. fight1 in game: 26.65 fps.
- 2026-11-20 (late night): Tried and reverted: **bone LOD** (distant models reuse their last
  bone setup for 1/20-1/10 s, through one old-to-new pose transform when they moved or turned).
  ~14 of ~37 bone setups a frame were reused in fight1, fps unchanged (26.7 vs 26.7): the reused
  ones are cheap models (shells, gibs, far props), and the expensive soldiers are close or
  changing sequences. At 26 fps a 1/20 s period can only skip every other frame anyway. Fresh
  profile (ms per in-game frame): studio models and other render work not split into stages
  ~10, world ~3.7, prediction ~3.8, sound mixing ~2.5 (costs the same per second at any fps:
  ~0.65 ms a frame at 100 fps), demo reads ~2.7 and the debug console ~2.2 (timedemo/debug only).
  The level load's resource list and server data are ~22% of a sampled timedemo, so read
  sampled profiles net of them.
- 2026-11-21: **Screen-size cull** (`r_studio_minpixels 2`): studio models whose bounding radius
  is under 2 pixels on screen are skipped in the pre-cull. Per-model profile before it (fight1,
  per frame): ~23 brass shells 3.8 ms (~170 us each for 20 triangles), 8 soldiers 3.7 ms,
  ~10 static props 3.5 ms (300-400 us each: light 40-145 us, GE output 95-165 us). With it the
  shells drop to ~0.15 ms: 27.0 -> 28.9 fps. Next: the props (static, so their bones and light
  could be kept between frames) and the per-draw GE output cost.
- 2026-11-21: `r_dynamic 0` by default on the PSP (`4993bc24`: no lightmap updates or model
  lights for muzzle flashes, 28.85 -> 29.4 fps), entity lights folded into the GE model light
  (`48bc9bce`: 1% low 14.5 -> 15.1), HOME quits through the system dialog (`2266b58d`).
  Static props measured by skipping them (TEMP `r_abl 2048`): ~2.5 ms a frame (29.35 -> 31.65
  fps without them). Keeping their bones and light between frames (`r_studio_propcache`) saves
  only ~0.3 ms of it, so most of a prop's cost is the per-draw path (setup, GE output). Next
  for props: a persistent GE command list per static prop, replayed with only the view
  matrix changing. Prediction runs ~1.5 player moves a frame (~37 physents each, out-of-reach
  ones already skipped): nothing to cut there without touching the movement code. Tried and
  reverted: saving bone names without string copies (no gain).
- 2026-11-21 (later): **Temp entity cap** (`f7509b73`, `cl_tents_cap 32`): a firefight kept
  150-300 temp entities alive (blood drips, shells) that the client DLL walks every frame.
  fight1 29.4 -> 33.6 fps, 1% low 15.8 -> 17.5 (cap 16: another ~0.4 fps, kept at 32 so
  explosions and muzzle flashes still get slots). Measured and left for now (TEMP `r_abl`,
  fight1 with the cap): ~140 bullet impact traces a frame from client events cost ~nothing
  (world-only traces: no change; skipping the traces saves only what their effects cost),
  impact particles ~0.35 ms, bullet decals ~0.35 ms, the impact sound's texture lookup ~0.
  The menu update under a timedemo costs nothing (its samples were the level load). The GPU
  wait at the end of a frame is 0.2%: the GE overlaps the CPU, everything is CPU bound.
  Fresh sampled profile, ms per in-game frame at ~34 fps: render work not split into stages
  (mostly studio models) ~5.4, world ~3.1, prediction ~2.9, brush entities ~2, mixing ~1.7.
  **The sampler overcounts blocking calls**: it put ~3 ms a frame in the net yield, but the
  threads' own run clocks (TEMP `psp_threads`, `sceKernelReferThreadStatus` of every thread
  over a fight1 timedemo, 25 s) show all Wi-Fi and network threads together at ~195 ms in
  25 s (~0.25 ms a frame), and a direct timer shows the 200 us yield takes ~0.45 ms a frame
  (wake-up latency). So the sampler's numbers for the debug console and demo reads (both
  blocking) are likely too high as well; time blocking work with timers or thread clocks.
  Without rendering a frame is ~8 ms (prediction, parsing, the client DLL's frame work,
  mixing). 100 fps still needs rendering at ~2 ms against ~17 ms now, and 60 fps ~8-9 ms:
  that needs the PSP model format (baked bones and animation, GE command lists) and world
  display lists, not more small cuts.
- 2026-11-21 (later still): **Exact stage profile** (TEMP `psp_stagetime`: on every stage
  change the main thread reads the clock and adds the time since the last change to the stage
  it was in; armed, it covers the in-game frames of the next timedemo). Checks: the stages sum
  to the wall time, the main thread's own run clock is 31.7 of 32.6 ms a frame (the rest is
  the net yield and higher priority threads), 2511 clock reads a frame at 0.77 us = 1.9 ms of
  overhead, subtracted below by each stage's read count. pspsdk's gprof (`libpspprof`) was
  not used: its 1 ms timer credits the function entered last, not the running code, and
  `-pg` adds a call with a divide to every function call. fight1, ms per in-game frame
  (~30.5 net, 33.8 fps untimed):

  | Part | ms | Sampler said |
  |---|---|---|
  | Render work outside marked stages (studio model setup, bones, lighting) | 6.7 | ~5.4 |
  | Prediction (c_predict + CL_RedoPrediction, traces, pmove) | 5.0 | ~2.9 |
  | World | 4.0 | ~3.1 |
  | Network message parsing (svc 41 packet entities ~1.8) | 2.75 | |
  | Brush entities | 2.7 | ~2 |
  | Mixing | 2.5 | ~1.7 |
  | Studio GE output (meshes, skin/bone/light blocks) | 2.0 | |
  | Entity and temp entity emit | 1.75 | |
  | HUD, console, 2D | 0.75 | |
  | Effects, sprites | 0.5 | |
  | Net yield (Wi-Fi threads included) | 0.46 | ~3 |
  | View model | 0.3 | |

  Without rendering that is ~13.5 ms a frame (prediction, parsing, emit, mixing), so both
  halves need cuts: rendering ~17 -> ~3-5 ms and the rest ~13.5 -> ~11 for 60 fps.
- 2026-11-21 (night): **Prediction** (user decision: float math is fine in prediction, small
  errors are ok). The client's movement code spent ~250 us per move, mostly software doubles:
  `sin`/`cos` of a double ~23 us each (six per `AngleVectors`), `sqrt` ~8 us, against 0.2-0.7 us
  for the float versions (TEMP `psp_mathbench`). The game libraries now build with
  `-fsingle-precision-constant` and `<tgmath.h>` for C (hlsdk `7a1818c4`): movement ~250 -> ~70 us
  a move, prediction 5.0 -> 3.5 ms a frame, fight1 ~1.4 ms a frame faster. What's left
  (exact stages, ms a frame): engine traces and position tests ~1.7 (~77 us each, no doubles in
  them), client movement ~0.6, player state copies ~0.4 (a `local_state_t` with its 64 weapons
  is copied for every move), the prediction loop ~0.5. **The timedemo overstates prediction**:
  fight1 was recorded at ~7-10 fps, so its commands are 100-150 ms long and each runs as 3-4
  moves of up to 50 ms (~8 moves a frame). Playing at 60 fps a command is ~16 ms, one move,
  so about half the moves per second. A demo recorded at the current frame rate would measure
  prediction closer to real play. Note: `docker-build.sh hlsdk` can fail with
  `psp/exports.c: No such file`: client and server both generate and delete that file and race
  under `make -j`; a plain `make` works.
  Then (`be055ae1`): `PM_TestPlayerPosition` traces only when the point test hits something
  (pm_shared reads that trace only when the player is stuck), and client prediction keeps the
  physents' link boxes in one small array while it runs its commands, so each trace's broad
  phase no longer reads two cache lines of every physent. Traces 0.49 -> 0.38, position tests
  0.61 -> 0.24 ms a frame. Left alone: the per-move `local_state_t` copy (~0.4 ms; the client
  reads back part of the weapon data with `-=`, so it has to be copied). Prediction ~5.0 ->
  ~3.0 ms a frame in fight1, which is now 35.6 fps (1% low 18.8; it was 33.6 before the float
  math).
- 2026-11-21 (night, later): **Prediction, all of it** (exact stages, fight1, ms a frame, net of
  timer overhead): it was 2.9 after the float math, made of client movement 0.52, traces 0.52
  (0.17 world hull, the rest the loop that rejects ~36 out-of-reach physents a trace), state
  copies 0.52, the prediction functions' own code 0.51, position tests 0.23, contents 0.22,
  footstep texture traces 0.16, setup and hull lookups ~0.2. Done: the reach cache keeps one box
  per physent and each trace computes its swept box once (`a5056aa8`), the cache also lists the
  physents that can have contents, the footstep texture is reused while the player stays within
  4 units on the same ground, the per-move state copy stops at the highest weapon slot ever
  used and the split temporary isn't cleared (`cc19b674`). Prediction 2.9 -> 1.7 ms a frame:
  client movement ~0.48 (~60 us a move, no doubles left in it), world hull ~0.2, traces' own
  code ~0.3, position tests ~0.15, contents ~0.12, the prediction functions ~0.28, setup and
  copies ~0.15. fight1 37.0 fps, 1% low 19.5.
- 2026-11-21 (early morning): **Software doubles outside prediction, clipping, pre-cull.**
  A count of `jal __*df*` calls per function in the engine ELF found the remaining hot
  doubles: `DEG2RAD`/`RAD2DEG` cast to double (every `AngleVectors`, entity transform and
  interpolation) and the delta decoder compared each field's multipliers with `1.0` as doubles
  (`cc6d3abe`: both float now, decoding math unchanged; no timedemo change, the demo skips
  entity interpolation). The mixer's per channel bookkeeping was ~60 double calls (~30 us) per
  channel and paint call; it steps the position in 32.32 fixed point now (`f9ff9c91`): channel
  mixing 2.26 -> 1.58 ms a frame. What's left there is the per sample loops (~1.45 ms for
  ~17,400 channel-samples a frame, nearly all 8-bit mono, ~28 cycles each): a cap on mixed
  channels (~28 at once in a firefight) or table lookups instead of multiplies would be next.
  **World clipping** (`8e5e4e6d`): the CPU clipped polygons against the view frustum, ~65 a
  frame in fight1, but the GE only drops primitives that leave its 4096 pixel guard band
  (~2 a frame do). The clip planes sit inside the guard band now (6x / 12x the half view), and
  each surface's box is tested once against them, so its passes skip the per vertex test
  (~800 of ~815 polygon draws a frame): lightmap pass 0.88 -> 0.50 ms, water 0.74 -> 0.49 (more
  static lists), fight1 37.8 -> 38.9 fps. Needs a look on the screen for missing polygons near
  the camera. Studio pre-cull in one pass per draw list (`06bcb12c`): ~+0.2 fps.
  **Measuring pitfall:** the stage timer showed the pre-cull at ~17 us a call cold against ~3 us
  in a loop and the batched pass saving 0.66 ms, but an A/B timedemo showed ~0.15 ms: the
  timer's own ticks evict cache lines around tiny functions. A/B small changes with timedemos.
  Fresh split of the studio work (fight1, ms a frame, ~12 models past the cull): bone setup
  1.74, GE output ~2.6 (meshes, skin/bone/light blocks), lighting ~0.55, save bones 0.39, bbox
  0.35, header 0.2. Grunts are ~4 of it (~8 a frame, ~510 us each). The world texture chains
  are ~1.7 ms for ~350 surfaces (~5 us each, ~0.2 ms of it the draws): cache misses on the
  surface structs, a display list per visible area is still the way to cut it.
- 2026-11-21 (morning): **Memory costs measured, world and studio per item costs.**
  A one-shot benchmark in game: a cache miss on a read is ~0.2 us (~66 cycles, random lines in
  1 MB), a write that misses ~0.32 us (the dirty line goes back too), an uncached store ~25 ns,
  and the GE drawing the world slows CPU misses by only ~10%. Everything below is "many small
  items at ~1-5 us each", mostly misses on scattered structs, not one hot spot.
  World (fight1, ms a frame): node walk ~1.3 (~630 nodes at ~1 us, most of it the frustum box
  tests, plus ~410 surfaces that pass at ~1.4 us), texture chains ~1.65 for ~350 surfaces
  (~3 us each: draw 0.84, texture animation check 0.61, lightmap chain 0.57, box test 0.48,
  bind 0.36), lightmap pass ~0.5, sky clipping ~0.3. No surface is ever back face culled
  in the walk: a node's own surfaces come before its back side, so the ones facing away aren't
  marked yet and the visframe check drops them. Tried and dropped: the walk's surface marks in
  their own array (skips touching ~460 rejected msurface_t a frame, no fps change).
  `GU_ClipBoxInside` on the VFPU (`d7868af8`): ~0.2 us a box warm, world chains 1.77 ->
  1.65 ms, +0.2-0.3 fps against the scalar version in the same build. Sky polygons with all
  vertices on one face skip the clipping (`c3bd6b9b`, same bounds, checked for a whole run):
  ~70% of ~29 a frame, ~0.07 ms. The node walk's side plane tests on the VFPU (checked, same
  results) gave no fps change in a same build A/B: dropped. **Builds differ by up to
  ~0.5-1 fps with no related change** (39.3 one build, 38.3 the next; code layout and the
  16 KB caches): compare changes only with a switch inside one build.
  Studio GE path (~13 models, ~170 meshes a frame): ~12.7 us a mesh (skin block lookup ~3,
  light block ~1.4, bone upload and calls ~4, the loop ~5), ~31 us a body part before the
  mesh loop. Next steps that can still give whole milliseconds, all restructuring: the world
  as prebuilt GE blocks per node side (static surfaces only, dlights/lightstyles/decals on the
  old path; ~1.3 ms), or compact per surface draw records in one cache line (~1.2 ms); fewer
  meshes per model (texture atlas per model, palettes are the problem); half rate animation
  for distant models (fidelity, ~0.5 ms).
- 2026-11-21 (afternoon): **The GE became the limit; deferred GE wait + studio texture atlas
  (committed later: `17d26a33`, `9f972138`; the to-do list below is done up to (3)).** fight1 breakdown before
  (26 ms a frame, 38.5 fps): SCR_UpdateScreen 17.1 (studio 8.0 for 19 models, world 3.9,
  ~5.2 not split yet: brush models, effects, HUD, view model), CL_ReadPackets 2.6, sound 2.0,
  CL_EmitEntities 1.8, CL_Move 1.2, prediction 0.7.
  **Finding:** `R_EndFrame` waited for the GE (`sceGuSync`) at the end of every frame, so the
  GE sat idle during the ~9 ms of game code and had only the ~17 ms of rendering to draw a
  frame. Its work per frame is about that long now: the atlas cut studio CPU by 1.1-1.2 ms but
  the CPU then waited 0.6-0.9 ms for the GE (TEMP `gpu_wait`, PROF3 slot 190) and fps didn't
  move. CPU render savings only count while the GE has headroom.
  **Deferred GE wait** (`ref/gu/gu_rmain.c`): `R_EndFrame` only does `sceGuFinish`; the new
  `R_WaitFrame` (GE sync, swap, vertex ring reset, `extGuStart`) runs at `R_BeginFrame` and
  before anything that frees or rewrites memory the GE may still read: `Mod_ProcessRenderData`,
  `R_NewMap`, `Mod_StudioLoadTextures`/`Unload`, `GL_FreeTexture`, `GU_UpdateTextureRect`,
  `VID_ScreenShot`, `GU_Shutdown`. TEMP switch `gu_deferred_sync` (1), TEMP warning
  "GU: N command words between frames" if something emits GU commands while no list is open
  (none seen in fight1). Frame shows ~9 ms later (one frame of extra latency).
  **Studio texture atlas** (`ref/gu/gu_studio.c`, "STUDIO TEXTURE ATLAS", cvar
  `r_studio_atlas` 1, built at model load): plain textures of a model (not masked, additive,
  chrome, flat shade, fullbright, remap, nomips) go into 8-bit pages (skyline packer on the
  16 px grid, up to 512x512, median cut palette per page = fidelity loss), the GE meshes of a
  submodel in a page are one merged batch list per skin family (`R_StudioGEGetMerge`, atlas
  UVs in the vertices, skin block binds the page). Memory: `GL_CreateAtlasPage` stores only
  the used rows of each mip level (`gl_texture_t.rows`, `GL_TextureRows`), and each original
  texture becomes an alias of its slot (`GL_AliasTexture`: base level only, `stride` = page
  width, own pixels and palette freed), so the CPU path and single meshes still work
  (wrapping included, but without mips). Per-mesh GE batches are built lazily
  (`R_StudioGEMeshBatches`) so merged meshes don't keep a second copy. Pages: ~1.8 MB for
  1.6 MB of textures on fight1's map; heap in game 29.0 MB without atlases, 29.1-29.3 with.
  The first version (pow2 pages, originals kept) cost +2.5 MB and ran out of memory on the
  4th map load. Results (same build, fight1): old wait + no atlas 37.6, deferred 38.2,
  deferred + atlas 39.1-39.6 fps; GE meshes a frame ~160 -> ~25 (+ merged batches).
  **To do next:** (1) the user hasn't checked the atlas on screen yet: model textures (palette
  banding, seams/bleeding at slot edges, distant models), no flicker or tearing from the
  deferred wait, map changes and the menu. (2) Commit: the working tree also holds older TEMP
  instrumentation in the same files and `stage_clean.py` mangles these files (it dropped real
  lines like `R_DrawWorld()`); stage by hand (`git add -p`) or apply
  `tools/dbg/atlas_defer_wip.patch` (untracked: diff of the 7 ref/gu files against HEAD,
  includes the old TEMP code) selectively; build the staged tree with `verify_staged.sh`.
  Probably two commits: deferred wait, then atlas. Remove the TEMP `ATLAS`/`itemkb` log, keep
  or drop `gu_deferred_sync`. (3) With the GE no longer waited on, CPU savings count again:
  split the ~5.2 ms of unattributed rendering, then the world restructuring above.
  (4) Watch the GE budget: it has ~25 ms now instead of ~17, but heavier GE work (more
  passes, unswizzled textures) can make it the limit again; `gpu_wait` shows it.
- 2026-11-21 (evening): **Render time split, brush models grouped.** The user checked the
  atlas and the deferred GE wait on screen (fine). Committed by hand from the mixed working
  tree, in order: deferred GE wait (`17d26a33`, without the `gu_deferred_sync` switch),
  upload scratch buffers from the P5 RAM (`43c1124a`, older uncommitted work the atlas
  needs), studio atlas (`9f972138`, the load log is now a `Con_Reportf`), static brush model
  grouping (`66e0c2ec`). Method that works: list `git diff -U0` hunks, apply the chosen ones
  to the index with `git apply --cached --unidiff-zero` (recompute each hunk's `+` start from
  the selected hunks only, or insertions land on the wrong lines), then fix the mixed hunks on
  the staged blob (`git show :file`, `git hash-object -w`, `git update-index --cacheinfo`).
  Some working tree hunks are older text that HEAD has since cleaned up (comments,
  `R_StaticWaterClear` prototype, `r_studio_ge`): leave those out.
  **Render breakdown** (fight1 timedemo, `psp_stagetime`, exclusive stage times, ms a frame,
  16.9 before the grouping): studio models 7.7 (bone setup 1.8, mesh submit 1.7, client DLL
  glue between the engine callbacks ~0.8 over ~20 models, prop cache render 0.6, dynamic light
  0.5, bbox 0.4, bone save 0.4), world 3.7 (chains 1.5, node walk 1.3, lightmaps 0.5),
  **brush models 2.9**, entity loops and glue 0.7, 2D (HUD 0.24, console 0.13) 0.5,
  effects/sprites/tris 0.5, frame setup and precull 0.5, view model 0.4 (its bones are set up
  twice: once for the view model events in `R_RunViewmodelEvents`, ~0.11 ms).
  **Brush models:** ~17 drawn a frame, all but 2 alpha-tested func_walls at the world origin
  (rendermode 4, 6-10 surfaces, ~3 visible), ~170 us each, nearly all fixed cost (matrix and
  model frustum ~19 us, own lightmap/fullbright/detail passes ~24, cull and sort ~27, restore
  ~10). Now the ones at the origin, unrotated, normal or alpha test, are drawn together
  (`R_StaticBrushModel`, `R_DrawStaticBrushGroup`, `R_DrawStaticBrushModels`): state and
  passes once, no matrix. 2.9 -> 1.6 ms; same build A/B 39.6 -> 41.6 fps (TEMP switch
  `r_abl 8388608` = old path). The user checked the walls on screen: fine. What's left
  there: cull and sort ~35 us a model (`R_CullSurface` misses on ~10 surfaces, dlight push).
  Note: the demo load once ran out of memory (256 KB TGA in `img_tga.c`) after a long
  session with many map loads; a restart fixed it. Watch for heap fragmentation from the
  atlas pages across map changes.
  **Next:** studio models stay the biggest bucket (bone setup and the client DLL glue);
  the world restructuring above (~1.3 ms); the view model's double bone setup (~0.1 ms).
- 2026-11-21 (night): **Props from recorded GE commands.** Skipping all small models
  (TEMP `r_abl 2048`, <= 2 bones) in fight1: 40.75 -> 44.95 fps, so they were ~2.3 ms a
  frame, ~150 us each even on a prop cache hit (cache misses all along the studio path).
  Now a prop the cache hits is recorded once (`52ff7a17`, `r_studio_proplist`): bones
  relative to its origin, its own ramp and light blocks, calls of the persistent skin
  blocks and batches; later frames test its box, send a view matrix with the origin in
  the translation and call it, ~30 us (checks ~12 of cache misses, emit ~14). The prop
  cache also takes sequences that don't play (`caccbf20`: shells, `w_357`), and
  MOVETYPE_STEP items that don't move. Same build A/B: 40.5 -> 41.0 fps (~5 props a frame
  on the fast path in the demo); the cache extension ~+0.3 fps more. The user checked the
  props on screen: fine. Note: the "studio" exclusive time (~0.8 ms) isn't client DLL
  code as the last entry says: the engine renderer is the PSP default. Most likely it is
  the stage timer's own ticks and the TEMP per-model timers between the stages (not
  measured).
  Left out of the fast path: moving props (shells in flight), chrome skins (`w_crossbow`:
  the chrome matrix follows the view), animated 1-bone models (`dirtycloths.mdl`, ~0.35 ms,
  ~120 us of it lighting), a light expiry every 0.25 s per prop (2 slow frames each).
  Still the biggest: grunts, ~0.5-0.65 ms each (bones ~150 us, GE output ~230 us).
  **Lazy bone save** (`afc68a32`): R_StudioSaveBones only keeps a pointer, the names and
  matrices are copied when a merge reads them. Same build A/B: 41.35 -> 42.3 fps. Not yet
  seen on screen with a merged model (a player's weapon on the test server).
  **The user doesn't want animation rate LOD** (bones every other frame for distant
  models): it makes the scene look low fps. Keep full rate poses.
  `sm_render` split (stage timer, ~12 models, ~23 body parts, ~3.1 ms with the timer's
  overhead): merged atlas draws 0.61 (27 us a group), submit loop glue 0.67 (view matrix,
  ramp, state, restore: 29 us a body part), per mesh draws 0.34 (14 us), render glue 0.47
  (~40 us a model), pre (GE model, merge lookup, batches, flags) 0.5, light loop 0.2. About
  150 us a model is set up around the GE commands, per body part again: candidates are
  one view matrix/ramp/restore per model instead of per body part, and the batched
  renderer of the plan above.
  **Bone setup, measured in game** (TEMP `psp_bonebench`: the last soldier drawn, 50 runs
  warm): it is FPU latency bound, not cache bound (cold only ~20% slower). Soldier, 31 bones,
  one blend: ~100 us = decode ~50 (channel decode ~1.5 us a bone, ~2 cycles per instruction,
  already tight code), matrices ~18, light copy 5, the rest; a second blend or a sequence
  change in progress adds a whole decode pass and an 11 us slerp. Done: bone matrix in
  locals, slerp by the dot sign, angle interpolation for small keyframe steps (`d3032a77`,
  41.77 -> 42.0 fps), quaternions 4 bones at a time on the VFPU (`0246fae9`: 15 -> 3.7 us
  a pass, exact, below the timedemo noise). What's left would need fewer decodes (a per
  entity cache of the decoded keyframe pair, which changes only when the integer frame
  does) rather than faster ones.
- 2026-11-21 (late night): **Whole frame budget, by switching parts off** (fight1 timedemo,
  one run each, TEMP `r_abl` bits): all on 42.8 fps (23.4 ms); studio models off 58.7
  (17.0: studio 6.4 ms); also world and brush models off 87.6 (11.4: 5.6 ms); also
  sprites, temp entity update, mixing off 107.9 (9.3 ms). That floor is nearly the whole
  100 fps budget: prediction ~3.0 ms (two player move runs a frame), packet parsing 2.4,
  render work that is left (translucent effects, view model, HUD, setup) 2.1, entity list
  1.2, the network yield 1.0, sound 0.5. The yield (`net_psp_yield`, TEMP, 1000 us of
  sceKernelDelayThread per frame for the network threads): 100 us gives 107.8 -> 117.4 fps
  on the floor; 0 starves the network threads (debug console commands get lost). 100 still
  needs a check in multiplayer. `cl_predict 0` changes nothing in a timedemo.
  Correction: the committed yield is 200 us; the TEMP cvar had overridden it with 1000, so
  the floor above is ~0.8 ms too high.
- 2026-11-22: **Floor work** (fight1, all 3D content off = `r_abl 252`). The timedemo ran ~8
  player moves a frame: after every packet the whole unacknowledged window (~280 ms of
  commands in fight1) was simulated again, and the recorded newest command (~160 ms, 4
  moves) every frame. Prediction reuse when the server agrees with it (`0193bc4a`): all on
  42.8 -> 44.5 fps, the redo is left for damage punches and the like. Floor now 129.5 fps
  (7.7 ms), all on ~45 (22 ms): the 3D content costs ~14.5 ms, 100 fps leaves 2.3 ms for it.
  Note the timedemo has a packet in ~90% of its frames (recorded at ~40 fps); at 100 fps in
  real play only every 3rd-5th frame gets one, so parsing and the redo count 3-4x more
  there than in a game. What is left of the floor is spread thin: entity state copies into
  cl_entity_t ~5 us an entity (cache misses on two 340 byte structs, 0.49 ms), physents
  0.41, delta decode 0.64, the emit loop 1.2 (every frame), HUD 0.3, console 0.14,
  particles 0.15, beams 0.14, the newest player move ~0.5. Dropping the main thread below
  the network threads (instead of the yield) locks the game up: they busy-poll once they
  get the CPU, the map load stalled and the watchdog could not get through.
  World alone (`r_abl 236`): 3.7 ms = BSP walk 1.33, texture chains 1.51 (~350 surfaces,
  two passes each, ~4.3 us a surface), lightmaps 0.48, decals 0.19.
- 2026-11-22: **World surfaces** (fight1, `r_abl 236` = world only). Per frame the walk visits
  ~550 nodes, ~880 node surfaces, ~470 of them not marked by a visible leaf (the back faces
  too: a node's own surfaces are tested before its back side's leaves mark them, so the
  backface test never culls one), ~380 drawn. Switching parts off showed the cost is the
  cache misses on the scattered per surface structures (msurface_t, mextrasurf_t, texinfo,
  texture, polygon), not GE commands: the texture draw ~0.3 ms, the guard band box test
  0.29 (mostly reading `info`), the lightmap chain and pass ~0.6. Now each world surface has a
  32 byte copy (short box, vertices, texture, lightmap page, flags) that the leaves mark and
  the visible ones are chained through per texture and per lightmap page; random tiling
  textures get their frame at load, animated ones once per texture per frame. Dlit, decal,
  light style, fullbright/detail, water, conveyor surfaces still go through
  R_RenderBrushPoly (~10 a frame). Light style 0 arrives from the server after the
  lightmaps are built: every copy is marked stale then and redrawn the old way once, which
  updates its lightmap. The sky polygons (~30 a frame, ~10 us each cutting them at the box
  edges) only mark the sky as seen, and the box faces are drawn whole behind the world.
  World 3.7 -> 2.0 ms (walk 1.33 -> 1.07, chains 1.51 -> 0.35, lightmaps 0.48 -> 0.22),
  world only 83 -> 97 fps, all on ~45 -> ~48. Screenshots of the old and new path at the
  spawn, at the sky and at fresh decals: identical (only the fps counter, console text and
  the view model idle differ). 190 KB for blastcorpse1's 5987 surfaces.
- 2026-11-22: **BSP walk** (fight1, `r_abl 236`). Per frame: ~530 nodes in the PVS (~140
  leaves), ~220 children outside it, ~240 node and ~160 surface box tests, ~700 leaf marks.
  Nodes and leaves are copied in DFS order into 32 byte records (short box, plane type and
  distance, children, surface range, parent; leaf 0 is shared by all solid children),
  blastcorpse1: 4302 records, 6250 marks, ~150 KB. Node and surface visframes are 16 bit
  arrays of their own (refilled when they could wrap), so a child outside the PVS or a
  surface no visible leaf marked costs no record read. One VFPU test per box gives the
  frustum side planes it is behind or in front of and the clip planes (GU_ClipBoxInside) it
  is in front of; the clip planes are clip flags like the frustum ones, so a node inside the
  guard band spares its subtree the test (node boxes are grown at load to hold their children
  and surfaces). Pitfalls: reading the VFPU CC with `mfvc` right after `vcmp` gave stale
  bits (~10% wrong answers), the distances are stored with `sv.q` and their sign bits read
  instead (`mfv` of the 12 was slower); a check against the scalar tests found no mismatches.
  Walk 1.21 (old walk, same build) -> 0.92 ms; screenshots of both identical. Ablations of
  what is left: no surface culling 0.55, no leaf marks 0.64, neither 0.49 (the plane reads
  0.01, efrags 0.04); doing every box test twice costs +0.26.
- 2026-11-22: **Working tree cleanup.** HEAD couldn't play fight1 (out of memory loading the
  demo): memory and robustness work from earlier sessions had never been committed and sat in
  the working tree among the TEMP probes. Committed by theme (`23adbf2c`..`80e5b234`): entity
  array and packet ring kept between connections, server tables on the heap only for a local
  server, loopback buffers on first use and fresh sockets on a Wi-Fi retry, bzip2 buffers from
  the P5 RAM, HTTP connection/receive buffer limits, decoders that give up on a failed
  allocation, 64 KB file copy buffer and 300 temp entities, textures without data or memory
  no longer fatal, studio models skipped when the display list is nearly full, the timedemo's
  slow frame list, and particles with a float client time (bullet impact particles 0.175 ->
  0.13 ms a frame: each allocation did ~5 software double operations). Older text that HEAD
  had since cleaned up (comments, stick axes, the yield) was dropped. **The clean build runs
  fight1 at 54.3 fps all on; the probes cost ~2 ms a frame** (~48-50 with them): measure fps
  on a clean build, use the probes only for the split. The probes are back in the working
  tree on top (one `TEMP` per line or block, as before). Floor findings: physents built from
  the visent copy saved nothing (0.41 -> 0.39), processing the packet in reverse order (states
  still in cache) saved nothing (the cost is the ~3 KB cl_entity_t, ~15-25 lines an entity a
  pass), -O2 instead of -Ofast no better (floor 127.6 vs 129.0 fps), the timedemo's svc_tempentity
  is ~0.34 ms a frame (gunshot decals ~1 ms each: particles, decal, ricochet sound), an
  explosion sound loads from the stick mid fight (one ~78 ms hitch, by design: sounds load on
  first use to keep the heap free).
- 2026-11-22: **Clean floor/content split, keyframe cache, batching bound** (fight1).
  `r_drawworld` (`42ca6b89`) with `r_drawentities`, `r_drawviewmodel` and
  `r_draw{particles,beams,tracers}` splits the timedemo on a build without probes: all on
  53.5-54.0 fps (18.6 ms), all 3D off 139.2 (7.18 ms), world only 104.0 (world 2.43 ms),
  everything but entities 94.8 (10.55 ms: entities 8.05 ms), world off 61.5. At 100 fps the
  3D content has ~2.8 ms, against 11.4 ms now. Tried and reverted: a cache of each
  animation's decoded keyframe pair at an integer frame (2-way, 32 entries of 64 bones,
  48 KB): ~52% hits, but bone setup only 1.65 -> 1.61 ms (most lookups are small models; a
  hit saves ~5 us), fps unchanged (53.3 vs 53.75 all on, 60.7 vs 61.5 world off). The decode
  is not what bone setup costs: the per bone float work around it is. Batching bound
  (probe build, `r_abl 4` / `r_abl 1024`): studio models cost 6.6 ms a frame cold, a warm
  repeat of every draw 5.9 ms, so cache misses are ~10% of the studio cost.
- 2026-11-22: **Brush model surface cull; VFPU bone values tried** (fight1). The static brush
  models' pass (`bg_sort`, 0.62 ms a frame) was 0.47 ms of R_SortBrushModelSurfaces: ~177
  surfaces a frame through R_CullSurface (surface, texinfo, texture, plane, extents: ~2.6 us
  each, cache misses). A 32 byte copy of the plane and extents for the world BSP's brush
  models, water included, and the frustum planes the model's box is inside dropped per model
  (`c3d14579`): fight1 53.7-53.9 -> 54.7-54.9 fps on clean builds. Same surfaces and cull
  types. Bone math: per bone (hgrunt, 31 bones, warm) the decode of the keyframes is 0.46 us,
  the float values 0.36 us and the lerp/angle tail 0.52 us of 1.34 us. Tried and reverted:
  the values, lerp and angles on the VFPU, one bone at a time (channels 0-3 and 2-5 as two
  quads): exact, but slower (1.65 vs 1.3 us a bone): filling the aligned record of keys and
  controller values alone is 1.0 us, and the VFPU's latencies, ulv.q and reading the result
  back cost ~0.6 us. Four bones a pass would hide the latency, but the per bone gather costs
  as much as it saves. The bench's "getanim" figure included a calcrot loop (it's ~4 us).
- 2026-11-22 (evening): **Sound, water, brush models, instruction cache** (fight1, clean
  builds, A/B of 2-4 timedemo runs each). Mixing at 11 kHz (`c1e4bef8`, most HL sounds are
  11 kHz and mix at the unit rate; the 22 kHz ones lose their top octave): 54.6 -> 55.5.
  Static water whose box is inside the clip planes in one draw (`789a94b7`: the brush model
  water was ~100 us a surface of per polygon clip tests): 55.75 -> 56.8. Static brush model
  surfaces through the world's surface chains (`d35978a2`, ~55 of ~60 a frame, most of them
  kRenderTransAlpha func_walls) and dlights culled by the brush model's box (`dd97b2f2`):
  56.75 -> 57.1, screenshots identical. **The per model studio overhead is instruction
  cache misses**: `R_StudioCheckBBox` is 15-18 us cold, 3.5 us called again right away,
  16 us again after `sceKernelIcacheInvalidateAll` (only ~9 after a data cache flush).
  Cold/warm per phase, us a frame: header+transform 88/35, dynamic light 207/81, bbox
  125/31, light setup 56/43, GE output 814/498. The studio code a model can run is ~69 KB.
  Solid studio models in passes over the list (`487172ca`): 56.9 -> 57.9. Tried: `-Os` for
  everything (EBOOT 4.9 -> 3.8 MB) is slower, 57.5 -> 55.7 (loses inlining in hot loops);
  recorded props replayed in the precull pass: only ~4 a frame hit, the rest were tested
  twice, no gain. Bone setup itself is FPU bound (amortized over 30 bones), and a full
  pre-decode of the animations doesn't fit (hgrunt ~1.5 MB of quaternions). Mixing is still
  ~1 ms a frame at ~50 fps (26-28 channels, most through the pitched loop: 22 kHz sounds and
  random weapon pitch). A third pass for the light alone (bones and light restored
  per model) was no faster: 57.7 vs 58.0 (invalid: `r_studio_batch` was left at 0 in the
  config, see below).
- 2026-11-22 (late): **GE lighting for studio models** (`53253f1c`): the plan path lights
  with one GE directional light toward -lightvec and the global ambient (the ramp's colors
  edge on and facing the light) instead of drawing every batch a second time through the
  ramp texture: no ramp, no light block, half the draws, one skin block per mesh. The side
  away from the light stays at the edge on color (a bit lighter). 57.65 -> 60.1 fps clean,
  frames under 60 ~285 -> ~215. After it, the two pass studio batch gives nothing (60.0 vs
  60.25, three passes 60.5 vs 60.7): removed (`5bb434f7`). Batching the client's packet
  entities (DLL AddEntity, renderer lists, effects in three passes) was slower, 57.1 vs
  57.75. Pitfall: `FCVAR_ARCHIVE` cvars toggled for an A/B are saved to config.cfg on the
  next restart and carry over to every build after it. Clean split now (ms a frame): all
  16.6, entities ~6.9, floor 6.6, world 2.3, view model/particles/beams/tracers 1.05.
- 2026-11-22 (night): **Floor and studio leftovers** (fight1 clean A/B): 8 bit mono mixing
  with one multiply for both channels and the volumes in locals (`99711055`, 59.95 ->
  61.35); the floor light of a studio model kept per entity for 0.25 s while it stays within
  2 units (`a6cc3a30`, 60.6 -> 61.55: where the trace down finds no light direction four
  more traces build a gradient, an animated prop was ~120 us a frame); small rigid models
  (<= 2 bones) skipped under 6 pixels instead of 2 (`d3e1a637`, 60.9 -> 64.0: a shell goes
  at ~120 units, an item at ~400, players and monsters keep 2); the channel position to
  fixed point from the double's bits (`e3af08ef`, 63.95 -> 64.2); the view model's bones
  set up once a frame instead of twice (`b37b0ab8`, 64.1 -> 64.7). Sound in the fight:
  ~47 channels at once, mostly loud (a higher inaudible threshold drops few). Temp entity
  traces are for flying ones only (landed ones drop their collide flags). Studio GE output
  after the GE lighting, per frame: bone uploads ~0.25 ms (~3.5 us a batch), light setup
  ~0.15, skin blocks ~0.15, the rest of the plan loop ~0.4.
- 2026-11-22 (night, 2): **Caps** (fight1 clean A/B): decals from shots and blood capped at
  100 (`r_decals_max`, the map's own permanent decals on top; `0ed4523d`, 64.25 -> 65.2);
  only the 16 loudest sounds mixed, the others move on silently (`s_mixchannels`,
  `c66cb45c`, all 65.3, 24 65.8, 16 66.0, 12 66.2). Clean split at 66 fps (15.15 ms): the
  studio models the precull can drop are 4.1 ms (all culled: 90.4 fps), other entities 1.65,
  the rest 9.4: with no studio models at all the frame would still be 11 ms. Toggles:
  HUD ~0.1 ms, particles ~0.4, beams/tracers ~0.3, view model ~0.6 (drawn last, the studio
  code cold: drawing it right after the solid entities gave nothing). Tried and dropped:
  props that move (flying shells) drawn from commands recorded relative to the entity with
  its transform in the view matrix, chrome kept as recorded (+0.15 fps, not worth changing
  how every prop keeps its bones and light); temp entity collision traces (landed ones
  don't trace anyway). hgrunt has 2 blend sequences only for crouched shooting; player.mdl
  has 52, with ~70% of the bones identical in both blends (an exact saving for players).
- 2026-11-22 (night, 3): delta entities read only the sent fields over a copy of the old state
  (`0bb78023`, 65.95 -> 66.15). `#pragma GCC optimize( "Os" )` on gu_studio.c (icache): no
  change. **Media Engine** (`7889528f`): the second CPU (same clock, no VFPU, no syscalls,
  caches not coherent with the main CPU) runs engine code from main RAM, started from kernel
  mode via kuKernelCall with a reset handler at 0xbfc00000 (mcidclan's me-minimal-handler).
  Empty job round trip 15 us; as fast as the main CPU; ME polling its control block flat out
  (~600k uncached reads/s) cost the main CPU ~1.5 fps, one read per ~10 us costs nothing. The
  mixer's sample loops run there a frame behind (fight1 66.15 -> 66.35, and all channels mixed
  again: the 16 cap only applies without the ME; `s_mixme 0` mixes on the main CPU). While our
  code runs on the ME, Sony's ME codecs (sceMp3/sceAudiocodec) can't be used: MP3 music and
  Opus voice would be software decoders as ME jobs. The VME (reconfigurable DSP beside the ME,
  loaded by Sony's firmware) is undocumented. Next ME candidate: bone setup (~1.3 ms in fight1).
- 2026-11-22 (night, 4): sound back to 22 kHz with the ME mixing (`dfb3ccf2`, 66.4 -> 65.7: the
  main CPU's DSP, gain and transfer doubled), then those on the ME too (`df6a341f`, 65.65 ->
  65.95): the ME job adds the main CPU's raw/sentence buffers, runs the room DSP loops (their
  state and delay lines cache line aligned, their own random seed; presets and allocation stay
  on the main CPU), the gain and the clipping; the main CPU copies 16 bit samples to the DMA
  buffer. Left on the main CPU for sound: spatialization and the per-channel bookkeeping.
- 2026-11-23: ME jobs queue (`5331f815`, tickets, the mixer waits for its own) and studio bones
  on the ME (`9458e2db`, 65.8 -> 67.9 fps): before the world is drawn the main CPU gathers each
  unculled animated model's frames, blend weights, controller adjustments and transform, one ME
  job computes all their bones with plain float math (no VFPU there), and R_StudioSetupBones
  takes them if a key of the entity state, time and transform still matches. fight1: ~9 models
  a frame posted, ~7 used, no misses, no waits; within 0.08 units of the VFPU results.
  `r_studio_me 0` turns it off. Writeback-only instead of writeback-invalidate before a job:
  no difference, kept the invalidate (the mixer's DSP state is read back by the main CPU).
- 2026-11-23 (2): **World, brush models and whole studio models on the ME** (fight1, clean
  builds, 68.2 -> 80.0 fps). World walk, surface culls and the GE commands of both passes as an ME
  job (`de1a6eb9`, 68.2 -> 73.4): scalar box tests, called lists, the main CPU keeps the slow
  surfaces, the clipped polygons and the efrag leaves; ~1.15 ms on the ME, with the old order the
  main CPU waited 0.7 ms for it, so the view model (if nothing of it blends: its depth range is in
  front of everything) and props are drawn first. Static brush models the same way (`e3fd377e`,
  74.9). Solid models drawn from plans: cull, attachments and all GE commands on the ME, 36 us a
  model left on the main CPU (events, light, two calls) (`3602291d`, 77.8; hgrunt has smooth and
  flat shaded meshes: a list for each). Their bones are posted right after CL_EmitEntities for the
  models visible in the last frame: posted after the world they kept the main CPU waiting ~1 ms.
  The renderer is linked with local symbols: the engine calls it through a hook in me_psp.c.
  Pitfalls: a job struct the ME writes to must not share a cache line with anything the main CPU
  writes (the ME's write back put old values over the main CPU's: the ME hung after ~150 frames
  and everything ran on the main CPU, only visible as lower fps and the "ME: stopped answering"
  line); flushing the ME's whole cache after every model was slow; the ME can't read the system
  timer (0xbc600000) and its COP0 count stands still, so time jobs from the main CPU. Waits move:
  each saving on the main CPU showed up as a longer wait for the ME until the ME was faster too.
- 2026-11-23: **GE time** (fight1). Measured with the GE held by a jump to itself at the start
  of the list, released after sceGuFinish: ~9 ms a frame alone (world ~4, entities ~4), ~4 of
  them vertices and commands (a 32x32 scissor), ~5 fill; not sensitive to texture filter or lod
  bias. In a normal frame it starts ~2 ms after the list is opened and the main CPU waits only
  ~0.1-0.4 ms for it, but while it draws (during the next frame's floor too) it competes for the
  bus: the floor is ~2 ms slower than without drawing. 4 bones a skinned batch instead of 8
  (`37bcb018`): 9.0 -> 8.3 ms; 3 no better and more CPU. Lightmaps were in RAM, the VRAM (4 MB
  here) was full before they were built while 512 KB were kept for CPU skinned vertices
  (`63437061`, +1.5 fps). Front to back inside a texture: no difference. VRAM/RAM use in KB:
  lightmaps 288/16, world 1438/0, models 409/1941, sprites 349/895, HUD and menu 500/1215.
  Render split on the main CPU now, ms: cull and gather 0.65, setup and dlights 0.3, view model
  0.28, world 0.55 (slow surfaces, decals, sky), brushes 0.15, ME models 0.25, other solid models
  ~0.2, particles and beams 0.38, translucent 0.35. Next: HUD and menu images out of the VRAM for
  models, fewer GE vertices, players on the ME, culling on the ME.
- 2026-11-23 (3): **Culling and players on the Media Engine** (`e8879404`, fight1 clean 80.0 -> 84.1).
  The ME tells the entities of the draw lists apart (a table of 32 byte entries: bounding sphere,
  model, body and skin, who may draw it, brush models ready to queue, sort keys of the translucent
  ones) before the view is known; the main CPU tests spheres against the planes. Preparation on the
  main CPU (cull, gather, brush models, sort) 1.0 -> 0.4 ms. Alone that gave only +1.8 fps: the ME
  was the longer path then and the main CPU waited 0.5 ms for the models. What got it back: two
  early jobs (one after CL_LinkPacketEntities, before the temp entity update, ~0.75-1.2 ms earlier
  in fight1), the early job giving way to the world's between two models, the world posted before
  the view model's events and the sort. On the ME alone (main CPU waiting): early 1.3-1.6 ms, world
  0.9-1.1, models 0.4-0.5, brush models 0.2. Left to wait: world ~0.1 ms, models ~0.1 ms, GE ~0.06.
  Players: gait on the main CPU before the first job, then blends, legs of the gait sequence,
  weapon model merged by bone name, draws with the remapped skins (no atlas). Tried with 3 fake
  players (gordon, barney, gina walking with an MP5, TEMP code, c1a3a live): 15.1 -> 12.1 ms a
  frame, same picture. A player is ~560 us on the ME after: each bone computed only in the sequence
  that decides it, one rotation where both blends agree, normalized lerp below ~25 degrees (1 ms
  before). Pitfall: `__attribute__(( aligned( 64 )))` on a variable doesn't pad its size, the last
  cache line is shared with whatever follows: align the type of anything the ME writes. A Wi-Fi join
  that never finishes after a pushed restart looks like a crash from the PC, the game was fine.
- 2026-11-23 (4): **Light and translucent models on the Media Engine** (`8eed13a6`, fight1 clean 84.1 ->
  88.6, frames under 60 5-8 -> 4). Measured before, us a frame on the main CPU: light of the ME's
  models 115-160 (15 a model), their events 100, commands 60, fading grunts 140-310 (1.3 ms each,
  drawn on the CPU with every vertex), props and shells 530-850 (crossbow clips, shotguns, shells:
  recorded commands that had to be recorded again every quarter second, or lit 5 traces at a time
  while they fly). Now: those props always on the ME (they were only there between recordings), a
  fading grunt 32 us, events only looked at if the ME says there may be one. The light on the ME has
  its own traces and caches (sky by entity and 16 unit cell, floor for 0.25 s within 2 units, 16 for
  models of 1 or 2 bones). Translucent: blend function and alpha (the global ambient's alpha) from the
  main CPU, the ME turns blending on after each skin block. All that alone gave +1 fps: the main CPU
  waited 0.5-0.6 ms for the models job (0.5 -> 1 ms on the ME). It doesn't need the commands, the GE
  does, and that one is still at the world: the lists are called through gates (BASE, a jump to
  itself, replaced with a NOP by the ME with uncached writes), events and attachments moved to
  R_EndFrame, the brush models' job before the models'. +3.5 fps. What the ME can't draw (no space, a
  sequence group not loaded) is missing for one frame, then the main CPU draws it for half a second.
  Lights made by an event show a frame later. Compared with the main CPU: solid and
  half transparent grunts look the same up close, additive ones from afar; a live firefight ran without
  ME errors (its pictures weren't compared). A pushed restart left the Wi-Fi join hanging twice
  more (menu running fine both times); cause not known.
- 2026-11-23 (5): **Wi-Fi with -dbgcon**: the PSP stayed offline after pushed restarts with the menu
  running fine. The join was already retried forever (and blocked the main thread for up to a
  minute), but once joined the link was never looked at again. Now the join is a state machine polled
  every frame (first frame after 10 s instead of 23-42 s), the link is checked every second, and a
  link with nothing from the PC for 60 s (doubling, only while not in a game) is left and joined
  again; output is kept while the PC is silent. Tested: pushed restart (5 attempts, 27 s), listener
  off for 100 s (left after 60 s, back after 9 attempts and 44 s), then a server connect. Whether the
  outages were dead links isn't known, `wifi.log` on the stick should tell next time.
- 2026-11-23 (6): **Waits, the bus and temp entities** (fight1 clean 87.8 -> 102 fps, frames under 60:
  4 -> 2). Timers in the clean build (PSP_MeWaitFor, sceGuSync): the main CPU waited 0.36 ms a frame
  for the GE and 0.35 ms for the world's job, which is why savings on it hardly showed. Steps:
  - Temp entities on the ME (`2c27b78f`): it runs HUD_TempEntUpdate's simulation (gravity, bounces off
    the world, animation, fading, PVS) after the packets are read, while the main CPU predicts and
    links, and only reads them; CL_TempEntUpdate applies the results. Callbacks, players, entities,
    sine waves: still the client DLL. **The benchmark didn't use it at first**: CL_LerpPoint sets cl.time
    in timedemos after the job is posted and the results were dropped; found with the stage profile
    (`b9e3826c`, 95.4 -> 99.6). Visents filled in on demand (16 KB of writes a packet).
  - Two display lists (`9102f2be`, 90.3 -> 92): the next frame is built while the GE draws the last one,
    shown by the GE's finish interrupt. Everything it reads that changes every frame is there twice.
  - The sound update while waiting for the world (`e6d386f5`): world wait 340 -> 60 us, but no fps.
    With the waits gone the main CPU itself got slower: the GE and both CPUs share the bus.
  - So: textures (`4b32313f`, 92 -> 95.4). gu_texture_maxsize 32 gave 98.2 and 64 97.1, 128 only 93.4:
    it's the fetches from RAM. The textures drawn lately now move into VRAM (one a frame), the cold
    ones out: a run of neighbouring cold and free blocks, the cheapest. First versions: evicting
    until the largest block fits never ends when the VRAM is fragmented (2 MB of VRAM empty, the heap
    full); one victim that is big enough is hardly ever there. A NULL palette must never get to the
    GE (address 0 can hang the console).
  - Light styles and ideal pitch (`b1ab7a4e`, 99.6 -> 102): 64 structs of 1.2 KB scanned every frame
    with double math, 6 traces for the ideal pitch.
  The PSP switched off once, right after a single player map had loaded, with a build that had the
  endless eviction and the palette bug; the same steps ran through with the fixed one. Cause not proven
  (the battery was fine). Memory: +100 KB, stays flat in a long live fight.
  Left on the main CPU (stage profile, ms): packet entities ~2 (states copied twice, physents), linking
  and adding entities ~0.9, the net yield 0.25, prediction ~1. A frame now and then takes 90 ms in some runs.
- 2026-11-23/24: **Stress scene, levels of detail** (`867e70f5`, `6f6332f3`, `a753dbe9`, `ad07adb1`).
  A metamod plugin on the test server fills crossfire with 31 bots and 32 monsters in combat (up to 200
  studio models in view, 56 of them dropped rifles). Demo `stress1` has it; memory is nearly gone there
  (31 of 32 MB), a second join in the same session can run out (Host_Error, back to the menu).
  - First it didn't survive: entities now come in chunks of 16 instead of 3 KB each for all 2047, player
    models are only loaded if they fit, the models' GE commands are budgeted exactly (they flickered),
    and whatever frees world memory waits for the ME and the GE (R_DropFrame).
  - The ME's model job is the limit there. Levels of detail are made from the models at run time: bones by
    how little they move against their parent in 10 frames of the model's own sequences (root mean square,
    with a limit), triangles by edge collapse in the frame closest to the others. The first version merged
    bones by size in the default pose down to 8: legs welded to the pelvis, off by 40 units, the user saw
    broken models at once. Now 0.3 units on average,
    ~15 at worst, 48 on the garg (PC measurement, 4 frames of each sequence in the model file).
    Commands of a grunt 1352 words -> 604 -> 484.
  - Poses are kept for 2-3 frames at those levels, longer and for all models (`frame->hold`, 0-4) while the
    main CPU waits for the ME; the light traces as well. 224 models instead of 96 in the ME's list: the
    ones that didn't fit cost 0.2-1 ms each on the main CPU. Same when a server update can't be used and
    the lists stay: the ME had no table then and the main CPU drew all 200.
  - The coprocessor 0 count register stands still on the ME: for a profile it stores what it's doing in an
    uncached word, the main CPU's sampling thread reads that.
  - stress1: about 10 -> 20-21 fps (with probes). fight1 (recorded again, the old one was lost): 106 fps with levels of
    detail, 103 without, 5-9 of 1800 frames under 60.
  - Demo recording took ~40 ms a frame (dozens of 1-40 byte writes to the stick): buffered. `timedemo in
    game` counted server updates instead of frames; older numbers can't be compared with the new ones.
  - The level of detail code ran on the PC first (extracted from gu_studio.c, 32 bit, address and undefined
    behaviour sanitizers, all models of the game, failing mallocs, unloads in the middle). It found a
    stack overflow that had switched the PSP off, and `doctor.mdl`, whose bones have parents out of range.
  - Left in stress1 (ms a frame, ~45): packets ~9, the ME wait ~5, entities 6, sound 2-3, world 4-5. Sounds
    that play for the first time stop the game for 30-95 ms each (open and read from the stick).
