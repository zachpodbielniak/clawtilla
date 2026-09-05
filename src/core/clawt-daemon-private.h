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
#include "memory/clawt-operator-profile.h"
#include "memory/clawt-summariser.h"
#include "memory/clawt-transcript-index.h"
#include "integration/clawt-notify.h"
#include "plugin/clawt-automation.h"
#include "task/clawt-handoff-store.h"
#include "task/clawt-routine-runner.h"
#include "integration/clawt-venture-bridge.h"
#include "trigger/clawt-trigger-store.h"
#include "trigger/clawt-webhook-ingress.h"
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

    /*
     * The fleet's searchable transcript, and the model of the person it
     * works for.
     *
     * The index is fed by the router, which is the only place that knows
     * which room a message landed in.  The profile is read when an
     * agent's workspace is scaffolded, so a new agent inherits what the
     * fleet already worked out instead of learning it again.
     */
    ClawtTranscriptIndex *transcripts;
    ClawtOperatorProfile *operator_profile;

    /*
     * Built on the first task that asks for a summary, not at start.
     *
     * Building it eagerly would construct an AI provider for every fleet
     * whether or not any agent has `memories.summarise` on, and the
     * provider is the part that costs money and reaches the network.
     */
    ClawtSummariser *summariser;

    ClawtExchange      *exchange;

    /*
     * The screen: frames while somebody is watching, and who is holding
     * the pointer.
     *
     * Two objects rather than one because they answer to different
     * things -- the observer is stopped when the last tab closes, the
     * lease outlives every client and lapses on its own clock -- and a
     * lease that went away with the last subscriber would hand the agent
     * back the pointer the moment somebody scrolled the page.
     */
    ClawtObserver      *observer;
    ClawtTakeover      *takeover;
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
    gboolean    registry_refreshing; /* an import is already in flight */

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

    /*
     * Turn hygiene: what stops a turn, and an exchange, going round for
     * ever.  See src/core/daemon-turn.c, which owns all of it.
     */
    ClawtRepeatWatch *repeats;      /* the same tool call, over and over */
    ClawtTurnWatch   *turn_watch;   /* agents.runtime.turn_timeout_seconds */
    ClawtTurnWatch   *room_watch;   /* rooms.turn_timeout_seconds */
    ClawtSteerQueue  *steers;       /* messages typed at a busy agent */
    GHashTable       *room_holder;  /* room id -> agent id holding its turn */

    /*
     * The steps of each room's running turn: room id -> GPtrArray of
     * ClawtTurnStep*.
     *
     * Kept only while the turn runs, and only so a client that opens a
     * room mid-turn is not left staring at a typing dot with no idea
     * what the agent has been doing.  Both clients rebuild a chat pane
     * when somebody switches rooms, so without this every switch throws
     * the running turn's history away.
     *
     * Never written to the transcript.  A step is not part of the
     * answer, and the last thing in a thread is what
     * clawt_task_manager_complete_on_turn_end() reads as a task's
     * result -- a persisted step would make "Ran 6 commands" the
     * reported outcome of a delegated task.
     *
     * Dropped by clawt_daemon_turn_settle_room(), which is the one
     * place a room's turn ends.
     */
    GHashTable       *room_steps;
    GHashTable       *turn_grace;   /* agent id -> GSource*, see arm_grace() */
    GSource          *turn_sweep;
    guint             turn_grace_seconds;

    /*
     * Ownership transfers waiting for the turn that asked for them, and
     * a receipt for every one that has finished.
     *
     * Durable because #ClawtTaskManager is not: after a restart these
     * receipts are the only answer clawtilla has to "what became of the
     * task I handed over", and an agent that reads silence as "it never
     * happened" hands the same work over twice.  See daemon-handoff.c.
     */
    ClawtHandoffStore *handoffs;
    gboolean           handoff_pumping;

    /* Standing work, and when it is next due. */
    ClawtRoutineRunner *routines;

    /*
     * Which agents are being held, and what to put back afterwards.
     *
     * Always built, never NULL: "there is no hold" is a state this
     * object holds rather than an absence a reader has to check for, and
     * it is what remembers the running set across the restart the hold
     * exists to serve.  See src/agent/clawt-hold.c.
     */
    ClawtHold *hold;

    /*
     * Whether a newer clawtilla exists.
     *
     * Built only when daemon.update_check is on, because it is the one
     * thing here that reaches the network without somebody asking it to.
     * NULL is the ordinary case and every reader has to expect it.
     */
    ClawtUpdateCheck *updates;

    /*
     * Work started by something happening elsewhere.
     *
     * The store is built whether or not the receiver is: it holds each
     * trigger's endpoint and its receipts, and `clawtilla trigger list`
     * has to be able to print an address before anybody turns the
     * listener on. The ingress is NULL unless daemon.webhook_enabled.
     */
    ClawtTriggerStore   *trigger_store;
    ClawtWebhookIngress *ingress;

    /*
     * VENTURE's staged writes, as decisions.
     *
     * Built whether or not a venture connector is configured, because
     * whether one *is* changes on a reload and the bridge is what asks
     * -- it simply arms no timer when nothing is bound, so a fleet that
     * has never heard of venture pays nothing for it.
     */
    ClawtVentureBridge  *venture;

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

    /*
     * Recordings, and the drafts written from them.
     *
     * Built on first use rather than at start: a fleet that never
     * teaches a task pays nothing, and the tables are the only state
     * either half needs. See src/core/daemon-teach.c.
     */
    GHashTable *teach_recorders;  /* id -> ClawtTeachRecorder */
    GHashTable *teach_drafts;     /* id -> ClawtSkillSynthesizer */

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

/*
 * `connector.registry_refresh`'s own deferred request.  The periodic
 * sweep import shares the same underlying refresh and completion
 * bookkeeping (clawt_daemon_registry_refresh_landed()) but has no
 * request to answer, so it is not built out of this struct at all.
 */
typedef struct {
    ClawtDaemon     *daemon;
    ClawtIpcPending *pending;
} RegistryRefreshJob;

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

void
clawt_daemon_registry_refresh_landed(ClawtDaemon *self);

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
clawt_daemon_on_registry_refresh_requested(GObject      *source,
                                           GAsyncResult *result,
                                           gpointer      user_data);

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

/*
 * The trigger receiver's own lifecycle.
 *
 * In daemon-trigger.c rather than clawt-daemon.c so that everything
 * about triggers -- the store, the listener, the delivery path and the
 * verbs -- is in one file, and clawt_daemon_start() gains two lines
 * rather than a subsystem.
 */
void clawt_daemon_triggers_start(ClawtDaemon *self);

/*
 * Arms the update check, when daemon.update_check says to.
 *
 * Nothing leaves the machine here: the timer is set and the first
 * request happens one interval later.  A check at start would make every
 * fixture that builds a daemon reach the network, and `make test` opens
 * no network socket at all.
 */
void clawt_daemon_updates_start(ClawtDaemon *self);

/*
 * Loads any recorded hold, puts the gate on the agents it names, and
 * says what is being resumed.
 *
 * Called from clawt_daemon_start() after the agents exist, because the
 * gate lives on each agent's runtime and there is nothing to set it on
 * before that.
 */
void clawt_daemon_hold_start(ClawtDaemon *self);

/*
 * Re-applies the gate to every agent the hold covers.
 *
 * A computer is derived from the config and so is a runtime: an agent
 * started after a hold landed builds a fresh one, which knows nothing
 * about it.  Called wherever an agent starts, so a fleet hold covers
 * agents that appear while it is on -- otherwise `agent start` under a
 * hold is a way to take work out of it.
 */
void clawt_daemon_hold_reapply(ClawtDaemon *self);

/*
 * How many of the held agents are still finishing a turn.
 *
 * Derived from clawt_agent_get_busy() rather than remembered, because a
 * remembered count is a thing that outlives what it describes -- and
 * this one decides when an operator is told it is safe to restart.
 */
guint clawt_daemon_hold_draining(ClawtDaemon *self);

/*
 * Writes the hold's `held`/`draining` view into an object.
 */
void clawt_daemon_hold_describe(ClawtDaemon *self, JsonBuilder *builder);

/*
 * Whether @agent_id was running when the fleet was held.
 *
 * Read by autostart_schedule(), which is the one place agents come up.
 * A hold's record wins over `runtime.autostart` for the agents it names,
 * because the two are different sets: after a restart, which agents came
 * back was decided by configuration rather than by what was running a
 * second earlier.
 */
gboolean clawt_daemon_hold_was_running(ClawtDaemon *self,
                                       const gchar *agent_id);

/*
 * Drops the remembered running set, once it has been queued.
 *
 * Left in place it would resurrect that set at the next unrelated start
 * -- a note about a moment outliving the moment.
 */
void clawt_daemon_hold_forget_running(ClawtDaemon *self);

/*
 * Whether this agent's runtime is gated by a hold.
 */
gboolean clawt_daemon_agent_held(ClawtAgent *agent);

/*
 * control.pause and control.resume.  See src/core/daemon-hold.c.
 */
JsonNode *
clawt_daemon_handle_hold(ClawtDaemon  *self,
                         const gchar  *kind,
                         JsonNode     *request,
                         JsonObject   *payload,
                         gboolean     *handled);
void clawt_daemon_triggers_stop(ClawtDaemon *self);

/*
 * The VENTURE bridge's own lifecycle, in daemon-venture.c for the same
 * reason: which connectors are bound, where their tokens are and the
 * one outbound HTTP request the daemon makes on its own context are all
 * one subject.
 */
void clawt_daemon_venture_start(ClawtDaemon *self);
void clawt_daemon_venture_stop(ClawtDaemon *self);
void clawt_daemon_venture_sync(ClawtDaemon *self);

gboolean
clawt_daemon_venture_answer(ClawtDaemon *self, ClawtDecision *decision);

JsonNode *
clawt_daemon_handle_trigger(
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
clawt_daemon_handle_memory(
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

/*
 * The observer's and the lease's news, turned into events.
 *
 * In daemon-screen.c beside the verbs they belong with rather than in
 * clawt-daemon.c, which is already the file this split exists to stop
 * growing.
 */
void
clawt_daemon_on_observer_frame(ClawtObserver *observer,
                               const gchar   *agent_id,
                               const gchar   *path,
                               gpointer       user_data);

void
clawt_daemon_on_observer_failed(ClawtObserver *observer,
                                const gchar   *agent_id,
                                const gchar   *message,
                                gpointer       user_data);

void
clawt_daemon_on_takeover_changed(ClawtTakeover *takeover,
                                 const gchar   *agent_id,
                                 gpointer       user_data);

JsonNode *
clawt_daemon_handle_screen(
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
clawt_daemon_handle_teach(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

/*
 * An agent's step, as it happens.
 *
 * Both are fed from points the daemon already has -- the MCP tool
 * observer and the desktop control gate -- rather than from hooks of
 * their own. A second place that sees a tool call is a second place to
 * forget one, which is the shape behind a good deal of this file.
 *
 * Both are no-ops unless that agent is being recorded right now.
 */
void clawt_daemon_teach_note_tool_call(ClawtDaemon *self,
                                       const gchar *agent_id,
                                       const gchar *tool,
                                       const gchar *args);

void clawt_daemon_teach_note_desktop(ClawtDaemon *self,
                                     const gchar *agent_id,
                                     const gchar *tool);

/*
 * Stops every running recording, synchronously, on the way down.
 *
 * Synchronously because the async form would post a worker and a
 * completion onto a context that is about to stop being iterated -- and
 * for a demonstration that means the compositor goes on recording after
 * clawtilla has gone, with its indicator still on the screen and nobody
 * holding the token that would end it.
 */
void clawt_daemon_teach_teardown(ClawtDaemon *self);

JsonNode *
clawt_daemon_handle_config(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
);

/* ── Turn hygiene (src/core/daemon-turn.c) ───────────────────────── */

/*
 * Built once when the daemon starts, and torn down when it stops.
 * Separate from clawt_daemon_turn_configure() because the objects
 * outlive a reload and the budgets do not.
 */
void clawt_daemon_turn_setup(ClawtDaemon *self);
void clawt_daemon_turn_configure(ClawtDaemon *self);
void clawt_daemon_turn_teardown(ClawtDaemon *self);

/*
 * A turn started, produced something, or settled.
 *
 * Called from the link's typing and message handlers and from the
 * interrupt verb.  Settling is idempotent, which is what lets the
 * interrupt and the runtime's own end-of-turn frame both call it.
 */
void clawt_daemon_turn_begin(ClawtDaemon *self, const gchar *agent_id,
                             const gchar *room_id);
void clawt_daemon_turn_activity(ClawtDaemon *self, const gchar *agent_id);
void clawt_daemon_turn_settle(ClawtDaemon *self, const gchar *agent_id);

/*
 * The room half of a turn beginning and ending.
 *
 * Separate from the agent half because they fire on different edges: an
 * agent becomes busy once, when it was idle, while a room's turn starts
 * every time that room's turn starts -- and an agent talking to three
 * peers is mid-turn in three rooms at once.  Driven from the agent half
 * alone, the second and later rooms were never registered at all, so
 * `rooms.turn_timeout_seconds` could only reach whichever room an idle
 * agent happened to enter first.
 */
void clawt_daemon_turn_begin_room(ClawtDaemon *self, const gchar *agent_id,
                                  const gchar *room_id);
ClawtRoom *clawt_daemon_create_room(ClawtDaemon  *self,
                                    const gchar  *room_id,
                                    const gchar  *name,
                                    const gchar  *members,
                                    GError      **error);

void clawt_daemon_turn_sweep_now(ClawtDaemon *self);

void clawt_daemon_turn_settle_room(ClawtDaemon *self, const gchar *agent_id,
                                   const gchar *room_id);

/**
 * clawt_daemon_note_step:
 * @self: the daemon
 * @step: one step of a running turn
 *
 * Redacts @step, keeps it for its room while the turn runs, and
 * publishes it as a `turn.step` event.
 *
 * Deliberately not a delivery.  See daemon-step.c.
 */
void clawt_daemon_note_step(ClawtDaemon *self, ClawtTurnStep *step);

/**
 * clawt_daemon_room_steps:
 * @self: the daemon
 * @room_id: (nullable): a room
 *
 * Returns: (transfer container) (element-type ClawtTurnStep): what the
 *   room's running turn has done so far, oldest first.  Empty when no
 *   turn is running, which is not an error.
 */
GPtrArray *clawt_daemon_room_steps(ClawtDaemon *self, const gchar *room_id);

/*
 * A turn is waiting on a person, and is waiting no longer.
 *
 * Both budgets hold: stopping a turn under an unanswered question
 * manufactures a stranded decision, which the daemon then has to repair.
 */
void clawt_daemon_turn_hold(ClawtDaemon *self, const gchar *agent_id);
void clawt_daemon_turn_release(ClawtDaemon *self, const gchar *agent_id);

/*
 * Waits for a turn that has been stopped to report that it ended, and
 * releases the agent itself when it never does.
 *
 * Armed by every path that stops a turn.  A stop that only signals is
 * not a stop: without this the agent stays marked busy for ever, the
 * next delivery overlaps a turn nobody is running, and the watch never
 * begins again.
 */
void clawt_daemon_turn_arm_grace(ClawtDaemon *self, const gchar *agent_id);

/*
 * How long that wait is.  0 restores the default.
 *
 * Settable for tests.  The default is fifteen seconds, and a test that
 * waits fifteen seconds for a timer is a test people start skipping --
 * which for the one mechanism that catches a stop that did not stop is
 * the worst thing that could happen to it.
 */
void clawt_daemon_turn_set_grace_seconds(ClawtDaemon *self, guint seconds);

/* -- Handing work over (src/core/daemon-handoff.c) -------------- */

/*
 * Opens the handoff store, wires the tool's hook to it, and runs
 * anything that was queued when the daemon last stopped.
 *
 * Called after clawt_daemon_turn_setup(), because the queue drains from
 * clawt_daemon_turn_settle() and the objects that settle a turn have to
 * exist before one can.
 */
void clawt_daemon_handoff_setup(ClawtDaemon *self);
void clawt_daemon_handoff_teardown(ClawtDaemon *self);

/*
 * Runs every queued handoff whose source agent is no longer mid-turn.
 *
 * Called from clawt_daemon_turn_settle() for *every* agent rather than
 * only the one whose queue it is: a handoff waiting on a busy recipient
 * is retried when that recipient's own turn ends, and that settle
 * belongs to a different agent.
 */
void clawt_daemon_handoff_pump(ClawtDaemon *self);

/*
 * Ends every handoff an agent had queued, with a receipt each.
 *
 * Called from the interrupt path, before the turn settles.  A turn
 * somebody stopped did not finish deciding, and carrying out the
 * handoffs it had queued would be acting on half a decision -- the
 * opposite of what pressing stop means.
 */
void clawt_daemon_handoff_drop_queued(ClawtDaemon *self,
                                      const gchar *agent_id,
                                      const gchar *why);

/*
 * One tool call the daemon served, for the repeat counter.  Both MCP
 * dispatch paths reach it -- the synchronous chain and the deferred
 * computer_exec -- because a rule applied at one of two call sites is a
 * rule about that call site.
 */
void clawt_daemon_turn_note_tool_call(ClawtDaemon *self,
                                      const gchar *agent_id,
                                      const gchar *tool,
                                      const gchar *args);

/*
 * Whether a message to @target should be held rather than routed.
 *
 * Returns %TRUE when it was queued as a steer, in which case nothing has
 * been sent and nothing is in the transcript yet.
 */
gboolean clawt_daemon_turn_steer(ClawtDaemon *self,
                                 const gchar *from,
                                 const gchar *target,
                                 const gchar *body);

G_END_DECLS
