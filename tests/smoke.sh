#!/bin/sh
# Headless smoke test for Pluto.
#
# Usage:
#   tests/smoke.sh             # Wayland startup + optional foot client
#   tests/smoke.sh --xwayland  # also run an override-redirect X11 client
#
# The compositor is started with the headless backend and pixman renderer, so
# this works on CI without DRM, a seat, or a display.

set -eu

xwayland=0
if [ "${1:-}" = "--xwayland" ]; then
	xwayland=1
fi

runtime=$(mktemp -d "${TMPDIR:-/tmp}/pluto-smoke.XXXXXX")
chmod 700 "$runtime"
before=$(mktemp)

cleanup() {
	if [ -n "${pid:-}" ]; then
		kill "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	fi
	rm -rf "$runtime" "$before"
}
trap cleanup EXIT INT TERM

# Record existing X sockets so we can find the one XWayland creates.
find /tmp/.X11-unix -maxdepth 1 -name 'X[0-9]*' -type s 2>/dev/null |
	sort >"$before" || true

export XDG_RUNTIME_DIR="$runtime"
export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_LIBINPUT_NO_DEVICES=1

./pluto >"$runtime/pluto.out" 2>"$runtime/pluto.err" &
pid=$!

i=0
while [ "$i" -lt 50 ]; do
	for sock in "$runtime"/wayland-*; do
		if [ -S "$sock" ]; then
			break 2
		fi
	done
	if ! kill -0 "$pid" 2>/dev/null; then
		echo "pluto exited before creating a Wayland socket" >&2
		cat "$runtime/pluto.err" >&2
		exit 1
	fi
	i=$((i + 1))
	sleep 0.1
done

wayland_socket=
for sock in "$runtime"/wayland-*; do
	if [ -S "$sock" ]; then
		wayland_socket=$sock
		break
	fi
done
if [ -z "$wayland_socket" ]; then
	echo "pluto did not create a Wayland socket" >&2
	cat "$runtime/pluto.err" >&2
	exit 1
fi
export WAYLAND_DISPLAY=${wayland_socket##*/}

# A real Wayland client, when available.  foot uses fontconfig, which may not
# be configured in every CI environment; a failure here is therefore a warning,
# not a test failure.
if command -v foot >/dev/null 2>&1; then
	TERM=xterm-256color foot -e sleep 2 >"$runtime/foot.out" 2>"$runtime/foot.err" &
	foot_pid=$!
	sleep 1
	if ! kill -0 "$foot_pid" 2>/dev/null; then
		echo "warning: foot exited early" >&2
		cat "$runtime/foot.err" >&2
	fi
	kill "$foot_pid" 2>/dev/null || true
	wait "$foot_pid" 2>/dev/null || true
fi

if [ "$xwayland" -eq 1 ]; then
	if ! command -v cc >/dev/null 2>&1 || ! command -v pkg-config >/dev/null 2>&1; then
		echo "--xwayland needs cc and pkg-config" >&2
		exit 1
	fi
	cc -o "$runtime/override_unmap" tests/override_unmap.c \
		$(pkg-config --cflags --libs xcb)

	display=
	i=0
	while [ "$i" -lt 50 ]; do
		for sock in /tmp/.X11-unix/X*; do
			[ -S "$sock" ] || continue
			if ! grep -qxF "$sock" "$before"; then
				display=":${sock##*X}"
				break 2
			fi
		done
		i=$((i + 1))
		sleep 0.1
	done
	if [ -z "$display" ]; then
		echo "XWayland did not create an X socket" >&2
		cat "$runtime/pluto.err" >&2
		exit 1
	fi

	DISPLAY=$display "$runtime/override_unmap"
fi

if ! kill -0 "$pid" 2>/dev/null; then
	echo "pluto died during the smoke test" >&2
	wait "$pid" 2>/dev/null || true
	cat "$runtime/pluto.err" >&2
	exit 1
fi

kill -TERM "$pid"
wait "$pid" || true
pid=
echo "smoke test passed"
