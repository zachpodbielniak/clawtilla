/*
 * clawtilla.h - clawtilla umbrella header
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Include this header to access all clawtilla public API.  Individual
 * headers refuse to be included directly; they check for CLAWT_INSIDE,
 * which only this file defines.
 *
 * Includes are ordered by dependency, not alphabetically -- moving one
 * earlier than the type it needs will break the build in a way that looks
 * like a missing type rather than a misordered include.
 */

#pragma once

#define CLAWT_INSIDE

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

/* libreclaw defines LC_INSIDE itself and undefines nothing we rely on. */
#include <libreclaw.h>

#include "clawt-version.h"
#include "clawt-types.h"
#include "clawt-enums.h"
#include "clawt-error.h"
#include "clawt-util.h"

#include "config/clawt-config-schema.h"
#include "config/clawt-secret-ref.h"
#include "computer/clawt-mount.h"
#include "config/clawt-config.h"

#include "link/clawt-link.h"
#include "link/clawt-link-server.h"

#undef CLAWT_INSIDE
