#!/bin/sh
# test-web-open.sh - Open-room links and the installable manifest, served
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# tests/test-web-render.c holds the manifest, the folded request and the
# exchange list to their markup, and tests/test-chat-fold.c holds the
# open-room lookup to its rules.  Neither can see a route: `/r/<room>`
# answering an agent with a redirect, a name nobody has with a 404 that
# says so, the manifest and its icons actually reachable at the paths
# every page names, and the exchange page registered ahead of the
# `/a/:id/:view` catch-all that swallowed three routes before.
#
# Behind CLAWT_TEST_INTEGRATION because it needs a daemon, a socket and
# a listening port, none of which `make test` may touch.

set -eu

if [ "${CLAWT_TEST_INTEGRATION:-0}" != "1" ]; then
	echo "test-web-open: skipped (set CLAWT_TEST_INTEGRATION=1)"
	exit 0
fi

BUILD="${CLAWT_BUILD_DIR:-build/debug}"
PORT="${CLAWT_TEST_WEB_PORT:-8972}"

command -v curl >/dev/null 2>&1 || {
	echo "test-web-open: skipped (curl is not installed)"
	exit 0
}

[ -x "$BUILD/clawtilla-web" ] || {
	echo "test-web-open: skipped (no web client in $BUILD)"
	exit 0
}

# Under XDG_RUNTIME_DIR: sun_path is 108 bytes and a build tree's
# temporary path is longer than that.
RUN=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/clawt-webopen-XXXXXX")

cleanup () {
	[ -n "${WEB_PID:-}" ] && kill "$WEB_PID" 2>/dev/null
	[ -n "${DAEMON_PID:-}" ] && kill "$DAEMON_PID" 2>/dev/null
	rm -rf "$RUN"
}
trap cleanup EXIT

fail () {
	echo "test-web-open: FAILED: $*"
	exit 1
}

# Polls until the check passes or the deadline runs out.
wait_for () {
	seconds="$1"
	shift
	waited=0

	while [ "$waited" -lt "$seconds" ]
	do
		if "$@"
		then
			return 0
		fi

		sleep 1
		waited=$((waited + 1))
	done

	return 1
}

cat > "$RUN/config.yaml" <<EOF
daemon:
  tailscale: false
  state_dir: "$RUN/state"
  socket: "$RUN/d.sock"
  automation_dir: "$RUN/pods"
defaults:
  workspace_root: "$RUN/agents"
  autostart: false
agents:
  - id: alice
    name: Alice
  - id: bob
    name: Bob
EOF

"$BUILD/clawtillad" --config "$RUN/config.yaml" > "$RUN/daemon.log" 2>&1 &
DAEMON_PID=$!

CLI="$BUILD/clawtilla --socket $RUN/d.sock"

daemon_up () {
	$CLI agent list >/dev/null 2>&1
}

wait_for 20 daemon_up || fail "the daemon never came up"

$CLI room create standup --members alice,bob >/dev/null 2>&1 \
	|| fail "could not create the room"

"$BUILD/clawtilla-web" --socket "$RUN/d.sock" --port "$PORT" \
	--no-tailscale > "$RUN/web.log" 2>&1 &
WEB_PID=$!

URL="http://127.0.0.1:$PORT"

web_up () {
	curl -sf "$URL/" >/dev/null 2>&1
}

wait_for 20 web_up || fail "the web client never started listening"

# The manifest, with the media type an installability check reads.
curl -s -D "$RUN/manifest.head" -o "$RUN/manifest.json" \
	"$URL/manifest.webmanifest"
grep -qi '^content-type: application/manifest+json' "$RUN/manifest.head" \
	|| fail "the manifest is not served as application/manifest+json"
grep -q '"display" : "standalone"' "$RUN/manifest.json" \
	|| fail "the manifest does not ask for a standalone window"

# Every icon it names is reachable, as a PNG, at the path it names.
grep -o '/static/icons/[a-z0-9-]*\.png' "$RUN/manifest.json" | sort -u \
	> "$RUN/icons.txt"
[ -s "$RUN/icons.txt" ] || fail "the manifest names no icons"

while read -r icon
do
	curl -s -D "$RUN/icon.head" -o "$RUN/icon.png" "$URL$icon"
	grep -qi '^content-type: image/png' "$RUN/icon.head" \
		|| fail "$icon is not served as a PNG"
done < "$RUN/icons.txt"

# A page links it.
curl -s "$URL/r/standup" > "$RUN/room.html"
grep -q 'rel="manifest"' "$RUN/room.html" \
	|| fail "the room page does not link the manifest"

# /r/<group room> still opens the room.
grep -q 'class="composer-inner"' "$RUN/room.html" \
	|| fail "/r/standup no longer draws the room"

# /r/<agent> is the agent's chat, by redirect.
code=$(curl -s -o /dev/null -w '%{http_code} %{redirect_url}' "$URL/r/alice")
case "$code" in
	30[0-9]*"/a/alice/chat") ;;
	*) fail "/r/alice did not redirect to the agent's chat ($code)" ;;
esac

# A name nobody has: a 404 that names it, not an empty room.
code=$(curl -s -o "$RUN/ghost.html" -w '%{http_code}' "$URL/r/ghost")
[ "$code" = "404" ] || fail "/r/ghost answered $code, not 404"
grep -q 'no agent or room called' "$RUN/ghost.html" \
	|| fail "/r/ghost does not say there is nothing by that name"

# A spelling no id can have is refused the same way.
code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/r/Not%20A%20Room")
[ "$code" = "404" ] || fail "a malformed /r/ target answered $code"

# The exchange page is reachable, not swallowed by /a/:id/:view.
curl -s "$URL/a/alice/exchange?with=bob" > "$RUN/exchange.html"
grep -q 'Between bob and alice' "$RUN/exchange.html" \
	|| fail "the exchange page did not render"

echo "test-web-open: OK"
