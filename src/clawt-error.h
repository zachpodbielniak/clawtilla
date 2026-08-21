/*
 * clawt-error.h - clawtilla error domain
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One GError domain for the whole library, mirroring libreclaw's LC_ERROR.
 * Codes are grouped by subsystem but share a single enum so a caller can
 * switch on them without knowing which layer produced the failure.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_ERROR:
 *
 * Error domain for clawtilla.  Errors in this domain are from the
 * #ClawtError enumeration.
 */
#define CLAWT_ERROR (clawt_error_quark())

/**
 * ClawtError:
 * @CLAWT_ERROR_FAILED: generic failure with no better code
 * @CLAWT_ERROR_INVALID_ARGUMENT: a caller passed something unusable
 * @CLAWT_ERROR_NOT_FOUND: the named agent, room, task or item does not exist
 * @CLAWT_ERROR_ALREADY_EXISTS: the id is taken
 * @CLAWT_ERROR_NOT_SUPPORTED: the backend cannot do this
 * @CLAWT_ERROR_PERMISSION_DENIED: refused by policy, not by the OS
 * @CLAWT_ERROR_TIMEOUT: an operation exceeded its deadline
 * @CLAWT_ERROR_CANCELLED: cancelled via a #GCancellable
 * @CLAWT_ERROR_CONFIG_PARSE: the YAML could not be parsed
 * @CLAWT_ERROR_CONFIG_INVALID: the YAML parsed but says something impossible
 * @CLAWT_ERROR_CONFIG_WRITE: the config could not be written back
 * @CLAWT_ERROR_SECRET: a secret reference could not be resolved
 * @CLAWT_ERROR_AGENT_STATE: the agent is in the wrong state for this
 * @CLAWT_ERROR_RUNTIME_SPAWN: the agent process could not be started
 * @CLAWT_ERROR_COMPUTER_PROVISION: the container or VM could not be created
 * @CLAWT_ERROR_COMPUTER_EXEC: the command could not be run
 * @CLAWT_ERROR_CONFINEMENT: the path or command is outside what the agent may touch
 * @CLAWT_ERROR_MOUNT: a mount specification is invalid or could not be applied
 * @CLAWT_ERROR_MAILBOX_FULL: the mailbox is at max_depth and the policy is reject
 * @CLAWT_ERROR_MAILBOX_BUSY: the mailbox is full but the policy says to try again
 * @CLAWT_ERROR_MAILBOX_STATE: the item is not in a state this operation allows
 * @CLAWT_ERROR_PROTOCOL: a malformed or unsupported frame
 * @CLAWT_ERROR_AUTH: authentication or authorisation failed
 * @CLAWT_ERROR_NOT_CONNECTED: the link or client is not connected
 * @CLAWT_ERROR_LOOP_LIMIT: hop count, rate limit or budget exhausted
 * @CLAWT_ERROR_PLUGIN_LOAD: a plugin could not be loaded
 * @CLAWT_ERROR_PLUGIN_ABI: a plugin was built against a different ABI
 * @CLAWT_ERROR_AI: the AI provider failed or returned something unusable
 *
 * Error codes for the %CLAWT_ERROR domain.
 */
typedef enum {
    CLAWT_ERROR_FAILED = 0,
    CLAWT_ERROR_INVALID_ARGUMENT,
    CLAWT_ERROR_NOT_FOUND,
    CLAWT_ERROR_ALREADY_EXISTS,
    CLAWT_ERROR_NOT_SUPPORTED,
    CLAWT_ERROR_PERMISSION_DENIED,
    CLAWT_ERROR_TIMEOUT,
    CLAWT_ERROR_CANCELLED,

    CLAWT_ERROR_CONFIG_PARSE,
    CLAWT_ERROR_CONFIG_INVALID,
    CLAWT_ERROR_CONFIG_WRITE,
    CLAWT_ERROR_SECRET,

    CLAWT_ERROR_AGENT_STATE,
    CLAWT_ERROR_RUNTIME_SPAWN,

    CLAWT_ERROR_COMPUTER_PROVISION,
    CLAWT_ERROR_COMPUTER_EXEC,
    CLAWT_ERROR_CONFINEMENT,
    CLAWT_ERROR_MOUNT,

    CLAWT_ERROR_MAILBOX_FULL,
    CLAWT_ERROR_MAILBOX_STATE,

    CLAWT_ERROR_PROTOCOL,
    CLAWT_ERROR_AUTH,
    CLAWT_ERROR_NOT_CONNECTED,
    CLAWT_ERROR_LOOP_LIMIT,

    CLAWT_ERROR_PLUGIN_LOAD,
    CLAWT_ERROR_PLUGIN_ABI,

    CLAWT_ERROR_AI,

    /*
     * New codes are appended here, never inserted.  Inserting one
     * renumbers every code after it, so anything already compiled --
     * a plugin, a test binary, another build of the library -- reports a
     * different failure than the one that happened.
     */
    CLAWT_ERROR_MAILBOX_BUSY
} ClawtError;

/**
 * clawt_error_quark:
 *
 * Gets the clawtilla error domain quark.
 *
 * Returns: the #GQuark for the %CLAWT_ERROR domain
 */
GQuark clawt_error_quark(void);

/**
 * clawt_error_code_to_string:
 * @code: a #ClawtError
 *
 * Maps an error code to a short stable identifier, suitable for putting on
 * the wire or into a log line where the human-readable message would be
 * too long or too variable.
 *
 * Returns: (transfer none): a static string, never %NULL
 */
const gchar *clawt_error_code_to_string(ClawtError code);

G_END_DECLS
