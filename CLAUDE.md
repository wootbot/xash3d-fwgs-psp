# Xash3D FWGS — PSP port (current upstream)

## Goal

Bring the PlayStation Portable port of Xash3D FWGS up to **current upstream FWGS**, so the PSP can
play Half-Life online on **official GoldSrc (HLDS) servers**:

- GoldSrc network protocol (`connect ip:port gs`, `engine/client/parse/cl_parse_gs.c`).
- Steam ticket authentication through [steam-broker](https://github.com/FWGS/steam-broker)
  (`engine/client/cl_steam.c`, `cl_ticket_generator steam`, `cl_steam_broker_addr <pc ip>`): the
  broker runs on a PC with Steam + Half-Life and hands the PSP its auth ticket over TCP.
- Singleplayer must keep working as well as it did in the old port.

The only working PSP port is Crow-bar's fork, based on upstream from Oct 2020 (~5,700 upstream
commits behind). This repo starts from fresh upstream and re-implements the PSP port on top of it.
The step-by-step plan and progress tracking live in **[PLAN.md](PLAN.md)** — keep it updated.

## Repository layout

- Root: fork of upstream FWGS (`origin` = github.com/wootbot/xash3d-fwgs-psp). Add upstream with
  `git remote add upstream https://github.com/FWGS/xash3d-fwgs` to merge later updates.
- `reference/` (git-ignored, read-only reference material):
  - `reference/xash3d-fwgs-crowbar-psp/` — the old, working PSP fork (branch `psp`). Key PSP code:
    `engine/platform/psp/` (sys, vid, input, sound, module loader, P5 RAM, `ka.prx` VRAM unlock,
    hw MP3 decoder, filesystem helper), `ref_gu/` (~25k-line sceGu renderer, REF_API v1),
    PSP changes in `engine/common/filesystem.c`, math/memory tweaks across engine/public.
    `grep -rl XASH_PSP` there lists every touched file. Our last 5 commits there are toolchain fixes.
  - `reference/hlsdk-portable/` — game code, branch `psp-hl` (Crow-bar/hlsdk-xash3d), builds
    `valve/dlls/server.prx` and `valve/cl_dlls/client.prx` with `make -f Makefile.psp`.
- Upstream already contains a partial, untested PSP scaffold from 2023 (see PLAN.md, phase 1):
  `XASH_PSP` in `3rdparty/library_suffix/include/build.h`, `*_PSP` backends in `common/backends.h` /
  `common/defaults.h`, a `PSP` class + `--psp=` option in `scripts/waifulib/xcompile.py`, and EBOOT/PRX
  tasks in `scripts/waifulib/psp.py`. It has known bugs — don't trust it blindly.

## Conventions

- Keep PSP code in `engine/platform/psp/`, `ref/gu/` and PSP-specific waf scripts. Changes to shared
  engine code go behind `#if XASH_PSP` (or a generic feature macro) and stay minimal, so future
  upstream merges stay cheap.
- **Don't shrink network/protocol limits** (`protocol.h`, `netchan.h`, delta sizes) the way the old
  fork did: they must stay GoldSrc-compatible. Save memory elsewhere.
- Follow upstream style: tabs, `.clang-format`/`uncrustify.cfg`, commit subjects like
  `engine: platform: psp: ...`. LF line endings (watch out when editing from Windows tools).
- **Commit as you make progress** — small, focused commits whenever a piece of work builds or a
  step in PLAN.md is done; don't wait until the end of a session. Update PLAN.md (checkboxes,
  progress log, measured numbers) in the same or a following commit. Don't push unless asked.
- `3rdparty/*` are submodules (mainui, library_suffix, ...). Changes there need a fork of that
  submodule — avoid if possible and note it in PLAN.md when unavoidable.

## Building (Docker, no native toolchain)

There is no Windows toolchain. Builds run in the `pspdev/pspdev:latest` image (GCC 15.x, Alpine).
Docker Desktop must be in **Linux containers** mode.

- Install build deps in the container: `apk add python3 make bash`.
- Copy the source into the container (e.g. tar into `/src`, excluding `build*`, `reference/`) rather
  than building on the Windows bind mount — it's much faster and avoids permission/CRLF trouble.
- Old fork engine: `python3 waf configure -T fast|debug --psp=prx,660,HW --prefix=/out && python3 waf build`
  → `build/engine/EBOOT.PBP`, `xash.prx`, and `xash` (unstripped ELF, keep it for symbols).
  The new port should keep the same `--psp=<elf|prx>,<fw>,<SW|HW|ALL>` interface.
- Game DLLs: in `reference/hlsdk-portable`, `make -f Makefile.psp` → `build/dist/valve/...`.

### Toolchain pitfalls already solved (old fork; re-apply in the new port)

- GCC 15 defaults to C23 (`bool` is a keyword) → add `-std=gnu11` to C flags. `vasprintf` needs
  `-D_GNU_SOURCE` for C++.
- newlib: `int32_t` is `long` → typedef clashes (e.g. mainui's miniutl `int32`/`uint32`), pointer-type
  mismatches with `SceInt32*` APIs. `SceIoStat` now has `sce_st_mtime` (check `#ifdef st_mtime`).
  `struct timeval` may already be defined (`_TIMEVAL_DEFINED`).
- libcglue: `__psp_libc_init` is now `__libcglue_init` (custom PRX startup code in hlsdk's
  `psp/server_psp.c` / `client_psp.c` had to be updated, plus `_exit` and `.init_array` handling).
- Heap: any negative `PSP_HEAP_SIZE_KB` now means "all free minus `PSP_HEAP_THRESHOLD_SIZE_KB`"
  (default 512 KB). Use `PSP_HEAP_SIZE_KB(-1)` + `PSP_HEAP_THRESHOLD_SIZE_KB(3*1024)` to leave room for
  the game PRX modules.
- **Link order / unresolved imports (the nasty one):** psp-gcc's specs already append
  `-lpthreadglue -lpthread -lcglue -lc ... -lpsputility -lpsprtc -lpspnet_inet -lpspnet_resolver
  -lpspsdk -lpspmodinfo -lpspuser` at the end. Listing those libs (or `-lpspkernel`, which is
  kernel-only) explicitly earlier splits a library's import stubs into two groups, and
  `psp-fixup-imports` only registers the first contiguous run. The rest stay `jr ra; nop` and silently
  return garbage (this caused the "Mem_AllocPool: out of memory" at startup: `sceKernelMaxFreeMemSize`
  was a dead stub). Upstream `xcompile.py` `PSP.stdlibs()` still has this bug. **After every link,
  audit**: every symbol in `.sceStub.text` must be covered by a `.lib.stub` entry (a small script over
  `psp-objdump`/`psp-readelf` output; zero uncovered stubs is the pass condition). Check game PRXs too.

## Target hardware & deployment

- User's PSP: 64 MB model (partition 2 ≈ 55 MB usable), 6.20 PRO CFW. Old engine left ~3 MB free
  after startup — memory is the main constraint.
- Memory stick layout: `PSP/GAME/Xash3D/` → `EBOOT.PBP`, `xash.prx`, `start.cmd`, `valve/`
  (game data + `dlls/server.prx`, `cl_dlls/client.prx`). PSPLink in `PSP/GAME/PSPLink32/`.
- PSP needs PSP-specific configs (from Crow-bar's release `2603`, binds translated to gamepad key
  names) plus `extras.pak`; PC configs cause no forward movement, a grey bar and upside-down text.
  The user's PC configs on the stick were renamed `*.pc`.
- PSP Wi-Fi: WEP / WPA-PSK (TKIP/AES) only — WPA2-only/WPA3 routers may need mixed mode.

## Debugging over USB (PSPLink)

- PSPLink 3.2.1 on the PSP; on Windows `usbhostfs_pc.exe` + `pspsh.exe` (+ cygwin DLLs), libusb driver
  installed with Zadig. **These PC tools lived in the old `build-psp/psplink/` folder, which did not
  survive the move — re-download them** and keep them out of git (e.g. `tools/psplink/`, ignored).
- Run `usbhostfs_pc -g -c <dir>`: `<dir>` becomes `host0:` on the PSP and must be a relative or
  cygwin-style path (a Windows path breaks `host0:`). `-g` opens the gdb port (10001).
- Point `host0:` at the build output and run the debug build straight from the PC (no stick copy).
  Engine printf/console output appears in the usbhostfs/pspsh console.
- pspsh: **never send `exit`** — it quits PSPLink back to the XMB. `pspsh -e` doesn't split
  arguments; pipe commands into pspsh instead. Useful: `meminfo`, `modlist`, `modinfo`, `peekw`,
  `disasm`, `bpset`, `exprint`, `exresume`, `modload`/`modstart`, `malloc`/`mfree`, `cp`, `reset`.
- Crashes: PSPLink catches exceptions and dumps registers. Map the EPC with `psp-addr2line` on the
  unstripped ELF (`xash`); for game modules subtract the module base from `modlist` and use the
  unstripped `server.elf`/`client.elf`.
- gdb: `psp-gdb` from the Docker image, `target remote host.docker.internal:10001` (untested end to end).
- Games launched from the XMB can't be attached to — reproduce problems under PSPLink.
