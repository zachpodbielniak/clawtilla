/*
 * clawt-connection.h - Saved ways of reaching a daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A client that can reach a daemon on another machine needs somewhere to
 * remember which machine, on what port, with which token -- otherwise
 * every connection is an address typed in again, and a bearer token typed
 * in again after it.
 *
 * These live in the *client's* config, not in clawtilla.yaml, and that is
 * the whole reason this is a separate file rather than a schema section.
 * The point of a profile is to reach a daemon that is somewhere else; a
 * laptop connecting to a workstation may have no fleet of its own, no
 * state directory and no clawtilla.yaml at all.  Reading the daemon's
 * config to find out how to reach a different daemon is backwards.
 *
 * The file holds bearer tokens, so it is written 0600 and its directory
 * is created 0700.  There is no way to write a secret into clawtilla.yaml
 * on purpose; here a token *is* the thing being remembered, and pushing
 * it into a second file the person also has to manage would mean most
 * people keeping it in their shell history instead.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "ipc/clawt-client.h"

G_BEGIN_DECLS

/**
 * CLAWT_DEFAULT_TCP_PORT:
 *
 * The port a daemon listens on when it listens on the network at all.
 *
 * Named here as well as in the config schema because the client has to
 * offer it as a default without reading the daemon's config -- which is
 * on the machine it is trying to reach.
 */
#define CLAWT_DEFAULT_TCP_PORT (8792)

#define CLAWT_TYPE_CONNECTION (clawt_connection_get_type())

typedef struct _ClawtConnection ClawtConnection;

GType clawt_connection_get_type(void) G_GNUC_CONST;

/**
 * ClawtReachability:
 * @CLAWT_REACH_UNKNOWN: not asked yet
 * @CLAWT_REACH_REACHABLE: the daemon answered
 * @CLAWT_REACH_REFUSED: the daemon answered and would not have us
 * @CLAWT_REACH_UNREACHABLE: nothing answered
 *
 * What a saved connection is, before anybody switches to it.
 *
 * Refused and unreachable are deliberately separate, and it is the whole
 * point of this type. A rotated token and a sleeping host produce the
 * same silence from a client that does not tell them apart, and sending
 * somebody to check the network when the answer is a credential -- or
 * the reverse -- costs far more than the probe does.
 *
 * Unknown is not a failure. It is the honest state of a connection
 * nobody has asked about yet, and drawing it as "unreachable" would be a
 * client asserting something it has not established.
 */
typedef enum {
    CLAWT_REACH_UNKNOWN = 0,
    CLAWT_REACH_REACHABLE,
    CLAWT_REACH_REFUSED,
    CLAWT_REACH_UNREACHABLE
} ClawtReachability;

/**
 * clawt_reachability_from_error:
 * @error: (nullable): what a probe failed with, or %NULL if it did not
 *
 * Which of the four a probe's outcome was.
 *
 * An authentication refusal comes back as %CLAWT_ERROR_AUTH from the
 * hello the client sends inside connect; everything else -- a refused
 * socket, a timeout, a route that is gone -- is the network.
 * %G_IO_ERROR_CONNECTION_REFUSED is a *network* refusal and is read as
 * unreachable, however much the two are spelled the same in English.
 *
 * Returns: the verdict
 */
ClawtReachability clawt_reachability_from_error(const GError *error);

/**
 * clawt_reachability_word:
 * @reach: a verdict
 *
 * One word for it, in both clients.
 *
 * Here rather than in each client because the distinction only helps if
 * the two of them draw it the same way, and a word chosen twice is a
 * word that will eventually be chosen differently.
 *
 * Returns: (transfer none): the word
 */
const gchar *clawt_reachability_word(ClawtReachability reach);

/**
 * ClawtConnectionStatus:
 * @reach: what the probe found
 * @version: (nullable): the daemon's version, when it answered
 * @agents: how many agents it has, when it answered
 * @detail: (nullable): why, when it did not
 *
 * What one probe learned. Freed with clawt_connection_status_free().
 */
typedef struct {
    ClawtReachability  reach;
    gchar             *version;
    guint              agents;
    gchar             *detail;
} ClawtConnectionStatus;

/**
 * clawt_connection_status_free: (skip)
 * @self: (nullable) (transfer full): what a probe returned
 *
 * Releases it.
 */
void clawt_connection_status_free(ClawtConnectionStatus *self);

/**
 * clawt_connection_probe: (skip)
 * @self: a saved connection
 *
 * Asks a saved connection whether it is up, without switching to it.
 *
 * `control.status` already answers everything a client wants to show for
 * a remote -- version, agent count -- and needs no new frame kind. Until
 * now the only way to find out whether a saved connection was alive was
 * to switch to it and fail, which is a destructive way to ask a
 * read-only question: switching tears down and rebuilds the whole window
 * state, so "is that machine up?" could not be asked without committing
 * to the answer.
 *
 * This connects, asks, and hangs up. It blocks for as long as the far
 * end takes, so a caller with a main loop to keep running must not call
 * it on that loop -- a remote host that is asleep takes as long to fail
 * as its route takes to time out.
 *
 * Safe to call from any thread: it runs on a #GMainContext of its own,
 * so it never acquires, iterates or dispatches the caller's. Without
 * that the client it builds settled on whatever context was
 * thread-default when it connected -- on a fresh worker thread, none,
 * which means the *global* default -- and then pumped the application's
 * own loop from the wrong thread.
 *
 * Returns: (transfer full): what it found; never %NULL
 */
ClawtConnectionStatus *clawt_connection_probe(ClawtConnection *self);

/**
 * ClawtVersionVerdict:
 * @CLAWT_VERSION_SAME: the daemon is this build
 * @CLAWT_VERSION_DAEMON_NEWER: the daemon is ahead of this client
 * @CLAWT_VERSION_DAEMON_OLDER: the daemon is behind this client
 * @CLAWT_VERSION_UNKNOWN: it did not say, or said something unreadable
 *
 * How a daemon's version stands against the client talking to it.
 *
 * Which way round matters, because the two need different actions from
 * whoever reads it: a newer daemon may answer a frame this client cannot
 * send, and an older one may refuse a frame this client will send.  A
 * single "mismatch" would leave somebody guessing which of their two
 * machines to update.
 */
typedef enum {
    CLAWT_VERSION_SAME = 0,
    CLAWT_VERSION_DAEMON_NEWER,
    CLAWT_VERSION_DAEMON_OLDER,
    CLAWT_VERSION_UNKNOWN
} ClawtVersionVerdict;

/**
 * clawt_version_compare:
 * @daemon_version: (nullable): what the far end reported
 * @client_version: (nullable): what this end is
 *
 * Where @daemon_version stands against @client_version.
 *
 * Both ends are parameters rather than one being %CLAWT_VERSION_STRING,
 * because the trap this exists for cannot otherwise be tested: the
 * comparison somebody reaches for is `strcmp`, under which "0.10.0"
 * sorts *before* "0.9.0" -- and a test written against the build's own
 * version can only exercise that once the build has reached 0.10 itself,
 * which is exactly the release where finding out would be too late.
 *
 * Returns: the verdict
 */
ClawtVersionVerdict clawt_version_compare(const gchar *daemon_version,
                                          const gchar *client_version);

/**
 * clawt_version_compare_to_client:
 * @daemon_version: (nullable): what `control.status` reported
 *
 * Where @daemon_version stands against %CLAWT_VERSION_STRING.
 *
 * Compared as three numbers rather than as strings, because "0.10.0" and
 * "0.9.0" order the wrong way round under strcmp -- which is the one
 * comparison somebody reaches for and the one that is wrong exactly when
 * it starts to matter.
 *
 * Anything that does not parse as at least a major and a minor is
 * %CLAWT_VERSION_UNKNOWN rather than a guess.  A daemon we cannot place
 * is not a daemon we have established anything about.
 *
 * Returns: the verdict
 */
ClawtVersionVerdict clawt_version_compare_to_client(
    const gchar *daemon_version);

/**
 * clawt_version_mismatch_text:
 * @daemon_version: (nullable): what `control.status` reported
 *
 * What to say about it, or %NULL when there is nothing to say.
 *
 * The version was reported by `control.status` from the day that frame
 * existed and nobody compared it to anything, so a client talking to an
 * older or newer daemon found out by a frame kind being refused -- in
 * whichever feature the person happened to open, with a message about
 * that feature.  Saying it plainly once, at connect, is the difference
 * between "this daemon is older than your client" and an afternoon spent
 * on the wrong bug.
 *
 * %NULL for a match *and* for an unknown version: a client that
 * announced "this daemon's version is unreadable" on every connect to a
 * daemon that simply predates the field would be crying wolf.
 *
 * Returns: (transfer full) (nullable): the sentence, or %NULL
 */
gchar *clawt_version_mismatch_text(const gchar *daemon_version);

/**
 * ClawtDaemonLink:
 * @CLAWT_DAEMON_LINK_UP: the daemon is on the other end
 * @CLAWT_DAEMON_LINK_NEVER: this client has never reached it
 * @CLAWT_DAEMON_LINK_LOST: it was there and went away
 *
 * How a client stands with the daemon it is pointed at.
 *
 * Never and lost are deliberately separate, and telling them apart is
 * the whole reason this type exists.  They look identical from a
 * #ClawtClient -- not connected, a retry scheduled -- and they call for
 * opposite sentences.  "Lost the connection, what is shown is from
 * before it went" is nonsense about a window that has never had anything
 * on it, and it hides the only advice that helps: start the daemon, or
 * point at a different machine.
 */
typedef enum {
    CLAWT_DAEMON_LINK_UP = 0,
    CLAWT_DAEMON_LINK_NEVER,
    CLAWT_DAEMON_LINK_LOST
} ClawtDaemonLink;

/**
 * clawt_daemon_link_state:
 * @client: (nullable): the client, or %NULL
 * @reached_once: whether this client has ever completed a connection
 *
 * Which of the three a client is in.
 *
 * @reached_once cannot be asked of the client, and that is not an
 * oversight: `connected` is emitted from inside clawt_client_connect(),
 * which both graphical clients call before they have anything to hear
 * it with.  Whoever owns the client is the only one who knows, so it is
 * passed in.
 *
 * Returns: the state
 */
ClawtDaemonLink clawt_daemon_link_state(ClawtClient *client,
                                        gboolean     reached_once);

/**
 * clawt_connection_notice_text:
 * @link: what clawt_daemon_link_state() said
 * @connection: (nullable): which daemon, for the name and the address
 * @daemon_version: (nullable): what `control.status` reported
 *
 * The one sentence a client shows about its connection, or %NULL when
 * there is nothing to say.
 *
 * Here rather than in each client because both draw it, in a banner
 * each, and a sentence written twice is a sentence that will eventually
 * be written differently -- which for this one means two clients
 * disagreeing about whether a connection was lost or never made.  It is
 * also the only way the precedence gets tested: a window, a browser and
 * a daemon that is down are three things a hermetic test cannot have.
 *
 * The precedence is deliberate and is the whole content of the
 * function.  A connection that is not up outranks a version mismatch,
 * because while it is down the version is whatever it was before it
 * went and telling somebody to update a daemon they cannot reach is
 * advice about the wrong problem.
 *
 * A local connection is told to start clawtillad and a remote one is
 * not, because the remedies are different and only one of them is on
 * this machine.  Somebody told to start a daemon for a workstation they
 * cannot reach will start one here and be no closer.
 *
 * @available_update is a newer clawtilla that `control.status`
 * reported, or %NULL when none is known or the check is off.  It is
 * last, below the version mismatch, because a client and daemon that
 * disagree is a thing that is broken now and an update is a thing that
 * could be better later.  Saying the second while the first is true
 * answers the wrong question.
 *
 * Returns: (transfer full) (nullable): the sentence, or %NULL
 */
gchar *clawt_connection_notice_text(ClawtDaemonLink        link,
                                    const ClawtConnection *connection,
                                    const gchar           *daemon_version,
                                    const gchar           *available_update);

/**
 * clawt_connection_new_local:
 * @name: what to call it in the client
 * @socket_path: (nullable): the daemon's socket, or %NULL for the default
 *
 * A connection to a daemon on this machine.
 *
 * Returns: (transfer full): a new #ClawtConnection
 */
ClawtConnection *clawt_connection_new_local(const gchar *name,
                                            const gchar *socket_path);

/**
 * clawt_connection_new_remote:
 * @name: what to call it in the client
 * @host: the daemon's address
 * @port: its port
 * @token: (nullable): the bearer token that daemon expects
 *
 * A connection to a daemon on another machine.
 *
 * Returns: (transfer full): a new #ClawtConnection
 */
ClawtConnection *clawt_connection_new_remote(const gchar *name,
                                             const gchar *host,
                                             guint16      port,
                                             const gchar *token);

ClawtConnection *clawt_connection_copy(ClawtConnection *self);
void             clawt_connection_free(ClawtConnection *self);

const gchar *clawt_connection_get_name(ClawtConnection *self);
gboolean     clawt_connection_is_local(ClawtConnection *self);
const gchar *clawt_connection_get_socket_path(ClawtConnection *self);
const gchar *clawt_connection_get_host(ClawtConnection *self);
guint16      clawt_connection_get_port(ClawtConnection *self);
const gchar *clawt_connection_get_token(ClawtConnection *self);
gboolean     clawt_connection_get_tls(ClawtConnection *self);
gboolean     clawt_connection_get_accept_unknown_certificate(
    ClawtConnection *self);

void clawt_connection_set_name(ClawtConnection *self, const gchar *name);
void clawt_connection_set_tls(ClawtConnection *self,
                              gboolean         enabled,
                              gboolean         accept_unknown_certificate);

/**
 * clawt_connection_describe:
 * @self: a #ClawtConnection
 *
 * Where this connection goes, for showing beside its name.
 *
 * Never includes the token: this ends up in a subtitle in the client and
 * in `clawtilla remote list`, and a bearer token printed for convenience
 * is a bearer token in somebody's scrollback.
 *
 * Returns: (transfer full): a short description
 */
gchar *clawt_connection_describe(ClawtConnection *self);

/**
 * clawt_connection_create_client:
 * @self: a #ClawtConnection
 *
 * Builds the client this connection describes, without connecting it.
 *
 * Returns: (transfer full): a new #ClawtClient
 */
ClawtClient *clawt_connection_create_client(ClawtConnection *self);

/**
 * clawt_connection_list_default_path:
 *
 * Returns: (transfer full): `$XDG_CONFIG_HOME/clawtilla/connections.yaml`
 */
gchar *clawt_connection_list_default_path(void);

/**
 * clawt_connection_list_parse:
 * @text: the contents of a connections file
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a set of profiles.
 *
 * Separated from the file so a round trip can be asserted on without
 * writing anything: the failure this guards against is a token or a port
 * that survives being saved and does not survive being loaded, which on a
 * file only touched when someone edits their connections would otherwise
 * be found by that person, months later.
 *
 * Returns: (transfer full) (element-type ClawtConnection) (nullable): the
 *   profiles, or %NULL on error
 */
GPtrArray *clawt_connection_list_parse(const gchar *text, GError **error);

/**
 * clawt_connection_list_to_data:
 * @connections: (element-type ClawtConnection): the profiles
 *
 * Renders profiles back to YAML.
 *
 * Returns: (transfer full): the file contents
 */
gchar *clawt_connection_list_to_data(GPtrArray *connections);

/**
 * clawt_connection_list_load:
 * @path: (nullable): the file, or %NULL for the default
 * @error: (out) (optional): return location for a #GError
 *
 * A missing file is an empty list rather than an error: not having saved
 * a remote host yet is the ordinary state, not a fault.
 *
 * Returns: (transfer full) (element-type ClawtConnection) (nullable): the
 *   profiles, or %NULL if the file exists and could not be read
 */
GPtrArray *clawt_connection_list_load(const gchar *path, GError **error);

/**
 * clawt_connection_list_save:
 * @path: (nullable): the file, or %NULL for the default
 * @connections: (element-type ClawtConnection): the profiles
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the profiles, 0600, into a directory created 0700.
 *
 * Returns: %TRUE if written
 */
gboolean clawt_connection_list_save(const gchar *path,
                                    GPtrArray   *connections,
                                    GError     **error);

/**
 * clawt_connection_list_find:
 * @connections: (element-type ClawtConnection): the profiles
 * @name: the name to look for
 *
 * Returns: (transfer none) (nullable): the profile called @name
 */
ClawtConnection *clawt_connection_list_find(GPtrArray   *connections,
                                            const gchar *name);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtConnection, clawt_connection_free)

G_END_DECLS
