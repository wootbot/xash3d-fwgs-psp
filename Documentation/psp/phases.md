# PSP port phases: details

Full notes for the phases of [PLAN.md](../../PLAN.md): what was done and how, measured
numbers, quirks found along the way. PLAN.md keeps only the summary and the open items; keep the
checkboxes there and here in sync when an item changes.

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

- [x] Local test server: the user's ReHLDS 3.15 (folder in `tools/dbg/README.md`)
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
