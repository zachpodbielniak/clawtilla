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

main () {
    check_public_headers
    check_doc_config_keys

    if [ "${FAIL}" -ne 0 ]
    then
        echo "docs-check: FAILED"
        exit 1
    fi

    echo "docs-check: OK"
    exit 0
}

main "$@"
