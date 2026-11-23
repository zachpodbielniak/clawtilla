#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# list-mcp-tools.sh - print every clawtilla_* orchestration tool registered
# in src/mcp/clawt-mcp-tools.c, sorted, one per line.
#
# Reads the registration table (TOOL(...) and HOP_TOOL(...) entries), not
# every string that happens to start with clawtilla_, so a name mentioned
# only in a description or a comment is not reported as a tool.

set -euo pipefail

usage () {
	cat <<'EOF'
Usage: list-mcp-tools.sh [-h|--help] [--license] [REPO_ROOT]

Print every clawtilla_* tool registered in REPO_ROOT/src/mcp/clawt-mcp-tools.c,
sorted, one per line. REPO_ROOT defaults to the clawtilla checkout that holds
this script (three directories above it).

Examples:
  list-mcp-tools.sh
  list-mcp-tools.sh ~/source/projects/clawtilla
  list-mcp-tools.sh | wc -l
EOF
}

main () {
	local script_dir
	local repo_root
	local source_file

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
	source_file="${repo_root}/src/mcp/clawt-mcp-tools.c"

	if [[ ! -r "${source_file}" ]]
	then
		echo "list-mcp-tools.sh: cannot read ${source_file}" >&2
		exit 1
	fi

	grep -oE '^[[:space:]]*(HOP_)?TOOL\("clawtilla_[a-z0-9_]+"' "${source_file}" \
		| grep -oE 'clawtilla_[a-z0-9_]+' \
		| sort -u
}

main "$@"
