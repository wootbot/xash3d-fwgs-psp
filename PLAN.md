# PSP port plan

Goal: current upstream Xash3D FWGS on the PSP, able to join official GoldSrc (HLDS) servers using the
GoldSrc protocol and Steam tickets from steam-broker. See [CLAUDE.md](CLAUDE.md) for context, build
and debugging notes.

Base: upstream `master` @ `11ed246c` (2026-11-08). Reference: `reference/xash3d-fwgs-crowbar-psp`
(upstream Oct 2020 + PSP port, REF_API v1) and `reference/hlsdk-portable` (`psp-hl`).

Status legend: `[ ]` todo, `[~]` in progress, `[x]` done. Each phase ends with a milestone that must be
verified **on the PSP**, not just compiled.

---

## Performance target (set by the user, 2026-11-20)

**A frame under 60 fps is a bug.** The ceiling scene that must hold 60 fps: **32 players and 32
monsters on screen in heavy combat, in a ~1000 world polygon area**. The current benchmark scene
(the `blastcorpse1` firefight, demo `fight1`: ~10 grunts, a few dozen shells and gibs) is much
lighter and must reach **~100 fps**, which is the headroom for the ceiling scene. 30 fps is not an
acceptable goal.

**Anything goes to get there.** Readability matters, fidelity does not:
- Decimate models (even to ~20 polygons), cut bones, shrink textures (even to 16x16).
- Cheaper or approximate lighting, effects and sounds. Caps on temp entities.
- Bypassing the client DLL's per-model studio rendering.

What still has to hold: GoldSrc protocol and prediction/delta math stay exact (see CLAUDE.md),
multiplayer works on official servers, and the screen stays readable (who is where, who shoots).

Frame budget at 60 fps (16.7 ms) for the ceiling scene, roughly:

| Part | Budget | Measured 2026-11-20 (fight1) |
|---|---|---|
| Net parsing, prediction, HUD, client frame, sound | <= 4 ms | ~7-8 ms |
| World, ~1000 polygons | <= 2.5 ms | ~3-4 ms |
| 64 animated models | <= 6 ms (~90 us each with animation) | grunt ~550 us, MP5 shell ~200 us |
| Effects, temp entities, sprites, decals | <= 2 ms | ~3-9 ms in a firefight |
| Slack | ~2 ms | |

**What the numbers say so far** (details in the progress log):
- Per-model cost is mostly **fixed overhead and bone work, not geometry**: `shell.mdl` is 20
  triangles and 2 bones and still costs ~200 us; a 790 triangle, 30 bone grunt ~550 us.
  Decimating meshes alone won't get there. `scripts/psp/mdlstat.py` prints bones, triangles and
  textures of models.
- The profile is flat: no single stage is above ~5% of the frame. Removing one small cost
  (light traces, a few draw calls) usually doesn't show at all. Only structural changes that
  remove whole stages for many models at once have moved the number.
- Engine built-in studio renderer vs the client DLL's: same speed overall (props and shells
  ~15% cheaper, grunts slower without the client's bone cache).
- GE skinned models save ~130 us each over the CPU path; muzzle flash entity lights used to
  push ~20 models a frame in a live firefight back to the CPU path.

**Plan, by expected gain:**
- [~] Benchmark that measures what matters: `timedemo` now also prints the in-game frames only
      (no level load), the 1% low, the minimum and how many frames were under 60 / 100 fps.
      Still to do: a stress scene with many bots and monsters on the test server.
- [ ] Batched studio renderer in the engine: run each phase (transform + cull, bones,
      lighting, GE output) for all models in a row instead of the whole pipeline per model.
      First check with the TEMP `r_abl 8192` experiment whether warm repeats of bone setup and
      lighting are much cheaper than cold ones (I-cache/D-cache misses), which is what batching
      would buy.
- [ ] Animation: decode only the bones that matter (reduced skeletons), cache decoded frames.
- [ ] PSP model variants generated offline (fewer bones, decimated meshes, small textures),
      loaded by the renderer only, so server consistency checks still see the original files.
- [ ] Shells, gibs, blood drips: tiny batched meshes/sprites, caps on count and lifetime.
- [ ] World: prebuilt GE display lists per visible area, fewer state changes.
- [ ] The non-render floor: engine-side traces, delta parsing, HUD and client frame (same math).

---

## Phase 0 — Setup

- [x] Add `reference/`, `tools/psplink/` to `.gitignore` (build output: `build-*` already ignored).
- [x] Add `upstream` remote (FWGS/xash3d-fwgs).
- [x] PSPLink PC tools: official `pspsh-windows.zip` from pspdev/psplinkusb v3.2.1 (matches PSPLink 3.2.1
      on the PSP, cygwin + libusb-win32) in the ignored `tools/psplink/`, with `usbhostfs.bat` and
      `pspsh.bat`. psp-gdb via Docker still untested. **usbhostfs root arguments don't work** with
      this build (`host0:` stays unreadable for relative and `/cygdrive` paths); roots that aren't
      given default to the current directory, so start `usbhostfs_pc -g` *from* `build-psp/`.
      pspsh can be driven by piping commands into it (`(printf 'cmd
'; sleep N) | pspsh.exe`).
- [x] Docker build: `scripts/psp/docker-build.sh` (persistent `xash3d-psp-build` container, tar sync,
      `configure|build|all|shell|exec|hlsdk|rm`) -> `build-psp/` (`EBOOT.PBP`, `xash.prx`, `xash.elf`).
- [x] Import-stub audit `scripts/psp/check_stubs.py`, run automatically after every engine and game
      PRX build. Verified it catches the split-library bug (19 unresolved stubs with `-lpsputility`
      listed early, 0 with the fixed link line).
- [~] Game PRXs: `docker-build.sh hlsdk` builds `reference/hlsdk-portable` (`psp-hl`) and installs them
      as `valve/dlls/hl_psp_mipsel.prx` and `valve/cl_dlls/client_psp_mipsel.prx` (current engine's
      library naming, see below). 0 unresolved stubs. Still todo: keep them with the PSP data
      (configs from Crow-bar release `2603`, `extras.pak`).

## Phase 1 — Toolchain and engine bring-up (no renderer)

Upstream already contains a partial 2023 PSP scaffold. Fix and complete it:

- [x] `scripts/waifulib/xcompile.py` `PSP` class: fixed `PSP_SDK_ENVVARS` NameError and the stdlibs
      link-order bug, `-std=gnu11` (C) / `-D_GNU_SOURCE` (C++), `NULL` render type, `psp-ld` for
      relocatable links, `PROFILING` option guard. **`int32_t` is made an `int`** with
      `-U/-D__INT32_TYPE__` & co (psp-gcc makes it `long`; newlib follows the macros). This removed
      every format/`CRC32_t*` error in shared code and the mainui miniutl clash without source changes.
- [x] `scripts/waifulib/psp.py`: imports, `configure()` sets `STATIC_LINKING` (filesystem_stdio, menu,
      renderer by render type) and EBOOT parameters; EBOOT/PRX tasks only for real build targets;
      installs only `EBOOT.PBP` + `xash.prx` (+ `valve/extras.pk3`).
- [x] `common/defaults.h`: `XASH_LIB LIB_PSP` typo fixed, `XASH_ALLOW_SAVERESTORE_OFFSETS`.
      Not `XASH_INTERNAL_GAMELIBS`: game libraries come from `gameinfo`/`liblist.gam` paths.
      `xash3d_types.h`: PSP keeps `MAX_QPATH 64` (GoldSrc), `MAX_MODS 16`. Note: mainui's copy of
      `xash3d_types.h` (submodule) still says 48 for PSP; harmless unless it ends up in a shared struct.
- [x] `wscript` / `engine/wscript`: PSP defaults (no GL/soft, `NULL` renderer for now, bundled deps,
      stb_truetype, **no mbedTLS** (HTTPS downloads only), no libbacktrace, no SDL, no large files),
      `platform/psp/*.c` + `*/*.S` (gas), no POSIX platform code. `LOW_MEMORY` stays 0 (1 and 2
      shrink protocol limits). Still built and linked: opus/opusfile (voice), vorbis, mpg123,
      bzip2, MultiEmulator; revisit when trimming memory.
- [x] `engine/platform/psp/`: `sys_psp.c` (module info, heap threshold 3 MB, power callback thread,
      333 MHz, `ka.prx` VRAM unlock, timer, debug-screen message box, `start.cmd` arguments, PRX
      load/unload), `lib_psp.c` (static table + PRX modules, same `module_start` export-table ABI
      as the old port), `vid_psp.c` (fixed 480x272, software framebuffer), `snd_psp.c`, `in_psp.c`
      (same key names and axes as the old port so its configs keep working), `net_psp.h`.
      `vmem_psp.c`: P5 RAM (4 MB volatile partition) behind `MEM_VOLATILE` pools, see the log.
      Not ported yet: power suspend/resume handling, hw MP3, `fsh_psp.c`.
- [x] Network: no `XASH_NO_NETWORK` (the old stub header has rotted); instead `net_psp.h` makes
      `net_ws.c` build against pspsdk sockets (IPv4 only, `getaddrinfo` emulated with
      `gethostbyname`, `SO_NONBLOCK`). Sockets just fail until Phase 3 brings the net modules up.
- [x] Game PRX interface: `psp-hl` server and client PRXs load through the export-table ABI and run
      `c0a0` (server: extended EntityAPI 140; client: only optional exports missing).
- [x] File descriptors: only **9** kernel file handles are free for the engine under PSPLink
      (`0x80020320` = too many open files). `XASH_REDUCE_FD` was dead code in the filesystem library
      (it doesn't include `defaults.h`) — revived, keeps one read-only file open. pspsdk bug to keep
      in mind: libcglue `__set_errno` only maps `0x8001xxxx` errors, other SCE errors come back from
      `open()` as huge "descriptors" (opendir then crashes in `getdents`).
- [x] Memory budget (debug build, `c0a0` running, `-ref null`): partition 2 has 693 KB free,
      468 KB largest block; that's what's left of the 3 MB module reserve after both game PRXs.
      Engine heap: `psp_meminfo` console command (newlib arena use, largest `malloc`, kernel free).
- [ ] Client identification (`identification.c`): use OpenPSID like the Vita uses its PSID.

**Game library names:** the current engine generates `<name>_psp_mipsel.prx` from `liblist.gam`
(`gamedll_linux "dlls/hl.so"` -> `dlls/hl_psp_mipsel.prx`, client -> `cl_dlls/client_psp_mipsel.prx`).
The old port used `dlls/server.prx`/`cl_dlls/client.prx`; rename on the stick.

**Static memory (engine image, measured 2026-11-17):** release (`-O3`) text 3.40 MB, data 30 KB,
**BSS 13.4 MB**, so ~16.9 MB before any heap. Debug: text 4.6 MB. Biggest BSS users: `cl` 3.4 MB,
`sv_` 2.4 MB, `net` 1.6 MB, `mod_known` 1.6 MB, `frame_ents` 0.7 MB, `s_knownSfx` 0.6 MB,
`cls` 0.4 MB, 2x `gpMove` 0.3 MB, `clgame` 0.3 MB, `net_message_buffer`/`msg_buf` 0.2 MB each.
Game PRXs (`-Ofast`): server 1.33 MB text + 0.12 MB data/bss, client 0.56 + 0.14 MB, ~2.2 MB of
the 3 MB module reserve plus their own heaps; may need a bigger threshold.

**How to test Milestone 1 under PSPLink:** build (`scripts/psp/docker-build.sh all` and
`scripts/psp/docker-build.sh hlsdk`), copy `build-psp/xash.prx` and `EBOOT.PBP` to
`ms0:/PSP/GAME/Xash3D/` and `build-psp/valve/` over the stick's `valve/` (adds `extras.pk3`,
`dlls/hl_psp_mipsel.prx`, `cl_dlls/client_psp_mipsel.prx`), then in pspsh:
`cd ms0:/PSP/GAME/Xash3D/` and `./xash.prx -ref null -dev 2 -log +map c0a0`
(the working directory is the root directory). Or put the same arguments into `start.cmd` and
launch from the XMB.

**Milestone 1:** with `ref/null`, EBOOT boots under PSPLink, loads `server.prx`, runs `map c0a0`
(headless), console output visible over USB, zero unresolved import stubs. **Reached 2026-11-17**
(debug build): time to first frame 35 s, of which ~17 s is mainui rendering its TrueType fonts
with stb_truetype (old port used bitmap fonts — fix in Phase 2); `c0a0` loaded at 58 s, the tram
ride and scripted sequences run. Resource verification `stat()`s every file on the stick (slow).

## Phase 2 — Renderer (`ref/gu`)

- [x] Port `reference/.../ref_gu` to `ref/gu`, REF_API v1 → v20 (builds and links, 2026-11-17; not
      run on hardware yet). Method: 3-way merge (old `ref_gl` → `ref_gu` diff onto today's `ref/gl`)
      for small files; image/rsurf/studio/warp/beams/decals rebuilt from today's GL code with the GU
      parts transplanted. `ref/common/*.c` is compiled into `ref_gu` (the relocatable `ld -r` link
      doesn't pull static libraries). Left out: VBO, ripples, lightmapped water, cubemap shots,
      cinematics (`GL_UpdateTexture`), detail-texture tuning, P5 RAM temp buffers (heap for now).
- [x] Shared headers untouched: polygons are `gu_poly_t` cast from `msurface_t::polys`, PSP pixel
      formats (`PF_RGB_332`…) and `TF_IMG_*` flags are private to `ref/gu`. The old `sprite.h`
      `#pragma pack` and `ref_api.h` P5 RAM hooks were not needed so far.
- [~] VRAM management, texture formats/palettes (`gu_vram.c`, pixel converter), clipping, studio,
      sprites, beams, decals, particles, triapi, warp/sky, lightmaps — ported, needs hardware testing.
      GU gotchas: `sceGuDepthMask( GU_TRUE )` *disables* depth writes; the GE reads textures when it
      executes the list (dynamic lightmap updates mid-frame race, same as the old port).
- [ ] Menu: upstream `3rdparty/mainui` vs Crow-bar's PSP mainui changes (fonts, resolution 480x272,
      controls). Avoid forking the submodule if possible.
- [ ] Engine-side PSP optimizations from the reference, as profiling shows they're needed:
      filesystem cache / `fsh_psp.c` (now in `filesystem/`), hardware MP3 decoder (`scemp3`) for
      soundlib streams, VFPU math (`public/xash3d_mathlib.*`, `matrixlib.c`), studio/bmodel memory trims.
- [ ] Memory: get singleplayer to fit with headroom; record numbers here.
      `c0a0` (`-T fast`, 900 edicts): heap 27.8 MB in use, largest `malloc` 5.5 MB; kernel 677 KB
      free (457 KB largest block). Menu only: largest `malloc` 24.2 MB.

**Milestone 2:** singleplayer on par with the old port (menu, new game, save/load, map changes, sound,
music), launched from the XMB.

## Phase 3 — PSP networking

- [x] PSP network backend (`net_psp.c`): on the first `NET_Config( true )` (connect, listen server,
      server browser) it loads the COMMON/INET modules, inits sceNet/Inet/Resolver/Apctl and connects
      to profile `net_psp_profile` (falls back to the first existing profile; blocking, 30 s
      timeout), retried on the next `NET_Config( true )` after a failure, then sockets are reopened.
      BSD sockets via libcglue, non-blocking UDP via `SO_NONBLOCK`, `gethostbyname` resolver,
      `gethostname` = our IP. IPv6 off. **UDP verified both ways on hardware** (2026-11-17).
      Still untested: TCP (steam-broker), DNS names. **The stack's threads (prio 42/48) only run
      when the main thread (0x20) blocks**: without the 1 ms per-frame yield (`PSP_NetFrame`) the
      PSP sends fine but receives almost nothing (and doesn't answer ARP). Raising them to 0x18
      locked up the whole PSP instead.
- [x] Access point connection: `net_psp_profile`, `net_psp_profiles`, `net_psp_status` (SSID, IP,
      gateway, DNS, signal, WLAN power save), state progress + apctl error codes in the console.
      Optionally the system netconf dialog (`sceUtilityNetconf*`, needs GU dialog drawing) later.
- [~] Shut down cleanly: `quit` with the stack up works (apctl handler removed, **wait for the
      disconnect to finish** (terminating right away hung the PSP), terms, modules unloaded).
      Power/sleep events not handled yet.
- [x] Remove `XASH_NO_NETWORK` for PSP (never enabled in the new port).

**Milestone 3:** PSP joins a Xash3D dedicated server on the LAN (normal Xash protocol) and plays.

## Phase 4 — GoldSrc protocol

- [x] Local test server: the user's ReHLDS 3.15 in `C:\Games\hlds\steamapps\common\Half-Life`
      (other mods/plugins installed there; plain `valve` with stock `hl.dll` works):
      `hlds.exe -console -game valve -condebug -port 27015 +ip <pc ip> +sv_lan 1
      +maxplayers 4 +servercfgfile "" +map crossfire`. `qconsole.log` is shared with older runs.
- [x] `connect <pc ip>:27015 gs` from the PSP: challenge, connect accept, serverdata,
      resources, map load, spawn, delta entities, `clc_move`, `psp-hl` user messages: **plays**
      (2026-11-17): walking, shooting (ammo, decals), HUD, `status` forwarded to the server
      (ping ~61 ms, 0 loss over Wi-Fi), 38–47 fps on crossfire. Connect -> spawned in ~15 s.
      The first OOM was the client: 64 packet-entity frames (5.3 MB), see the memory note below.
      ReHLDS's A2S_PLAYER reply always says 0 players; use `status` instead.
- [ ] Downloads/custom resources: decide policy (`cl_allowdownload`, size limits) given memory stick
      speed and RAM; sprays/customizations off by default.
- [ ] Voice: GoldSrc voice codec isn't usable here — make sure it's cleanly disabled.
- [ ] Performance/memory on real servers (`rate`, `cl_updaterate`, `cl_cmdrate`, entity counts,
      32-player maps). Tune defaults in the PSP config.

**Milestone 4:** PSP plays on a GoldSrc server on the LAN. **Reached 2026-11-17.**

## Phase 5 — Steam authentication via steam-broker

- [x] `engine/client/cl_steam.c` on PSP: TCP connect to `cl_steam_broker_addr`, non-blocking
      send/recv, `sb_gamedir` / `sb_connect` / `sb_disconnect` / `sb_terminate` flow. PSP `select()`
      reports connecting sockets as writable: re-`connect()` (EISCONN) instead. Engine fixes (shared
      code): no `sb_disconnect` without a prior `sb_connect` (the broker restarts the session), and
      the first two GoldSrc connection attempts wait for the broker link.
- [x] Test with steam-broker on the PC: the user's Windows build in `<home>\Projects\steam-broker`
      (`target/i686-pc-windows-msvc/release/steam-broker.exe 0.0.0.0:27420`, needs a restart loop:
      it exits after every session, `steam-broker.bat` does that). PSP with `cl_ticket_generator steam`,
      `cl_steam_broker_addr <pc ip>:27420`: 218-byte ticket for the user's SteamID, joins the
      LAN server with it (2026-11-17). Not validated by Steam yet: the test HLDS runs `sv_lan 1`.
- [ ] Join a real VAC-secured official/community HLDS server over the internet. Note: upstream warns
      some servers flag Xash clients as fake clients.
- [ ] Console input for `connect`/cvars on the PSP: on-screen keyboard (`sceUtilityOsk*`) or a small
      server list/favourites UI so typing IPs isn't needed.

**Milestone 5:** PSP joins an official HLDS server with a Steam ticket from the broker.

## Phase 6 — Release polish

- [ ] EBOOT metadata (title, icon), packaged `valve/` PSP configs + `extras.pak`, `start.cmd`.
- [ ] Docs: install, Wi-Fi setup, steam-broker setup, known limitations.
- [ ] Decide what to upstream to FWGS (scaffold fixes, platform layer, `ref/gu`).

---

## Risks / open questions

- **Memory**: the old engine left ~3 MB free on a 64 MB PSP; current upstream is larger, and online
  play adds network buffers, more entities and server content. May require aggressive trimming.
  32 MB PSP-1000 is out of scope unless it falls out naturally.
- **Renderer port size**: `ref_gu` is the largest single job; REF_API changed 19 times.
- **Wi-Fi**: PSP only supports WEP/WPA; modern routers may need mixed mode or a separate SSID.
- **Fake-client kicks** on some GoldSrc servers (upstream known issue).
- **Submodule changes** (mainui, library_suffix) would need forks — track any here.

## Progress log

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
