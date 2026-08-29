/*
 * clawt-daemon-private.h - The daemon's own state, and its dispatch
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "core/clawt-daemon.h"
#include "computer/clawt-computer.h"
#include "computer/clawt-vm-image.h"
#include "decision/clawt-decision-store.h"
#include "integration/clawt-notify.h"
#include "plugin/clawt-automation.h"
#include "task/clawt-routine-runner.h"
#include "usage/clawt-usage.h"

G_BEGIN_DECLS

/*
 * Private to src/core/, and deliberately absent from <clawtilla.h> and
 * from PUBLIC_HEADERS: nothing here is API.
 *
 * The client surface used to be one chain of ~100 frame kinds inside
 * clawt_daemon_handle_request(), which meant every branch that added a
 * verb edited the same few hundred lines of the same file and every one
 * of them conflicted with the others.  It is one file per family now,
 * and those files need the daemon's own struct, the jobs a deferred
 * reply is carried in, and the helpers that build a reply.
 */

struct _ClawtDaemon {
    GObject parent_instance;

    /*
     * Set by clawt_daemon_set_bind_addresses(), which replaces whatever
     * the config says about network listeners.  The flag is separate from
     * the list because "bind nothing" and "bind whatever is configured"
     * are different answers and an empty list has to mean the first.
     */
    gboolean   bind_override;
    GPtrArray *bind_specs;   /* BindSpec*, owned */

    gchar        *config_path;
    GMainContext *main_context;
    GMainLoop    *loop;

    ClawtConfig        *config;
    ClawtAgentManager  *agents;
    ClawtRoomManager   *rooms;
    ClawtTaskManager   *tasks;
    ClawtMailboxRouter *router;
    ClawtLoopGuard     *guard;
    ClawtUsage         *usage;
    ClawtEventBus      *bus;
    ClawtEventLog      *log;

    /*
     * Choices agents need a person to make.
     *
     * Beside the alerts rather than inside them: an alert is something
     * that happened and a decision is something that needs you, so one
     * badge meaning both would be a badge nobody could act on.  Durable
     * for the same reason the mailbox is -- an agent that asked and got
     * no answer carried on with its default, and an operator who never
     * saw the question has no way to know that happened.
     */
    ClawtDecisionStore *decisions;
    ClawtExchange      *exchange;
    ClawtLinkServer    *link_server;
    ClawtIpcServer     *ipc_server;
    ClawtMcpTools      *mcp_tools;
    ClawtPodBridge     *pod_bridge;
    ClawtPluginManager *plugins;
    ClawtVmImageStore  *vm_images;

    /*
     * The connector catalogue, and the authorizations in progress.
     *
     * The catalogue is cached because it is read on paths a person is
     * waiting on; it is dropped on reload so that editing a file in
     * connectors.dir takes effect without a restart.
     */
    GPtrArray  *connector_catalog;
    GHashTable *connector_flows;   /* flow id -> ConnectorFlow */
    GSource    *connector_refresh;

    /*
     * The fleet coming up, one agent per idle turn.
     *
     * Autostart used to run inline in clawt_daemon_start(), which is
     * called before the main loop exists -- so for the whole time the
     * fleet took to provision, the daemon dispatched nothing.  No IPC
     * frame was answered, no signal source ran, and on ~30 container
     * agents that was minutes of a process that could not be talked to
     * and could not be asked to stop; systemd eventually escalated to
     * SIGABRT and the agents were SIGKILLed rather than stopped.
     */
    GSource    *autostart_source;
    GPtrArray  *autostart_queue;   /* gchar*, agent ids, in order */
    guint       autostart_next;

    /*
     * Designs waiting to be reviewed.
     *
     * design.agent used to run the model, show a preview, and then --
     * when the person said yes -- run the model *again* with commit set.
     * The second run is a fresh conversation, so what was created was not
     * what was reviewed, which is the one thing the preview exists to
     * guarantee. The designer is kept here between the two steps instead.
     */
    GHashTable *drafts;          /* draft id -> ClawtAgentDesigner */

    /*
     * What each provider says it runs, cached.
     *
     * model.list used to ask the providers while the client waited, and
     * both the new-agent dialog and the agent inspector ask on every
     * build -- so pressing + or clicking an agent stalled for as long as
     * the slowest provider took. Warmed in the background instead, and
     * every request answers from here at once.
     */
    GHashTable *model_cache;     /* provider id -> GStrv (owned) */
    gint64      model_cache_at;  /* monotonic, when it was last warmed */

    /*
     * Who to tell when something is worth interrupting somebody for.
     *
     * Rebuilt on every reload, because that is when its credentials are
     * resolved -- see ClawtNotifier.
     */
    ClawtNotifier *notifier;

    /* Standing work, and when it is next due. */
    ClawtRoutineRunner *routines;

    /* Pods that watch the fleet and act on it. */
    ClawtAutomation *automation;

    /*
     * The fleet's skills, scanned once and watched.
     *
     * One library for the whole daemon rather than one per agent: a
     * skill lives in exactly one directory and is linked into whichever
     * workspaces need it, so a second scan would be a second answer
     * about the same files.  %NULL when `skills.enabled` is off, which
     * every caller has to treat as "no skills" rather than as an error
     * -- the feature is opt-out and a fleet that never wanted it should
     * pay nothing for it.
     */
    ClawtSkillLibrary *skills;

    gchar   *libreclaw_binary;
    gchar   *state_dir;

    /*
     * An exclusive flock on <state_dir>/daemon.lock, held for the life of
     * the daemon.  See acquire_state_lock().
     */
    gint     state_lock_fd;
    gchar   *link_socket;

    /*
     * Where a file an agent sent its operator is kept, so `attachment.get`
     * can serve the bytes to a client that may be on another machine.
     */
    gchar   *attachment_dir;
    guint    sweep_source_id;
    gboolean running;
};

/*
 * How many designs may sit unreviewed at once.  Small on purpose: a
 * draft is a step in a conversation somebody is having right now, not
 * something to accumulate.
 */
#define MAX_PENDING_DRAFTS (8)

/*
 * How long a cached model list is trusted.
 *
 * Providers add models in weeks, not minutes, so this only has to be
 * short enough that a daemon left running for days notices.
 */
#define MODEL_CACHE_TTL_SECONDS (6 * 60 * 60)

/*
 * A flow in progress.
 *
 * Authorising takes as long as a person takes, which is far longer than
 * an IPC request may block -- so `connector.begin` answers as soon as
 * there is something to show them, and `connector.await` is the deferred
 * one that finishes when they have done it.  Splitting it in two is what
 * lets a client display the code the instant it exists rather than after
 * the whole thing has completed, which would be no use to anybody.
 */
typedef struct {
    ClawtDaemon     *daemon;      /* not owned; the daemon outlives a flow */
    gchar           *id;
    gchar           *name;
    gchar           *token_url;
    gchar           *client_id;
    gchar           *client_secret;
    gchar           *verifier;
    gchar           *redirect_uri;
    ClawtIpcPending *waiter;
    gboolean         settled;
    gboolean         ok;
    gchar           *message;
    gint64           settled_at;
} ConnectorFlow;

/*
 * The client waiting to be shown a user code.
 *
 * Separate from the flow because it is answered once, the moment the
 * provider hands over the codes -- long before the flow itself settles.
 */
typedef struct {
    ConnectorFlow   *flow;
    ClawtIpcPending *pending;
} BeginWait;

typedef struct {
    gchar           *name;
    ClawtIpcPending *pending;
} RevokeJob;

/*
 * A renewal, which may or may not have somebody waiting on it: the timer
 * starts these with no client attached, and `connector.refresh` starts
 * one with a deferred request to answer.
 */
typedef struct {
    ClawtDaemon     *daemon;
    gchar           *name;
    ClawtIpcPending *pending;
} RefreshJob;

typedef struct {
    gchar    *name;
    gchar    *type_id;
    gboolean  ok;
    gchar    *message;
} HealthResult;

typedef struct {
    ClawtIpcPending *pending;
    GPtrArray       *checks;    /* ClawtIntegrationBinding* */
    GPtrArray       *results;   /* HealthResult* */
    guint            timeout;
    guint            next;
} HealthRun;

typedef struct {
    ClawtDaemon     *daemon;      /* unowned; it outlives the request */
    ClawtIpcPending *pending;
    gchar           *name;
    gchar           *agent_id;
    gchar           *homeserver;
} MatrixLogin;

/*
 * One operator-run command, waiting for its own answer.
 *
 * `computer.exec` used to run on the daemon's main context and answer
 * from there, so a command from the CLI blocked every other agent's
 * messages, task delivery and timers for as long as it took -- up to the
 * advertised 120 second default.  The tool path had already been moved
 * off the loop; this is the same defect reached from the caller most
 * likely to run something long on purpose.
 */
typedef struct {
    ClawtDaemon     *daemon;     /* reffed: the trail is written at the end */
    ClawtIpcPending *pending;
    ClawtComputer   *computer;   /* reffed: the agent may stop meanwhile */
    gchar           *agent_id;
    gchar           *command;
} ExecJob;

typedef struct {
    ClawtDaemon            *daemon;
    ClawtIpcPending        *pending;
    ClawtComputer          *computer;   /* reffed: the agent may stop */
    gchar                  *agent_id;
    ClawtComputerLifecycle  op;
    gboolean                removes;
} LifecycleJob;

/* ── Helpers the family files build their replies with ───────────── */

void
clawt_daemon_add_agent_object(JsonBuilder *builder, ClawtAgent *agent);

void
clawt_daemon_add_agent_settings(JsonBuilder *builder, ClawtAgent *agent);

void
clawt_daemon_add_binding_object(JsonBuilder *builder,
                                ClawtIntegrationBinding *binding);

void
clawt_daemon_add_decision_object(JsonBuilder   *builder,
                                 ClawtDecision *decision,
                                 gint64         now);

void
clawt_daemon_add_integration_object(JsonBuilder            *builder,
                                    ClawtConfig            *config,
                                    ClawtIntegrationConfig *instance,
                                    const gchar            *agent_id);

void
clawt_daemon_add_key_array(JsonBuilder *builder, const gchar *member,
                           const gchar *const *keys);

void
clawt_daemon_add_mailbox_item(JsonBuilder *builder, ClawtMailboxItem *item);

void
clawt_daemon_add_render_refusals(JsonBuilder *builder, GPtrArray *refusals);

void
clawt_daemon_add_string_array(JsonBuilder *builder, const gchar *name,
                              const gchar * const *items);

void
clawt_daemon_add_string_member(JsonBuilder *builder, const gchar *name,
                               const gchar *value);

void
clawt_daemon_add_task_object(JsonBuilder *builder, ClawtTask *task);

const ClawtSchemaEntry *
clawt_daemon_agent_setting_entry(const gchar *key);

gboolean
clawt_daemon_apply_integration_fields(ClawtIntegrationConfig  *instance,
                                      JsonObject              *payload,
                                      GError                 **error);

gboolean
clawt_daemon_authenticate_agent(const gchar *agent_id, const gchar *token,
                                gpointer user_data);

gboolean
clawt_daemon_purge_agent_files(ClawtDaemon      *self,
                               ClawtAgentConfig *config,
                               gboolean         *out_was_linked,
                               GError          **error);

gboolean
clawt_daemon_quit_idle(gpointer user_data);

gint
clawt_daemon_compare_by_order(gconstpointer a, gconstpointer b,
                              gpointer user_data);

gboolean
clawt_daemon_computer_stop_removes(ClawtAgentConfig *config);

ClawtIntegrationBinding *
clawt_daemon_connector_binding(ClawtDaemon               *self,
                               const gchar               *name,
                               const ClawtConnectorInfo **out_info,
                               GError                   **error);

gchar *
clawt_daemon_connector_client_secret(ClawtDaemon *self,
                                     ClawtIntegrationBinding *binding);

GPtrArray *
clawt_daemon_catalog(ClawtDaemon *self);

ClawtAgentConfig *
clawt_daemon_create_agent(ClawtDaemon  *self,
                          const gchar  *agent_id,
                          GHashTable   *fields,
                          const gchar  *purpose,
                          gboolean     *purpose_landed,
                          GError      **error);

gboolean
clawt_daemon_reload_internal(ClawtDaemon *self, GPtrArray *refusals,
                             GError **error);

void
clawt_daemon_deliver_decision_answer(ClawtDaemon *self,
                                     ClawtDecision *decision);

void
clawt_daemon_exec_job_free(ExecJob *job);

gboolean
clawt_daemon_forget_connector_token(ClawtDaemon *self, const gchar *name,
                                    GError **error);

void
clawt_daemon_health_result_free(HealthResult *self);

void
clawt_daemon_health_run_free(HealthRun *run);

void
clawt_daemon_health_run_start(HealthRun *run);

void
clawt_daemon_lifecycle_job_free(LifecycleJob *job);

ClawtMailbox *
clawt_daemon_mailbox_for(ClawtDaemon *self, JsonObject *payload,
                         GError **error);

void
clawt_daemon_matrix_login_free(MatrixLogin *self);

ClawtMount *
clawt_daemon_mount_from_payload(ClawtConfig  *config,
                                JsonObject   *payload,
                                const gchar  *target,
                                GError      **error);

void
clawt_daemon_on_connector_begun(GObject *source, GAsyncResult *result,
                                gpointer user_data);

void
clawt_daemon_on_connector_redirected(GObject *source, GAsyncResult *result,
                                     gpointer user_data);

void
clawt_daemon_on_connector_refreshed(GObject *source, GAsyncResult *result,
                                    gpointer user_data);

void
clawt_daemon_on_connector_revoked(GObject *source, GAsyncResult *result,
                                  gpointer user_data);

void
clawt_daemon_on_ipc_exec_finished(GObject *source, GAsyncResult *result,
                                  gpointer user_data);

void
clawt_daemon_on_ipc_lifecycle_finished(GObject *source, GAsyncResult *result,
                                       gpointer user_data);

void
clawt_daemon_on_matrix_login(GObject *source, GAsyncResult *result,
                             gpointer user_data);

void
clawt_daemon_on_matrix_rooms(GObject *source, GAsyncResult *result,
                             gpointer user_data);

void
clawt_daemon_on_notify_tested(GObject *source, GAsyncResult *result,
                              gpointer user_data);

void
clawt_daemon_on_tool_rpc_finished(GObject *source, GAsyncResult *result,
                                  gpointer user_data);

gboolean
clawt_daemon_prepare_state_git(const gchar *state_dir, gboolean init_repo,
                               gboolean *created, gchar **ignore_path,
                               GError **error);

void
clawt_daemon_refresh_job_free(RefreshJob *job);

void
clawt_daemon_reload_skills(ClawtDaemon *self);

void
clawt_daemon_render_all_agents_into(ClawtDaemon *self, GPtrArray *refusals);

GPtrArray *
clawt_daemon_render_refusals_new(void);

gboolean
clawt_daemon_setting_needs_a_new_session(const gchar *key);

gboolean
clawt_daemon_store_connector_token(ClawtDaemon      *self,
                                   const gchar      *name,
                                   ClawtOauthToken  *token,
                                   GError          **error);

void
clawt_daemon_sweep_connector_flows(ClawtDaemon *self);

void
clawt_daemon_warm_model_cache(ClawtDaemon *self);

/*
 * One verb family's share of the client surface.
 *
 * @handled is separate from the return value because a deferred handler
 * answers later and returns %NULL now -- clawt_ipc_server_defer() has
 * already claimed the right to reply, and reading that %NULL as "not
 * mine" would hand the frame to the next family and then to the
 * unknown-kind error, so the client would be answered twice.
 *
 * A family that does not recognise @kind leaves @handled alone.
 */
typedef JsonNode *(*ClawtDaemonFamilyFunc)(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_control(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_misc(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_agent(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_mount(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_image(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_team(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_room(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_mailbox(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_task(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_computer(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_design(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_connector(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_integration(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_routine(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_skill(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

JsonNode *
clawt_daemon_handle_config(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

G_END_DECLS
