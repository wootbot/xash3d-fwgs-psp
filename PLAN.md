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

- [ ] Add `reference/` and local tool/build output folders to `.gitignore`.
- [ ] Add `upstream` remote (FWGS/xash3d-fwgs).
- [ ] Re-acquire the PSPLink PC tools (usbhostfs_pc, pspsh, cygwin DLLs) into an ignored `tools/`
      folder with launcher scripts (usbhostfs with `-g -c <relative build dir>`, pspsh, psp-gdb via Docker).
- [ ] Build script/doc for the Docker build (copy source into container, configure, build, copy
      `EBOOT.PBP`/`xash.prx`/unstripped ELF back out).
- [ ] Import-stub audit script (every `.sceStub.text` symbol covered by `.lib.stub`), run after each link.
- [ ] Build the game PRXs from `reference/hlsdk-portable` (`psp-hl`) and keep them with the PSP data
      (configs from Crow-bar release `2603`, `extras.pak`).

## Phase 1 — Toolchain and engine bring-up (no renderer)

Upstream already contains a partial 2023 PSP scaffold. Fix and complete it:

- [ ] `scripts/waifulib/xcompile.py` `PSP` class: `PSP_ENVVARS` is defined but `PSP_SDK_ENVVARS` is used
      (NameError); `stdlibs()` has the link-order bug (drop `-lpspkernel`, `-lpsputility`, `-lpspsdk`,
      `-lpsprtc`, `-lpspuser` — psp-gcc specs append them); add `-std=gnu11` / `-D_GNU_SOURCE`.
- [ ] `scripts/waifulib/psp.py`: missing `from waflib import Task, TaskGen`; check it's loaded from
      `wscript` and that EBOOT.PBP / PARAM.SFO / PRX tasks work with the current waf.
- [ ] `common/defaults.h`: `#define XASH_PSP LIB_PSP` should be `#define XASH_LIB LIB_PSP`; review all
      PSP defaults (renderer `gu`, backends, timer, messagebox).
- [ ] `wscript` / `engine/wscript` PSP configuration: static linking of ref, menu and filesystem into
      the engine (upstream `STATIC_LINKING`/`XASH_STATIC_LIBS`, compare with old fork's `SINGLE_BINARY`
      + `xshlib` setup), disable what the PSP can't afford: VGUI, voice/opus, AVI, freetype/maintui,
      async DNS; decide on bzip2/mpg123/vorbis/MultiEmulator (MultiEmulator = revemu ticket
      generator, keep it — useful for non-Steam GoldSrc servers).
- [ ] `engine/platform/psp/`: port from the reference to current upstream platform APIs —
      `sys_psp.c` (module info, heap threshold, power callback, clock 333 MHz, `ka.prx` VRAM, P5 RAM),
      `in_psp.c` (pad, analog curve, HOME combos/AUX keys), `snd_psp.c`, `vid_psp.c`, `lib_psp.c`
      (load/unload game PRX modules and their export tables against the current `library.h`/
      `lib_common.c`), timer, message box.
- [ ] Network stays stubbed (`XASH_NO_NETWORK`) for now.
- [ ] Game PRX interface: confirm `psp-hl` server/client still work with the current engine's game
      API (it should — upstream keeps the old SDK ABI), including the custom PRX export table.
- [ ] Memory budget: measure free memory after startup (`meminfo`) and record it here.

**Milestone 1:** with `ref/null`, EBOOT boots under PSPLink, loads `server.prx`, runs `map c0a0`
(headless), console output visible over USB, zero unresolved import stubs.

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
