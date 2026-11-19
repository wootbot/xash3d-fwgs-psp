#!/bin/sh
# docker-build.sh -- build the PSP port inside the pspdev/pspdev Docker image
#
# Usage (from the repository root, Git Bash or any POSIX shell with docker):
#   scripts/psp/docker-build.sh configure [extra waf configure args]
#   scripts/psp/docker-build.sh build
#   scripts/psp/docker-build.sh all [extra waf configure args]   # configure + build
#   scripts/psp/docker-build.sh shell                            # interactive shell in /src
#   scripts/psp/docker-build.sh exec <command...>                # run a command in /src
#   scripts/psp/docker-build.sh hlsdk [hlsdk dir]                # build game PRXs (default: reference/hlsdk-portable)
#   scripts/psp/docker-build.sh rm                               # remove the build container
#
# Environment:
#   PSP_OPTS   --psp= value (default: prx,660,HW; prx,660,NULL builds the headless engine)
#   BUILD_TYPE waf -T value (default: fast, like the old port; debug is -O0 and several times slower)
#   OUT        host output directory (default: build-psp)
#
# $OUT is also the game root for PSPLink: put the game data into $OUT/valve, start
# `usbhostfs_pc -g` from inside $OUT (host0: = $OUT) and run `./xash.prx` from host0:/ in pspsh.
#
# A persistent container keeps installed packages and the waf build directory between runs,
# so rebuilds are incremental. Sources are synced into /src with tar (tracked and untracked,
# non-ignored files), which is much faster than building on a Windows bind mount.
set -e

CONTAINER=${CONTAINER:-xash3d-psp-build}
IMAGE=${IMAGE:-pspdev/pspdev:latest}
PSP_OPTS=${PSP_OPTS:-prx,660,HW}
BUILD_TYPE=${BUILD_TYPE:-fast}
OUT=${OUT:-build-psp}

# don't let MSYS mangle container paths
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL='*'

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

ensure_container()
{
	if ! docker container inspect "$CONTAINER" >/dev/null 2>&1; then
		docker create --name "$CONTAINER" "$IMAGE" sleep infinity >/dev/null
	fi
	if [ "$(docker container inspect -f '{{.State.Running}}' "$CONTAINER")" != "true" ]; then
		docker start "$CONTAINER" >/dev/null
	fi
	if ! docker exec "$CONTAINER" sh -c 'command -v python3' >/dev/null 2>&1; then
		docker exec "$CONTAINER" apk add --no-cache python3 make bash
	fi
}

sync_sources()
{
	docker exec "$CONTAINER" mkdir -p /src
	# skip submodule gitlinks (their files are added below) and files deleted from the working tree
	git ls-files --cached --others --exclude-standard \
		| grep -v -x -F "$(git submodule foreach --quiet 'echo $sm_path')" \
		| tar --ignore-failed-read --warning=no-failed-read -T - -cf - \
		| docker exec -i "$CONTAINER" tar -xf - -C /src
	# submodules (checked out ones only)
	git submodule foreach --quiet 'git ls-files -z --cached --others --exclude-standard | sed -z "s|^|$sm_path/|"' \
		| tar --null -T - -cf - \
		| docker exec -i "$CONTAINER" tar -xf - -C /src
}

run()
{
	docker exec -w /src "$CONTAINER" sh -c "$*"
}

do_configure()
{
	run "python3 waf configure -T $BUILD_TYPE --psp=$PSP_OPTS --prefix=/ $*"
}

do_build()
{
	run "python3 waf build"
	run "rm -rf /out && python3 waf install --destdir=/out >/dev/null"
	# copy over, don't wipe: $OUT doubles as the game root served as host0: by usbhostfs
	# (game data in $OUT/valve, game PRXs from the hlsdk command)
	mkdir -p "$OUT"
	docker cp "$CONTAINER:/out/." "$OUT/"
	# unstripped ELF for psp-addr2line / psp-gdb
	docker cp "$CONTAINER:/src/build/engine/xash" "$OUT/xash.elf" 2>/dev/null || true
	run "python3 scripts/psp/check_stubs.py build/engine/xash_fixup"
	echo "Output in $OUT/"
}

# hlsdk-portable psp-hl branch, Makefile.psp. The engine looks for game libraries with
# platform suffixes: dlls/hl_psp_mipsel.prx (from gamedll_linux "dlls/hl.so"), cl_dlls/client_psp_mipsel.prx
do_hlsdk()
{
	hlsdk=${1:-reference/hlsdk-portable}
	docker exec "$CONTAINER" rm -rf /hlsdk
	docker exec "$CONTAINER" mkdir -p /hlsdk
	git -C "$hlsdk" ls-files --cached --others --exclude-standard | (cd "$hlsdk" && tar -T - -cf -) | docker exec -i "$CONTAINER" tar -xf - -C /hlsdk
	docker exec -w /hlsdk "$CONTAINER" sh -c 'make -f Makefile.psp -j$(nproc) >/dev/null'
	for elf in build/server/server.elf build/client/client.elf; do
		docker exec -w /src "$CONTAINER" python3 scripts/psp/check_stubs.py "/hlsdk/$elf"
	done
	mkdir -p "$OUT/valve/dlls" "$OUT/valve/cl_dlls"
	docker cp "$CONTAINER:/hlsdk/build/server/server.prx" "$OUT/valve/dlls/hl_psp_mipsel.prx"
	docker cp "$CONTAINER:/hlsdk/build/client/client.prx" "$OUT/valve/cl_dlls/client_psp_mipsel.prx"
	docker cp "$CONTAINER:/hlsdk/build/server/server.elf" "$OUT/server.elf"
	docker cp "$CONTAINER:/hlsdk/build/client/client.elf" "$OUT/client.elf"
	echo "Game libraries in $OUT/valve/"
}

cmd=${1:-all}
[ $# -gt 0 ] && shift

case "$cmd" in
configure) ensure_container; sync_sources; do_configure "$@" ;;
build)     ensure_container; sync_sources; do_build ;;
all)       ensure_container; sync_sources; do_configure "$@"; do_build ;;
shell)     ensure_container; sync_sources; docker exec -it -w /src "$CONTAINER" bash ;;
exec)      ensure_container; sync_sources; run "$@" ;;
hlsdk)     ensure_container; sync_sources; do_hlsdk "$@" ;;
rm)        docker rm -f "$CONTAINER" ;;
*)         echo "unknown command: $cmd" >&2; exit 1 ;;
esac
