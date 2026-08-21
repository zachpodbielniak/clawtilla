/*
 * clawt-error.c - clawtilla error domain
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawt-error.h"

G_DEFINE_QUARK(clawt-error-quark, clawt_error)

/*
 * The wire needs a stable name for each code.  Deriving one from the enum
 * value at the call site would mean every producer inventing its own
 * spelling, so the mapping lives here once.  A code with no entry falls
 * through to "failed" rather than returning NULL -- a caller formatting an
 * error is already on an unhappy path and should not have to null-check.
 */
const gchar *
clawt_error_code_to_string(ClawtError code)
{
    switch (code) {
    case CLAWT_ERROR_FAILED:             return "failed";
    case CLAWT_ERROR_INVALID_ARGUMENT:   return "invalid-argument";
    case CLAWT_ERROR_NOT_FOUND:          return "not-found";
    case CLAWT_ERROR_ALREADY_EXISTS:     return "already-exists";
    case CLAWT_ERROR_NOT_SUPPORTED:      return "not-supported";
    case CLAWT_ERROR_PERMISSION_DENIED:  return "permission-denied";
    case CLAWT_ERROR_TIMEOUT:            return "timeout";
    case CLAWT_ERROR_CANCELLED:          return "cancelled";
    case CLAWT_ERROR_CONFIG_PARSE:       return "config-parse";
    case CLAWT_ERROR_CONFIG_INVALID:     return "config-invalid";
    case CLAWT_ERROR_CONFIG_WRITE:       return "config-write";
    case CLAWT_ERROR_SECRET:             return "secret";
    case CLAWT_ERROR_AGENT_STATE:        return "agent-state";
    case CLAWT_ERROR_RUNTIME_SPAWN:      return "runtime-spawn";
    case CLAWT_ERROR_COMPUTER_PROVISION: return "computer-provision";
    case CLAWT_ERROR_COMPUTER_EXEC:      return "computer-exec";
    case CLAWT_ERROR_CONFINEMENT:        return "confinement";
    case CLAWT_ERROR_MOUNT:              return "mount";
    case CLAWT_ERROR_MAILBOX_FULL:       return "mailbox-full";
    case CLAWT_ERROR_MAILBOX_BUSY:       return "mailbox-busy";
    case CLAWT_ERROR_MAILBOX_STATE:      return "mailbox-state";
    case CLAWT_ERROR_PROTOCOL:           return "protocol";
    case CLAWT_ERROR_AUTH:               return "auth";
    case CLAWT_ERROR_NOT_CONNECTED:      return "not-connected";
    case CLAWT_ERROR_LOOP_LIMIT:         return "loop-limit";
    case CLAWT_ERROR_PLUGIN_LOAD:        return "plugin-load";
    case CLAWT_ERROR_PLUGIN_ABI:         return "plugin-abi";
    case CLAWT_ERROR_AI:                 return "ai";
    default:                             return "failed";
    }
}
