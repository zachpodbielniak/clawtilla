#!/usr/bin/env bash
# test-pod-module-symbols.sh - Every module-loading binary hosts every module
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# podomation's loadable modules (libpod-module-*.so) are dlopen'd by the
# binary that wants them, and resolve their pod_* calls against that
# executable, which exports its symbols dynamically.  So a binary can load
# a module only if it *defines* every pod_* symbol the module leaves
# undefined.
#
# That used to depend on what else was in the binary: podomation came out
# of a plain static archive, which contributes only the objects a binary's
# own code references.  podomation 1.0.5's container module started
# calling pod_params_get_boolean; the daemon happened to define it and
# test-container-keep did not, and died after its second case with
#   symbol lookup error: ... libpod-module-container.so:
#   undefined symbol: pod_params_get_boolean
# Modules are opened G_MODULE_BIND_LAZY, so nothing failed at load time --
# only when the call was reached.  config.mk now links podomation with
# --whole-archive; this is what says it stays that way.
#
# Checked: every executable in the build directory and its tests/ that
# contains a module loader -- ours (clawt_pod_bridge_load_module_for) or
# podomation's own (pod_module_manager_load_module, which PodEngine uses).
# A binary with neither cannot load a module, so it does not have to host
# one.
#
# Environment:
#   CLAWT_BUILD_DIR       the build tree to check (default build/release)
#   CLAWT_POD_MODULE_DIR  the modules (default: CLAWT_BUILD_DIR/pod-modules,
#                         then deps/libreclaw/build/release/modules)
#
# Exits 0 when every loader binary defines everything, 1 when one does
# not, 77 (skipped) when there are no modules to check against or no nm.

# `make test` runs tests/test-*.sh with sh, which is not bash everywhere.
if [ -z "${BASH_VERSION:-}" ]; then
	exec bash "$0" "$@"
fi
set -euo pipefail

PROGRAM_NAME="test-pod-module-symbols"

# The scratch directory, for the EXIT trap (set in main).
WORK_DIR=""

# The symbols a module expects its host to provide.  Only podomation's:
# a module's other undefined references (GLib, libsoup, ...) are satisfied
# by the shared libraries it is itself linked against.
POD_SYMBOL_REGEX='^pod_'

# Symbols that mark a binary as able to load a module at all.
LOADER_SYMBOLS=(clawt_pod_bridge_load_module_for pod_module_manager_load_module)

usage () {
	cat <<-USAGE
	Usage: ${PROGRAM_NAME}.sh [-h|--help] [--license]

	Checks that every clawtilla executable that can load a podomation
	module defines every pod_* symbol that the built modules need from it.

	Environment:
	  CLAWT_BUILD_DIR       build tree to check (default: build/release)
	  CLAWT_POD_MODULE_DIR  directory of libpod-module-*.so

	Examples:
	  bash tests/${PROGRAM_NAME}.sh
	  CLAWT_BUILD_DIR=build/debug bash tests/${PROGRAM_NAME}.sh
	  CLAWT_POD_MODULE_DIR=/usr/lib64/podomation/modules bash tests/${PROGRAM_NAME}.sh
	USAGE
}

license () {
	cat <<-LICENSE
	${PROGRAM_NAME} is part of clawtilla.
	SPDX-License-Identifier: AGPL-3.0-or-later
	GNU Affero General Public License, version 3 or later.
	LICENSE
}

skip () {
	echo "${PROGRAM_NAME}: skipped (${1})"
	exit 77
}

# Prints the dynamically exported, defined symbol names of an ELF file,
# one per line, version suffixes removed, sorted.  Prints nothing for a
# file nm cannot read (a script, say).
defined_symbols () {
	local file="${1}"

	nm -D --defined-only -- "${file}" 2>/dev/null \
		| awk 'NF >= 3 { sub(/@.*/, "", $3); print $3 }' \
		| sort -u \
		|| true
}

# Prints the pod_* symbols a module needs from its host, sorted.  Weak
# references ("w") are left out: a missing weak symbol is NULL, not an
# error.
needed_symbols () {
	local module="${1}"

	nm -D --undefined-only -- "${module}" \
		| awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' \
		| grep -E "${POD_SYMBOL_REGEX}" \
		| sort -u \
		|| true
}

main () {
	local build_dir
	local module_dir
	local work_dir
	local module
	local binary
	local sym
	local missing
	local binaries_checked=0
	local modules_checked=0
	local failures=0
	local -a modules=()
	local -a candidates=()

	case "${1:-}" in
		-h|--help)
			usage
			exit 0
			;;
		--license)
			license
			exit 0
			;;
		'')
			;;
		*)
			echo "${PROGRAM_NAME}: unknown option: ${1}" >&2
			usage >&2
			exit 2
			;;
	esac

	command -v nm > /dev/null 2>&1 || skip "no nm; install binutils"

	build_dir="${CLAWT_BUILD_DIR:-build/release}"
	if [[ -n "${CLAWT_POD_MODULE_DIR:-}" ]]
	then
		module_dir="${CLAWT_POD_MODULE_DIR}"
	elif [[ -d "${build_dir}/pod-modules" ]]
	then
		module_dir="${build_dir}/pod-modules"
	else
		module_dir="deps/libreclaw/build/release/modules"
	fi

	[[ -d "${build_dir}" ]] || skip "no build directory ${build_dir}"
	[[ -d "${module_dir}" ]] || skip "no podomation modules in ${module_dir}"

	shopt -s nullglob
	modules=("${module_dir}"/libpod-module-*.so)
	candidates=("${build_dir}"/* "${build_dir}"/tests/*)
	shopt -u nullglob
	[[ ${#modules[@]} -gt 0 ]] || skip "no libpod-module-*.so in ${module_dir}"

	work_dir="$(mktemp -d "${TMPDIR:-/tmp}/clawt-pod-symbols-XXXXXX")"
	# The directory is ours alone; remove only it, on every exit.  The
	# trap runs after main has returned, so it reads the global copy.
	WORK_DIR="${work_dir}"
	trap 'rm -rf -- "${WORK_DIR}"' EXIT

	# What each module needs, once: "<module> <symbol>" per line, and the
	# union of every symbol, which is all a binary is compared against.
	for module in "${modules[@]}"
	do
		needed_symbols "${module}" \
			| sed "s|^|$(basename -- "${module}") |" \
			>> "${work_dir}/needs"
		modules_checked=$((modules_checked + 1))
	done
	touch "${work_dir}/needs"
	awk '{ print $2 }' "${work_dir}/needs" | sort -u > "${work_dir}/needed"

	for binary in "${candidates[@]}"
	do
		[[ -f "${binary}" && -x "${binary}" ]] || continue

		defined_symbols "${binary}" > "${work_dir}/defined"
		[[ -s "${work_dir}/defined" ]] || continue

		# Not a loader: it cannot open a module, so it need not host one.
		for sym in "${LOADER_SYMBOLS[@]}"
		do
			if grep -qx -- "${sym}" "${work_dir}/defined"
			then
				break
			fi
			sym=""
		done
		[[ -n "${sym}" ]] || continue

		binaries_checked=$((binaries_checked + 1))
		missing="$(comm -23 "${work_dir}/needed" "${work_dir}/defined")"
		[[ -n "${missing}" ]] || continue

		failures=$((failures + 1))
		echo "FAIL: ${binary} cannot host every podomation module;" \
			"it does not define:"
		# Name the module along with the symbol, so the failure says which
		# module would die, not only that one would.
		printf '%s\n' "${missing}" \
			| awk 'NR == FNR { want[$1] = 1; next }
			       ($2 in want) { print "    " $2 "  (needed by " $1 ")" }' \
			- "${work_dir}/needs"
	done

	if [[ ${binaries_checked} -eq 0 ]]
	then
		# Every build has the daemon, and it loads modules; finding no
		# loader at all means the detection broke, not that all is well.
		echo "FAIL: no module-loading binary found in ${build_dir};" \
			"looked for: ${LOADER_SYMBOLS[*]}"
		exit 1
	fi

	if [[ ${failures} -ne 0 ]]
	then
		echo "${PROGRAM_NAME}: ${failures} of ${binaries_checked} module-loading" \
			"binaries miss pod_* symbols that ${module_dir} needs"
		echo "  podomation must be linked --whole-archive (config.mk, LIBRECLAW_LIBS)"
		exit 1
	fi

	echo "${PROGRAM_NAME}: ${binaries_checked} module-loading binaries define" \
		"all $(wc -l < "${work_dir}/needed") pod_* symbols" \
		"${modules_checked} modules need"
}

main "$@"
