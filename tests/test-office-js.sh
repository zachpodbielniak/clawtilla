#!/bin/sh
# test-office-js.sh - data/web/office.js draws the usage board, headless
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# The usage board is screenshot-tested through clawtilla-office only;
# the web client's own script was never run by any test.  This runs
# tests/office-js-harness.js under node with a stub document, window,
# fetch and canvas 2D context, feeds office.js a scene carrying a board
# in the shape clawt_office_board_to_json() makes, and asserts that the
# script evaluates and runs its frames without throwing and that the
# canvas text API is called with each window's label.
#
# Without node it exits 77, prints "SKIP: node not found", and
# `make test` counts it as skipped rather than passed or failed.
set -eu

CDPATH='' cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1
then
	echo "SKIP: node not found"
	exit 77
fi

exec node tests/office-js-harness.js data/web/office.js
