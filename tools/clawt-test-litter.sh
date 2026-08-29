#!/bin/sh
# clawt-test-litter.sh - Fail when a green test run left temporary dirs.
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# g_rmdir() does nothing to a directory that is not empty, so a fixture
# that unlinks the file it wrote and stops there leaves the directory
# above it -- silently, once per test per run, for ever.  Three fixtures
# did: 2710 directories had accumulated in /tmp over six days before
# anybody counted them, and nothing in a green run said so.
#
# This adds the missing half of "all tests passed": and the machine is as
# it was.  It is deliberately a before/after comparison of the same run
# rather than a check that the temporary directory is empty, because a
# previous *failing* run leaves its directories behind on purpose -- they
# are the evidence -- and those must not be reported as this run's fault.
#
# Usage: clawt-test-litter.sh snapshot <file>
#        clawt-test-litter.sh check <file>
#
# What it cannot see: a fixture whose template is not a string literal in
# tests/test-*.c, because the prefixes come from those literals.  A
# hand-written list of prefixes would have drifted instead, which is
# worse; this at least grows with the suite.

set -eu

# Where GLib will put them.  g_get_tmp_dir() reads TMPDIR, then TMP, then
# TEMP, then falls back to /tmp -- looking only at /tmp would make this
# check report OK while the suite littered somewhere else, which is the
# failure it exists to catch wearing a different hat.
tmp_dir () {
    if [ -n "${TMPDIR:-}" ]
    then
        echo "${TMPDIR}"
    elif [ -n "${TMP:-}" ]
    then
        echo "${TMP}"
    elif [ -n "${TEMP:-}" ]
    then
        echo "${TEMP}"
    else
        echo '/tmp'
    fi
}

# Every prefix any fixture asks g_dir_make_tmp() for, read from the
# fixtures themselves.  The suite grows; a list written here would not.
list_dirs () {
    local_root="$(tmp_dir)"
    local_prefixes="$(sed -n \
        's/.*g_dir_make_tmp *( *"\([^"]*\)XXXXXX".*/\1/p' \
        tests/test-*.c | sort -u)"

    if [ -z "${local_prefixes}" ]
    then
        return 0
    fi

    # sort -u at the call sites, not here: the prefixes overlap
    # (clawt-daemon- and clawt-daemon-data-, clawt-mbox- and
    # clawt-mbox-bad-), so a directory under the longer one is found
    # twice and comm would report a genuinely new one twice over.
    for local_prefix in ${local_prefixes}
    do
        # A prefix that matches nothing leaves the glob unexpanded, which
        # is why each candidate is tested for being a directory rather
        # than printed straight out.
        for local_path in "${local_root}/${local_prefix}"*
        do
            if [ -d "${local_path}" ]
            then
                echo "${local_path}"
            fi
        done
    done

    return 0
}


# Whether some other test run is going on right now.
#
# The glob cannot tell whose directories it is looking at, so a second
# run's fixtures are indistinguishable from this run's litter -- and
# with several branches building at once that is the common case, not
# the rare one. Asked of the kernel rather than of `pgrep`, which
# matches the probe's own command line.
other_test_processes () {
    local_count=0

    for local_proc in /proc/[0-9]*
    do
        local_exe="$(readlink "${local_proc}/exe" 2>/dev/null)"

        case "${local_exe}" in
            */tests/test-*) local_count=$((local_count + 1)) ;;
        esac
    done

    echo "${local_count}"
}

main () {
    if [ $# -ne 2 ]
    then
        echo 'clawt-test-litter.sh requires 2 positional arguments'
        echo 'clawt-test-litter.sh <snapshot|check> <file>'
        exit 1
    fi

    local_mode="${1}"
    local_file="${2}"

    case "${local_mode}" in
        snapshot)
            list_dirs | sort -u > "${local_file}"
            exit 0
            ;;
        check)
            ;;
        *)
            echo "test-litter: unknown mode '${local_mode}'"
            exit 1
            ;;
    esac

    if [ ! -f "${local_file}" ]
    then
        echo "test-litter: no snapshot at ${local_file}"
        echo 'test-litter: refusing to call this run clean'
        exit 1
    fi

    # This run's own binaries have all exited by the time the check
    # runs, so anything still alive belongs to somebody else and the
    # comparison below cannot mean anything. Saying which is better than
    # failing a clean run for another branch's temporary files.
    if [ "$(other_test_processes)" -gt 0 ]
    then
        echo 'test-litter: another test run is in progress -- skipped'
        echo 'test-litter: this check cannot tell two runs apart'
        rm -f "${local_file}"
        exit 0
    fi

    local_after="${local_file}.after"
    local_new="${local_file}.new"

    list_dirs | sort -u > "${local_after}"

    # comm rather than a pipe into grep: under `set -o pipefail` a
    # `| grep -q` that matches early exits 141 on SIGPIPE, which reads as
    # the check itself having failed.
    comm -13 "${local_file}" "${local_after}" > "${local_new}"

    if [ -s "${local_new}" ]
    then
        echo 'test-litter: the suite passed but left temporary directories behind:'
        sed 's/^/  /' "${local_new}"
        echo 'test-litter: a fixture removed its file and not the directory holding it'
        echo 'test-litter: use clawt_test_remove_tree(), not g_rmdir() or g_unlink()'
        rm -f "${local_after}" "${local_new}"
        exit 1
    fi

    rm -f "${local_file}" "${local_after}" "${local_new}"
    exit 0
}

main "$@"
