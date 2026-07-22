#!/bin/sh
# Smoke test for swm.
#
# Launches swm nested inside the running Wayland compositor, discovers the
# Wayland socket it creates, connects a client (wayland-info), and then
# shuts swm down cleanly.
#
# Requires: wayland-info, a running parent compositor on wayland-0.

set -e

BINARY="${1:-./build/swm}"

if [ ! -x "$BINARY" ]; then
	echo "error: $BINARY not found or not executable"
	echo "usage: $0 [path-to-swm-binary]"
	exit 1
fi

if ! WAYLAND_DISPLAY=wayland-0 wayland-info >/dev/null 2>&1; then
	echo "error: parent compositor not reachable on wayland-0"
	echo "  (this test requires a running Wayland session)"
	exit 1
fi

# Record sockets before launching swm.
BEFORE=$(ls /run/user/"$(id -u)"/wayland-*.lock 2>/dev/null || true)

echo "  launching swm nested..."

WLR_BACKEND=wayland env -u WAYLAND_DISPLAY "$BINARY" >/dev/null 2>&1 &
SWM_PID=$!

# Wait for swm to create its socket.
sleep 1

# Find the socket swm created (present in AFTER but not in BEFORE).
AFTER=$(ls /run/user/"$(id -u)"/wayland-*.lock 2>/dev/null || true)
SOCKET=""
for f in $AFTER; do
	found=0
	for g in $BEFORE; do
		if [ "$f" = "$g" ]; then found=1; break; fi
	done
	if [ "$found" -eq 0 ]; then
		SOCKET="${f##*/}"
		SOCKET="${SOCKET%.lock}"
		break
	fi
done

if [ -z "$SOCKET" ]; then
	echo "  smoke test FAILED: could not detect swm's Wayland socket"
	kill "$SWM_PID" 2>/dev/null || true
	exit 1
fi

echo "  detected socket: $SOCKET"

# Connect as a client and verify the compositor responds.
if WAYLAND_DISPLAY="$SOCKET" wayland-info >/dev/null 2>&1; then
	echo "  smoke test PASSED: compositor running on $SOCKET"
else
	echo "  smoke test FAILED: could not connect to $SOCKET"
	kill "$SWM_PID" 2>/dev/null || true
	exit 1
fi

# Shut down cleanly.
kill "$SWM_PID" 2>/dev/null || true
wait "$SWM_PID" 2>/dev/null || true
echo "  compositor shut down cleanly"
