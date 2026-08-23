#!/bin/sh
# clawt-test-floor.sh - Fail when suspiciously few tests ran.
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# A green test run only means "everything that ran passed".  It says
# nothing about whether everything we expected to run actually did.  A
# botched Makefile edit, a wildcard that stopped matching, or a test
# binary that failed to link all produce a perfectly green, nearly empty
# run -- and that reads as success.
#
# This adds the missing half of the claim: "and roughly everything we
# expected to run, ran."  Raise TEST_FLOOR whenever the suite grows.
#
# Usage: clawt-test-floor.sh <number-of-test-binaries-that-ran>

set -eu

# One per tests/test-*.c.  Left at 1 since the suite was three files, which
# made the check decorative -- a run where eleven of fourteen binaries
# failed to link was still "green, and at least one ran".
TEST_FLOOR=20

main () {
    if [ $# -ne 1 ]
    then
        echo 'clawt-test-floor.sh requires 1 positional argument'
        echo 'clawt-test-floor.sh <count>'
        exit 1
    fi

    local_count="${1}"

    case "${local_count}" in
        ''|*[!0-9]*)
            echo "test-floor: could not determine how many tests ran (got '${local_count}')"
            echo "test-floor: refusing to call this run green"
            exit 1
            ;;
    esac

    if [ "${local_count}" -lt "${TEST_FLOOR}" ]
    then
        echo "test-floor: only ${local_count} test binaries ran, expected at least ${TEST_FLOOR}"
        echo "test-floor: a green run this small usually means tests failed to build"
        exit 1
    fi

    exit 0
}

main "$@"
