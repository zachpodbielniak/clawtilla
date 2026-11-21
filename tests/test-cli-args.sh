#!/usr/bin/env bash
#
# test-cli-args.sh - What the CLI says about a command line it cannot run.
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# Global options are parsed from a copy of argv, and when no known verb
# followed them argv itself was never updated -- so `clawtilla -s SOCK
# list` answered "unknown command '-s'", naming the one word that had been
# understood and sending the reader to look at the socket flag. Found by
# typing it during a live check.
#
# Hermetic: an unknown command is refused before any connection is made,
# so no daemon is needed and the socket named need not exist.

set -u

CDPATH='' cd "$(dirname "$0")/.." || exit 1

# The build under test: `make test` passes its own, so a release run never
# checks a stale debug binary or the other way round.
cli="${CLAWT_BUILD_DIR:-build/release}/clawtilla"

if [[ ! -x "${cli}" ]]
then
    echo "test-cli-args: ${cli} is not built"
    exit 1
fi

failures=0

check () {
    if [[ $# -ne 2 ]]
    then
        echo "\`check()\` requires 2 positional arguments"
        echo 'check <expected-word> <args...as one string>'
        exit 1
    fi

    local expected="${1}"
    local args="${2}"
    local output

    # shellcheck disable=SC2086
    output="$("${cli}" ${args} 2>&1)"

    if [[ "${output}" != *"unknown command '${expected}'"* ]]
    then
        echo "test-cli-args: '${args}' should name '${expected}', said: ${output}"
        failures=$((failures + 1))
    fi
}

check list "list"
check list "-s /nonexistent/d.sock list"
check list "--socket=/nonexistent/d.sock list"
check frobnicate "-s /nonexistent/d.sock frobnicate extra words"

# Non-ASCII survives the trip to the terminal.  Without setlocale() the
# CLI ran in the "C" locale, and GLib converts what g_print()/g_printerr()
# write to a *terminal* into that locale's ASCII, every other character
# becoming '?': `device pair --qr` drew its QR code as a field of question
# marks.  Only a tty shows it -- a pipe or file gets the bytes untouched --
# so the CLI runs under script(1) for a pseudo-terminal.  An unknown verb
# is echoed back, which is the hermetic way to see it.  C.UTF-8 is built
# into glibc, so no locale package is needed.
if command -v script > /dev/null
then
    utf8_output="$(LC_ALL=C.UTF-8 script -qec "${cli} frobnicaté" /dev/null 2>&1)"

    if [[ "${utf8_output}" != *"unknown command 'frobnicaté'"* ]]
    then
        echo "test-cli-args: a UTF-8 word came back mangled on a terminal: ${utf8_output}"
        failures=$((failures + 1))
    fi
else
    echo "test-cli-args: script(1) not found (util-linux); terminal UTF-8 check skipped"
fi

#
# The notification verbs (docs/clients.org, "Notifications") read their
# own arguments before they reach for a daemon, so these need none: help
# is asked for with --help and exits 0, and a mute length that does not
# parse is refused with the usage rather than sent -- regressing that
# mutes for two minutes when somebody typed "2hours".
#
check_usage () {
    if [[ $# -ne 3 ]]
    then
        echo "\`check_usage()\` requires 3 positional arguments"
        echo 'check_usage <expected-rc> <expected-text> <args...as one string>'
        exit 1
    fi

    local expected_rc="${1}"
    local expected="${2}"
    local args="${3}"
    local output
    local rc

    # shellcheck disable=SC2086
    output="$("${cli}" -s /nonexistent/d.sock ${args} 2>&1)"
    rc=$?

    if [[ ${rc} -ne ${expected_rc} || "${output}" != *"${expected}"* ]]
    then
        echo "test-cli-args: '${args}' should exit ${expected_rc} saying '${expected}', exited ${rc}: ${output}"
        failures=$((failures + 1))
    fi
}

check_usage 0 "--for DURATION" "mute --help"
check_usage 1 "--for DURATION" "mute scout --for 2hours"
check_usage 1 "--for DURATION" "mute scout --for"
check_usage 1 "clawtilla unmute <agent>" "unmute"
check_usage 0 "notices dismiss <ID>" "notices --help"
check_usage 1 "notices dismiss <ID>" "notices dismiss"

# `agent face` (0.53.0): the shape list and a shape's SVG need no daemon,
# so they are answered against a socket that does not exist; a colour
# that is not hex is refused rather than written into the markup.
check_usage 0 "squircle" "agent face --list"
check_usage 0 "initials" "agent face --list"
check_usage 0 "<svg " "agent face --svg cloud --size 48"
check_usage 0 'fill="#22b573"' "agent face --svg cloud --color #22b573"
check_usage 1 "is not a colour" "agent face --svg cloud --color red"
check_usage 1 "clawtilla agent face <agent>" "agent face"
check_usage 0 "--color HEX|auto" "agent face --help"

if [[ ${failures} -ne 0 ]]
then
    exit 1
fi

echo "test-cli-args: ok"
