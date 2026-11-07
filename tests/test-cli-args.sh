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

if [[ ${failures} -ne 0 ]]
then
    exit 1
fi

echo "test-cli-args: ok"
