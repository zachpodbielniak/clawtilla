#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Keep clean-build ordering explicit: clean must not race parallel targets.
set -euo pipefail

case "${1:-}" in
	-h|--help)
		cat <<'HELP'
Usage: bash tools/clawt-ui-check.sh [--help|--license]
Clean-build all targets and tests, reject compiler warnings, then run tests.
Run from the repository root. Existing build outputs are regenerated.
Example: bash tools/clawt-ui-check.sh
HELP
		exit 0
		;;
	--license)
		cat LICENSE
		exit 0
		;;
	'') ;;
	*) echo 'Unknown argument; use --help.' >&2; exit 2 ;;
esac

review_log="$(mktemp /tmp/clawt-ui-check-XXXXXX.log)"
trap 'echo "Build and test log: ${review_log}"' EXIT

# These are distinct builds: test binaries must be rebuilt after clean,
# and the final all restores the runnable clients for manual inspection.
for review_target in all tests
do
	make clean >> "${review_log}" 2>&1
	if ! make -j8 "${review_target}" >> "${review_log}" 2>&1
	then
		tail -60 "${review_log}"
		exit 1
	fi
done
if ! make test >> "${review_log}" 2>&1
then
	tail -80 "${review_log}"
	exit 1
fi
make -j8 all >> "${review_log}" 2>&1
if rg -n '(^|[[:space:]])(warning:|error:)' "${review_log}"
then
	echo 'Compiler diagnostics found.' >&2
	exit 1
fi
echo 'UI verification passed'
