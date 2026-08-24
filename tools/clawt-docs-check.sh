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

check_doc_config_keys () {
    [ -f src/config/clawt-config-schema.c ] || return 0
    [ -d docs ] || return 0

    # Keys the docs mention, in `=daemon.socket=` org verbatim markup.
    for local_key in $(grep -rhoE '=[a-z_]+(\.[a-z_]+)+=' docs/ 2>/dev/null \
                       | tr -d '=' | sort -u)
    do
        if ! grep -q "\"${local_key}\"" src/config/clawt-config-schema.c
        then
            echo "docs-check: docs reference config key '${local_key}' which is not in the schema"
            FAIL=1
        fi
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

main () {
    check_public_headers
    check_doc_config_keys
    check_doc_tool_names
    check_double_encoded_utf8

    if [ "${FAIL}" -ne 0 ]
    then
        echo "docs-check: FAILED"
        exit 1
    fi

    echo "docs-check: OK"
    exit 0
}

main "$@"
