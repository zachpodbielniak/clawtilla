#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# clawt_web_add() and lrg's clawt_lrg_take(), clawt_lrg_card() and
# clawt_lrg_row_add_prefix()/_suffix() take the child's reference.  A
# child held in g_autoptr and handed to one of them without
# g_steal_pointer() is released twice, and so is one handed over and then
# g_object_unref()ed on the next line.  Either renders by luck until the
# freed memory is reused: the web Federation page's buttons were freed
# seven times over and crashed clawtilla-web, and lrg's peer rows drew no
# buttons and crashed a tree walk (0.73.3).
#
# What this cannot see: a reference released further away than the next
# line, through another variable, or in a function the helper is not
# named in.  It is a per-function scan by name, not an ownership checker.
set -eu

CDPATH='' cd "$(dirname "$0")/.."

offenders="$(awk '
	# A function body ends at a closing brace in column 0: names held in
	# g_autoptr are forgotten there, so a later function reusing "go" is
	# judged on its own declarations.
	FNR == 1 { delete held; previous = "" }
	/^}/ { delete held }
	{
		line = $0
		while (match(line, /g_autoptr\([[:space:]]*(Htmx|Lrg)[A-Za-z]*[[:space:]]*\)[[:space:]]*[A-Za-z_][A-Za-z_0-9]*/)) {
			decl = substr(line, RSTART, RLENGTH)
			sub(/.*\)[[:space:]]*/, "", decl)
			held[decl] = 1
			line = substr(line, RSTART + RLENGTH)
		}
		if (match($0, /(clawt_web_add|clawt_lrg_take|clawt_lrg_card|clawt_lrg_row_add_prefix|clawt_lrg_row_add_suffix)\([^;]*,[[:space:]]*[A-Za-z_][A-Za-z_0-9]*[[:space:]]*\)[[:space:]]*;/)) {
			call = substr($0, RSTART, RLENGTH)
			name = call
			sub(/[[:space:]]*\)[[:space:]]*;$/, "", name)
			sub(/.*,[[:space:]]*/, "", name)
			if (name in held)
				printf "%s:%d: %s (held in g_autoptr)\n", FILENAME, FNR, call
			given = name
		} else {
			given = ""
		}
		if (previous != "" && $0 ~ ("g_object_unref\\(" previous "\\);"))
			printf "%s:%d: g_object_unref(%s) after handing it over\n", FILENAME, FNR, previous
		previous = given
	}
' clients/web/*.c clients/lrg/*.c)"

if [ -n "${offenders}" ]
then
	echo 'client-ownership: a child released twice:' >&2
	echo "${offenders}" >&2
	echo 'Pass g_steal_pointer(&child), or drop the extra g_object_unref().' >&2
	exit 1
fi

echo 'client-ownership: OK'
