# PSP port plan

Goal: current upstream Xash3D FWGS on the PSP, able to join official GoldSrc (HLDS) servers using the
GoldSrc protocol and Steam tickets from steam-broker. See [CLAUDE.md](CLAUDE.md) for context, build
and debugging notes.

Base: upstream `master` @ `11ed246c` (2026-11-08). Reference: `reference/xash3d-fwgs-crowbar-psp`
(upstream Oct 2020 + PSP port, REF_API v1) and `reference/hlsdk-portable` (`psp-hl`).

Status legend: `[ ]` todo, `[~]` in progress, `[x]` done. Each phase ends with a milestone that must be
verified **on the PSP**, not just compiled.

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
