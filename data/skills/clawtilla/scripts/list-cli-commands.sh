#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# list-cli-commands.sh - print every top-level `clawtilla` command, sorted,
# one per line.
#
# Reads the verb table in clients/cli/main.c (`static const ClawtVerb
# verbs[] = { ... }`), which is the one list main() dispatches on, find_verb()
# parses around and --help prints. Nothing outside that table is a command,
# so a name that only appears in a usage string is not reported.

set -euo pipefail

usage () {
	cat <<'EOF'
Usage: list-cli-commands.sh [-h|--help] [--license] [REPO_ROOT]

Print every top-level clawtilla command from the verb table in
REPO_ROOT/clients/cli/main.c, sorted, one per line. REPO_ROOT defaults to
the clawtilla checkout that holds this script (four directories above it).

Examples:
  list-cli-commands.sh
  list-cli-commands.sh ~/source/projects/clawtilla
  list-cli-commands.sh | wc -l
EOF
}

main () {
	local script_dir
	local repo_root
	local source_file
	local commands

	case "${1:-}" in
	-h|--help)
		usage
		exit 0
		;;
	--license)
		echo 'AGPL-3.0-or-later'
		exit 0
		;;
	esac

	if [[ $# -gt 1 ]]
	then
		usage >&2
		exit 1
	fi

	script_dir="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
	repo_root="${1:-${script_dir}/../../../..}"
	source_file="${repo_root}/clients/cli/main.c"

	if [[ ! -r "${source_file}" ]]
	then
		echo "list-cli-commands.sh: cannot read ${source_file}" >&2
		exit 1
	fi

	# Only the lines between the table's opening and its NULL sentinel;
	# each entry opens with `{ "name",` at the start of its line.
	commands="$(sed -n '/^static const ClawtVerb verbs\[\] = {/,/^[[:space:]]*{ NULL/p' "${source_file}" \
		| grep -oE '^[[:space:]]*\{[[:space:]]*"[a-z][a-z0-9-]*"' \
		| grep -oE '"[a-z][a-z0-9-]*"' \
		| tr -d '"' \
		| sort -u)"

	if [[ -z "${commands}" ]]
	then
		echo "list-cli-commands.sh: no verb table found in ${source_file}" >&2
		exit 1
	fi

	printf '%s\n' "${commands}"
}

main "$@"
