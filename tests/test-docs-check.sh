#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Exercise the real checker with a minimal tree, including deliberate errors.
set -eu

CDPATH='' cd "$(dirname "$0")/.."
checker="$(pwd)/tools/clawt-docs-check.sh"
fixture="$(mktemp -d "${TMPDIR:-/tmp}/clawt-docs-check-XXXXXX")"

# The fixture belongs only to this invocation, including failure evidence.
cleanup () {
	rm -rf -- "${fixture}"
}
trap cleanup EXIT

mkdir -p "${fixture}/src/config" "${fixture}/docs" "${fixture}/external"
: > "${fixture}/Makefile"
: > "${fixture}/config.mk"
: > "${fixture}/rules.mk"
printf '%s\n' '"daemon.socket"' > "${fixture}/src/config/clawt-config-schema.c"
printf '%s\n' 'channels:' '  webhook:' '    endpoints: []' > "${fixture}/external/defaults.yaml"
cd "${fixture}"
export LIBRECLAW_DEFAULTS="${fixture}/external/defaults.yaml"

# Existing make files and an upstream option from the configured checkout pass.
printf '%s\n' '=daemon.socket= =config.mk= =rules.mk= =channels.webhook.endpoints=' > docs/test.org
sh "${checker}" > result.log 2>&1
grep -qF 'docs-check: OK' result.log

# A suffix must not hide a misspelled filename or an unknown schema key.
for bad_key in absent.mk daemon.missing channels.webhook.missing
 do
	printf '=%s=\n' "${bad_key}" > docs/test.org
	if sh "${checker}" > result.log 2>&1
	then
		echo "docs-check test: accepted ${bad_key}" >&2
		exit 1
	fi
	grep -qF "config key '${bad_key}'" result.log
done

# An unavailable upstream checkout must never silently exempt upstream keys.
printf '%s\n' '=channels.webhook.endpoints=' > docs/test.org
LIBRECLAW_DEFAULTS="${fixture}/missing.yaml"
export LIBRECLAW_DEFAULTS
if sh "${checker}" > result.log 2>&1
then
	echo 'docs-check test: accepted an unchecked upstream option' >&2
	exit 1
fi
grep -qF "config key 'channels.webhook.endpoints'" result.log
echo 'docs-check regression tests passed.'
