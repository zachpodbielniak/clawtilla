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
# Two things are compared.
#
# The set of IPC frame kinds each client sends is a proxy for "feature",
# not a definition of one: a client could name a kind and do nothing
# useful with it.  But every real feature has to talk to the daemon to do
# anything, so a kind one client sends and the other never mentions is a
# capability that exists in one place only.
#
# The set of slash commands is compared because the frame-kind check
# missed them entirely -- thirteen of the eighteen were absent from the
# web client and nothing said so, since /files and /agents and /export are
# all built out of frames both clients already sent.  A check that only
# looks at one layer will keep finding nothing at the others.
#
# Two more layers, added after that sentence came true a second time.
# Catppuccin Mocha went into libclawt, which both clients link, and was
# selectable in one of them; the unread marker was built for the GTK
# transcript and not the web one.  Neither sends a frame or answers a
# slash command, so the two checks above reported OK throughout.
#
#   3. **Choice enumerations.**  A `_count()`/`_nth()` family in the
#      public API exists so a client can offer a set of values without
#      naming them.  If either client walks one, both must -- that is
#      exactly the Mocha shape, and it needs nobody to declare anything.
#
#   4. **Hardcoded vocabulary.**  The cause, rather than the symptom: a
#      client naming a value the library enumerates has a copy of the
#      list, and a copy is what drifts.  The values are read out of the
#      library's own table, so a palette added there is checked from the
#      moment it exists.
#
#   5. **Declared affordances.**  Some capabilities are pure interface --
#      the unread marker is a rule and a pill -- and touch no shared
#      symbol at all.  Those are declared here with a marker per client.
#      This layer is honest about its limit: it catches one half being
#      removed or never written, and it cannot catch a feature nobody
#      declared.  Layers 3 and 4 need no declaration; this one does.

set -euo pipefail

CDPATH=

gtk=""
web=""
only_gtk=""
only_web=""
gtk_cmds=""
web_cmds=""
only_gtk_cmds=""
gtk_code=""
web_code=""

cleanup () {
    rm -f "${gtk}" "${web}" "${only_gtk}" "${only_web}" \
          "${gtk_cmds}" "${web_cmds}" "${only_gtk_cmds}" \
          "${gtk_code}" "${web_code}"
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
    ["agent.file_read"]="same capability, different mechanism: /edit opens the file in \$EDITOR there, which is the better answer when the client and the daemon share a machine"
    ["agent.file_write"]="the other half of the same; \$EDITOR writes the file directly"
)

#
# Where the library keeps a set of values a person chooses between, and
# how to read the values out of it.
#
# Each entry is a file and a sed script that prints one value per line.
# A client is not allowed to contain any of them as a string literal:
# having the list is what makes a client able to disagree with it.
#
# shellcheck disable=SC2034
declare -A VOCABULARIES=(
    ["colour scheme"]="src/config/clawt-appearance.c|s/.*{ *CLAWT_[A-Z_]*, *\"\([a-z-]*\)\".*/\1/p"
)

#
# Capabilities that are interface and nothing else, with the marker that
# proves each client has one.
#
# A marker is a symbol or a class the feature cannot work without, not a
# label somebody might reword.  Adding a row here is the declaration; the
# check is that both halves answer to it.
#
# shellcheck disable=SC2034
declare -A AFFORDANCES=(
    ["unread marker"]="unread_marker|unread-rule"
    ["jump to latest"]="jump_revealer|jump-pill"
    ["transcript measure"]="adw_clamp_new|transcript-inner"
    ["markdown rendering"]="clawt_markdown_to_pango|msg-body"
    ["attachment previews"]="append_attachment_previews|attachment"
)

usage () {
    cat <<'USAGE'
clawt-client-parity.sh - check the two graphical clients cover the same daemon

Usage:
  clawt-client-parity.sh [--list] [--help]

  --list    print each client's coverage instead of only the differences
  --help    this text

Exits non-zero when the two clients differ at any of five layers:

  1. an IPC frame kind one sends and the other never mentions
  2. a slash command the GTK composer answers and the web one does not
  3. a library choice enumeration one walks and the other does not
  4. a value from a library vocabulary that either client spells out
  5. a declared interface affordance missing from one of them

Layers 1-4 need nothing declared.  Layer 5 does, and cannot catch a
feature nobody declared -- it is there for the capabilities that are pure
interface and touch no shared symbol at all.

An exception map in this script records the deliberate differences.  An
entry there is a decision, not a to-do.

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

#
# The slash commands a client answers.
#
# Both keep them in a table of "/name" literals, which is enough to
# compare without either having to declare anything for this script's
# benefit.
#
client_commands () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_commands()` requires 1 positional argument' >&2
        exit 1
    fi

    grep -ohE '"/[a-z][a-z-]*"' "${1}"/*.c \
        | tr -d '"' \
        | sort -u
}

#
# The choice enumerations the public API offers.
#
# A `_count()` with a matching `_nth()` is the library saying "here is a
# set, walk it".  Read from the headers rather than listed here, so one
# added later is compared from the moment it exists.
#
enumerations () {
    local family

    for family in $(grep -ohE '\bclawt_[a-z0-9_]+_count[[:space:]]*\(' \
                        "${ROOT}"/src/*.h "${ROOT}"/src/*/*.h \
                    | tr -d ' (' | sed 's/_count$//' | sort -u)
    do
        #
        # Only the ones that are a *set of choices*.  clawt_memory_store_count()
        # and clawt_room_get_message_count() are counts of things that
        # happened, and have no _nth() beside them precisely because
        # there is nothing to offer.
        #
        if grep -qhE "\b${family}_nth[[:space:]]*\(" \
               "${ROOT}"/src/*.h "${ROOT}"/src/*/*.h
        then
            echo "${family}"
        fi
    done
}

#
# Whether a client walks a given enumeration.
#
walks_enumeration () {
    if [[ $# -ne 2 ]]
    then
        # shellcheck disable=SC2016
        echo '`walks_enumeration()` requires 2 positional arguments' >&2
        echo 'walks_enumeration <directory> <family>' >&2
        exit 1
    fi

    grep -qhE "\b${2}_(count|nth)[[:space:]]*\(" "${1}"/*.c
}

#
# A client's sources with the comments taken out.
#
# Layers 4 and 5 look for a literal, and this file is full of prose that
# quotes the very things they look for -- the comment explaining why the
# stylesheet spells `data-theme="light"` contains "light", and the one
# explaining the fallback contains "system".  Grepping the prose reported
# two hardcodings that were not there and would have taught somebody to
# ignore the check.
#
# Written to a file and grepped, never piped into `grep -q`.  Under
# `set -o pipefail` that pipeline reports *failure on success*: grep
# exits the moment it matches, awk dies of SIGPIPE with 141, and the
# pipeline takes the worst status -- so every marker that was present
# reported missing.  It is also read once per client rather than once
# per value.
#
# Only `/*  */`, because that is the only comment form the codebase uses.
# A `/*` inside a string literal would end the visible text early; there
# are none, and the failure mode is a missed hit rather than a false one.
#
client_code () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_code()` requires 1 positional argument' >&2
        exit 1
    fi

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
    }' "${1}"/*.c
}

#
# Every value a library vocabulary holds.
#
vocabulary_values () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`vocabulary_values()` requires 1 positional argument' >&2
        exit 1
    fi

    local spec="${1}"
    local file="${spec%%|*}"
    local script="${spec#*|}"

    sed -n "${script}" "${ROOT}/${file}" | sort -u
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
    gtk_cmds="$(mktemp)"
    web_cmds="$(mktemp)"
    only_gtk_cmds="$(mktemp)"
    gtk_code="$(mktemp)"
    web_code="$(mktemp)"

    client_code "${GTK_DIR}" > "${gtk_code}"
    client_code "${WEB_DIR}" > "${web_code}"

    client_kinds "${GTK_DIR}" > "${gtk}"
    client_kinds "${WEB_DIR}" > "${web}"

    comm -23 "${gtk}" "${web}" > "${only_gtk}"
    comm -13 "${gtk}" "${web}" > "${only_web}"

    client_commands "${GTK_DIR}" > "${gtk_cmds}"
    client_commands "${WEB_DIR}" > "${web_cmds}"

    comm -23 "${gtk_cmds}" "${web_cmds}" > "${only_gtk_cmds}"

    if [[ "${list}" -eq 1 ]]
    then
        echo "clawtilla-gtk reaches $(wc -l < "${gtk}") frame kinds:"
        sed 's/^/  /' "${gtk}"
        echo
        echo "clawtilla-web reaches $(wc -l < "${web}") frame kinds:"
        sed 's/^/  /' "${web}"
        echo
        echo "slash commands: $(wc -l < "${gtk_cmds}") in gtk, \
$(wc -l < "${web_cmds}") in web"
        echo

        echo "choice enumerations, and who walks them:"
        local shown
        for shown in $(enumerations)
        do
            local who=""
            walks_enumeration "${GTK_DIR}" "${shown}" && who="gtk"
            walks_enumeration "${WEB_DIR}" "${shown}" && who="${who:+${who}+}web"
            printf '  %-38s %s\n' "${shown}()" "${who:-nobody}"
        done
        echo

        echo "library vocabularies neither client may copy:"
        local vocab
        for vocab in "${!VOCABULARIES[@]}"
        do
            printf '  %-24s %s\n' "${vocab}" \
                "$(vocabulary_values "${VOCABULARIES[${vocab}]}" | tr '\n' ' ')"
        done
        echo

        echo "declared affordances (gtk marker | web marker):"
        local aff
        for aff in "${!AFFORDANCES[@]}"
        do
            printf '  %-24s %s\n' "${aff}" "${AFFORDANCES[${aff}]}"
        done
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

    #
    # Commands are only checked one way. The GTK composer is the one with
    # the established set; the web client answering something extra is a
    # thing to notice by reading, not a build failure.
    #
    while read -r command
    do
        [[ -z "${command}" ]] && continue

        printf '  %-28s a command in clawtilla-gtk, missing from clawtilla-web\n' \
            "${command}"
        failures=$((failures + 1))
    done < "${only_gtk_cmds}"

    #
    # Layer 3: a set of choices the library offers must be offered by
    # both clients, or by neither.
    #
    local family
    local in_gtk
    local in_web

    for family in $(enumerations)
    do
        in_gtk=0
        in_web=0

        walks_enumeration "${GTK_DIR}" "${family}" && in_gtk=1
        walks_enumeration "${WEB_DIR}" "${family}" && in_web=1

        [[ "${in_gtk}" -eq "${in_web}" ]] && continue

        if [[ "${in_gtk}" -eq 1 ]]
        then
            printf '  %-28s walked by clawtilla-gtk, not by clawtilla-web\n' \
                "${family}()"
        else
            printf '  %-28s walked by clawtilla-web, not by clawtilla-gtk\n' \
                "${family}()"
        fi

        failures=$((failures + 1))
    done

    #
    # Layer 4: and neither may keep its own copy of the values.
    #
    local vocabulary
    local value
    local client

    for vocabulary in "${!VOCABULARIES[@]}"
    do
        for value in $(vocabulary_values "${VOCABULARIES[${vocabulary}]}")
        do
            for client in "gtk:${gtk_code}" "web:${web_code}"
            do
                grep -qF "\"${value}\"" "${client#*:}" || continue

                printf '  %-28s clawtilla-%s names it; walk the %s list\n' \
                    "${value}" "${client%%:*}" "${vocabulary}"
                failures=$((failures + 1))
            done
        done
    done

    #
    # Layer 5: the declared interface affordances, both halves present.
    #
    local affordance
    local markers

    for affordance in "${!AFFORDANCES[@]}"
    do
        markers="${AFFORDANCES[${affordance}]}"

        grep -qF "${markers%%|*}" "${gtk_code}" || {
            printf '  %-28s declared, missing from clawtilla-gtk\n' \
                "${affordance}"
            failures=$((failures + 1))
        }

        grep -qF "${markers#*|}" "${web_code}" || {
            printf '  %-28s declared, missing from clawtilla-web\n' \
                "${affordance}"
            failures=$((failures + 1))
        }
    done

    if [[ "${failures}" -gt 0 ]]
    then
        echo
        echo "${failures} capability(ies) exist in one client and not the other."
        echo "Build it in both, or record the reason in the exception map in"
        echo "  tools/clawt-client-parity.sh"
        exit 1
    fi

    echo "client parity: OK ($(wc -l < "${gtk}") kinds and \
$(wc -l < "${gtk_cmds}") commands in gtk, $(wc -l < "${web}") kinds and \
$(wc -l < "${web_cmds}") commands in web; \
$(enumerations | wc -l) enumeration(s), \
${#VOCABULARIES[@]} vocabulary(ies), \
${#AFFORDANCES[@]} affordance(s))"
}

main "$@"
