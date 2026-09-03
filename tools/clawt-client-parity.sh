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
#
# What layer 2 cannot see, stated rather than implied: it compares
# `"/name"` *literals*, so a command whose name comes from a skill the
# agent happens to have is invisible to it.  Neither client contains
# those names.  They are declared as affordances instead -- which is
# weaker, and is the honest amount of checking available.

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
gtk_render=""
web_render=""
api_text=""

cleanup () {
    rm -f "${gtk}" "${web}" "${only_gtk}" "${only_web}" \
          "${gtk_cmds}" "${web_cmds}" "${only_gtk_cmds}" \
          "${gtk_code}" "${web_code}" "${gtk_render}" "${web_render}" \
          "${api_text}"
}

trap cleanup EXIT

# CDPATH is cleared above, so a bare cd cannot echo the directory it
# resolved to and have the command substitution capture it as well as pwd's
# own output.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GTK_DIR="${ROOT}/clients/gtk"
WEB_DIR="${ROOT}/clients/web"
#
# The daemon's client surface, which is one file per verb family.
#
# A glob rather than a name, because the dispatch used to be one chain in
# clawt-daemon.c and moving it out left this reading a file with no kinds
# in it at all -- which reported OK for every client, since a client
# cannot be missing a kind that nothing found.
#
DAEMON_SOURCES=("${ROOT}"/src/core/clawt-daemon.c "${ROOT}"/src/core/daemon-*.c)

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
    ["colour scheme"]="src/config/clawt-appearance.c|s/.*{ *CLAWT_THEME_[A-Z_]*, *\"\([a-z-]*\)\".*/\1/p"
    #
    # CLAWT_MEASURE_DEFAULT is deliberately not in this list. Its nick is
    # "default", which is also a button style in the web client and a
    # dozen other things besides -- so it is evidence of nothing, and a
    # check that reports a hardcoding which is not there is one people
    # learn to ignore. The units that can actually go missing from a
    # client are the three with values.
    #
    ["measure unit"]="src/config/clawt-appearance.c|s/.*{ *CLAWT_MEASURE_\(PERCENT\|COLUMNS\|PIXELS\), *\"\([a-z]*\)\".*/\2/p"
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
    ["markdown rendering"]="clawt_markdown_to_pango|clawt_markdown_to_html"
    ["attachment previews"]="append_attachment_previews|attachment"
    ["unread count"]="clawt-unread-badge|unread-badge"
    ["unread tab total"]="adw_view_stack_page_set_badge_number|unread_total"
    ["message runs"]="clawt_chat_run_is_start|clawt_chat_run_is_start"
    ["transcript stamps"]="clawt_chat_time_label|clawt_chat_time_label"
    ["day dividers"]="day_divider|day-divider"
    ["sender avatars"]="adw_avatar_new|msg-avatar"
    ["own-turn bubbles"]="clawt-bubble|msg-self"
    ["alerts surface"]="build_alerts_panel|alert-row"
    ["alert severity tiers"]="clawt_alert_tier_for_event|clawt_alert_tier_for_event"
    ["unread rule"]="clawt_unread_should_count|clawt_unread_should_count"
    ["team tally"]="clawt_team_tally|clawt_team_tally"
    ["fleet warnings"]="append_warning_rows|clawt_web_warnings"
    ["peer conversations"]="fill_conversation_menu|conversation_switcher"
    ["team busy marker"]="clawt-team-busy|clawt-team-busy"
    ["follow the live edge"]="clawt_transcript_is_at_bottom|transcript-end"
    ["alert filter"]="alerts_show_all|show_all"
    ["agent attachments"]="fetch_attachment|attachment_element"
    ["flow groups by sender"]="flow_run_sender|flow_run_sender"
    ["alert arrives read"]="clawt_alert_arrives_read|clawt_alert_arrives_read"
    ["composer on the message column"]="CHAT_BODY_INSET|chat-gutter"
    ["reading measurements"]="build_reading_group|clawt_run_gap"
    ["measure units"]="clawt_measure_unit_count|clawt_measure_unit_count"
    ["computer types"]="clawt_computer_type_count|clawt_computer_type_count"
    ["import modes"]="clawt_import_mode_count|clawt_import_mode_count"
    ["palettes from disk"]="clawt_appearance_scheme_count|clawt_appearance_scheme_count"
    ["message boundary in a run"]="CHAT_MESSAGE_SPACING|msg-time"
    ["decision inbox"]="build_decision_page|decision-row"
    #
    # The count on the top-level tab, which is a different thing from
    # the page: the GTK client sums a section's pages onto its own tab
    # and the web client draws a pill on Work.  Neither touches a shared
    # symbol, so layers 3 and 4 cannot see it -- and the web half did
    # not exist until the daemon started saying when a decision was
    # settled, without which a count could only ever go up.
    #
    ["decision count on a tab"]="clawt_gtk_set_page_badge|clawt_web_app_open_decisions"
    ["connection reachability"]="clawt_connection_probe|clawt_connection_probe"
    ["identity size"]="clawt-identity-size|clawt-identity-size"
    ["connection banner"]="connection_banner|clawt-connection-banner"
    ["daemon version check"]="note_daemon_version|note_daemon_version"
    ["connection notice"]="clawt_connection_notice_text|clawt_connection_notice_text"
    ["start on a remote daemon"]="opt_profile|opt_profile"
    ["resync after a gap"]="\"resync\"|\"resync\""
    ["stop the running turn"]="on_stop_turn|stop_turn_button"
    ["decision options stack"]="decision_option_button|decision-options"
    ["sidebar face"]="clawt_gtk_build_avatar|agent-face"
    ["picture editor"]="build_avatar_group|on_avatar_set"
    #
    # A description under each agent's name, and the setting that keeps
    # it for the pointer instead.
    #
    # The GTK sidebar has drawn this as a row subtitle since it was
    # written and the web sidebar drew nothing at all -- for as long,
    # and reported by nothing, because a subtitle sends no frame,
    # answers no command and spells out no library value.  Layer 5 is
    # the only one that can see it, which is exactly what layer 5 is
    # for.
    #
    ["agent descriptions"]="clawt-agent-row|agent-desc"
    ["hide agent descriptions"]="build_fleet_list_group|clawt_agent_desc"
    #
    # Which standing is drawn beside a name.  The rule itself is in the
    # library, so layer 3 would catch a client that stopped walking it
    # -- but neither client walked it for a year while `team_role` sat
    # unread in every `agent.list` reply, so the *drawing* is declared
    # too.
    #
    ["team standing badges"]="CLAWT_TEAM_BADGE_LEAD|CLAWT_TEAM_BADGE_LEAD"
    #
    # Clicking a face to see the picture properly.
    #
    # Neither half shares a symbol with the other -- one opens a
    # GtkWindow, the other toggles an overlay from the page-head script
    # -- so layers 1 to 4 are all blind to it.
    #
    ["enlarge a profile picture"]="on_avatar_preview_clicked|avatar-zoom"
    ["trigger secret shown once"]="secret_shown_once|secret_shown_once"
    ["trigger verification capture"]="on_trigger_capture|on_trigger_capture"
    ["composer drafts"]="clawt_gtk_persist_draft|draft_key_for"
    ["steer a busy agent"]="\"steered\"|\"steered\""
    #
    # What an agent is doing, as opposed to whether its process is up.
    #
    # Both clients call the same library function, so the marker is that
    # call in each -- layer 3 does not see it because the rule is a
    # helper rather than a _count()/_nth() enumeration, and layer 4 does
    # not because neither client spells any of its output out.
    #
    ["agent activity"]="clawt_agent_activity_label|clawt_agent_activity_label"
    #
    # A routine that can never fire says why.
    #
    # The daemon has answered this as `problem` since routine.list was
    # written and only the web client drew it -- and the GTK row was not
    # merely silent, it drew "only when you ask", which is what it draws
    # for a `manual` routine.  Layers 1 to 4 are all blind to it: the
    # frame kind is shared, no command names it, it is not an
    # enumeration, and neither client spells the message out.
    #
    # Matched on each client's own rendering rather than on the member
    # name, because `"problem"` is also how an integration binding
    # reports its own trouble in three other files.
    #
    ["a broken routine says why"]="will never run|routine, \"problem\""
    #
    # An update the daemon found.
    #
    # Both clients read it out of control.status and hand it to the same
    # library sentence, so the marker is the field each keeps it in.
    # Layer 1 cannot see it -- control.status is a frame both already
    # send, and this is a member inside its reply.
    #
    ["an available update"]="daemon_update|daemon_update"
    #
    # Holding the fleet, and saying which agents are held.
    #
    # The frame kinds are caught by layer 1 -- both clients send
    # control.pause and control.resume as literals -- but the *badge*
    # that says an agent is held is not a frame, so it is declared.
    #
    ["a held agent is drawn as held"]="clawt_hold_label|clawt_hold_label"
    #
    # A skill's `/name` is dynamic, so layer 2 cannot see it.
    #
    # That layer compares `"/name"` string literals, which is exactly
    # right for the built-in commands and useless here: these come from
    # whatever skills the selected agent has been assigned, and neither
    # client contains their names at all.  Declared here instead, with
    # the limitation stated rather than left for somebody to discover
    # when the check reports OK through a client that lost the feature
    # entirely.
    #
    ["skill commands in the composer"]="clawt_gtk_skill_commands|slash-popover"
    ["skill command expansion"]="clawt_gtk_skill_expand|skill.expand"
    ["skills library"]="clawt_gtk_refresh_skills|clawt_web_skills_body"
    ["skill provenance"]="Provenance|sha256"
    ["skill scan warnings"]="Not copied|Not copied"
    ["fleet recall"]="recall_entry|recall-form"
    ["recall results"]="refresh_recall_once|recall-hit"
    ["operator profile"]="operator_view|operator-profile"
    ["screen preview"]="screen_picture|screen-frame"
    ["screen takeover"]="on_screen_take|on_screen_take"
    ["screen input"]="on_screen_input|on_screen_input"
    ["stale frame label"]="\"stale\"|\"stale\""
    ["vnc viewer"]="remote-viewer|viewer.vv"
    ["teach recordings"]="teach_group|add_teach_section"
    #
    # The caveat, which is the one part of this feature that must not be
    # in only one client.
    #
    # A recording of a demonstration is credential material until
    # somebody has read it, and the sentence saying so is carried on the
    # trace precisely so that every surface shows it. A client that drew
    # the recordings and not the caveat would be the one somebody used
    # to decide a trace was safe to share.
    #
    ["teach caveats"]="caveats|caveats"
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
# Comments removed first, for the same reason the client sources are read
# that way: this file is full of prose quoting the code it describes, and
# a kind named only in a paragraph is a kind the daemon does not answer.
#
daemon_kinds () {
    strip_comments "${DAEMON_SOURCES[@]}" \
        | grep -o 'kind, "[a-z_.]*"' \
        | sed 's/kind, "//; s/"//' \
        | sort -u
}

#
# The frame kinds a client's code -- comments already removed -- mentions.
#
# Intersected with the daemon's own list, because a client is full of
# dotted strings that are not frame kinds -- css classes, file names,
# member names.
#
# It takes the stripped corpus rather than a directory because it used to
# take a directory and grep the sources raw, while layers 3 to 5 all went
# through strip_comments().  That is the rule this script states twice
# and applied in three of five places, and the failure it produces is the
# one already written down here: a comment explaining that something was
# *removed* still contains its name, so the check reads the removed thing
# as present and reports OK.  Nothing was being masked when this was
# fixed -- every kind and command in a client comment was also in that
# client's code -- which is exactly how long a latent one waits.
#
client_kinds () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_kinds()` requires 1 positional argument' >&2
        echo 'client_kinds <stripped-source-file>' >&2
        exit 1
    fi

    comm -12 \
        <(grep -o '"[a-z_]\+\.[a-z_.]\+"' "${1}" \
            | tr -d '"' | sort -u) \
        <(daemon_kinds)
}

#
# The slash commands a client answers.
#
# Both keep them in a table of "/name" literals, which is enough to
# compare without either having to declare anything for this script's
# benefit.  Read from the stripped corpus, for the reason above: the GTK
# client's own comments name commands, and a comment is not an answer.
#
client_commands () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_commands()` requires 1 positional argument' >&2
        echo 'client_commands <stripped-source-file>' >&2
        exit 1
    fi

    #
    # A client with no slash commands at all is an empty set, not a
    # failure -- and under `set -o pipefail` grep's "no matches" exit
    # would otherwise take the whole script down before it printed
    # anything, which is a parity check that reports nothing and exits 1.
    # Exit 1 is "found none"; exit 2 is a real error and still counts.
    #
    { grep -ohE '"/[a-z][a-z-]*"' "${1}" || [[ $? -eq 1 ]]; } \
        | tr -d '"' \
        | sort -u
}

#
# The public headers with the comments taken out.
#
# For the same reason the client sources are read that way, one layer
# down -- and it came true here too.  clawt_page_count()'s own doc
# comment explains that there is deliberately no clawt_page_nth() beside
# it, and naming the function it does not have was enough to make this
# read the pair as present: the family was then reported as walked by
# one client and not the other, which is a real failure mode being
# announced about an API that does not exist.
#
# Built once and reused, since enumerations() consults it twice per
# family.
#
public_api () {
    if [[ -z "${api_text}" ]]
    then
        api_text="$(mktemp)"
        strip_comments < <(cat "${ROOT}"/src/*.h "${ROOT}"/src/*/*.h) \
            > "${api_text}"
    fi

    echo "${api_text}"
}

#
# The choice enumerations the public API offers.
#
# A `_count()` with a matching `_nth()` is the library saying "here is a
# set, walk it".  Read from the headers rather than listed here, so one
# added later is compared from the moment it exists.
#
enumerations () {
    local api
    local family

    api="$(public_api)"

    for family in $(grep -ohE '\bclawt_[a-z0-9_]+_count[[:space:]]*\(' \
                        "${api}" \
                    | tr -d ' (' | sed 's/_count$//' | sort -u)
    do
        #
        # Only the ones that are a *set of choices*.  clawt_memory_store_count()
        # and clawt_room_get_message_count() are counts of things that
        # happened, and have no _nth() beside them precisely because
        # there is nothing to offer.
        #
        if grep -qhE "\b${family}_nth[[:space:]]*\(" "${api}"
        then
            echo "${family}"
        fi
    done
}

#
# Whether a client walks a given enumeration.
#
# Takes the client's comment-stripped code rather than its directory,
# for the third time in this file and the same reason each time: prose
# that *names* a function is not a call to it.  The topbar's comment
# says it is "walked out of clawt_section_count()", and with the raw
# sources that sentence alone was enough to report the enumeration as
# walked -- deleting the loop under it changed nothing.
#
# The two other layers already read stripped code; this one did not, so
# the rule was about those call sites rather than about the check.
#
walks_enumeration () {
    if [[ $# -ne 2 ]]
    then
        # shellcheck disable=SC2016
        echo '`walks_enumeration()` requires 2 positional arguments' >&2
        echo 'walks_enumeration <code-file> <family>' >&2
        exit 1
    fi

    grep -qE "\b${2}_(count|nth)[[:space:]]*\(" "${1}"
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
strip_comments () {
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
    }' "$@"
}

#
# Every line of a client's own code, comments removed.
#
client_code () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_code()` requires 1 positional argument' >&2
        exit 1
    fi

    strip_comments "${1}"/*.c
}

#
# The same, minus the stylesheet.
#
# A CSS rule is not a capability.  A class the sheet styles and nothing
# renders is dead CSS -- and it satisfied the affordance check on its own,
# because the marker for a drawn thing is usually the class name and the
# class name is in both files.  Eight of the declared markers were in
# web-style.c as well as in a renderer, so deleting the renderer half of
# any of them reported OK: the exact shape of gap this check exists to
# catch, in the check.
#
# Layers 1 to 4 keep the whole corpus.  A stylesheet can genuinely reach a
# frame kind (it cannot) and can genuinely hardcode a library vocabulary
# value (it does, in each client's own dialect, on purpose) -- so the
# narrowing is for layer 5 alone, which is the layer about things being
# *drawn*.
#
client_render_code () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_render_code()` requires 1 positional argument' >&2
        exit 1
    fi

    local files=()
    local file

    for file in "${1}"/*.c
    do
        [[ "${file}" == *-style.c ]] && continue
        files+=("${file}")
    done

    strip_comments "${files[@]}"
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
    gtk_render="$(mktemp)"
    web_render="$(mktemp)"

    client_code "${GTK_DIR}" > "${gtk_code}"
    client_code "${WEB_DIR}" > "${web_code}"
    client_render_code "${GTK_DIR}" > "${gtk_render}"
    client_render_code "${WEB_DIR}" > "${web_render}"

    #
    # Every layer reads the same stripped corpus, built once above.
    #
    client_kinds "${gtk_code}" > "${gtk}"
    client_kinds "${web_code}" > "${web}"

    comm -23 "${gtk}" "${web}" > "${only_gtk}"
    comm -13 "${gtk}" "${web}" > "${only_web}"

    client_commands "${gtk_code}" > "${gtk_cmds}"
    client_commands "${web_code}" > "${web_cmds}"

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
            walks_enumeration "${gtk_code}" "${shown}" && who="gtk"
            walks_enumeration "${web_code}" "${shown}" && who="${who:+${who}+}web"
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

        walks_enumeration "${gtk_code}" "${family}" && in_gtk=1
        walks_enumeration "${web_code}" "${family}" && in_web=1

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

        grep -qF "${markers%%|*}" "${gtk_render}" || {
            printf '  %-28s declared, missing from clawtilla-gtk\n' \
                "${affordance}"
            failures=$((failures + 1))
        }

        grep -qF "${markers#*|}" "${web_render}" || {
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
