#!/usr/bin/env bash
# test-office-board-shots.sh - The usage board, drawn, in both shipped worlds
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# tests/test-office-board.c checks the board's layout with a font of
# known widths.  This checks the drawn thing: clawtilla-office draws the
# usage fixture (tests/fixtures/office/usage-board.json -- long login
# names, four windows on one login, a balance, a login with none) in
# modern-office and cozy-town, at Fit and 2x, from disk with --pack so no
# daemon is involved, and tests/office-board-check.py holds each frame
# to its --usage-report:
#
#   * the board is inside its zone, every item and every text within its
#     column, at the expected level of detail (gauges at Fit, rows at 2x);
#   * the frame with the board and the frame with --usage off differ
#     nowhere outside the zone -- the first board spilled over walls,
#     servers and people;
#   * the details panel, opened, is inside the view and lists or counts
#     every window;
#   * in 3D, with the default camera, at the default window size and at
#     4K with --ui-scale 2, the board is drawn and both the board and the
#     rectangle its corners cover are inside the view (VENTURE #24: in
#     modern-office it stood below the frame), and the board still changes
#     the frame only inside that rectangle.
#
# The frames with the board are kept as references in
# tests/fixtures/office/board-shots/ (CLAWT_BOARD_SHOTS_UPDATE=1 rewrites
# them); they are compared by the checks above, not byte for byte, since
# a software renderer's pixels differ between Mesa versions.
# CLAWT_BOARD_SHOTS_OUT=DIR copies every frame and report there, to look
# at.
#
# Needs an OFFICE=1 build, Xvfb and python3; skipped, saying why,
# without any of them.

set -euo pipefail

BUILD="${CLAWT_BUILD_DIR:-build/release}"
HERE="$(CDPATH='' cd -- "$(dirname -- "${0}")" && pwd)"
TOP="$(dirname -- "${HERE}")"
FIXTURES="${HERE}/fixtures/office"
OFFICE="${BUILD}/clawtilla-office"

skip () {
	echo "test-office-board-shots: skipped (${1})"
	exit 0
}

[[ -x "${OFFICE}" ]] || skip "no clawtilla-office in ${BUILD}: build with OFFICE=1"
command -v Xvfb >/dev/null 2>&1 || skip "Xvfb is not installed"
command -v python3 >/dev/null 2>&1 || skip "python3 is not installed"

RUN=$(mktemp -d "${TMPDIR:-/tmp}/clawt-board-XXXXXX")
XVFB_PID=""

cleanup () {
	if [[ -n "${XVFB_PID}" ]]
	then
		kill "${XVFB_PID}" 2>/dev/null || true
		wait "${XVFB_PID}" 2>/dev/null || true
	fi
	rm -rf "${RUN}"
}
trap cleanup EXIT

# a display of our own: -displayfd picks a free number and says which
Xvfb -displayfd 3 -screen 0 1920x1080x24 -nolisten tcp 3> "${RUN}/display" \
	> "${RUN}/xvfb.log" 2>&1 &
XVFB_PID=$!
for _ in $(seq 50)
do
	[[ -s "${RUN}/display" ]] && break
	sleep 0.1
done
[[ -s "${RUN}/display" ]] || skip "Xvfb did not start: $(tail -n 1 "${RUN}/xvfb.log")"
DISPLAY=":$(head -n 1 "${RUN}/display")"
LIBGL_ALWAYS_SOFTWARE=1
export DISPLAY LIBGL_ALWAYS_SOFTWARE

fail=0

# shot NAME WORLD [clawtilla-office args...]
shot () {
	local name="${1}"
	local world="${2}"
	shift 2

	if ! "${OFFICE}" --pack "${TOP}/data/office/worlds/${world}" \
		--layout-file "${FIXTURES}/board-layout-${world}.json" \
		--usage-file "${FIXTURES}/usage-board.json" \
		--screenshot "${RUN}/${name}.png" --usage-report "${RUN}/${name}.json" \
		"$@" > "${RUN}/${name}.log" 2>&1
	then
		echo "  FAIL ${name}: clawtilla-office failed"
		sed 's/^/    /' "${RUN}/${name}.log"
		fail=1
		return 1
	fi
}

check () {
	local what="${1}"
	shift

	if python3 -I "${HERE}/office-board-check.py" "$@"
	then
		echo "  ok ${what}"
	else
		echo "  FAIL ${what}"
		fail=1
	fi
}

for world in modern-office cozy-town
do
	for zoom in fit 2x
	do
		case "${zoom}" in
			fit) detail=gauges ;;
			*)   detail=rows ;;
		esac
		name="${world}-${zoom}"
		# lynx stands at the board: 2x is centred on it, so the board is in view
		shot "${name}" "${world}" --zoom "${zoom}" --focus lynx || continue
		shot "${name}-off" "${world}" --zoom "${zoom}" --focus lynx --usage off || continue
		check "${name}: the board stays in its zone (${detail})" board \
			"${RUN}/${name}.png" "${RUN}/${name}-off.png" "${RUN}/${name}.json" "${detail}"
		if [[ "${CLAWT_BOARD_SHOTS_UPDATE:-0}" == "1" ]]
		then
			mkdir -p "${FIXTURES}/board-shots"
			cp "${RUN}/${name}.png" "${FIXTURES}/board-shots/${name}.png"
		fi
	done
	shot "${world}-panel" "${world}" --usage-open || continue
	check "${world}: the details panel stays in view and lists every window" panel \
		"${RUN}/${world}-panel.json"
	shot "${world}-corner" "${world}" --usage corner || continue
	check "${world}: the corner panel stays in view" panel "${RUN}/${world}-corner.json"

	# 3D at the default camera: the window's size, and 4K at --ui-scale 2
	for size in default 4k
	do
		case "${size}" in
			default) args=(--3d) ;;
			*)       args=(--3d --width 3840 --height 2160 --ui-scale 2) ;;
		esac
		name="${world}-3d-${size}"
		shot "${name}" "${world}" "${args[@]}" || continue
		shot "${name}-off" "${world}" "${args[@]}" --usage off || continue
		check "${name}: the board is in the default 3D view" inview "${RUN}/${name}.json" 3d
		# a gauge per login: at the default framing the board is ~130-470 px
		# wide (the window to 4K), room for gauges and not for rows
		check "${name}: the board stays in its rectangle (gauges)" board \
			"${RUN}/${name}.png" "${RUN}/${name}-off.png" "${RUN}/${name}.json" gauges
		if [[ "${CLAWT_BOARD_SHOTS_UPDATE:-0}" == "1" ]]
		then
			mkdir -p "${FIXTURES}/board-shots"
			cp "${RUN}/${name}.png" "${FIXTURES}/board-shots/${name}.png"
		fi
	done
done

if [[ -n "${CLAWT_BOARD_SHOTS_OUT:-}" ]]
then
	mkdir -p "${CLAWT_BOARD_SHOTS_OUT}"
	cp "${RUN}"/*.png "${RUN}"/*.json "${CLAWT_BOARD_SHOTS_OUT}/"
fi

if [[ "${fail}" -ne 0 ]]
then
	echo "test-office-board-shots: FAILED (frames left in ${RUN}.kept)"
	cp -a "${RUN}" "${RUN}.kept"
	exit 1
fi
echo "test-office-board-shots: passed"
