/*
 * clawt-broker.h - The integration broker clawtilla runs in podman
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * With `broker.enabled` on, clawtilla runs a gateway of ready-made app
 * integrations -- AnythingMCP, or Jentic One -- in rootless podman and
 * owns its whole lifecycle: the containers, their state under
 * `<state_dir>/broker/`, a restart policy, and a state every client can
 * show.  Each agent or team names its own gateway server
 * (`agents.broker_server`, `teams.broker_server`) and reaches it as an
 * ordinary connector, so every call goes through the relay, the tool
 * list and the action rules like any other connector's.
 *
 * Two halves.  The functions are pure and hermetic: what the containers
 * are (image, binds, ports, environment) is data built from the config,
 * and the tests assert it as data.  #ClawtBroker is the running half,
 * built on the same container machinery agents' computers use; it never
 * waits on podman or on the gateway from the caller's stack.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "clawt-types.h"
#include "clawt-enums.h"
#include "config/clawt-config.h"
#include "computer/clawt-pod-bridge.h"

G_BEGIN_DECLS

/**
 * CLAWT_BROKER_ANYTHINGMCP_IMAGE:
 *
 * The AnythingMCP image this release was run against.  amd64 only: an
 * aarch64 machine builds its own and names it in `broker.image`.
 */
#define CLAWT_BROKER_ANYTHINGMCP_IMAGE "docker.io/helpcodeai/anythingmcp:v0.54.0"

/**
 * CLAWT_BROKER_JENTIC_IMAGE:
 *
 * The Jentic One image this release was run against (amd64 and arm64).
 */
#define CLAWT_BROKER_JENTIC_IMAGE "ghcr.io/jentic/jentic-one-app:0.44.0"

/**
 * CLAWT_BROKER_DATABASE_IMAGE:
 *
 * The PostgreSQL beside an AnythingMCP gateway.
 */
#define CLAWT_BROKER_DATABASE_IMAGE "docker.io/library/postgres:17.12-alpine"

/* ── What each broker is ─────────────────────────────────────────── */

/**
 * clawt_broker_type_label:
 * @type: a #ClawtBrokerType
 *
 * The broker's own name, as a refusal or a client row says it.
 *
 * Returns: (transfer none): "AnythingMCP" or "Jentic One"
 */
const gchar *clawt_broker_type_label(ClawtBrokerType type);

/**
 * clawt_broker_type_default_image:
 * @type: a #ClawtBrokerType
 *
 * Returns: (transfer none): the pinned image an empty `broker.image`
 *   means for @type
 */
const gchar *clawt_broker_type_default_image(ClawtBrokerType type);

/**
 * clawt_broker_type_for_provider:
 * @provider: (nullable): a connector's `provider`
 * @type: (out) (optional): the broker it names
 *
 * Whether a connector's provider is one of the brokers' catalogue
 * entries, which is how the relay knows a gateway is answering rather
 * than the service itself.
 *
 * Returns: %TRUE for `anythingmcp` and `jentic`
 */
gboolean clawt_broker_type_for_provider(const gchar     *provider,
                                        ClawtBrokerType *type);

/**
 * clawt_broker_state_label:
 * @state: a #ClawtBrokerState
 *
 * The word every client shows on the Broker row, so the three cannot
 * disagree about what "starting" is called.
 *
 * Returns: (transfer none): a capitalised word or two
 */
const gchar *clawt_broker_state_label(ClawtBrokerState state);

/**
 * clawt_broker_check_image:
 * @image: (nullable): an image reference
 * @error: (out) (optional): why it is refused
 *
 * Refuses an image with no tag or tagged `latest`.  An upgrade of the
 * thing holding every SaaS credential should be a change somebody made,
 * not something a pull did on a Tuesday.  A digest is accepted.
 *
 * Returns: %TRUE if @image is pinned
 */
gboolean clawt_broker_check_image(const gchar  *image,
                                  GError      **error);

/**
 * clawt_broker_get_image:
 * @config: a #ClawtConfig
 *
 * Returns: (transfer full): `broker.image`, or the pinned default for
 *   `broker.type` when it is empty
 */
gchar *clawt_broker_get_image(ClawtConfig *config);

/**
 * clawt_broker_get_type_value:
 * @config: a #ClawtConfig
 *
 * Returns: `broker.type`
 */
ClawtBrokerType clawt_broker_get_type_value(ClawtConfig *config);

/* ── Where it is ─────────────────────────────────────────────────── */

/**
 * clawt_broker_dir:
 * @state_dir: the daemon's state directory
 *
 * Returns: (transfer full): `<state_dir>/broker`, where the gateway's
 *   database, encryption key and per-agent keys live
 */
gchar *clawt_broker_dir(const gchar *state_dir);

/**
 * clawt_broker_dial_host:
 * @bind: (nullable): `broker.bind`
 *
 * What to connect to, for an address something listens on: the
 * wildcard addresses are dialled on loopback, an IPv6 address is
 * bracketed for a URL.
 *
 * Returns: (transfer full): a host for a URL
 */
gchar *clawt_broker_dial_host(const gchar *bind);

/**
 * clawt_broker_mcp_url:
 * @config: a #ClawtConfig
 * @server: (nullable): a gateway server id, or %NULL for the endpoint
 *   itself
 *
 * The gateway's MCP endpoint as the relay dials it: `/mcp/<server>` for
 * AnythingMCP, `/mcp` for Jentic One, where the key alone says which
 * agent is calling.
 *
 * Returns: (transfer full): a URL
 */
gchar *clawt_broker_mcp_url(ClawtConfig *config,
                            const gchar *server);

/**
 * clawt_broker_dashboard_url:
 * @config: a #ClawtConfig
 *
 * Where the person signs in to SaaS apps, makes gateway servers and
 * keys: AnythingMCP's dashboard port, or Jentic One's one port.
 *
 * Returns: (transfer full): a URL
 */
gchar *clawt_broker_dashboard_url(ClawtConfig *config);

/**
 * clawt_broker_health_url:
 * @config: a #ClawtConfig
 *
 * Returns: (transfer full): what the watch asks to learn that the
 *   gateway answers
 */
gchar *clawt_broker_health_url(ClawtConfig *config);

/* ── Who reaches it ──────────────────────────────────────────────── */

/**
 * clawt_broker_target_is_valid:
 * @target: (nullable): an agent id, or `team:<id>`
 *
 * Returns: %TRUE if @target can name a key file and a connector
 */
gboolean clawt_broker_target_is_valid(const gchar *target);

/**
 * clawt_broker_connector_name:
 * @target: an agent id, or `team:<id>`
 *
 * The connector a target's agents reach the gateway through:
 * `broker-agent-<id>` or `broker-team-<id>`.  One name per gateway
 * server, because the server's connector assignment is the permission.
 *
 * Returns: (transfer full): the connector's name
 */
gchar *clawt_broker_connector_name(const gchar *target);

/**
 * clawt_broker_target_for_connector:
 * @name: (nullable): a connector's name
 *
 * The inverse of clawt_broker_connector_name(), so a refusal can say
 * which `clawtilla broker key` fixes it.
 *
 * Returns: (transfer full) (nullable): the target, or %NULL when @name
 *   is not a broker connector
 */
gchar *clawt_broker_target_for_connector(const gchar *name);

/**
 * clawt_broker_key_path:
 * @state_dir: the daemon's state directory
 * @target: an agent id, or `team:<id>`
 *
 * Where the key for @target's gateway server is kept, 0600, in the
 * shape every connector's token file has.
 *
 * Returns: (transfer full): `<state_dir>/broker/keys/<agent-or-team>.json`
 */
gchar *clawt_broker_key_path(const gchar *state_dir,
                             const gchar *target);

/**
 * clawt_broker_agent_refusal:
 * @config: a #ClawtConfig
 * @agent: an agent with a gateway server
 *
 * Why an agent that names a gateway server is not given it, or %NULL
 * when it is.  Two reasons: a gated or closed network cannot reach the
 * gateway on this machine's loopback, and Jentic One is refused to an
 * agent without `runtime.confine: bwrap`, because its own documentation
 * says an agent running as the same OS user can read its credential
 * store.  The connector is withheld rather than offered and broken, and
 * clawt_broker_check_confinement() refuses the broker itself.
 *
 * Returns: (transfer full) (nullable): the reason
 */
gchar *clawt_broker_agent_refusal(ClawtConfig      *config,
                                  ClawtAgentConfig *agent);

/**
 * ClawtBrokerAssignment:
 * @target: an agent id, or `team:<id>`
 * @server: the gateway server it names
 * @connector: the connector its agents reach it through
 * @key_file: where its key is kept
 * @agents: (array zero-terminated=1): the agents given it
 *
 * One gateway server and the agents that reach it.
 */
typedef struct {
    gchar  *target;
    gchar  *server;
    gchar  *connector;
    gchar  *key_file;
    GStrv   agents;
} ClawtBrokerAssignment;

/**
 * clawt_broker_assignment_free: (skip)
 * @self: (nullable): a #ClawtBrokerAssignment
 */
void clawt_broker_assignment_free(ClawtBrokerAssignment *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtBrokerAssignment,
                              clawt_broker_assignment_free)

/**
 * clawt_broker_assignments: (skip)
 * @config: a #ClawtConfig
 * @warnings: (nullable) (element-type utf8): where to add a sentence per
 *   agent that names a server and is not given it
 *
 * Every gateway server the fleet names and who reaches it.  An agent's
 * own `broker_server` wins over its team's; an agent refused by
 * clawt_broker_agent_refusal() is left out, with a warning.  Empty when
 * `broker.enabled` is off.
 *
 * Returns: (transfer full) (element-type ClawtBrokerAssignment): the
 *   assignments, agents first and then teams, in file order
 */
GPtrArray *clawt_broker_assignments(ClawtConfig *config,
                                    GPtrArray   *warnings);

/**
 * clawt_broker_assignment_for_agent: (skip)
 * @config: a #ClawtConfig
 * @agent_id: an agent
 *
 * The assignment clawt_broker_assignments() makes for @agent_id.  The
 * bwrap box no longer asks this: it binds the key as the token_file of
 * the gateway connector the config loader made from the same
 * assignments, through clawt_agent_confine_connector_paths(), the list
 * every connector credential goes through.
 *
 * Returns: (transfer full) (nullable): @agent_id's assignment, or %NULL
 */
ClawtBrokerAssignment *clawt_broker_assignment_for_agent(ClawtConfig *config,
                                                         const gchar *agent_id);

/**
 * clawt_broker_check_confinement:
 * @config: a #ClawtConfig
 * @error: (out) (optional): names every agent that stops it
 *
 * Jentic One runs only when every agent that names a gateway server has
 * `runtime.confine: bwrap`.  AnythingMCP always passes.
 *
 * Returns: %TRUE if the broker may run
 */
gboolean clawt_broker_check_confinement(ClawtConfig  *config,
                                        GError      **error);

/**
 * clawt_broker_connector_nodes: (skip)
 * @config: a #ClawtConfig whose agents are loaded
 * @warnings: (nullable) (element-type utf8): where a refused agent is said
 *
 * The broker's connectors as integration entries, for the config loader:
 * one `type: connector` mapping per assignment, its provider the broker
 * type, its URL the gateway server's endpoint, its `token_file` the
 * target's key, scoped to exactly the agents given it.
 *
 * Returns: (transfer full) (nullable): the YamlNode mappings, or %NULL
 *   when the broker is off
 */
GPtrArray *clawt_broker_connector_nodes(ClawtConfig *config,
                                        GPtrArray   *warnings);

/**
 * clawt_broker_check_setting:
 * @config: a #ClawtConfig
 * @agent_id: (nullable): the agent being changed, or %NULL for a team
 * @team_id: (nullable): the team being changed, when @agent_id is %NULL
 * @key: the agent- or team-relative key being set
 * @value: (nullable): its new value
 * @error: (out) (optional): why it would be refused later
 *
 * Refuses, where it arrives, a value this daemon can already prove the
 * broker will refuse at its next start: under Jentic One, a gateway
 * server for an unconfined agent (its own, or its team's reaching an
 * unconfined member), or `runtime.confine` taken off an agent that has
 * one.  Everything else passes.
 *
 * Returns: %TRUE if the value may be saved
 */
gboolean clawt_broker_check_setting(ClawtConfig  *config,
                                    const gchar  *agent_id,
                                    const gchar  *team_id,
                                    const gchar  *key,
                                    const gchar  *value,
                                    GError      **error);

/* ── Its state on disk, and its containers ───────────────────────── */

/**
 * clawt_broker_prepare_state:
 * @type: which broker
 * @state_dir: the daemon's state directory
 * @error: (out) (optional): return location for a #GError
 *
 * Makes `<state_dir>/broker/` (0700) and whatever @type keeps there: the
 * database directory and socket directory for AnythingMCP and its
 * PostgreSQL, the data directory for Jentic One, and the secrets each
 * needs -- generated from /dev/urandom the first time, 0600, and never
 * replaced, since a new encryption key would make every stored SaaS
 * credential unreadable.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_broker_prepare_state(ClawtBrokerType   type,
                                    const gchar      *state_dir,
                                    GError          **error);

/**
 * ClawtBrokerContainerSpec:
 * @name: the container's name
 * @image: its image
 * @command: (nullable): its command, as a JSON array
 * @env: (element-type utf8 utf8): its environment
 * @ports: (nullable): its published ports, as a libpod JSON array
 * @mounts: (element-type ClawtMount): its bind mounts
 *
 * One of the broker's containers, as data.  @env holds secrets read from
 * `<state_dir>/broker/`: it goes to podman's API and nowhere else.
 */
typedef struct {
    gchar      *name;
    gchar      *image;
    gchar      *command;
    GHashTable *env;
    gchar      *ports;
    GPtrArray  *mounts;
} ClawtBrokerContainerSpec;

/**
 * clawt_broker_container_spec_free: (skip)
 * @self: (nullable): a #ClawtBrokerContainerSpec
 */
void clawt_broker_container_spec_free(ClawtBrokerContainerSpec *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtBrokerContainerSpec,
                              clawt_broker_container_spec_free)

/**
 * clawt_broker_container_spec_env_json: (skip)
 * @self: a #ClawtBrokerContainerSpec
 *
 * Returns: (transfer full): @self's environment as the JSON object
 *   podman's create takes, names sorted
 */
gchar *clawt_broker_container_spec_env_json(ClawtBrokerContainerSpec *self);

/**
 * clawt_broker_build_spec: (skip)
 * @config: a #ClawtConfig
 * @state_dir: the daemon's state directory, prepared by
 *   clawt_broker_prepare_state()
 * @error: (out) (optional): return location for a #GError
 *
 * The broker's containers, in the order they start: AnythingMCP's
 * database and then the gateway, or Jentic One's one container (its
 * control plane and its broker as two processes, because the broker
 * must be reached on the control plane's own loopback).  Refuses an
 * unpinned image and a port outside 1-65535.
 *
 * Returns: (transfer full) (element-type ClawtBrokerContainerSpec): the
 *   containers, or %NULL
 */
GPtrArray *clawt_broker_build_spec(ClawtConfig  *config,
                                   const gchar  *state_dir,
                                   GError      **error);

/* ── What the relay says ─────────────────────────────────────────── */

/**
 * clawt_broker_refusal_reply:
 * @line: the JSON-RPC request the gateway refused or never answered
 * @gateway: the broker's name, from clawt_broker_type_label()
 * @connector: the connector's name
 * @status: the HTTP status, or 0 when the gateway did not answer at all
 * @detail: (nullable): the gateway's own one-line reason, or the
 *   transport error
 *
 * The reply the relay writes for a request the gateway refused, naming
 * the gateway -- "AnythingMCP refused it" reads differently from
 * clawtilla refusing it, and the two have different fixes.  A tool call
 * gets a failed tool result, which is what a model reads; anything else
 * a JSON-RPC error.  A 401 says which `clawtilla broker key` stores the
 * missing key.
 *
 * Returns: (transfer full) (nullable): the reply line, or %NULL for a
 *   notification, which has nobody to answer
 */
gchar *clawt_broker_refusal_reply(const gchar *line,
                                  const gchar *gateway,
                                  const gchar *connector,
                                  guint        status,
                                  const gchar *detail);

/**
 * clawt_broker_reason_from_body:
 * @body: (nullable): what a gateway sent with a refusal
 *
 * The gateway's own one-line reason, if it gave one in a shape that can
 * be read: a JSON-RPC error's message, a `detail` (Jentic One) or a
 * `message` (AnythingMCP's API), as JSON or as the first SSE event.  Never
 * the raw body, which the agent would read as instructions.
 *
 * Returns: (transfer full) (nullable): the reason, bounded, or %NULL
 */
gchar *clawt_broker_reason_from_body(const gchar *body);

/**
 * clawt_broker_annotate_reply:
 * @line: one JSON-RPC message from the gateway
 * @gateway: the broker's name
 *
 * Names the gateway in an error it sent: a JSON-RPC error's message and
 * a failed tool result's first text both gain "<gateway> answered: ".
 * A successful reply is left alone.
 *
 * Returns: (transfer full) (nullable): the rewritten line, or %NULL when
 *   @line needs nothing
 */
gchar *clawt_broker_annotate_reply(const gchar *line,
                                   const gchar *gateway);

/* ── The running broker ──────────────────────────────────────────── */

#define CLAWT_TYPE_BROKER (clawt_broker_get_type())
G_DECLARE_FINAL_TYPE(ClawtBroker, clawt_broker, CLAWT, BROKER, GObject)

/**
 * clawt_broker_new:
 * @bridge: the podomation bridge computers use
 * @context: (nullable): the context to answer on; the thread-default
 *   when %NULL
 *
 * Returns: (transfer full): a stopped broker
 */
ClawtBroker *clawt_broker_new(ClawtPodBridge *bridge,
                              GMainContext   *context);

/**
 * clawt_broker_set_config:
 * @self: a #ClawtBroker
 * @config: the fleet's config
 * @state_dir: the daemon's state directory
 *
 * What the next start builds from.  A running broker keeps the
 * containers it has until it is restarted; a broker switched off is
 * %CLAWT_BROKER_STATE_DISABLED once stopped.
 */
void clawt_broker_set_config(ClawtBroker *self,
                             ClawtConfig *config,
                             const gchar *state_dir);

/**
 * clawt_broker_start:
 * @self: a #ClawtBroker
 *
 * Starts the broker in the background and returns at once, in
 * %CLAWT_BROKER_STATE_STARTING: the containers are made again from the
 * config (what they hold is under `<state_dir>/broker/`, so nothing is
 * lost), pulled if they are missing, and started in order; the state
 * becomes running when the gateway answers its health check.  Nothing
 * when it is already starting or running.
 */
void clawt_broker_start(ClawtBroker *self);

/**
 * clawt_broker_stop:
 * @self: a #ClawtBroker
 *
 * Stops the containers in the background, leaving them and everything
 * under `<state_dir>/broker/` in place.
 */
void clawt_broker_stop(ClawtBroker *self);

/**
 * clawt_broker_restart:
 * @self: a #ClawtBroker
 *
 * A stop and then a start, in the background.
 */
void clawt_broker_restart(ClawtBroker *self);

/**
 * clawt_broker_stop_sync:
 * @self: a #ClawtBroker
 *
 * Stops the containers and waits.  For daemon shutdown only, where there
 * is no loop left to answer on.
 */
void clawt_broker_stop_sync(ClawtBroker *self);

/**
 * clawt_broker_get_state:
 * @self: a #ClawtBroker
 *
 * Returns: where the broker is
 */
ClawtBrokerState clawt_broker_get_state(ClawtBroker *self);

/**
 * clawt_broker_get_detail:
 * @self: a #ClawtBroker
 *
 * Returns: (transfer none) (nullable): what it is doing, or why it
 *   failed
 */
const gchar *clawt_broker_get_detail(ClawtBroker *self);

/**
 * clawt_broker_get_restarts:
 * @self: a #ClawtBroker
 *
 * Returns: how many times the restart policy has started it again
 */
guint clawt_broker_get_restarts(ClawtBroker *self);

/**
 * clawt_broker_build_status:
 * @self: (nullable): the running broker, or %NULL before there is one
 * @config: the fleet's config
 * @builder: a #JsonBuilder inside an object
 *
 * The members of `broker.status`: whether it is on, which broker, its
 * state and the word for it, the detail, the image, the dashboard and
 * MCP URLs, and every assignment with whether its key is stored.  Never
 * a secret: the environment the containers were given is not read here
 * at all.
 */
void clawt_broker_build_status(ClawtBroker *self,
                               ClawtConfig *config,
                               JsonBuilder *builder);

/**
 * clawt_broker_logs_async:
 * @self: a #ClawtBroker
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the logs
 * @user_data: data for @callback
 *
 * The last hundred lines of each container, on a worker thread.
 */
void clawt_broker_logs_async(ClawtBroker         *self,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data);

/**
 * clawt_broker_logs_finish:
 * @self: a #ClawtBroker
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): each container's lines under its
 *   name, with anything shaped like a credential redacted
 */
gchar *clawt_broker_logs_finish(ClawtBroker   *self,
                                GAsyncResult  *result,
                                GError       **error);

/**
 * clawt_broker_read_logs:
 * @bridge: the #ClawtPodBridge whose `container` module reads them
 * @connection: (nullable): the podman connection, as `broker.connection`
 * @names: (array zero-terminated=1): the containers, in order
 * @tail: (nullable): lines from the end of each: a count or `all`;
 *   %NULL for a hundred
 * @error: (out) (optional): why the container module could not be loaded
 *
 * What clawt_broker_logs_async() runs on its worker, blocking: each
 * container's log under a `== name ==` heading.  A container whose
 * standard output and standard error both have lines gets each under a
 * `-- stdout --` / `-- stderr --` subheading, because podman keeps them
 * as two streams and the order between them is not kept; one with a
 * single stream's lines gets them bare.  A container podman cannot read
 * says why in its section and the rest are still read.
 *
 * Returns: (transfer full) (nullable): the text, with anything shaped
 *   like a credential redacted, or %NULL when the module cannot be loaded
 */
gchar *clawt_broker_read_logs(ClawtPodBridge      *bridge,
                              const gchar         *connection,
                              const gchar * const *names,
                              const gchar         *tail,
                              GError             **error);

/**
 * clawt_broker_container_is_up:
 * @status: (nullable): a podman container state, as the `container`
 *   module's inspect gives it in `status`
 *
 * Whether a container in @status still counts as up for the watch:
 * `running`, and the transient `paused` and `stopping` (the gateway's
 * health check decides about those).  `exited`, `stopped`, `created`,
 * `configured`, `initialized`, `dead` and `removing` are down.
 *
 * Returns: %TRUE when @status is up
 */
gboolean clawt_broker_container_is_up(const gchar *status);

/**
 * clawt_broker_find_stopped:
 * @bridge: the #ClawtPodBridge whose `container` module asks
 * @connection: (nullable): the podman connection, as `broker.connection`
 * @ids: (array zero-terminated=1): each container's podman id, in order;
 *   an empty one is a container that was never made
 * @names: (array zero-terminated=1): each container's name, in the same
 *   order as @ids
 * @stopped: (out) (transfer full) (nullable): the first container that
 *   is down, gone or was never made, or %NULL when every one is up
 * @error: (out) (optional): why podman's answer could not be had
 *
 * What the watch asks podman, blocking: each container's inspect state,
 * through clawt_broker_container_is_up().  An inspect that fails is told
 * apart by asking for the container list: when podman answers that, the
 * container is gone; when it does not, podman did not answer, which is
 * not the gateway stopping.  An empty state -- podomation before 1.0.2,
 * or podman's own `unknown` -- is "did not say" too.
 *
 * Returns: %FALSE when podman did not answer, with @error set
 */
gboolean clawt_broker_find_stopped(ClawtPodBridge      *bridge,
                                   const gchar         *connection,
                                   const gchar * const *ids,
                                   const gchar * const *names,
                                   gchar              **stopped,
                                   GError             **error);

G_END_DECLS
