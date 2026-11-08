#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# clawt-gsurf-check.sh - refuse a gsurf too old for the browser tools, and
# build GSURF_DIR's binary when it is missing.
#
# Usage: tools/clawt-gsurf-check.sh GSURF_DIR MIN_VERSION MAKE

set -euo pipefail

gsurf_check () {
	if [[ $# -ne 3 ]]
	then
		echo 'usage: clawt-gsurf-check.sh GSURF_DIR MIN_VERSION MAKE' >&2
		exit 2
	fi

	local dir="${1}"
	local min="${2}"
	local make_cmd="${3}"
	local major minor micro have

	if [[ ! -f "${dir}/config.mk" ]]
	then
		echo "GSURF=1: no gsurf checkout at ${dir} (set GSURF_DIR=)" >&2
		exit 1
	fi

	# config.mk spells them VERSION_MAJOR := 0
	major=$(sed -n 's/^VERSION_MAJOR *:*= *//p' "${dir}/config.mk")
	minor=$(sed -n 's/^VERSION_MINOR *:*= *//p' "${dir}/config.mk")
	micro=$(sed -n 's/^VERSION_MICRO *:*= *//p' "${dir}/config.mk")
	have="${major}.${minor}.${micro}"

	if [[ "$(printf '%s\n%s\n' "${min}" "${have}" | sort -V | head -n 1)" != "${min}" ]]
	then
		echo "GSURF=1: ${dir} is gsurf ${have}; the browser tools need ${min} or newer" >&2
		exit 1
	fi

	if [[ ! -x "${dir}/build/release/gsurf" ]]
	then
		echo "GSURF=1: building gsurf ${have} in ${dir}"
		CDPATH='' "${make_cmd}" -C "${dir}" --no-print-directory
	fi
}

gsurf_check "$@"
