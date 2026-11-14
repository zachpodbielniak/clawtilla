#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Every SoupMessage clawtilla builds goes through clawt_http_message_new(),
# which makes the caller say whether the request carries a credential and
# refuses redirects when it does.  A raw soup_message_new() anywhere else
# is a request whose redirect behaviour nobody decided -- libsoup's default
# is to follow, Authorization header and all.
#
# What this cannot see: a message built inside a dependency (libreclaw,
# ai-glib, mcp-glib), or one built by a constructor this pattern does not
# name.  It reads clawtilla's own sources and nothing else.
set -eu

CDPATH='' cd "$(dirname "$0")/.."

pattern='soup_message_new(_from_uri|_from_encoded_form|_from_multipart|_options_ping)?[[:space:]]*\('
offenders="$(grep -rnE --include='*.c' --include='*.h' "${pattern}" src clients plugins \
	| grep -v '^src/clawt-http\.[ch]:' || true)"

if [ -n "${offenders}" ]
then
	echo 'http-constructors: a SoupMessage made outside clawt_http_message_new():' >&2
	echo "${offenders}" >&2
	echo 'Use clawt_http_message_new() and say whether it carries a credential.' >&2
	exit 1
fi

# The helper itself must still be the one place that sets the flag.
grep -q 'SOUP_MESSAGE_NO_REDIRECT' src/clawt-http.c

echo 'http-constructors: OK'
