#!/usr/bin/env bash
#
# test-release-drift.sh - What start() builds, release_components() releases.
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# clawt_daemon_start() assigns a member, and release_components() -- run
# by the next start and by dispose -- releases it.  That correspondence
# is a hand-maintained list, and it drifted the way every hand-maintained
# list here has: the automation, the routine runner, the notifier and the
# skill library were cleared only in finalize, which runs once, so an
# embedded host that stopped and started the daemon in one process leaked
# a generation of each per restart.  The ASAN gate found them one member
# at a time -- fix the first and the next is revealed -- which is a slow
# way to learn that the list is the problem.
#
# So this walks the source instead: every `self->member = ` assignment in
# the start-side functions must be matched by a `&self->member` (a
# g_clear_object / g_clear_pointer argument) or a `self->member = 0/NULL`
# reset in the release-side functions.  A member added to one side and
# not the other fails the suite with its name, instead of waiting for a
# sanitizer run to notice.
#
# What this cannot see: members created lazily outside start (the
# decision store, the summariser and the connector catalog build
# themselves on first use behind their own NULL guards), members assigned
# in start-side helpers not listed in SCOPE_A, and whether a release is
# *sufficient* -- it proves the member is named on the release side, not
# that unreffing it stops what it was doing.

set -u

# CDPATH= or the resolved directory is echoed into the output -- the
# trap this repo has already written down once.
CDPATH='' cd "$(dirname "$0")/.." || exit 1

# The two sides of the correspondence.  A start-side helper that assigns
# members belongs in SCOPE_A; a teardown helper release_components()
# calls belongs in SCOPE_B.  Format: file:function.
SCOPE_A=(
    "src/core/clawt-daemon.c:clawt_daemon_start"
    "src/core/clawt-daemon.c:clawt_daemon_reload_skills"
    "src/core/daemon-trigger.c:clawt_daemon_triggers_start"
    "src/core/daemon-venture.c:clawt_daemon_venture_start"
)

SCOPE_B=(
    "src/core/clawt-daemon.c:release_components"
    "src/core/clawt-daemon.c:sweep_cancel"
    "src/core/clawt-daemon.c:autostart_cancel"
    "src/core/daemon-trigger.c:clawt_daemon_triggers_stop"
    "src/core/daemon-venture.c:clawt_daemon_venture_stop"
    "src/core/daemon-turn.c:clawt_daemon_turn_teardown"
    "src/core/daemon-handoff.c:clawt_daemon_handoff_teardown"
    "src/core/daemon-teach.c:clawt_daemon_teach_teardown"
)

# Members assigned in start that deliberately have no release.  An entry
# here needs the reason beside it, or it is just the drift with a label.
EXCEPTIONS=(
    "running"    # a flag: stop() lowers it, and it holds no memory
)

# Comments are text too.  A member assigned in a comment, or a clear
# mentioned in one, must not count -- the parity check learnt this one
# layer at a time, so here it applies from the start.
strip_comments () {
    awk '
    {
        line = $0
        out = ""

        while (length(line) > 0) {
            if (incomment) {
                p = index(line, "*/")
                if (p == 0) { line = "" }
                else { incomment = 0; line = substr(line, p + 2) }
            } else {
                p = index(line, "/*")
                if (p == 0) { out = out line; line = "" }
                else {
                    out = out substr(line, 1, p - 1)
                    incomment = 1
                    line = substr(line, p + 2)
                }
            }
        }

        print out
    }' "$@"
}

# One function's body.  The name sits at column 0 on its own line, per
# the house style, and the body ends at the first column-0 brace -- inner
# blocks are always indented, so this cannot end early.
extract_body () {
    local file="${1}"
    local func="${2}"

    if ! grep -q "^${func}(" "${file}"; then
        echo "release-drift: ${file} has no function ${func}()" >&2
        return 1
    fi

    strip_comments "${file}" | awk -v fn="${func}" '
        $0 ~ "^" fn "\\(" { inside = 1 }
        inside { print }
        inside && /^}/ { exit }
    '
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/clawt-release-drift-XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

fail=0

for entry in "${SCOPE_A[@]}"
do
    extract_body "${entry%%:*}" "${entry##*:}" >> "${WORK}/start" || fail=1
done

for entry in "${SCOPE_B[@]}"
do
    extract_body "${entry%%:*}" "${entry##*:}" >> "${WORK}/release" || fail=1
done

[ "${fail}" -ne 0 ] && exit 1

# `self->name = value`, and never `==`: the pattern needs the space after
# a single equals sign, which a comparison does not have at that offset.
grep -oE 'self->[a-z_]+ = ' "${WORK}/start" \
    | sed -E 's/self->([a-z_]+) = /\1/' | sort -u > "${WORK}/assigned"

{
    grep -oE '&self->[a-z_]+' "${WORK}/release" \
        | sed -E 's/&self->//'
    grep -oE 'self->[a-z_]+ = (0|NULL|FALSE|-1)' "${WORK}/release" \
        | sed -E 's/self->([a-z_]+) = .*/\1/'
} | sort -u > "${WORK}/released"

while read -r member
do
    for exception in "${EXCEPTIONS[@]}"
    do
        [ "${member}" = "${exception}" ] && continue 2
    done

    if ! grep -qx "${member}" "${WORK}/released"; then
        echo "release-drift: clawt_daemon_start() assigns self->${member}" \
             "and nothing on the release side names it -- a stop/start" \
             "cycle leaks one per restart. Release it in" \
             "release_components(), or add it to EXCEPTIONS with the" \
             "reason."
        fail=1
    fi
done < "${WORK}/assigned"

if [ "${fail}" -ne 0 ]; then
    echo "release-drift: FAILED"
    exit 1
fi

echo "release-drift: OK ($(wc -l < "${WORK}/assigned") member(s) checked)"
