/*
 * daemon-notice.c - One notification list, held by the daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The clients each used to build an alert list from the events they
 * happened to receive, and kept it in memory -- so a notice cleared in
 * one window stayed lit in the next, a restart forgot everything, and a
 * badge on one client could not be put out from another.  The daemon is
 * the one process that sees every event, so it keeps the list:
 *
 * - It listens on its own bus.  Each event clawt_alert_tier_for_event()
 *   rates an error or a notice becomes a row in <state_dir>/notices.db,
 *   worded by clawt_alert_text_for_event() -- the same words every client
 *   used to make for itself.
 * - `notice.list` answers with those rows, the muted agents, and the
 *   three other things that want somebody: unread conversations
 *   (the read store), decisions waiting (the decision store) and Grok
 *   jobs needing attention.  Each of the three leaves muted agents out.
 * - `notice.read`, `notice.dismiss`, `notice.clear`, `notice.mute` and
 *   `notice.unmute` change it, and each publishes `notice.changed`, which
 *   is every client's cue to ask again.
 *
 * A mute is per agent and holds everywhere: the agent's notices arrive
 * already read, its counts leave every total, and clawt_daemon_notify()
 * -- the one door to the push and desktop notifiers -- does not open for
 * it.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/* The file the list lives in, under the state directory. */
#define NOTICES_DATABASE_NAME "notices.db"

/* How many notices notice.list sends when it is not told; the panel's. */
#define NOTICES_LIST_DEFAULT (200)

/* ── The clock ───────────────────────────────────────────────────── */

gint64
clawt_daemon_notices_now(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), 0);

    /*
     * A test's clock when one is set, so a timed mute can lapse and a
     * prune by age can bite without the suite waiting for either.
     */
    if (self->notices_clock > 0)
        return self->notices_clock;

    return g_get_real_time() / G_USEC_PER_SEC;
}

gboolean
clawt_daemon_notices_is_muted(ClawtDaemon *self, const gchar *agent,
                              gint64 now)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    if (self->notices == NULL || agent == NULL)
        return FALSE;

    return clawt_notice_store_is_muted(self->notices, agent, now);
}

/*
 * Says the list moved, and why.  Every client asks for notice.list again
 * on this, so it carries only enough to explain itself in a log.
 */
static void
publish_changed(ClawtDaemon *self, const gchar *reason, const gchar *agent,
                gint64 id)
{
    g_autoptr(ClawtEvent) event = NULL;

    if (self->bus == NULL)
        return;

    event = clawt_event_new("notice.changed", agent);
    clawt_event_set_detail(event, "reason", reason);

    if (id > 0) {
        g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, id);

        clawt_event_set_detail(event, "id", text);
    }

    clawt_event_bus_publish(self->bus, event);
}

/* ── Recording ───────────────────────────────────────────────────── */

gint64
clawt_daemon_notices_record(ClawtDaemon *self, ClawtEvent *event, gint64 now)
{
    ClawtNotice notice = { 0 };
    ClawtAlertTier tier;
    g_autofree gchar *title = NULL;
    g_autofree gchar *source = NULL;
    g_autofree gchar *agent = NULL;
    g_autoptr(GError) error = NULL;
    gboolean read;
    gint64 id;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), 0);
    g_return_val_if_fail(event != NULL, 0);

    if (self->notices == NULL)
        return 0;

    /*
     * Only the two loud tiers.  The routine stream is the event log's
     * (`event.list`), which already keeps every event with a retention
     * of its own; a second copy here would be a second retention policy
     * for the same rows.
     */
    tier = clawt_alert_tier_for_event(event);

    if (tier != CLAWT_ALERT_ERROR && tier != CLAWT_ALERT_NOTICE)
        return 0;

    title = clawt_alert_text_for_event(event, &source, &agent);

    if (title == NULL)
        return 0;

    /*
     * Arrives read when nothing would count it, or when its agent is
     * muted: it is still recorded -- the list is the record -- but it
     * does not reach the bell.  The surface is never "showing" from
     * here; whether somebody is looking is a client's to say, by opening
     * its panel, which reads everything.
     */
    read = clawt_alert_arrives_read(FALSE, tier) ||
           clawt_daemon_notices_is_muted(self, agent, now);

    notice.agent = agent;
    notice.tier = tier;
    notice.event_kind = (gchar *)clawt_event_get_kind(event);
    notice.source = source;
    notice.subject = (gchar *)clawt_event_get_subject(event);
    notice.title = title;
    notice.created_at = clawt_event_happened_at(event, now * G_USEC_PER_SEC) /
                        G_USEC_PER_SEC;

    id = clawt_notice_store_add(self->notices, &notice, read, &error);

    if (id == 0) {
        g_warning("notices: could not record %s: %s",
                  clawt_event_get_kind(event),
                  error != NULL ? error->message : "unknown");
        return 0;
    }

    publish_changed(self, "added", agent, id);

    return id;
}

static void
on_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    ClawtDaemon *self = user_data;

    (void)bus;

    clawt_daemon_notices_record(self, event, clawt_daemon_notices_now(self));
}

/* ── Pruning, and mutes lapsing ──────────────────────────────────── */

guint
clawt_daemon_notices_prune(ClawtDaemon *self, gint64 now)
{
    gint64 keep;
    gint64 days;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), 0);

    if (self->notices == NULL)
        return 0;

    keep = clawt_config_get_int(self->config, "notices.keep");
    days = clawt_config_get_int(self->config, "notices.keep_days");

    return clawt_notice_store_prune(self->notices,
                                    (guint)CLAMP(keep, 0, G_MAXINT),
                                    (guint)CLAMP(days, 0, 36500), now);
}

static void arm_lapse(ClawtDaemon *self);

/*
 * The soonest timed mute has lapsed.  Said at once, rather than at the
 * next hourly prune, because a client draws the agent dimmed and its
 * counts left out for exactly as long as the mute holds.
 */
static gboolean
on_lapse(gpointer user_data)
{
    ClawtDaemon *self = user_data;

    g_clear_pointer(&self->notices_lapse, g_source_unref);

    if (self->notices != NULL)
        clawt_notice_store_prune(self->notices, 0, 0,
                                 clawt_daemon_notices_now(self));

    publish_changed(self, "unmuted", NULL, 0);
    arm_lapse(self);

    return G_SOURCE_REMOVE;
}

static void
arm_lapse(ClawtDaemon *self)
{
    gint64 now;
    gint64 next;

    if (self->notices_lapse != NULL) {
        g_source_destroy(self->notices_lapse);
        g_clear_pointer(&self->notices_lapse, g_source_unref);
    }

    if (self->notices == NULL)
        return;

    now = clawt_daemon_notices_now(self);
    next = clawt_notice_store_next_lapse(self->notices, now);

    if (next <= now)
        return;

    /* On the daemon's own context; see clawt_timeout_add_seconds(). */
    self->notices_lapse = clawt_timeout_add_seconds(
        (guint)MIN(next - now, (gint64)G_MAXUINT / 2), on_lapse, self);
}

/* ── Setup ───────────────────────────────────────────────────────── */

void
clawt_daemon_notices_setup(ClawtDaemon *self)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->state_dir == NULL || self->notices != NULL)
        return;

    path = g_build_filename(self->state_dir, NOTICES_DATABASE_NAME, NULL);
    self->notices = clawt_notice_store_new(path, &error);

    if (self->notices == NULL) {
        /*
         * A warning rather than a refusal to start: without the list
         * notice.list says so, and the clients draw the other three
         * sources -- which live elsewhere -- with no alerts beside them.
         */
        g_warning("notices: could not open %s, so no notifications are "
                  "kept: %s", path,
                  error != NULL ? error->message : "unknown");
        return;
    }

    clawt_daemon_notices_prune(self, clawt_daemon_notices_now(self));

    if (self->bus != NULL)
        self->notices_bus_handler = g_signal_connect(
            self->bus, "event", G_CALLBACK(on_bus_event), self);

    arm_lapse(self);
}

void
clawt_daemon_notices_teardown(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->notices_bus_handler != 0 && self->bus != NULL)
        g_signal_handler_disconnect(self->bus, self->notices_bus_handler);

    self->notices_bus_handler = 0;

    if (self->notices_lapse != NULL) {
        g_source_destroy(self->notices_lapse);
        g_clear_pointer(&self->notices_lapse, g_source_unref);
    }

    g_clear_object(&self->notices);
}

/* ── The one door to the notifiers ───────────────────────────────── */

void
clawt_daemon_notify(ClawtDaemon *self, ClawtNotification *notification)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (notification == NULL)
        return;

    /*
     * A muted agent buzzes nobody.  The mute is the operator saying "not
     * this one, not now", and a phone going off for it would make the
     * clients' quiet a lie.
     */
    if (clawt_daemon_notices_is_muted(self, notification->agent_id,
                                      clawt_daemon_notices_now(self))) {
        g_debug("notices: %s is muted, so it is not notified about",
                notification->agent_id);
        return;
    }

    /* A test's sink in place of the notifiers, which would reach out. */
    if (self->notify_sink != NULL) {
        self->notify_sink(self, notification, self->notify_sink_data);
        return;
    }

    if (self->notifier != NULL)
        clawt_notifier_notify(self->notifier, notification);
}

/* ── notice.list ─────────────────────────────────────────────────── */

/*
 * A string member, or null for a missing one -- a client reads "no
 * agent" from null and would read "" as an agent called nothing.
 */
static void
add_string(JsonBuilder *builder, const gchar *name, const gchar *value)
{
    json_builder_set_member_name(builder, name);

    if (value != NULL)
        json_builder_add_string_value(builder, value);
    else
        json_builder_add_null_value(builder);
}

/* An id or a time, sent as a number or as a numeric string. */
static gint64
payload_number(JsonObject *payload, const gchar *key, gint64 fallback)
{
    JsonNode *node;

    if (payload == NULL || !json_object_has_member(payload, key))
        return fallback;

    node = json_object_get_member(payload, key);

    if (JSON_NODE_HOLDS_VALUE(node)) {
        if (json_node_get_value_type(node) == G_TYPE_INT64)
            return json_node_get_int(node);

        /*
         * The GTK and lrg clients build payloads of strings
         * (clawt_build_payload()), so an id arrives as "12".
         */
        if (json_node_get_value_type(node) == G_TYPE_STRING) {
            const gchar *text = json_node_get_string(node);
            gchar *end = NULL;
            gint64 value;

            if (text == NULL || *text == '\0')
                return fallback;

            value = g_ascii_strtoll(text, &end, 10);

            if (end != NULL && *end == '\0')
                return value;
        }
    }

    return fallback;
}

static void
add_notice(JsonBuilder *builder, ClawtNotice *notice, GHashTable *muted)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_int_value(builder, notice->id);
    add_string(builder, "agent", notice->agent);
    add_string(builder, "tier", clawt_notice_tier_nick(notice->tier));
    add_string(builder, "kind", notice->event_kind);
    add_string(builder, "source", notice->source);
    add_string(builder, "subject", notice->subject);
    add_string(builder, "title", notice->title);
    json_builder_set_member_name(builder, "created_at");
    json_builder_add_int_value(builder, notice->created_at);
    json_builder_set_member_name(builder, "read");
    json_builder_add_boolean_value(builder, notice->read);
    json_builder_set_member_name(builder, "dismissed");
    json_builder_add_boolean_value(builder, notice->dismissed);
    json_builder_set_member_name(builder, "muted");
    json_builder_add_boolean_value(
        builder, notice->agent != NULL &&
                     g_hash_table_contains(muted, notice->agent));
    json_builder_end_object(builder);
}

/*
 * Conversations with something unread: the operator's own conversation
 * with each agent, and every group room -- the rooms a sidebar draws.
 * Peer rooms between two agents are never counted, by anybody.  Muted
 * agents are left out, here and in the total.
 *
 * Returns: the total unread
 */
static guint
add_chats(ClawtDaemon *self, JsonBuilder *builder, GHashTable *muted,
          const gchar *only_agent)
{
    g_autoptr(GPtrArray) rooms = NULL;
    guint total = 0;
    guint i;

    json_builder_set_member_name(builder, "chats");
    json_builder_begin_array(builder);

    if (self->rooms != NULL && self->reads != NULL) {
        rooms = clawt_room_manager_list(self->rooms);

        for (i = 0; i < rooms->len; i++) {
            ClawtRoom *room = g_ptr_array_index(rooms, i);
            const gchar *room_id = clawt_room_get_id(room);
            g_autofree gchar *agent = clawt_unread_room_agent(room_id);
            guint unread;

            if (agent == NULL && !clawt_room_is_group(room))
                continue;

            if (agent != NULL && g_hash_table_contains(muted, agent))
                continue;

            if (only_agent != NULL && g_strcmp0(agent, only_agent) != 0)
                continue;

            unread = clawt_daemon_room_unread(self, room);

            if (unread == 0)
                continue;

            total += unread;

            json_builder_begin_object(builder);
            add_string(builder, "room", room_id);
            add_string(builder, "agent", agent);
            add_string(builder, "name", clawt_room_get_name(room));
            json_builder_set_member_name(builder, "unread");
            json_builder_add_int_value(builder, unread);
            json_builder_end_object(builder);
        }
    }

    json_builder_end_array(builder);

    return total;
}

/* Open decisions, less the muted agents'.  Returns: how many. */
static guint
add_decisions(ClawtDaemon *self, JsonBuilder *builder, GHashTable *muted,
              const gchar *only_agent)
{
    g_autoptr(GPtrArray) decisions = NULL;
    guint count = 0;
    guint i;

    json_builder_set_member_name(builder, "decisions");
    json_builder_begin_array(builder);

    if (self->decisions != NULL)
        decisions = clawt_decision_store_list(self->decisions, TRUE);

    for (i = 0; decisions != NULL && i < decisions->len; i++) {
        ClawtDecision *decision = g_ptr_array_index(decisions, i);
        const gchar *agent = clawt_decision_get_agent(decision);

        if (agent != NULL && g_hash_table_contains(muted, agent))
            continue;

        if (only_agent != NULL && g_strcmp0(agent, only_agent) != 0)
            continue;

        count++;

        json_builder_begin_object(builder);
        add_string(builder, "id", clawt_decision_get_id(decision));
        add_string(builder, "agent", agent);
        add_string(builder, "question", clawt_decision_get_question(decision));
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);

    return count;
}

/* Grok jobs that ask for somebody, less the muted agents'. */
static guint
add_grok(ClawtDaemon *self, JsonBuilder *builder, GHashTable *muted,
         const gchar *only_agent, gint64 now)
{
    g_autoptr(GPtrArray) jobs = NULL;
    guint count = 0;
    guint i;

    json_builder_set_member_name(builder, "grok");
    json_builder_begin_array(builder);

    /* The same predicate the Grok badge counts with; microseconds there. */
    jobs = clawt_daemon_grok_attention_jobs(self, now * G_USEC_PER_SEC);

    for (i = 0; jobs != NULL && i < jobs->len; i++) {
        ClawtGrokJob *job = g_ptr_array_index(jobs, i);
        const gchar *agent = clawt_grok_job_get_agent_id(job);

        if (agent != NULL && g_hash_table_contains(muted, agent))
            continue;

        if (only_agent != NULL && g_strcmp0(agent, only_agent) != 0)
            continue;

        count++;

        json_builder_begin_object(builder);
        add_string(builder, "id", clawt_grok_job_get_id(job));
        add_string(builder, "agent", agent);
        add_string(builder, "bot", clawt_grok_job_get_bot(job));
        add_string(
            builder, "state",
            clawt_grok_job_state_label(clawt_grok_job_get_state(job)));
        add_string(builder, "note", clawt_grok_job_get_note(job));

        /*
         * The question to ask before removing it, worded by the daemon
         * so a client that only holds this list -- not the job -- asks
         * the same thing the Grok page does.
         */
        add_string(builder, "remove_question",
                   clawt_grok_job_remove_question(
                       clawt_grok_job_get_state(job)));
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);

    return count;
}

static JsonNode *
handle_list(ClawtDaemon *self, JsonNode *request, JsonObject *payload)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GHashTable) muted = NULL;
    g_autoptr(GPtrArray) notices = NULL;
    g_autoptr(GPtrArray) mutes = NULL;
    const gchar *agent = clawt_ipc_payload_string(payload, "agent");
    gboolean dismissed = clawt_ipc_payload_boolean(payload,
                                                   "include_dismissed",
                                                   FALSE);
    gint64 limit = payload_number(payload, "limit", NOTICES_LIST_DEFAULT);
    gint64 now = clawt_daemon_notices_now(self);
    guint chats;
    guint decisions;
    guint grok;
    guint unread = 0;
    guint i;

    if (agent != NULL && *agent == '\0')
        agent = NULL;

    muted = (self->notices != NULL)
        ? clawt_notice_store_muted_set(self->notices, now)
        : g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    json_builder_begin_object(builder);

    /*
     * Whether this daemon keeps the list at all, so a client can tell
     * "nothing has needed you" from "nothing is being kept".
     */
    json_builder_set_member_name(builder, "kept");
    json_builder_add_boolean_value(builder, self->notices != NULL);

    json_builder_set_member_name(builder, "notices");
    json_builder_begin_array(builder);

    if (self->notices != NULL) {
        notices = clawt_notice_store_list(self->notices, agent, dismissed,
                                          (guint)CLAMP(limit, 0, G_MAXINT));

        for (i = 0; i < notices->len; i++)
            add_notice(builder, g_ptr_array_index(notices, i), muted);

        unread = clawt_notice_store_count_unread(self->notices, muted);
    }

    json_builder_end_array(builder);

    json_builder_set_member_name(builder, "mutes");
    json_builder_begin_array(builder);

    if (self->notices != NULL) {
        mutes = clawt_notice_store_list_mutes(self->notices, now);

        for (i = 0; i < mutes->len; i++) {
            ClawtNoticeMute *mute = g_ptr_array_index(mutes, i);

            json_builder_begin_object(builder);
            add_string(builder, "agent", mute->agent);
            json_builder_set_member_name(builder, "until");
            json_builder_add_int_value(builder, mute->until);
            json_builder_end_object(builder);
        }
    }

    json_builder_end_array(builder);

    chats = add_chats(self, builder, muted, agent);
    decisions = add_decisions(self, builder, muted, agent);
    grok = add_grok(self, builder, muted, agent, now);

    /*
     * The three other things that want somebody, counted here so every
     * client's bell is the same number: the badge is these plus the
     * unread alerts, and nothing a client keeps for itself.
     */
    json_builder_set_member_name(builder, "sources");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "chats_unread");
    json_builder_add_int_value(builder, chats);
    json_builder_set_member_name(builder, "decisions_pending");
    json_builder_add_int_value(builder, decisions);
    json_builder_set_member_name(builder, "grok_attention");
    json_builder_add_int_value(builder, grok);
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "unread");
    json_builder_add_int_value(builder, unread);
    json_builder_set_member_name(builder, "total");
    json_builder_add_int_value(builder,
                               (gint64)chats + decisions + grok + unread);

    json_builder_end_object(builder);

    return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

/* ── The handler ─────────────────────────────────────────────────── */

static JsonNode *
reply_count(JsonNode *request, const gchar *member, guint count)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, member);
    json_builder_add_int_value(builder, count);
    json_builder_end_object(builder);

    return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

JsonNode *
clawt_daemon_handle_notice(ClawtDaemon  *self,
                           const gchar  *kind,
                           JsonNode     *request,
                           JsonObject   *payload,
                           gboolean     *handled)
{
    if (kind == NULL || !g_str_has_prefix(kind, "notice."))
        return NULL;

    *handled = TRUE;

    if (g_strcmp0(kind, "notice.list") == 0)
        return handle_list(self, request, payload);

    if (self->notices == NULL)
        return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                   "this daemon keeps no notifications");

    /*
     * One, or all of them.  "Read" is what opening the panel does, so
     * every client sends `all` on open; one by id is the row menu's.
     */
    if (g_strcmp0(kind, "notice.read") == 0) {
        gint64 id = payload_number(payload, "id", 0);
        gboolean all = clawt_ipc_payload_boolean(payload, "all", FALSE);
        guint changed;

        if (id <= 0 && !all)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "say which notice: \"id\", or "
                                       "\"all\": true");

        changed = clawt_notice_store_mark_read(self->notices,
                                               all ? 0 : id);

        if (changed > 0)
            publish_changed(self, "read", NULL, all ? 0 : id);

        return reply_count(request, "read", changed);
    }

    if (g_strcmp0(kind, "notice.dismiss") == 0) {
        gint64 id = payload_number(payload, "id", 0);

        if (id <= 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "say which notice: \"id\"");

        if (!clawt_notice_store_dismiss(self->notices, id))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no notice showing by "
                                       "that id");

        publish_changed(self, "dismissed", NULL, id);

        return reply_count(request, "dismissed", 1);
    }

    if (g_strcmp0(kind, "notice.clear") == 0) {
        const gchar *agent = clawt_ipc_payload_string(payload, "agent");
        guint cleared;

        if (agent != NULL && *agent == '\0')
            agent = NULL;

        cleared = clawt_notice_store_clear(self->notices, agent);

        if (cleared > 0)
            publish_changed(self, "cleared", agent, 0);

        return reply_count(request, "cleared", cleared);
    }

    if (g_strcmp0(kind, "notice.mute") == 0 ||
        g_strcmp0(kind, "notice.unmute") == 0) {
        const gchar *agent = clawt_ipc_payload_string(payload, "agent");
        gboolean mute = g_strcmp0(kind, "notice.mute") == 0;
        gint64 now = clawt_daemon_notices_now(self);
        g_autoptr(JsonBuilder) builder = NULL;

        if (agent == NULL || *agent == '\0')
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "say which agent: \"agent\"");

        if (mute) {
            gint64 until = payload_number(payload, "until", 0);
            g_autoptr(GError) error = NULL;

            /*
             * Only an agent the fleet has: a mute for a typo would sit
             * in the table silencing nothing, and the person who typed
             * it would believe the real one was quiet.
             */
            if (clawt_config_get_agent(self->config, agent) == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "there is no agent by that id");

            if (until != 0 && until <= now)
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_INVALID_ARGUMENT,
                    "\"until\" is in the past; leave it out to mute "
                    "until unmuted");

            if (!clawt_notice_store_mute(self->notices, agent, until,
                                         &error))
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        } else {
            clawt_notice_store_unmute(self->notices, agent);
        }

        arm_lapse(self);
        publish_changed(self, mute ? "muted" : "unmuted", agent, 0);

        builder = json_builder_new();
        json_builder_begin_object(builder);
        add_string(builder, "agent", agent);
        json_builder_set_member_name(builder, "muted");
        json_builder_add_boolean_value(
            builder, clawt_notice_store_is_muted(self->notices, agent, now));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    {
        g_autofree gchar *message = g_strdup_printf(
            "this daemon does not understand '%s'", kind);

        return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                   message);
    }
}
