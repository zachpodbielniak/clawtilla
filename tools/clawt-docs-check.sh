#!/bin/sh
# clawt-docs-check.sh - Fail on undocumented public API or stale docs.
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Two checks, both of which catch things a compiler never will:
#
#   1. Every exported symbol declared in a public header has a gtk-doc
#      comment block.  Undocumented public API is how a library becomes
#      unusable from introspection without anybody noticing.
#
#   2. Every config key referenced in docs/ still exists in the schema.
#      Docs that name a removed option are worse than no docs: the reader
#      trusts them and then debugs why their setting does nothing.
#
#   3. Every clawtilla_* tool named in docs/ is actually registered.  A
#      doc that names a tool nobody built is worse than one that names
#      none: the reader tells their agent to use it and the agent
#      reports, accurately, that it does not exist.
#
#   4. No source file contains double-encoded UTF-8.  A tool that reads a
#      file as Latin-1 and writes it back turns an ellipsis into three
#      characters, and the compiler is perfectly happy: it is a string
#      literal either way.  It shows up as mojibake in the sidebar, which
#      is a long way from the edit that caused it -- and it has happened
#      here twice.

set -eu

FAIL=0

check_public_headers () {
    local_headers=$(grep -E '^\s+\$\(SRCDIR\)/.*\.h' Makefile 2>/dev/null \
        | sed 's|.*\$(SRCDIR)/||; s|[[:space:]]*\\*$||' || true)

    for local_h in ${local_headers}
    do
        [ -f "src/${local_h}" ] || continue

        # Exported functions are declared at column 0 as `type name(`, or as
        # `name(` on its own line following the return type.  Anything with a
        # gtk-doc block has a `/**` within the preceding few lines.
        awk -v file="src/${local_h}" '
            /^\/\*\*/ { doc = NR }
            /^[a-zA-Z_][a-zA-Z0-9_ *]*\**[a-zA-Z_][a-zA-Z0-9_]*\(/ {
                if ($0 ~ /^(static|typedef|G_|#)/) next

                # The GType boilerplate every boxed and object type
                # declares.  gtk-doc generates its entry from the type
                # itself, and nobody has ever written a useful comment
                # for one; requiring it would mean 20 identical stubs.
                if ($0 ~ /_get_type\(void\)/) next
                if (doc == 0 || NR - doc > 40) {
                    printf "docs-check: %s:%d: exported symbol without a doc comment: %s\n", file, NR, $0
                    bad = 1
                }
            }
            END { exit bad ? 1 : 0 }
        ' "src/${local_h}" || FAIL=1
    done
}

#
# A key the docs name in `=daemon.socket=` markup exists somewhere.
#
# Most belong to clawtilla's schema.  Some belong to *libreclaw* and are
# named here on purpose: the `libreclaw:` passthrough exists precisely so
# an option clawtilla does not model is still reachable, and the webhook
# routing at `channels.webhook.endpoints` is the case that has to be
# written down.  Those are checked against libreclaw's own documented
# defaults instead, by last segment, because the file is YAML rather than
# a table this can look a full path up in.
#
# Checked rather than exempted: a doc naming a libreclaw key that
# libreclaw does not have is exactly as misleading as one naming a
# clawtilla key that clawtilla does not have.
#
LIBRECLAW_SECTIONS="agent ai session database skills channels tools
                    memory logging otel plugins"
LIBRECLAW_DEFAULTS="deps/libreclaw/data/default-config.yaml"

# POSIX sh throughout, like the rest of this script: it runs from make
# and has no reason to need bash.
key_is_libreclaws () {
    if [ $# -ne 1 ]
    then
        echo "key_is_libreclaws() requires 1 positional argument" >&2
        exit 1
    fi

    local_head="${1%%.*}"
    local_tail="${1##*.}"

    [ -f "${LIBRECLAW_DEFAULTS}" ] || return 1

    for local_section in ${LIBRECLAW_SECTIONS}
    do
        [ "${local_head}" = "${local_section}" ] || continue

        if grep -qE "^[[:space:]]*#?[[:space:]]*${local_tail}:" \
                "${LIBRECLAW_DEFAULTS}"
        then
            return 0
        fi

        return 1
    done

    return 1
}

check_doc_config_keys () {
    [ -f src/config/clawt-config-schema.c ] || return 0
    [ -d docs ] || return 0

    # Keys the docs mention, in `=daemon.socket=` org verbatim markup.
    for local_key in $(grep -rhoE '=[a-z_]+(\.[a-z_]+)+=' docs/ 2>/dev/null \
                       | tr -d '=' | sort -u)
    do
        if grep -q "\"${local_key}\"" src/config/clawt-config-schema.c
        then
            continue
        fi

        if key_is_libreclaws "${local_key}"
        then
            continue
        fi

        echo "docs-check: docs reference config key '${local_key}' which is not in the schema"
        FAIL=1
    done
}

# Every clawtilla_* tool the docs name still exists.
#
# The same failure as a stale config key and a worse one to read: a
# reader trusts the name, tells their agent to use it, and the agent
# reports -- accurately -- that there is no such tool.  docs/computers.org
# promised clawtilla_computer_put_file, get_file and exchange_list for a
# long time and none of the three were ever built.
#
# A trailing underscore is skipped: `clawtilla_memory_*` in prose is a
# family, not a name.
check_doc_tool_names () {
    [ -f src/mcp/clawt-mcp-tools.c ] || return 0
    [ -d docs ] || return 0

    for local_tool in $(grep -rhoE 'clawtilla_[a-z_]+' docs/ README.org \
                        2>/dev/null | sort -u)
    do
        case "${local_tool}" in
            *_) continue ;;
        esac

        if ! grep -q "\"${local_tool}\"" src/mcp/clawt-mcp-tools.c
        then
            echo "docs-check: docs name tool '${local_tool}', which is not registered"
            FAIL=1
        fi
    done
}

# UTF-8 that has been through Latin-1 and back.
#
# Two signatures, because the two cases look different:
#
#   \xc2[\x80-\x9f] is a C1 control character.  Every multi-byte UTF-8
#   character has a continuation byte, and one in the 80-9F range becomes
#   exactly this.  Nothing legitimate in source is a C1 control -- note
#   the range stops short of A0, which is a non-breaking space.
#
#   \xc3[\x82\x83]\xc2 is an "A-circumflex" or "A-tilde" immediately
#   followed by another Latin-1 lead, which is what a two-byte character
#   turns into.
MOJIBAKE='\xc2[\x80-\x9f]|\xc3[\x82\x83]\xc2'

check_double_encoded_utf8 () {
    for source in $(git ls-files '*.c' '*.h' '*.org' '*.md' '*.yaml' \
                                '*.in' 2>/dev/null)
    do
        [ -f "${source}" ] || continue

        if LC_ALL=C grep -qP "${MOJIBAKE}" "${source}" 2>/dev/null
        then
            echo "docs-check: ${source} has double-encoded UTF-8:"
            LC_ALL=C grep -nP "${MOJIBAKE}" "${source}" | head -3
            FAIL=1
        fi
    done
}


# ---------------------------------------------------------------------------
# The other direction: something the code owns that no doc names.
#
# The three checks above all ask "does this name in a doc still exist".
# None of them asked "does this thing that exists appear in a doc at all",
# and that gap is how the tree came to hold 20 IPC frame kinds, five
# orchestration tools and a CLI verb documented nowhere.  It reads
# differently from a stale name and is worse for it: a stale name is
# reported by whoever trusts it, while a feature nobody wrote down is
# simply never used, and nothing anywhere says so.  `clawtilla event` was
# the sharp one -- the command that reads the daemon's event history, and
# the difference between diagnosing a message loop with a control and
# doing it with sqlite3 against a file on the host.
#
# Matched as a fixed string, not a pattern.  A kind is `agent.list` and
# a dot matches anything, so `task.changed` was satisfied by the prose
# word `on_task_changed` -- the check reported OK about an event whose
# documentation had been deleted.  Same family as the comment-stripping
# lesson in CLAUDE.md: a grep-based check can report the opposite of the
# truth, and only sabotaging the thing being checked reveals it.
#
# One mention is the bar.  This cannot tell prose from a row in a table,
# and pretending otherwise would make it a style check people argue with.
# Naming the thing once is the difference between discoverable and
# invisible; whether it is *explained* is a reader's judgement.
#
# Not extended to keys in `~key~` markup, which was measured rather than
# assumed.  The docs use `=key=` for config keys and `~key~` for
# identifiers generally -- frame kinds, filenames, function names -- but
# not strictly: 71 real config keys are written with tildes.  Filtering
# those 252 names down by section, by the agent-relative spelling, by the
# frame-kind and event vocabularies and by file suffix leaves seven, and
# all seven are false positives.  Five are events this file enumerates
# elsewhere; two are docs stating that a key *does not* exist ("there is
# no ~routines.quiet_hours~", "~computer.type~ is not
# ~defaults.computer.type~").  A negation is invisible to grep, so that
# last pair cannot be filtered at all -- the check would report seven
# wolves and no sheep, and a check that cries wolf is one people learn to
# ignore.
# ---------------------------------------------------------------------------

# Every orchestration tool an agent can be given is named in a doc.
#
# An operator reading docs/orchestration.org decides what their fleet may
# do.  A tool that is registered, offered to agents and named in no doc
# is a capability granted by a switch nobody can find.
check_tool_coverage () {
    [ -f src/mcp/clawt-mcp-tools.c ] || return 0
    [ -d docs ] || return 0

    for local_tool in $(grep -oE 'TOOL\("clawtilla_[a-z_]+"' \
                            src/mcp/clawt-mcp-tools.c \
                        | sed 's/.*"\(.*\)"/\1/' | sort -u)
    do
        if ! grep -rqF "${local_tool}" docs/ README.org 2>/dev/null
        then
            echo "docs-check: tool '${local_tool}' is registered but named in no doc"
            FAIL=1
        fi
    done
}

# Every CLI subcommand is named in a doc.
#
# The CLI dispatches on argv[1] before option parsing, so the verb list
# is exactly this grep and cannot drift from a second copy.
check_cli_verb_coverage () {
    [ -f clients/cli/main.c ] || return 0
    [ -d docs ] || return 0

    for local_verb in $(grep -oE 'g_strcmp0\(argv\[1\], "[a-z-]+"\)' \
                            clients/cli/main.c \
                        | sed 's/.*"\(.*\)".*/\1/' | sort -u)
    do
        # Either spelled out in prose, or as a row in the CLI reference
        # table, which drops the program name -- requiring "clawtilla foo"
        # alone reported `event` missing while it sat in that table.
        #
        # No \\b here: grep on this machine is ugrep, which refuses a word
        # boundary inside an alternation ("empty (sub)expression") and
        # then exits non-zero, which this reads as "not documented".  A
        # check whose regex fails reports every item as missing, which is
        # indistinguishable from a tree with no docs at all.
        if ! grep -rqE "(clawtilla|~)${local_verb}([ ~]|$)" \
                docs/ README.org 2>/dev/null
        then
            echo "docs-check: CLI verb 'clawtilla ${local_verb}' is documented nowhere"
            FAIL=1
        fi
    done
}

# Every frame kind the daemon answers is in the protocol reference.
#
# docs/ipc-protocol.org is the whole contract for anybody writing a
# client -- an in-process cmacs embed, a second UI, a script.  A kind the
# daemon handles and the reference omits is a capability that exists and
# cannot be discovered, which is one of the ways two clients drift apart
# without `make parity` being able to see it.
check_ipc_kind_coverage () {
    [ -f docs/ipc-protocol.org ] || return 0

    for local_kind in $(grep -rhoE 'g_strcmp0\(kind, "[a-z_.]+"\)' \
                            src/core/clawt-daemon.c src/ipc/clawt-ipc-server.c \
                            2>/dev/null \
                        | sed 's/.*"\(.*\)".*/\1/' | sort -u)
    do
        if ! grep -qF "${local_kind}" docs/ipc-protocol.org
        then
            echo "docs-check: frame kind '${local_kind}' is handled but absent from docs/ipc-protocol.org"
            FAIL=1
        fi
    done
}

# Every event the daemon publishes is in the protocol reference.
#
# An event is the only way a client learns something happened without
# asking, so an undocumented one is a thing clients poll for instead.
#
# Four construction sites, because there are four: the bus directly, the
# router's own publish(), the kinds spelled in clawt-event.c, and
# clawt_event_new() followed by a publish.  The last was missing at
# first, which hid `computer.exec`, `message` and `task.changed` -- all
# three documented, so this reported OK while being blind to them.  A
# check that cannot see a thing is not checking it, and the day one of
# those three had gone undocumented it would have said OK anyway.  The
# `.h`/`.c` exclusion drops an #include the pattern would match.
check_event_kind_coverage () {
    [ -f docs/ipc-protocol.org ] || return 0

    for local_event in $( { grep -rhoE 'clawt_event_bus_emit\([^,]+, *"[a-z_.]+"' src/ 2>/dev/null
                            grep -rhoE 'publish\(self, *"[a-z_.]+"' src/ 2>/dev/null
                            grep -rhoE 'clawt_event_new\("[a-z_.]+"' src/ 2>/dev/null
                          } | sed 's/.*"\(.*\)"/\1/'
                          grep -rhoE '"[a-z_]+\.[a-z_]+"' src/core/clawt-event.c 2>/dev/null \
                          | tr -d '"' \
                        | sort -u | grep -vE '\.[ch]$')
    do
        if ! grep -qF "${local_event}" docs/ipc-protocol.org
        then
            echo "docs-check: event '${local_event}' is published but absent from docs/ipc-protocol.org"
            FAIL=1
        fi
    done
}

main () {
    check_public_headers
    check_doc_config_keys
    check_doc_tool_names
    check_double_encoded_utf8
    check_tool_coverage
    check_cli_verb_coverage
    check_ipc_kind_coverage
    check_event_kind_coverage

    if [ "${FAIL}" -ne 0 ]
    then
        echo "docs-check: FAILED"
        exit 1
    fi

    echo "docs-check: OK"
    exit 0
}

main "$@"
