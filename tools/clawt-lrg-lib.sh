#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# clawt-lrg-lib.sh - keep LRG_DIR's libregnum archive current, and check
# that its yaml-glib can stand in for clawtilla's.
#
# Called by `make lrg-lib' and `make lrg-yaml-check'; see the comments
# beside those targets in the Makefile and the "Building" section of
# docs/lrg-client.org.
#
#   LRG_LIB=… LRG_DIR=… LRG_SYMBOLS="…" MAKE_CMD=make clawt-lrg-lib.sh
#   clawt-lrg-lib.sh --yaml-check <clawtilla's yaml-glib> <libregnum's yaml-glib>

set -euo pipefail

usage () {
	echo 'clawt-lrg-lib.sh: rebuild a stale libregnum archive, or check yaml-glib ancestry'
	echo
	echo 'Usage:'
	echo '  LRG_LIB=<archive> LRG_DIR=<checkout> LRG_SYMBOLS="<sym> ..." MAKE_CMD=make clawt-lrg-lib.sh'
	echo '  clawt-lrg-lib.sh --yaml-check <clawtilla yaml-glib dir> <libregnum yaml-glib dir>'
	echo '  clawt-lrg-lib.sh -h | --help | --license'
	echo
	echo 'Examples:'
	echo '  make LRG_BACKEND=1 lrg-lib'
	echo '  tools/clawt-lrg-lib.sh --yaml-check deps/libreclaw/deps/yaml-glib ../libregnum/deps/yaml-glib'
}

# yaml_check: libregnum's yaml-glib is the only one linked into the lrg
# client, so it must contain every commit of clawtilla's pin.
yaml_check () {
	if [[ $# -ne 2 ]]
	then
		echo 'yaml_check() requires 2 positional arguments'
		echo 'yaml_check <clawtilla yaml-glib dir> <libregnum yaml-glib dir>'
		exit 1
	fi

	local ours_dir="${1}"
	local theirs_dir="${2}"
	local ours theirs

	ours=$(git -C "${ours_dir}" rev-parse HEAD 2>/dev/null || true)
	theirs=$(git -C "${theirs_dir}" rev-parse HEAD 2>/dev/null || true)

	if [[ -z "${ours}" || -z "${theirs}" ]]
	then
		echo "lrg-yaml-check: cannot read both yaml-glib revisions (${ours_dir}, ${theirs_dir})"
		exit 1
	fi

	if ! git -C "${theirs_dir}" merge-base --is-ancestor "${ours}" "${theirs}" 2>/dev/null
	then
		echo "lrg-yaml-check: libregnum's yaml-glib (${theirs:0:7}) does not descend from clawtilla's (${ours:0:7})."
		echo "  The lrg client links one yaml-glib -- libregnum's -- so it must contain clawtilla's."
		echo "  Update ${theirs_dir}, or bump libreclaw's yaml-glib to match."
		exit 1
	fi
}

# stale_reason: prints why the archive needs rebuilding, or nothing.
stale_reason () {
	local sym symbols

	if [[ ! -f "${LRG_LIB}" ]]
	then
		echo 'the archive is missing'
		return
	fi

	if [[ -n "$(find "${LRG_DIR}/src" "${LRG_DIR}/deps/graylib/src" \
		-name '*.[ch]' -newer "${LRG_LIB}" -print -quit)" ]]
	then
		echo 'its sources are newer than the archive'
		return
	fi

	# Written to a variable, not piped into grep -q: under pipefail an early
	# exit from grep is a SIGPIPE failure of the producer.
	symbols=$(nm -g --defined-only "${LRG_LIB}" 2>/dev/null | awk '{print $3}')

	for sym in ${LRG_SYMBOLS}
	do
		if ! grep -qx -- "${sym}" <<< "${symbols}"
		then
			echo "it lacks ${sym}"
			return
		fi
	done
}

# rebuild: libregnum's own targets with its own defaults and none of ours;
# a DEBUG/ASAN/BUILDDIR from our command line would otherwise reach it.
rebuild () {
	local reason="${1}"
	local jobs

	jobs=$(nproc)
	echo "Building libregnum in ${LRG_DIR}: ${reason}"
	env -u MAKEFLAGS -u MAKEOVERRIDES -u MFLAGS \
		"${MAKE_CMD:-make}" -C "${LRG_DIR}" -j"${jobs}" DEBUG=0 deps
	env -u MAKEFLAGS -u MAKEOVERRIDES -u MFLAGS \
		"${MAKE_CMD:-make}" -C "${LRG_DIR}" -j"${jobs}" DEBUG=0 lib
}

main () {
	local reason

	case "${1:-}" in
		-h|--help)
			usage
			exit 0
			;;
		--license)
			echo 'AGPL-3.0-or-later: https://www.gnu.org/licenses/agpl-3.0.html'
			exit 0
			;;
		--yaml-check)
			shift
			yaml_check "$@"
			exit 0
			;;
	esac

	: "${LRG_LIB:?LRG_LIB is not set}"
	: "${LRG_DIR:?LRG_DIR is not set}"
	: "${LRG_SYMBOLS:=}"

	reason=$(stale_reason)

	if [[ -n "${reason}" ]]
	then
		rebuild "${reason}"
	fi
}

main "$@"
