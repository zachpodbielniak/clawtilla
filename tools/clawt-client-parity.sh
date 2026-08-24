#!/usr/bin/env bash
#
# clawt-client-parity.sh - The GTK client and the web client answer for
# the same daemon, so they should reach the same parts of it.
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# Two clients drifting apart is invisible: nothing breaks, nothing warns,
# and somebody finds out by reaching for the half that was not built.  So
# the rule -- a feature in one exists in the other -- is checked rather
# than remembered, in the same spirit as the stale-config-key and
# undefined-tool checks `make docs-check` already runs.
#
# What is compared is the set of IPC frame kinds each client sends.  It is
# a proxy for "feature", not a definition of one: a client could name a
# kind and do nothing useful with it.  But every real feature has to talk
# to the daemon to do anything, so a kind one client sends and the other
# never mentions is a capability that exists in one place only -- and that
# is the failure worth catching.

set -euo pipefail

CDPATH=

gtk=""
web=""
only_gtk=""
only_web=""

cleanup () {
    rm -f "${gtk}" "${web}" "${only_gtk}" "${only_web}"
}

trap cleanup EXIT

# CDPATH is cleared above, so a bare cd cannot echo the directory it
# resolved to and have the command substitution capture it as well as pwd's
# own output.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GTK_DIR="${ROOT}/clients/gtk"
WEB_DIR="${ROOT}/clients/web"
DAEMON="${ROOT}/src/core/clawt-daemon.c"

#
# Kinds a client is allowed to lack, each with the reason.
#
# An entry here is a decision, not a to-do.  Anything without a reason
# somebody would agree with belongs in the client instead.
#
# Read through a nameref in report_missing(), which shellcheck cannot see.
# shellcheck disable=SC2034
declare -A WEB_MAY_LACK=(
    ["config.render"]="reading a rendered libreclaw config is a debugging step; \`clawtilla config render\` is the surface for it"
)

# shellcheck disable=SC2034
declare -A GTK_MAY_LACK=(
    ["computer.status"]="the GTK computer panel shows state from agent.show; worth closing, not a blocker"
    ["exchange.list"]="the GTK client offers drag-and-drop into the exchange rather than a listing"
)

usage () {
    cat <<'USAGE'
clawt-client-parity.sh - check the two graphical clients cover the same daemon

Usage:
  clawt-client-parity.sh [--list] [--help]

  --list    print each client's coverage instead of only the differences
  --help    this text

Exits non-zero when either client sends an IPC frame kind the other never
mentions, unless that kind is listed as a deliberate exception in this
script.

Examples:
  # As part of the build
  make parity

  # See what each client actually reaches
  tools/clawt-client-parity.sh --list
USAGE
}

#
# Every frame kind the daemon answers.
#
# Taken from the daemon rather than from a list here, so a kind added
# there is compared from the moment it exists.
#
daemon_kinds () {
    grep -oh 'kind, "[a-z_.]*"' "${DAEMON}" \
        | sed 's/kind, "//; s/"//' \
        | sort -u
}

#
# The frame kinds a directory of client sources mentions.
#
# Intersected with the daemon's own list, because a client is full of
# dotted strings that are not frame kinds -- css classes, file names,
# member names.
#
client_kinds () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_kinds()` requires 1 positional argument' >&2
        echo 'client_kinds <directory>' >&2
        exit 1
    fi

    local dir="${1}"

    comm -12 \
        <(grep -oh '"[a-z_]\+\.[a-z_.]\+"' "${dir}"/*.c \
            | tr -d '"' | sort -u) \
        <(daemon_kinds)
}

report_missing () {
    if [[ $# -ne 4 ]]
    then
        # shellcheck disable=SC2016
        echo '`report_missing()` requires 4 positional arguments' >&2
        echo 'report_missing <label> <other> <missing-file> <exception-map>' >&2
        exit 1
    fi

    local label="${1}"
    local other="${2}"
    local missing_file="${3}"
    local -n exceptions="${4}"
    local failures=0
    local kind

    while read -r kind
    do
        [[ -z "${kind}" ]] && continue

        if [[ -v exceptions["${kind}"] ]]
        then
            printf '  %-28s allowed: %s\n' \
                "${kind}" "${exceptions[${kind}]}"
            continue
        fi

        printf '  %-28s in %s, missing from %s\n' \
            "${kind}" "${other}" "${label}"
        failures=$((failures + 1))
    done < "${missing_file}"

    return "${failures}"
}

main () {
    local list=0

    while [[ $# -gt 0 ]]
    do
        case "${1}" in
            --list) list=1 ;;
            -h|--help) usage; exit 0 ;;
            *) echo "clawt-client-parity: unknown option ${1}" >&2
               usage >&2
               exit 2 ;;
        esac
        shift
    done

    #
    # Not locals: the EXIT trap fires after main() has returned, and a
    # local is gone by then -- which under `set -u` turns a clean run into
    # "unbound variable" printed after the result.
    #
    gtk="$(mktemp)"
    web="$(mktemp)"
    only_gtk="$(mktemp)"
    only_web="$(mktemp)"

    client_kinds "${GTK_DIR}" > "${gtk}"
    client_kinds "${WEB_DIR}" > "${web}"

    comm -23 "${gtk}" "${web}" > "${only_gtk}"
    comm -13 "${gtk}" "${web}" > "${only_web}"

    if [[ "${list}" -eq 1 ]]
    then
        echo "clawtilla-gtk reaches $(wc -l < "${gtk}") frame kinds:"
        sed 's/^/  /' "${gtk}"
        echo
        echo "clawtilla-web reaches $(wc -l < "${web}") frame kinds:"
        sed 's/^/  /' "${web}"
        echo
    fi

    local failures=0

    if [[ -s "${only_gtk}" ]] || [[ -s "${only_web}" ]]
    then
        echo "client parity:"
    fi

    report_missing "clawtilla-web" "clawtilla-gtk" "${only_gtk}" \
        WEB_MAY_LACK || failures=$((failures + $?))
    report_missing "clawtilla-gtk" "clawtilla-web" "${only_web}" \
        GTK_MAY_LACK || failures=$((failures + $?))

    if [[ "${failures}" -gt 0 ]]
    then
        echo
        echo "${failures} capability(ies) exist in one client and not the other."
        echo "Build it in both, or record the reason in the exception map in"
        echo "  tools/clawt-client-parity.sh"
        exit 1
    fi

    echo "client parity: OK ($(wc -l < "${gtk}") kinds in gtk, $(wc -l < "${web}") in web)"
}

main "$@"
