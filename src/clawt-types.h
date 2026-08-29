/*
 * clawt-types.h - Forward type declarations for clawtilla
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every public type is forward-declared here so headers can reference one
 * another without an include cycle.  The umbrella header includes this
 * first and then the real headers in dependency order.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/* Core */
typedef struct _ClawtDaemon         ClawtDaemon;
typedef struct _ClawtEventBus       ClawtEventBus;
typedef struct _ClawtEventLog       ClawtEventLog;
typedef struct _ClawtEvent          ClawtEvent;

/* Agents */
typedef struct _ClawtAgent          ClawtAgent;
typedef struct _ClawtAgentManager   ClawtAgentManager;
typedef struct _ClawtAgentSpec      ClawtAgentSpec;
typedef struct _ClawtAgentRuntime   ClawtAgentRuntime;
typedef struct _ClawtProcessRuntime ClawtProcessRuntime;
typedef struct _ClawtEmbeddedRuntime ClawtEmbeddedRuntime;

/* Mailbox */
typedef struct _ClawtMailbox        ClawtMailbox;
typedef struct _ClawtMailboxItem    ClawtMailboxItem;
typedef struct _ClawtMailboxStore   ClawtMailboxStore;
typedef struct _ClawtMailboxRouter  ClawtMailboxRouter;
typedef struct _ClawtMailboxFilter  ClawtMailboxFilter;

/* Tasks */
typedef struct _ClawtTask           ClawtTask;
typedef struct _ClawtTaskManager    ClawtTaskManager;

/* Computers */
typedef struct _ClawtComputer       ClawtComputer;
typedef struct _ClawtExecResult     ClawtExecResult;
typedef struct _ClawtMount          ClawtMount;
typedef struct _ClawtMountPlan      ClawtMountPlan;
typedef struct _ClawtExchange       ClawtExchange;
typedef struct _ClawtSandbox        ClawtSandbox;
typedef struct _ClawtPodBridge      ClawtPodBridge;
typedef struct _ClawtDesktop        ClawtDesktop;
typedef struct _ClawtGuestDesktop   ClawtGuestDesktop;

/* Link and IPC */
typedef struct _ClawtLink           ClawtLink;
typedef struct _ClawtLinkServer     ClawtLinkServer;
typedef struct _ClawtIpcServer      ClawtIpcServer;
typedef struct _ClawtClient         ClawtClient;

/* Chat */
typedef struct _ClawtMessage        ClawtMessage;
typedef struct _ClawtRoom           ClawtRoom;
typedef struct _ClawtRoomManager    ClawtRoomManager;
typedef struct _ClawtTranscript     ClawtTranscript;

/* Config */
typedef struct _ClawtConfig         ClawtConfig;
typedef struct _ClawtAgentConfig    ClawtAgentConfig;
typedef struct _ClawtComputerConfig ClawtComputerConfig;
typedef struct _ClawtIntegrationConfig ClawtIntegrationConfig;
typedef struct _ClawtRoutine        ClawtRoutine;
typedef struct _ClawtTrigger        ClawtTrigger;
typedef struct _ClawtSecretRef      ClawtSecretRef;

/* Triggers */
typedef struct _ClawtTriggerEvent   ClawtTriggerEvent;
typedef struct _ClawtTriggerStore   ClawtTriggerStore;
typedef struct _ClawtWebhookIngress ClawtWebhookIngress;

/* Plugins and interfaces */
typedef struct _ClawtPlugin         ClawtPlugin;
typedef struct _ClawtPluginManager  ClawtPluginManager;
typedef struct _ClawtParamInfo      ClawtParamInfo;

/* Integrations */
typedef struct _ClawtIntegration    ClawtIntegration;

/* AI */
typedef struct _ClawtAgentDesigner  ClawtAgentDesigner;

G_END_DECLS
