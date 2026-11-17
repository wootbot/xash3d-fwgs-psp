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
      Not ported yet: P5 RAM, power suspend/resume handling, hw MP3, `fsh_psp.c`.
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
      Engine heap usage not measured yet (needs a `meminfo`-style engine command).
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

- [ ] Port `reference/.../ref_gu` to `ref/gu`, REF_API v1 → v20. Use the diff between the reference's
      `ref_gl` (2020) and today's `ref/gl` as the porting guide; move shared code onto `ref/common`.
- [ ] Re-apply PSP changes to shared headers only where still needed (`com_model.h`, `render_api.h`,
      `com_image.h`, `ref_api.h`, `sprite.h`) — prefer keeping them inside `ref/gu`.
- [ ] VRAM management, texture formats/palettes (`gu_vram.c`, pixel converter), clipping, studio,
      sprites, beams, decals, particles, triapi, warp/sky, lightmaps.
- [ ] Menu: upstream `3rdparty/mainui` vs Crow-bar's PSP mainui changes (fonts, resolution 480x272,
      controls). Avoid forking the submodule if possible.
- [ ] Engine-side PSP optimizations from the reference, as profiling shows they're needed:
      filesystem cache / `fsh_psp.c` (now in `filesystem/`), hardware MP3 decoder (`scemp3`) for
      soundlib streams, VFPU math (`public/xash3d_mathlib.*`, `matrixlib.c`), studio/bmodel memory trims.
- [ ] Memory: get singleplayer to fit with headroom; record numbers here.

**Milestone 2:** singleplayer on par with the old port (menu, new game, save/load, map changes, sound,
music), launched from the XMB.

## Phase 3 — PSP networking

- [ ] PSP network backend (template: `engine/platform/psvita/net_psvita.h` + `net_ws_private.h`):
      load `PSP_NET_MODULE_COMMON`/`INET`, `sceNetInit`, `sceNetInetInit`, `sceNetResolverInit`,
      `sceNetApctlInit`; BSD sockets via libcglue / `pspnet_inet`; non-blocking UDP; TCP (needed by
      steam-broker); DNS (sync resolver).
- [ ] Access point connection: start with a cvar/command selecting the Wi-Fi profile number
      (`sceNetApctlConnect`), poll state, show progress/errors in the console; optionally the system
      netconf dialog (`sceUtilityNetconf*`, needs GU dialog drawing) later.
- [ ] Shut down cleanly (disconnect, unload net modules) on exit and on power/sleep events.
- [ ] Remove `XASH_NO_NETWORK` for PSP.

**Milestone 3:** PSP joins a Xash3D dedicated server on the LAN (normal Xash protocol) and plays.

## Phase 4 — GoldSrc protocol

- [ ] Local test server: HLDS (steamcmd app 90) on the PC, LAN mode first; optionally ReHLDS +
      Reunion to test without Steam auth (`cl_ticket_generator revemu2013`).
- [ ] `connect <ip>:<port> gs` from the PSP: signon, resource list, delta entities, `clc_move`,
      user messages from the `psp-hl` client, map CRC.
- [ ] Downloads/custom resources: decide policy (`cl_allowdownload`, size limits) given memory stick
      speed and RAM; sprays/customizations off by default.
- [ ] Voice: GoldSrc voice codec isn't usable here — make sure it's cleanly disabled.
- [ ] Performance/memory on real servers (`rate`, `cl_updaterate`, `cl_cmdrate`, entity counts,
      32-player maps). Tune defaults in the PSP config.

**Milestone 4:** PSP plays on a GoldSrc server on the LAN.

## Phase 5 — Steam authentication via steam-broker

- [ ] `engine/client/cl_steam.c` on PSP: TCP connect to `cl_steam_broker_addr`, non-blocking
      send/recv, `sb_gamedir` / `sb_connect` / `sb_disconnect` / `sb_terminate` flow.
- [ ] Test with FWGS/steam-broker on the PC (Steam running, Half-Life owned), `cl_ticket_generator steam`.
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
  Open: ~25 s after "Verifying and uploading resources" before spawning (custom.hpk?).
