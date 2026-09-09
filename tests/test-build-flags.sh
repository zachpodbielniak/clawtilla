#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Compile a disposable target with the project's real flag stamp rules.

if [ -z "${BASH_VERSION:-}" ]; then
	exec bash "$0" "$@"
fi
set -euo pipefail

case "${1:-}" in
	-h|--help)
		printf 'Usage: %s [--help|--license]\nExample: bash %s\n' "$0" "$0"
		exit 0
		;;
	--license)
		printf 'SPDX-License-Identifier: AGPL-3.0-or-later\nGNU Affero General Public License, version 3 or later.\n'
		exit 0
		;;
	'') ;;
	*) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
esac

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." > /dev/null && pwd)"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/clawt-build-flags-XXXXXX")"
# The fixture is exclusively ours; remove only that directory on every exit.
trap 'rm -rf -- "$test_dir"' EXIT
mkdir -p "$test_dir/src"
cp "$repo_dir/config.mk" "$repo_dir/rules.mk" "$test_dir/"
cat > "$test_dir/src/probe.c" <<'SOURCE'
#include <stdio.h>
/* Expose the actual compiled flag, rather than inspect make's text. */
int
main(void)
{
	printf("%d\n", FLAG_VALUE);
	return 0;
}
SOURCE
cat > "$test_dir/Makefile" <<'MAKEFILE'
include config.mk
CFLAGS = -std=gnu89 -Wall -Wextra -Werror -DFLAG_VALUE=$(FLAG_VALUE)
LDFLAGS =
LIB_OBJECTS = $(OBJDIR)/probe.o
include rules.mk
.PHONY: probe
probe: $(OBJDIR)/probe.o
	@$(CC) $< -o $(OUTDIR)/probe
	@$(OUTDIR)/probe
MAKEFILE

# Both modes must notice a changed flag and preserve unchanged objects.
# Keeping the source untouched is essential: touching it would hide the bug.
for debug in 0 1
 do
	for value in 1 2
	 do
		actual="$(make --no-print-directory -s -C "$test_dir" probe DEBUG="$debug" FLAG_VALUE="$value")"
		if [[ "$actual" != "$value" ]]
		then
			printf 'DEBUG=%s: expected compiled value %s, got %s\n' "$debug" "$value" "$actual" >&2
			exit 1
		fi
	 done
	mode=release
	if [[ "$debug" == 1 ]]; then mode=debug; fi
	object="$test_dir/build/$mode/obj/probe.o"
	before="$(stat -c '%y' "$object")"
	make --no-print-directory -s -C "$test_dir" probe DEBUG="$debug" FLAG_VALUE=2 > /dev/null
	[[ "$(stat -c '%y' "$object")" == "$before" ]]
 done
printf 'build flag regressions passed\n'
