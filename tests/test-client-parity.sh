#!/usr/bin/env bash
#
# test-client-parity.sh - The parity check reads code, not prose.
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# clawt-client-parity.sh states the rule twice -- comments are text too,
# so a grep-based check strips them "in every layer" -- and applied it in
# three of the five.  Layers 1 and 2 read `clients/*/*.c` raw, so a frame
# kind or a slash command named only in a *comment* counted as a
# capability the client has.  That is the exact shape the script's own
# history records at layer 3: a comment explaining that a loop had been
# deleted still contained the deleted function's name, and the check
# reported the enumeration as walked.
#
# Nothing was masked when this was fixed -- every kind and every command
# appearing in a client comment also appeared in that client's code, in
# both clients -- which is how long a latent check waits before it is
# wrong about something.
#
# So this drives the real script against a tree of two synthetic clients,
# where the only difference between the two arms is whether the web
# client's mention is inside `/* */`.  src/ is symlinked from the repo so
# the daemon's frame kinds and the public headers are the real ones; the
# other layers report against the synthetic clients and are ignored here,
# because the assertion is on the one line each arm is about.

set -u

# CDPATH= or the resolved directory is echoed into the substitution.
CDPATH='' cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"

FAIL=0
WORK=""

cleanup () {
    [ -n "${WORK}" ] && rm -rf "${WORK}"
}

trap cleanup EXIT

#
# A tree the script will take as a checkout: its own ROOT comes from
# dirname("${BASH_SOURCE[0]}")/.., so a copy under <work>/tools reads
# <work>/clients and <work>/src.
#
build_tree () {
    WORK="$(mktemp -d "${TMPDIR:-/tmp}/clawt-parity-XXXXXX")"

    mkdir -p "${WORK}/tools" "${WORK}/clients/gtk" "${WORK}/clients/web"
    ln -s "${ROOT}/src" "${WORK}/src"
    cp "${ROOT}/tools/clawt-client-parity.sh" "${WORK}/tools/"
}

#
# @1: the line the web client carries the two probes on.
#
# Both clients name "agent.list" and "/other" in code in both arms, so
# the only difference the script can see is the probe pair -- otherwise
# an unrelated one-sided kind would appear in the same report and the
# assertions would be matching against somebody else's line.
#
write_clients () {
    cat > "${WORK}/clients/gtk/fake.c" <<'GTK'
void fake(void)
{
    send_frame("agent.list");
    answer("/other");
    send_frame("agent.start");
    answer("/parityprobe");
}
GTK

    {
        echo 'void fake(void)'
        echo '{'
        echo '    send_frame("agent.list");'
        echo '    answer("/other");'
        printf '    %s\n' "${1}"
        echo '}'
    } > "${WORK}/clients/web/fake.c"
}

#
# Run it and hand back everything it said.  It exits non-zero in both
# arms -- the synthetic clients declare none of the affordances -- so the
# status is not the answer and the output is.
#
run_parity () {
    bash "${WORK}/tools/clawt-client-parity.sh" 2>&1 || true
}

#
# @1: description, @2: "yes" or "no" -- whether the line must be there,
# @3: the extended regex for the line, @4: the whole output.
#
expect_line () {
    local wanted="${2}"
    local found="no"

    printf '%s\n' "${4}" | grep -Eq "${3}" && found="yes"

    if [ "${found}" = "${wanted}" ]
    then
        echo "  ok    ${1}"
        return
    fi

    echo "  FAIL  ${1} (expected ${wanted}, got ${found})"
    printf '%s\n' "${4}" | sed 's/^/        /'
    FAIL=1
}

main () {
    local output

    build_tree

    #
    # Arm one: the web client's only mention of either is inside a
    # comment.  Both must be reported missing.
    #
    write_clients \
        '/* Was: send_frame("agent.start"); answer("/parityprobe"); */'
    output="$(run_parity)"

    expect_line "a frame kind named only in a comment does not count" \
        yes '^ +agent\.start +in clawtilla-gtk, missing from clawtilla-web' \
        "${output}"
    expect_line "a slash command named only in a comment does not count" \
        yes '^ +/parityprobe +a command in clawtilla-gtk, missing from clawtilla-web' \
        "${output}"

    #
    # Arm two: the same two names, in code.  Neither may be reported --
    # otherwise arm one proves nothing, since a check that reports
    # everything always reports the thing you asked about.
    #
    write_clients 'send_frame("agent.start"); answer("/parityprobe");'
    output="$(run_parity)"

    expect_line "a frame kind in code does count" \
        no 'agent\.start' "${output}"
    expect_line "a slash command in code does count" \
        no '/parityprobe' "${output}"

    #
    # Arm three: it does not litter.
    #
    # The public API corpus used to be created inside a function every
    # caller invoked as `api="$(public_api)"`.  A command substitution is
    # a subshell, so the assignment never reached the parent and the EXIT
    # trap -- which lists that variable -- removed nothing.  Two 330KB
    # copies of every public header were left in /tmp per run, and 202 of
    # them had piled up before anybody counted.
    #
    # clawt-test-litter.sh cannot see this: it looks for *directories*
    # matching the g_dir_make_tmp() prefixes the C tests use, and these
    # are files from mktemp.  So the check goes here, beside the thing
    # that leaked.
    #
    # Counted in a directory of this test's own rather than in /tmp, so
    # that a parallel build cannot make it a false failure.
    #
    litter_before="$(find "${TMPDIR:-/tmp}" -maxdepth 1 -name 'tmp.*' \
                         -newer "${WORK}" 2>/dev/null | wc -l)"
    run_parity > /dev/null
    litter_after="$(find "${TMPDIR:-/tmp}" -maxdepth 1 -name 'tmp.*' \
                        -newer "${WORK}" 2>/dev/null | wc -l)"

    if [ "${litter_after}" -le "${litter_before}" ]
    then
        echo "  ok    the parity script cleans up after itself"
    else
        echo "  FAIL  the parity script left $(( litter_after - litter_before )) file(s) in ${TMPDIR:-/tmp}"
        FAIL=1
    fi

    if [ "${FAIL}" -ne 0 ]
    then
        echo "test-client-parity: FAILED"
        exit 1
    fi

    echo "test-client-parity: ok"
}

main "$@"
