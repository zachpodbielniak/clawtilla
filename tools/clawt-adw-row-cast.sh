#!/usr/bin/env bash
#
# clawt-adw-row-cast.sh - A libadwaita row cast the compiler cannot check
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# ADW_ACTION_ROW() is a runtime cast, so applying an AdwActionRow method
# to a row that is not one compiles perfectly and then does nothing.
# AdwEntryRow and AdwExpanderRow both derive from AdwPreferencesRow and
# not from AdwActionRow -- AdwSwitchRow and AdwComboRow do -- so the four
# row types a preferences dialog uses split two and two, and the wrong
# half is only wrong at runtime.
#
# It costs two criticals per call and sets nothing.  Twenty-two of these
# had accumulated: every explanatory subtitle on an entry field in the
# integration, routine and trigger editors, and both urgency badges on
# the Decisions page -- which are also leaked, since a widget that is
# never parented keeps its floating reference.  The one that mattered
# most said that a credential field takes a *reference* (`env:NAME`)
# rather than a literal token, so an operator with no hint pastes the
# token in plain text.
#
# None of it is visible in a build: -Wall cannot see through a cast
# macro, the page renders, and the missing text reads as a design
# decision.  gtk-decisions.c even carries a comment explaining that
# AdwExpanderRow is not an AdwActionRow, ten lines above two calls that
# cast it to one.
#
# So it is checked here instead.  Per function rather than per file,
# because `row` is reused for both kinds within a file and a file-wide
# rule reports twice as many as exist.
#
# Usage: clawt-adw-row-cast.sh [file...]   (default: clients/gtk/*.c)

set -eu

files=$*

if [ -z "$files" ]
then
    files=$(ls clients/gtk/*.c 2>/dev/null || true)
fi

if [ -z "$files" ]
then
    echo "adw-row-cast: no sources to check" >&2
    exit 1
fi

# shellcheck disable=SC2086
awk '
    # Constructors and this tree`s helpers whose result is not an
    # AdwActionRow.  A helper is named here rather than followed, so a new
    # one that returns an AdwEntryRow has to be added -- which is the
    # trade a grep-based check makes everywhere else in this tree.
    function is_non_action(text) {
        return text ~ /adw_entry_row_new[ \t]*\(/ ||
               text ~ /adw_expander_row_new[ \t]*\(/ ||
               text ~ /adw_password_entry_row_new[ \t]*\(/ ||
               text ~ /adw_preferences_row_new[ \t]*\(/ ||
               text ~ /clawt_gtk_add_entry[ \t]*\(/ ||
               text ~ /add_int_entry[ \t]*\(/
    }

    # A function body opens with a brace in column one and closes the
    # same way, which is this project`s style throughout.
    /^\{/ { delete tainted; next }

    {
        line = $0

        # "x = ctor(...)" and "x =" with the constructor on the next line,
        # which is how the longer calls are wrapped here.
        if (match(line, /[A-Za-z_][A-Za-z0-9_]*(->[A-Za-z_][A-Za-z0-9_]*)?[ \t]*=/)) {
            lhs = substr(line, RSTART, RLENGTH)
            sub(/[ \t]*=$/, "", lhs)
            gsub(/^[ \t]+/, "", lhs)
            rhs = substr(line, RSTART + RLENGTH)

            if (is_non_action(rhs))
                tainted[lhs] = 1
            else if (rhs ~ /^[ \t]*$/) {
                pending = lhs
                pending_line = FNR
            }
        }

        if (pending != "" && FNR == pending_line + 1) {
            if (is_non_action(line))
                tainted[pending] = 1
            pending = ""
        }

        if (line ~ /ADW_ACTION_ROW\(/) {
            for (v in tainted) {
                if (index(line, "ADW_ACTION_ROW(" v ")") > 0) {
                    printf "%s:%d: ADW_ACTION_ROW(%s) -- %s is not an AdwActionRow\n", FILENAME, FNR, v, v
                    bad++
                }
            }
        }
    }

    END { exit (bad > 0) }
' $files && status=0 || status=$?

if [ "${status}" -ne 0 ]
then
    echo "adw-row-cast: FAILED -- see above" >&2
    echo "adw-row-cast: an AdwEntryRow has no subtitle and an AdwExpanderRow" >&2
    echo "adw-row-cast: has its own add_suffix; use clawt_gtk_set_row_hint()" >&2
    exit 1
fi

echo "adw-row-cast: OK"
