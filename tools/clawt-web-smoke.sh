#!/usr/bin/env bash
#
# clawt-web-smoke.sh - Ask clawtilla-web for every page it serves
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# The unit tests cover the renderers; this covers the routing, which they
# cannot: a route registered after "/a/:id/:view" is unreachable, and the
# symptom is not an error. The view slug falls back to chat, so a
# swallowed route renders the chat page and answers 200 -- which reads as
# working from every direction except looking at what came back.
#
# So this checks the *content type and a marker string*, not the status.

set -uo pipefail

CDPATH=

#
# This script owns the defaults, and `make web-smoke` reads them: it
# passes WEB_SMOKE_URL/WEB_SMOKE_AGENT through the environment and sets
# neither, so there is one place either default is written down.  They
# were written down twice and disagreed -- the Makefile said 8790 and
# this said 8801 -- so running the script by hand and running it through
# make asked two different servers, and one of them was nowhere.
#
# 8790 because that is clawtilla-web's own default port (clients/web/main.c).
#
BASE="${1:-${WEB_SMOKE_URL:-http://127.0.0.1:8790}}"
AGENT="${2:-${WEB_SMOKE_AGENT:-alpha}}"

failures=0
checks=0

usage () {
    cat <<'USAGE'
clawt-web-smoke.sh - ask a running clawtilla-web for every page

Usage:
  clawt-web-smoke.sh [BASE_URL] [AGENT_ID]

  BASE_URL   default http://127.0.0.1:8790 (or $WEB_SMOKE_URL)
  AGENT_ID   an agent that exists; default "alpha" (or $WEB_SMOKE_AGENT)

Exits non-zero if any page is missing, errors, or renders something other
than what that route is for.

Examples:
  clawt-web-smoke.sh
  clawt-web-smoke.sh http://127.0.0.1:8790 chief
USAGE
}

#
# GET a path and require a marker in the body.
#
expect_get () {
    if [[ $# -ne 3 ]]
    then
        # shellcheck disable=SC2016
        echo '`expect_get()` requires 3 positional arguments' >&2
        echo 'expect_get <path> <marker> <what-it-is>' >&2
        exit 1
    fi

    local path="${1}"
    local marker="${2}"
    local what="${3}"
    local body

    checks=$((checks + 1))
    body="$(curl -sS --max-time 20 "${BASE}${path}" 2>&1)"

    if grep -qF -- "${marker}" <<< "${body}"
    then
        printf '  ok    GET  %-40s %s\n' "${path}" "${what}"
        return 0
    fi

    printf '  FAIL  GET  %-40s expected %s\n' "${path}" "${what}"
    failures=$((failures + 1))
}

#
# POST a form and require a marker in what comes back.
#
expect_post () {
    if [[ $# -lt 3 ]]
    then
        # shellcheck disable=SC2016
        echo '`expect_post()` requires at least 3 positional arguments' >&2
        echo 'expect_post <path> <marker> <what-it-is> [curl-args...]' >&2
        exit 1
    fi

    local path="${1}"
    local marker="${2}"
    local what="${3}"
    shift 3
    local body

    checks=$((checks + 1))
    #
    # No -X POST. It would force POST on the redirect too, and a 303 from
    # an action is meant to be followed with GET -- with -X the follow-up
    # asks a GET-only route to accept a POST and comes back empty, which
    # reads as the action having produced nothing.
    #
    if [[ $# -eq 0 ]]
    then
        body="$(curl -sSL --max-time 30 --data '' "${BASE}${path}" 2>&1)"
    else
        body="$(curl -sSL --max-time 30 "$@" "${BASE}${path}" 2>&1)"
    fi

    if grep -qF -- "${marker}" <<< "${body}"
    then
        printf '  ok    POST %-40s %s\n' "${path}" "${what}"
        return 0
    fi

    printf '  FAIL  POST %-40s expected %s\n' "${path}" "${what}"
    failures=$((failures + 1))
}

expect_header () {
    if [[ $# -ne 3 ]]
    then
        # shellcheck disable=SC2016
        echo '`expect_header()` requires 3 positional arguments' >&2
        exit 1
    fi

    local path="${1}"
    local marker="${2}"
    local what="${3}"
    local headers

    checks=$((checks + 1))
    headers="$(curl -sS --max-time 20 -D - -o /dev/null "${BASE}${path}" 2>&1)"

    if grep -qiF -- "${marker}" <<< "${headers}"
    then
        printf '  ok    HEAD %-40s %s\n' "${path}" "${what}"
        return 0
    fi

    printf '  FAIL  HEAD %-40s expected %s\n' "${path}" "${what}"
    failures=$((failures + 1))
}

main () {
    if [[ $# -gt 0 ]] && [[ "${1}" == "-h" || "${1}" == "--help" ]]
    then
        usage
        exit 0
    fi

    echo "clawtilla-web smoke: ${BASE} (agent ${AGENT})"
    echo

    echo "The seven views:"
    expect_get "/a/${AGENT}/chat"     'class="composer'   'the composer'
    expect_get "/a/${AGENT}/agent"    'name="k:model'     'the schema-built editor'
    expect_get "/a/${AGENT}/mailbox"  'mailbox/purge'     'the queue'
    expect_get "/a/${AGENT}/computer" 'Computer'          'the computer panel'
    expect_get "/a/${AGENT}/routines" 'Routines'          'the routines'
    expect_get "/a/${AGENT}/tasks"    'Tasks'             'the task board'
    expect_get "/a/${AGENT}/flow"     'Flow'              'the flow view'
    echo

    echo "The settings pages:"
    expect_get '/settings/images'       'Fetch an image'   'VM images'
    expect_get '/settings/teams'        'New team'         'teams'
    expect_get '/settings/folders'      'Share a folder'   'shared folders'
    expect_get '/settings/spending'     'Fleet'            'spending'
    expect_get '/settings/integrations' 'Add one'          'integrations'
    expect_get '/settings/connectors'   'Add a connector'  'connectors'
    expect_get '/settings/appearance'   'Interface font'   'appearance, with fonts'
    expect_get '/settings/connections'  'Add a daemon'     'connections'
    echo

    echo "The rest of the surface:"
    expect_get '/fleet'                     'The fleet'      'the fleet table'
    expect_get '/new'                       'Design one'     'agent creation'
    expect_get '/import'                    'From a directory' 'import'
    expect_get "/a/${AGENT}/files"          'Workspace files' 'the file list'
    expect_get "/a/${AGENT}/file?name=SOUL.org" 'name="content"' 'the file editor'
    expect_get "/a/${AGENT}/memories"       'Memories'       'the memory store'
    expect_get "/a/${AGENT}/compose"        'name="body"'    'the compose box'
    expect_get "/a/${AGENT}/copy?format=org" 'Download instead' 'the copy view'
    expect_get '/f/sidebar'                 'sidebar'        'the sidebar fragment'
    expect_get "/f/a/${AGENT}/transcript"   'transcript'     'the transcript fragment'
    expect_get '/static/htmx.min.js'        'htmx'           'the vendored script'
    echo

    echo "Downloads and refusals:"
    expect_header "/a/${AGENT}/export?format=org" 'filename="'"${AGENT}"'.org"' \
        'an org download'
    expect_header "/a/${AGENT}/export?format=markdown" \
        'filename="'"${AGENT}"'.md"' 'a markdown download'
    expect_get "/a/${AGENT}/file?name=../../secrets" \
        'not a plain file name' 'a refused traversal'
    echo

    echo "Actions:"
    expect_post "/a/${AGENT}/send" 'class="composer' 'a message sent' \
        --data-urlencode 'body=smoke test'
    expect_post "/a/${AGENT}/send" 'list these commands' '/help' \
        --data-urlencode 'body=/help'
    expect_post "/a/${AGENT}/send" 'Cleared on screen' '/clear' \
        --data-urlencode 'body=/clear'
    expect_post "/a/${AGENT}/send" 'The fleet' '/agents' \
        --data-urlencode 'body=/agents'
    expect_post "/a/${AGENT}/send" 'name="content"' '/edit' \
        --data-urlencode 'body=/edit SOUL.org'
    expect_post "/a/${AGENT}/send" 'Download instead' '/copy' \
        --data-urlencode 'body=/copy org'
    expect_post "/a/${AGENT}/send" 'No such command' 'an unknown command' \
        --data-urlencode 'body=/nonsense'
    expect_post "/a/${AGENT}/mailbox/purge" 'Purged' 'purging expired items'
    #
    # Both halves of the shared-folder round trip.
    #
    # The remove was 404 for every folder there could ever be, because
    # the target is always an absolute path and an encoded slash does not
    # match a route parameter. Nothing about the page looked wrong; only
    # posting to it found out.
    #
    expect_post '/settings/folders/add' 'Shared with every agent' \
        'sharing a folder with the fleet' \
        --data-urlencode 'source=/tmp' --data-urlencode 'target=/work/smoke' \
        --data-urlencode 'mode=ro'
    expect_post '/settings/folders/remove' 'No longer shared' \
        'unsharing one' --data-urlencode 'target=/work/smoke'
    #
    # A value that differs every run. Saving only sends what changed, so
    # a fixed string passes once and then reports "nothing changed" -- a
    # test that goes green on the first run and amber on every one after
    # is worse than no test.
    #
    expect_post "/a/${AGENT}/set" 'Saved 1 setting' 'a saved setting' \
        --data-urlencode "k:description=set by smoke run $$"
    expect_post '/settings/teams/add' 'not a usable team id' \
        'a refusal with the reason' --data-urlencode 'id=' \
        --data-urlencode 'name=x'
    #
    # Idempotent on purpose, unlike the save above: taking an agent off a
    # team says the same thing however many times it is done, so the
    # smoke run does not depend on what the last one left behind -- and
    # it puts the agent back where a fresh fleet has it.
    #
    expect_post "/a/${AGENT}/team" 'taken off its team' \
        'moving an agent between teams' --data-urlencode 'team='
    echo

    if [[ "${failures}" -gt 0 ]]
    then
        echo "${failures} of ${checks} checks failed."
        exit 1
    fi

    echo "all ${checks} checks passed."
}

main "$@"
