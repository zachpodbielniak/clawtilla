#!/bin/sh
# test-gtk-rooms.sh - Clicking a group room, and typing into it
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# Two null dereferences shipped in the group-rooms change and neither
# was reachable from `make test`: clicking a group room called
# g_hash_table_remove(unread, selected_agent) and sending into one
# called it on the drafts table, with selected_agent NULL by design for
# a room selection.  Both took the client down with nothing in the log,
# and the second did it *after* the message had been routed -- so the
# send worked and the client did not.
#
# The whole suite was green throughout, because nothing in it opens a
# window.  A rule two clients share can be pushed into libclawt and
# tested there; "does this widget survive being clicked" cannot, and
# that is the gap this closes.
#
# Behind CLAWT_TEST_INTEGRATION because it needs an X server, a daemon
# and a real socket, none of which `make test` may touch.

set -eu

if [ "${CLAWT_TEST_INTEGRATION:-0}" != "1" ]; then
    echo "test-gtk-rooms: skipped (set CLAWT_TEST_INTEGRATION=1)"
    exit 0
fi

BUILD="${CLAWT_BUILD_DIR:-build/debug}"
DISPLAY_NUM="${CLAWT_TEST_DISPLAY:-:96}"

for tool in Xvfb xdotool; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "test-gtk-rooms: skipped ($tool is not installed)"
        exit 0
    }
done

[ -x "$BUILD/clawtilla-gtk" ] || {
    echo "test-gtk-rooms: skipped (no GTK client in $BUILD)"
    exit 0
}

# Under XDG_RUNTIME_DIR: sockaddr_un.sun_path is 108 bytes and a build
# tree's temporary path is longer than that.
RUN=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/clawt-gtkroom-XXXXXX")

cleanup () {
    pkill -x clawtilla-gtk 2>/dev/null || true
    pkill -f "clawtillad --config $RUN/config.yaml" 2>/dev/null || true
    pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null || true
    rm -rf "$RUN"
}
trap cleanup EXIT

fail () {
    echo "test-gtk-rooms: FAILED: $*"
    exit 1
}

cat > "$RUN/config.yaml" <<EOF
daemon:
  tailscale: false
  state_dir: "$RUN/state"
  socket: "$RUN/d.sock"
  automation_dir: "$RUN/pods"
defaults:
  workspace_root: "$RUN/agents"
agents:
  - id: alice
    name: Alice
  - id: bob
    name: Bob
  - id: carol
    name: Carol
EOF

Xvfb "$DISPLAY_NUM" -screen 0 1280x720x24 > "$RUN/xvfb.log" 2>&1 &
sleep 2

"$BUILD/clawtillad" --config "$RUN/config.yaml" > "$RUN/daemon.log" 2>&1 &
sleep 2

CLI="$BUILD/clawtilla --socket $RUN/d.sock"
$CLI room create standup --members alice,bob,carol >/dev/null 2>&1 \
    || fail "could not create the room"
$CLI send standup "@alice what do you think?" >/dev/null 2>&1

# GDK_BACKEND, because GTK4 prefers Wayland when WAYLAND_DISPLAY is set
# and would then paint nothing onto the X server this test is watching.
env -u WAYLAND_DISPLAY GDK_BACKEND=x11 DISPLAY="$DISPLAY_NUM" \
    "$BUILD/clawtilla-gtk" --socket "$RUN/d.sock" > "$RUN/gtk.log" 2>&1 &
sleep 8

pgrep -x clawtilla-gtk >/dev/null || fail "the client did not start"

WID=$(DISPLAY="$DISPLAY_NUM" xdotool search --onlyvisible --name . | head -1)
[ -n "$WID" ] || fail "the client mapped no window"

# The room row sits under the three agents in the sidebar.
DISPLAY="$DISPLAY_NUM" xdotool mousemove --window "$WID" 128 296 click 1
sleep 3

pgrep -x clawtilla-gtk >/dev/null \
    || fail "the client died when a group room was clicked"

DISPLAY="$DISPLAY_NUM" xdotool mousemove --window "$WID" 700 665 click 1
sleep 1
DISPLAY="$DISPLAY_NUM" xdotool type --window "$WID" --delay 30 "@bob take this"
sleep 1
DISPLAY="$DISPLAY_NUM" xdotool key --window "$WID" Return
sleep 4

pgrep -x clawtilla-gtk >/dev/null \
    || fail "the client died after sending into a group room"

# And the message actually went somewhere: into the room's transcript,
# and into the mailbox of the one member it named.
$CLI room history standup 2>/dev/null | grep -q "@bob take this" \
    || fail "the message never reached the room"

$CLI mailbox list bob 2>/dev/null | grep -q "waiting" \
    || fail "the named member was not delivered to"

# The `@` completion, end to end: typing "@b" offers the one member
# whose id starts with it, clicking the row inserts "bob " at the
# cursor, and the message that arrives names bob.
#
# Driven through the window rather than asserted on fill_mention_list()
# because the two failures worth catching here are not in the matcher:
# a roster that never reached the window offers nothing, and clicking a
# row is where the two crashes in this composer were.  Both look like a
# completion that "just does not work" and neither logs anything.
DISPLAY="$DISPLAY_NUM" xdotool mousemove --window "$WID" 700 665 click 1
sleep 1
DISPLAY="$DISPLAY_NUM" xdotool type --window "$WID" --delay 30 "@b"
sleep 2

# One candidate, so its row is the only one and sits directly above the
# composer.
DISPLAY="$DISPLAY_NUM" xdotool mousemove --window "$WID" 511 608 click 1
sleep 2

pgrep -x clawtilla-gtk >/dev/null \
    || fail "the client died when a completion was chosen"

DISPLAY="$DISPLAY_NUM" xdotool type --window "$WID" --delay 30 "via completion"
sleep 1
DISPLAY="$DISPLAY_NUM" xdotool key --window "$WID" Return
sleep 4

# The trailing space is part of the insert: without it the id runs into
# the next word and names nobody.
$CLI room history standup 2>/dev/null | grep -q "@bob via completion" \
    || fail "the completion did not insert the member it offered"

echo "test-gtk-rooms: ok"
