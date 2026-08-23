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
#include "computer/clawt-exchange.h"
#include "computer/clawt-image-catalog.h"
#include "computer/clawt-exec-result.h"
#include "computer/clawt-sandbox.h"
#include "computer/clawt-computer.h"
#include "computer/clawt-null-computer.h"
#include "computer/clawt-host-computer.h"
#include "computer/clawt-pod-bridge.h"
#include "computer/clawt-container-computer.h"
#include "computer/clawt-cloud-init.h"
#include "computer/clawt-vm-image.h"
#include "computer/clawt-vm-computer.h"
#include "computer/clawt-desktop.h"
#include "computer/clawt-guest-desktop.h"
#include "computer/clawt-desktop-relay.h"
#include "computer/clawt-computer-factory.h"

#include "chat/clawt-message.h"
#include "chat/clawt-markdown.h"
#include "chat/clawt-export.h"
#include "chat/clawt-loop-guard.h"
#include "chat/clawt-room.h"
#include "chat/clawt-room-manager.h"

#include "mailbox/clawt-mailbox-router.h"

#include "task/clawt-task.h"
#include "task/clawt-task-manager.h"

#include "plugin/clawt-param-info.h"
#include "plugin/clawt-plugin.h"

#include "interfaces/clawt-event-handler.h"
#include "interfaces/clawt-tool-provider.h"
#include "interfaces/clawt-computer-provider.h"
#include "interfaces/clawt-integration-provider.h"

#include "plugin/clawt-plugin-manager.h"
#include "mcp/clawt-mcp-tools.h"
#include "ai/clawt-model-catalog.h"
#include "ai/clawt-agent-designer.h"
#include "core/clawt-event.h"
#include "core/clawt-event-bus.h"
#include "core/clawt-event-log.h"

#include "config/clawt-config.h"
#include "config/clawt-config-render.h"
#include "integration/clawt-integration.h"

#include "link/clawt-link.h"
#include "link/clawt-link-server.h"

#include "ipc/clawt-ipc-proto.h"
#include "ipc/clawt-ipc-server.h"
#include "ipc/clawt-client.h"

#include "core/clawt-daemon.h"

#include "mailbox/clawt-mailbox-item.h"
#include "memory/clawt-memory.h"
#include "memory/clawt-memory-store.h"
#include "mailbox/clawt-mailbox.h"

#include "agent/clawt-agent-runtime.h"
#include "agent/clawt-process-runtime.h"
#include "agent/clawt-embedded-runtime.h"
#include "agent/clawt-agent.h"
#include "agent/clawt-agent-manager.h"
#include "agent/clawt-workspace.h"

#undef CLAWT_INSIDE
