#!/usr/bin/env bash
# test-browser-integration.sh - A real browser session, end to end
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# tests/test-browser.c checks everything above gsurf's control socket
# with a fake standing in for gsurf.  This checks the part a fake cannot:
# that a daemon built with GSURF=1 starts an Xvfb and a gsurf, that the
# page it opens has run its JavaScript, that a person's click lands, and
# that the session stops when asked.
#
# Two things went wrong here that no unit test could see, and both look
# like "gsurf did not answer within 20 seconds":
#   * an older gsurf on PATH (0.4.4, with no control.name) was found
#     before the 0.5.10 build GSURF=1 had checked;
#   * Xvfb started with -nolisten unix-abstract is reachable only through
#     /tmp/.X11-unix, which a daemon with a private /tmp does not share.
#
# Behind CLAWT_TEST_INTEGRATION: it needs Xvfb, a GSURF=1 build and a
# gsurf, and starts real processes.

set -euo pipefail

if [[ "${CLAWT_TEST_INTEGRATION:-0}" != "1" ]]
then
	echo "test-browser-integration: skipped (set CLAWT_TEST_INTEGRATION=1)"
	exit 0
fi

BUILD="${CLAWT_BUILD_DIR:-build/release}"
FIXTURES="$(CDPATH='' cd -- "$(dirname -- "${0}")" && pwd)/fixtures"

skip () {
	echo "test-browser-integration: skipped (${1})"
	exit 0
}

command -v Xvfb >/dev/null 2>&1 || skip "Xvfb is not installed"
[[ -x "${BUILD}/clawtillad" ]] || skip "no daemon in ${BUILD}"

# Under XDG_RUNTIME_DIR: sockaddr_un.sun_path is 108 bytes.
RUN=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/clawt-browser-XXXXXX")
DAEMON_PID=""

cleanup () {
	if [[ -n "${DAEMON_PID}" ]]
	then
		kill "${DAEMON_PID}" 2>/dev/null || true
		wait "${DAEMON_PID}" 2>/dev/null || true
	fi
	rm -rf "${RUN}"
}
trap cleanup EXIT

cat > "${RUN}/clawtilla.yaml" <<YAML
daemon:
  state_dir: ${RUN}/state
  socket: ${RUN}/d.sock
defaults:
  workspace_root: ${RUN}/ws
agents:
  - id: surfer
    browser:
      mode: own
      width: 800
      height: 600
YAML

"${BUILD}/clawtillad" -c "${RUN}/clawtilla.yaml" > "${RUN}/daemon.log" 2>&1 &
DAEMON_PID=$!

cli () {
	"${BUILD}/clawtilla" -s "${RUN}/d.sock" "$@"
}

for _ in $(seq 50)
do
	[[ -S "${RUN}/d.sock" ]] && break
	sleep 0.1
done

if cli browser | grep -q 'built without GSURF=1'
then
	skip "the daemon in ${BUILD} was built without GSURF=1"
fi

fail () {
	echo "test-browser-integration: FAILED: ${1}"
	sed -n '1,40p' "${RUN}/daemon.log"
	exit 1
}

# The page's text is written by its script; the title changes on a click.
opened=$(cli browser open surfer "file://${FIXTURES}/browser-js.html") ||
	fail "open: ${opened:-no output}"
[[ "${opened}" == *"JavaScript ran"* ]] ||
	fail "the page's script did not run (title: ${opened})"

cli browser screenshot surfer "${RUN}/shot.png" >/dev/null || fail "screenshot"
[[ "$(head -c 8 "${RUN}/shot.png" | od -An -tx1 | tr -d ' \n')" == "89504e470d0a1a0a" ]] ||
	fail "the screenshot is not a PNG"

# The button fills the top left corner: a person's click there lands,
# and the page retitles itself.
clicked=$(cli browser click surfer 40 20) || fail "click: ${clicked:-no output}"
for _ in $(seq 20)
do
	[[ "${clicked}" == *"Clicked"* ]] && break
	sleep 0.25
	clicked=$(cli browser | sed -n '2p')
done
[[ "${clicked}" == *"Clicked"* ]] || fail "the click did not land (${clicked})"

cli browser stop surfer >/dev/null || fail "stop"
[[ "$(cli browser | sed -n '2p')" == *"Not running"* ]] || fail "still running after stop"

echo "test-browser-integration: ok"
