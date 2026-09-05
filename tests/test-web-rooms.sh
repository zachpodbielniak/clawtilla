#!/bin/sh
# test-web-rooms.sh - Slash commands in the web client's room chat
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# Three bugs shipped in the web client's composer and none of them was
# reachable from `make test`, because nothing in it serves a page:
#
#   * The slash list on a room page asked `/a/<room-id>/commands`.
#     `skill.commands` wants an agent and answers "no such agent" for
#     anything else, so the list came back empty and typing `/` in a
#     room drew nothing at all.
#   * The list was filled *only* from that call, so even on an agent
#     page it never held one of the commands the composer answers --
#     no /help, no /stop -- while the GTK list has shown both all along.
#   * on_room_send() sent whatever was typed to msg.send, so a slash
#     command in a room was posted into the conversation: `/help`
#     arrived as a message reading "/help", addressed to nobody.
#
# Each looked like a client that "just does not do that yet" rather than
# one that was wrong, which is why all three lasted.  A rule two clients
# share can be pushed into libclawt and tested there; "what does this
# page actually serve" cannot.
#
# Behind CLAWT_TEST_INTEGRATION because it needs a daemon, a real socket
# and a listening port, none of which `make test` may touch. It needs no
# display: the assertions are on the bytes the server sends.

set -eu

if [ "${CLAWT_TEST_INTEGRATION:-0}" != "1" ]; then
    echo "test-web-rooms: skipped (set CLAWT_TEST_INTEGRATION=1)"
    exit 0
fi

BUILD="${CLAWT_BUILD_DIR:-build/debug}"
PORT="${CLAWT_TEST_WEB_PORT:-8971}"

command -v curl >/dev/null 2>&1 || {
    echo "test-web-rooms: skipped (curl is not installed)"
    exit 0
}

[ -x "$BUILD/clawtilla-web" ] || {
    echo "test-web-rooms: skipped (no web client in $BUILD)"
    exit 0
}

# Under XDG_RUNTIME_DIR: sockaddr_un.sun_path is 108 bytes and a build
# tree's temporary path is longer than that.
RUN=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/clawt-webroom-XXXXXX")

cleanup () {
    [ -n "${WEB_PID:-}" ] && kill "$WEB_PID" 2>/dev/null
    [ -n "${DAEMON_PID:-}" ] && kill "$DAEMON_PID" 2>/dev/null
    rm -rf "$RUN"
}
trap cleanup EXIT

fail () {
    echo "test-web-rooms: FAILED: $*"
    exit 1
}

# Polls until the check passes or the deadline runs out. Starting a
# daemon and a listener are two things that finish when they finish, and
# a fixed sleep between them is a race rather than a wait.
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
  - id: carol
    name: Carol
EOF

"$BUILD/clawtillad" --config "$RUN/config.yaml" > "$RUN/daemon.log" 2>&1 &
DAEMON_PID=$!

CLI="$BUILD/clawtilla --socket $RUN/d.sock"

daemon_up () {
    $CLI agent list >/dev/null 2>&1
}

wait_for 20 daemon_up || fail "the daemon never came up"

$CLI room create standup --members alice,bob,carol >/dev/null 2>&1 \
    || fail "could not create the room"

"$BUILD/clawtilla-web" --socket "$RUN/d.sock" --port "$PORT" \
    > "$RUN/web.log" 2>&1 &
WEB_PID=$!

URL="http://127.0.0.1:$PORT"

web_up () {
    curl -sf "$URL/" >/dev/null 2>&1
}

wait_for 20 web_up || fail "the web client never started listening"

# The list a person sees after typing `/` in a room. Asserted on two
# entries rather than one: /help is the command the empty list made
# unreachable, and /stop is one this page will refuse -- both have to be
# offered, because the GTK list offers both and the refusal is what
# explains the difference.
ROOM=$(curl -s "$URL/r/standup")

echo "$ROOM" | grep -q 'data-command="/help"' \
    || fail "the room's slash list does not offer /help"

echo "$ROOM" | grep -q 'data-command="/stop"' \
    || fail "the room's slash list does not offer /stop"

# And it asks for no agent's skills: skill.commands needs an agent, and
# a room asking with its own id is what emptied this list.
echo "$ROOM" | grep -q 'hx-get="/a/standup/commands"' \
    && fail "the room still asks for an agent's skill commands"

# A fleet-wide command runs rather than being posted.
curl -s -L -X POST "$URL/r/standup/send" -d "body=/help" > "$RUN/help.html" \
    || fail "/help in a room was refused by the server"

grep -q "Back to the chat" "$RUN/help.html" \
    || fail "/help in a room did not render the command list"

grep -q 'href="/r/standup"' "$RUN/help.html" \
    || fail "/help in a room offers no way back to the room"

# An agent-scoped one says why it cannot run, in the room.
curl -s -L -X POST "$URL/r/standup/send" -d "body=/stop" > "$RUN/stop.html"

grep -q "about one agent" "$RUN/stop.html" \
    || fail "/stop in a room did not say it is about one agent"

# A typo is a typo, not a scope problem -- being told a misspelling is
# "about one agent" sends somebody looking for an agent to run it on.
curl -s -L -X POST "$URL/r/standup/send" -d "body=/halp" > "$RUN/halp.html"

grep -q "No such command" "$RUN/halp.html" \
    || fail "a misspelt command in a room was not reported as unknown"

grep -q "about one agent" "$RUN/halp.html" \
    && fail "a misspelt command was reported as being about one agent"

# None of that reached the conversation. This is the assertion the whole
# file is for: every one of those three used to arrive as a message.
HISTORY=$($CLI room history standup 2>/dev/null || true)

echo "$HISTORY" | grep -q "/help" \
    && fail "/help was posted into the room as a message"

echo "$HISTORY" | grep -q "/halp" \
    && fail "/halp was posted into the room as a message"

# And an ordinary message still goes where it always did.
curl -s -L -X POST "$URL/r/standup/send" -d "body=@bob morning" >/dev/null

room_has_message () {
    $CLI room history standup 2>/dev/null | grep -q "@bob morning"
}

wait_for 20 room_has_message \
    || fail "an ordinary message no longer reaches the room"

bob_has_mail () {
    $CLI mailbox list bob 2>/dev/null | grep -q "waiting"
}

wait_for 20 bob_has_mail \
    || fail "the named member was not delivered to"

# A skill command typed at an agent expands and is sent.
#
# The composer offered these in its list and answered "No such command"
# when one was sent, because run_command() had no skill fallback -- so
# every entry the list held was one that could not be run.  And the
# route that did expand built its own frame named `message.send`, which
# no handler answers, so even the skills page expanded correctly and
# then sent nothing while reporting success.
#
# Skipped rather than failed when the agent has no skills: this asserts
# on what a fleet happens to have installed, and a machine with none is
# not a broken build.
SKILL=$(curl -s "$URL/a/alice/commands" 2>/dev/null \
    | grep -o 'data-command="[^"]*"' | head -1 \
    | sed 's/data-command="//; s/"//')

if [ -n "$SKILL" ]; then
    curl -s -L -X POST "$URL/a/alice/send" -d "body=$SKILL" \
        > "$RUN/skill.html"

    grep -q "There is no" "$RUN/skill.html" \
        && fail "a skill command from the composer was reported as unknown"

    alice_has_mail () {
        $CLI mailbox list alice 2>/dev/null | grep -q "waiting"
    }

    wait_for 20 alice_has_mail \
        || fail "the expanded skill command reached nobody"
fi

# And a command that is neither built in nor a skill says so, in the
# words the GTK client uses -- the daemon's own message is accurate and
# stops short of saying where the list is.
curl -s -L -X POST "$URL/a/alice/send" -d "body=/definitely-not-a-command" \
    > "$RUN/nocmd.html"

grep -q "Type /help for the list" "$RUN/nocmd.html" \
    || fail "an unknown command did not point at /help"

echo "test-web-rooms: ok"
