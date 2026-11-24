# PSP port plan

Goal: current upstream Xash3D FWGS on the PSP, able to join official GoldSrc (HLDS) servers using the
GoldSrc protocol and Steam tickets from steam-broker. See [CLAUDE.md](CLAUDE.md) for context, build
and debugging notes.

Base: upstream `master` @ `11ed246c` (2026-11-08). Reference: `reference/xash3d-fwgs-crowbar-psp`
(upstream Oct 2020 + PSP port, REF_API v1) and `reference/hlsdk-portable` (`psp-hl`).

This file keeps only the current targets and the open items. More detail lives in:
- **[Documentation/psp/phases.md](Documentation/psp/phases.md)**: full notes per phase (how things
  were done, quirks, memory measurements, how to test each milestone).
- **[Documentation/psp/progress-log.md](Documentation/psp/progress-log.md)**: chronological log of
  experiments and measurements (what was tried, what worked, what didn't). Grep it for a topic
  before retrying an idea; append new entries at its end.

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
      The stress scene (31 bots, 32 monsters, crossfire) exists, demo `stress1`: ~20 fps (2026-11-24).
- [~] Batched studio renderer in the engine: run each phase (transform + cull, bones,
      lighting, GE output) for all models in a row instead of the whole pipeline per model.
      The first check (a whole model drawn twice in a row, 5.9 vs 6.6 ms) missed the point:
      the cost is instruction cache misses, and a whole model's code doesn't fit the 16 KB
      icache, so the repeat is cold too. Per phase cold vs warm repeat: ~45% misses. The
      solid list's plain models in two passes gave 56.9 -> 57.9 fps, but nothing once the GE
      lighting (one draw per batch) was in: dropped again.
- [x] Reduced skeletons and decimated meshes: levels of detail made from the models at run time
      (`r_studio_lod`), poses kept for a few frames when the ME has too much to do. A cache of decoded
      keyframes was tried twice and saves almost nothing (see the progress log).
- [ ] Sounds are loaded when they first play: 30-95 ms each. In the background instead.
- [ ] Memory in the stress scene: ~1 MB left, the second join of a session can fail.
- [ ] Shells, gibs, blood drips: tiny batched meshes/sprites, caps on count and lifetime.
- [~] World: compact copies of the world surfaces for the walk and both passes (done, world
      3.7 -> 2.0 ms in fight1), and a compact BSP tree for the walk (walk ~1.2 -> 0.92 ms).
      Left: the walk is still ~0.9 ms (~0.45 node traversal, ~0.25 VFPU box tests, the rest
      leaf marks and surface culling), decals.
- [~] Media Engine (second CPU): sound mixing, studio bones, the world walk with its GE commands,
      static brush models, whole studio models with their light (translucent ones and props too),
      players with their weapons, the culling and temp entities are ME jobs. The main CPU hardly waits
      for the ME or the GE anymore (two display lists), fight1 66 -> 106 fps (2026-11-24). Left: the ME
      is idle until the packets are read and from the models on; packet entities, particles, sprites.
- [~] GE time: ~8 ms of a fight1 frame (world ~4, models ~3.5; half fill, half vertices), and its
      texture and vertex fetches from RAM slow the CPUs down: the textures in sight move into VRAM
      now. Left: vertices and commands from RAM, fewer vertices, less overdraw.
- [ ] The non-render floor: engine-side traces, delta parsing, HUD and client frame (same math).

---

---

## Phases (summary; details in [phases.md](Documentation/psp/phases.md))

- **Phase 0 — Setup:** done. Docker build (`scripts/psp/docker-build.sh`), import-stub audit
  (`scripts/psp/check_stubs.py`), PSPLink PC tools, game PRXs (`docker-build.sh hlsdk`).
  - [ ] Ship the game PRXs with the PSP data (Crow-bar `2603` configs, `extras.pak`).
- **Phase 1 — Toolchain and engine bring-up:** done, Milestone 1 reached 2026-11-17.
  - [ ] Client identification (`identification.c`): use OpenPSID like the Vita uses its PSID.
- **Phase 2 — Renderer (`ref/gu`):** ported to REF_API 20 and running; performance work is in the
  section above.
  - [~] Hardware testing of VRAM/textures, clipping, studio, sprites, beams, decals, particles,
        triapi, warp/sky, lightmaps.
  - [ ] Menu: upstream mainui vs Crow-bar's PSP changes (fonts, 480x272, controls), no submodule fork.
  - [ ] Engine-side optimizations from the reference as needed (`fsh_psp.c`, hw MP3, VFPU math).
  - [ ] Memory: singleplayer fits with headroom; record numbers.
  - **Milestone 2:** singleplayer on par with the old port (menu, new game, save/load, map
    changes, sound, music), launched from the XMB.
- **Phase 3 — PSP networking:** Wi-Fi backend, UDP and TCP working.
  - [~] Clean shutdown: `quit` works; power/sleep events not handled.
  - Untested: DNS names.
  - **Milestone 3:** PSP joins a Xash3D dedicated server on the LAN (Xash protocol).
- **Phase 4 — GoldSrc protocol:** Milestone 4 (plays on a LAN GoldSrc server) reached 2026-11-17.
  - [ ] Downloads/custom resources policy (`cl_allowdownload`, size limits, sprays off).
  - [ ] Voice cleanly disabled.
  - [ ] Performance/memory on real servers (`rate`, update rates, 32-player maps), PSP config defaults.
- **Phase 5 — Steam authentication via steam-broker:** ticket from the broker works on the LAN
  server (`sv_lan 1`, not validated by Steam yet).
  - [ ] Join a real VAC-secured HLDS server over the internet (watch for fake-client kicks).
  - [ ] Console input on the PSP: on-screen keyboard (`sceUtilityOsk*`) or a server list/favourites UI.
  - **Milestone 5:** PSP joins an official HLDS server with a Steam ticket from the broker.
- **Phase 6 — Release polish:**
  - [ ] EBOOT metadata (title, icon), packaged `valve/` PSP configs + `extras.pak`, `start.cmd`.
  - [ ] Docs: install, Wi-Fi setup, steam-broker setup, known limitations.
  - [ ] Decide what to upstream to FWGS (scaffold fixes, platform layer, `ref/gu`).

## Risks / open questions

- **Memory**: the old engine left ~3 MB free on a 64 MB PSP; current upstream is larger, and online
  play adds network buffers, more entities and server content. May require aggressive trimming.
  32 MB PSP-1000 is out of scope unless it falls out naturally.
- **Renderer port size**: `ref_gu` is the largest single job; REF_API changed 19 times.
- **Wi-Fi**: PSP only supports WEP/WPA; modern routers may need mixed mode or a separate SSID.
- **Fake-client kicks** on some GoldSrc servers (upstream known issue).
- **Submodule changes** (mainui, library_suffix) would need forks — track any here.
