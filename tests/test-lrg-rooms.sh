#!/usr/bin/env bash
#
# test-lrg-rooms.sh - clawtilla-lrg against a real daemon
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# The libregnum client's pages are tested headless (tests/test-lrgui-*.c)
# and its decisions without libregnum at all (tests/test-lrg-models*.c).
# What neither can see is the client talking to a real clawtillad: the
# connection, the subscription, agent.list and room.history arriving and
# being drawn, a message sent from elsewhere landing in the transcript,
# and a real window rendering frames rather than a black rectangle.
#
# Structure is asserted through `--self-test PAGE:TARGET`, which prints
# the widget tree as JSON, rather than through pixel coordinates: a
# libregnum layout has no accessibility tree for xdotool to search, and
# a click at a guessed coordinate tests the guess.  The real window is
# still opened once, under Xvfb, and must draw something that is not the
# clear colour.
#
# Needs CLAWT_TEST_INTEGRATION=1, an LRG_BACKEND=1 build, and Xvfb.

set -eu

if [ "${CLAWT_TEST_INTEGRATION:-0}" != "1" ]; then
    echo "test-lrg-rooms: skipped (set CLAWT_TEST_INTEGRATION=1)"
    exit 0
fi

BUILD="${CLAWT_BUILD_DIR:-build/debug}"
DISPLAY_NUM="${CLAWT_TEST_DISPLAY:-:95}"

[ -x "$BUILD/clawtilla-lrg" ] || {
    echo "test-lrg-rooms: skipped (no clawtilla-lrg in $BUILD; build with LRG_BACKEND=1)"
    exit 0
}

command -v Xvfb >/dev/null 2>&1 || {
    echo "test-lrg-rooms: skipped (Xvfb is not installed)"
    exit 0
}

RUN=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/clawt-lrgroom-XXXXXX")

cleanup () {
    pkill -f "clawtilla-lrg --socket $RUN/d.sock" 2>/dev/null || true
    pkill -f "clawtillad --config $RUN/config.yaml" 2>/dev/null || true
    pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null || true
    rm -rf "$RUN"
}
trap cleanup EXIT

fail () {
    echo "test-lrg-rooms: FAILED: $*"
    exit 1
}

# Polls a check until it passes or the deadline runs out; "sent" and
# "delivered" are different moments, and a fixed sleep is a race.
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

# The widget tree for one page, as the client draws it.  WAYLAND_DISPLAY
# names a socket that does not exist: unset, libwayland tries wayland-0
# and GLFW opens on the desktop rather than on Xvfb.
tree () {
    env -u XDG_SESSION_TYPE WAYLAND_DISPLAY=no-wayland-here \
        "$BUILD/clawtilla-lrg" --socket "$RUN/d.sock" --self-test "$1" \
        2>"$RUN/selftest.err"
}

tree_says () {
    tree "$1" > "$RUN/tree.json" && grep -qF -- "$2" "$RUN/tree.json"
}

room_says () {
    $CLI room history standup 2>/dev/null | grep -q "$1"
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
    description: Finds sources
  - id: bob
    name: Bob
    description: Builds things
  - id: carol
    name: Carol
EOF

"$BUILD/clawtillad" --config "$RUN/config.yaml" > "$RUN/daemon.log" 2>&1 &
sleep 2

CLI="$BUILD/clawtilla --socket=$RUN/d.sock"
$CLI room create standup --members alice,bob,carol >/dev/null 2>&1 \
    || fail "could not create the room"
$CLI send standup "@alice the lrg probe says hello" >/dev/null 2>&1

wait_for 20 room_says "lrg probe says hello" \
    || fail "the message never reached the room"

# 1. the fleet is drawn: every agent's name is in the sidebar
tree_says shell "Alice" || fail "the sidebar does not name Alice"
grep -qF "Bob" "$RUN/tree.json" || fail "the sidebar does not name Bob"
# a room with no name of its own is titled by its members, as in GTK
grep -qF "3 members" "$RUN/tree.json" || fail "the sidebar does not list the room"

# 2. the room's transcript, opened by id
wait_for 10 tree_says chat:standup "lrg probe says hello" \
    || fail "the room's transcript does not show the message"

# 3. a message sent from elsewhere while nothing is selected still lands
$CLI send standup "second probe from the CLI" >/dev/null 2>&1
wait_for 10 tree_says chat:standup "second probe from the CLI" \
    || fail "a later message never reached the transcript"

# 4. the pages with lists render their content from a real daemon
tree_says agent:alice "Alice" || fail "the Overview page does not draw Alice"
tree_says activity "Activity" || fail "the Activity page did not build"
tree_says decisions "Decisions" || fail "the Decisions page did not build"

# 5. a real window draws frames under Xvfb, not a black rectangle
Xvfb "$DISPLAY_NUM" -screen 0 1280x800x24 > "$RUN/xvfb.log" 2>&1 &
sleep 2

env -u XDG_SESSION_TYPE WAYLAND_DISPLAY=no-wayland-here DISPLAY="$DISPLAY_NUM" \
    CLAWT_LRG_SCREENSHOT="$RUN/shot.png" CLAWT_LRG_SCREENSHOT_FRAMES=90 \
    timeout 60 "$BUILD/clawtilla-lrg" --socket "$RUN/d.sock" \
    > "$RUN/lrg.log" 2>&1 || fail "the window exited with an error ($(tail -3 "$RUN/lrg.log"))"

[ -s "$RUN/shot.png" ] || fail "the window saved no screenshot"

# a frame that is all one colour is a window that drew nothing
if command -v magick >/dev/null 2>&1
then
    colours=$(magick "$RUN/shot.png" -format '%k' info:)
    [ "${colours:-0}" -gt 64 ] \
        || fail "the window drew only ${colours} colours: nothing rendered"
fi

echo "test-lrg-rooms: ok"
