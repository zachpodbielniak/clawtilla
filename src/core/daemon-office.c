/*
 * daemon-office.c - The office: who is where, computed once, for everybody
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon is the office's single source of truth.  It listens to its
 * own event bus, translates the events that say what an agent is doing
 * into the presence fold (clawt-office-presence.c), and publishes an
 * `office.presence` event whenever what a participant should be drawn
 * doing changes -- the edge, never a repeat.  Every client and the
 * office renderer draw that, plus `office.snapshot` on connect, so two
 * windows cannot disagree about who is in the meeting.
 *
 * Off unless `office.enabled`.  It reads nothing and runs nothing then;
 * the IPC answers say it is off rather than refusing, so a client can
 * tell somebody how to turn it on.
 *
 * Nothing here invents activity.  An agent at a desk is one the daemon
 * saw start a turn; a phone call is a message between two agents; a
 * meeting is a group round.  A turn edge nobody closed decays after
 * `office.stale_seconds`.
 */

#include "clawtilla.h"

#include <stdlib.h>
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

/* How often decay is looked at, in seconds: idle is not urgent. */
#define OFFICE_TICK_SECONDS (5)
/* A participant reaped stays this long, walking out, before it is removed. */
#define OFFICE_LEAVE_SECONDS (12)

/* ── This fleet's id ────────────────────────────────────────────── */

const gchar *
clawt_daemon_fleet_id(ClawtDaemon *self)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(GError) error = NULL;

    if (self->fleet_id != NULL)
        return self->fleet_id;
    if (self->state_dir == NULL)
        return NULL;

    /*
     * Made once and kept: a fleet's identity outlives restarts and
     * renames, which is what a future federation link will key on.
     */
    path = g_build_filename(self->state_dir, "fleet-id", NULL);
    if (g_file_get_contents(path, &text, NULL, NULL) && *g_strstrip(text) != '\0') {
        self->fleet_id = g_steal_pointer(&text);
        return self->fleet_id;
    }
    text = clawt_generate_token(&error);
    if (text == NULL) {
        g_warning("office: no fleet id could be made: %s", error->message);
        return NULL;
    }
    if (!g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT,
                                  0600, &error))
        g_warning("office: the fleet id was not saved: %s", error->message);
    self->fleet_id = g_steal_pointer(&text);
    return self->fleet_id;
}

/* ── Where worlds live ─────────────────────────────────────────── */

static gchar *
executable_dir(void)
{
    g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);

    return exe != NULL ? g_path_get_dirname(exe) : NULL;
}

GStrv
clawt_daemon_office_world_dirs(ClawtDaemon *self)
{
    GPtrArray *dirs = g_ptr_array_new();
    g_auto(GStrv) extra = clawt_config_get_string_list(self->config, "office.world_dirs");
    g_autofree gchar *exe = executable_dir();
    gsize i;

    /* the person's own, then what the custodial team wrote, then shipped */
    for (i = 0; extra != NULL && extra[i] != NULL; i++) {
        g_autofree gchar *expanded = clawt_expand_path(extra[i]);

        g_ptr_array_add(dirs, g_steal_pointer(&expanded));
    }
    if (self->state_dir != NULL)
        g_ptr_array_add(dirs, g_build_filename(self->state_dir, "office", "worlds", NULL));
    if (exe != NULL)
        g_ptr_array_add(dirs, g_build_filename(exe, "..", "..", "data", "office",
                                               "worlds", NULL));
    g_ptr_array_add(dirs, g_build_filename(CLAWT_DATA_DIR, "office", "worlds", NULL));
    g_ptr_array_add(dirs, NULL);
    return (GStrv)g_ptr_array_free(dirs, FALSE);
}

/* ── The roster ─────────────────────────────────────────────────── */

static void
sync_roster(ClawtDaemon *self)
{
    GPtrArray *agents = clawt_agent_manager_list(self->agents);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        ClawtAgentConfig *config = clawt_agent_get_config(agent);
        gboolean ephemeral = config != NULL && clawt_agent_config_is_ephemeral(config);

        /* someone walking out keeps walking: the removal is theirs */
        if (clawt_office_presence_resolve(self->office, clawt_agent_get_id(agent), now,
                                          NULL, NULL, NULL) == CLAWT_OFFICE_LEAVING)
            continue;

        clawt_office_presence_set_participant(
            self->office,
            ephemeral ? CLAWT_OFFICE_KIND_EPHEMERAL : CLAWT_OFFICE_KIND_AGENT,
            clawt_agent_get_id(agent), clawt_agent_get_name(agent),
            config != NULL ? clawt_agent_config_get_string(config, "team") : NULL,
            ephemeral ? clawt_agent_config_get_ephemeral_string(config, "parent") : NULL,
            clawt_agent_get_state(agent) == CLAWT_AGENT_STATE_RUNNING, now);
        clawt_office_presence_set_role(self->office, clawt_agent_get_id(agent),
            clawt_agent_is_chief_of_staff(agent) ? "chief"
            : config != NULL && clawt_team_role_of(config) == CLAWT_TEAM_LEAD ? "lead" : NULL);
    }
}

/* Every lead is in @members, with the chief or a person: the town hall. */
static gboolean
is_town_hall(ClawtDaemon *self, GPtrArray *members, const gchar *from)
{
    GPtrArray *agents = clawt_agent_manager_list(self->agents);
    gboolean any_lead = FALSE;
    guint i;

    if (from == NULL)
        return FALSE;
    if (g_strcmp0(from, "user") != 0) {
        ClawtAgent *sender = clawt_agent_manager_get(self->agents, from);

        if (sender == NULL || !clawt_agent_is_chief_of_staff(sender))
            return FALSE;
    }
    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        ClawtAgentConfig *config = clawt_agent_get_config(agent);

        if (config == NULL || clawt_team_role_of(config) != CLAWT_TEAM_LEAD)
            continue;
        any_lead = TRUE;
        if (!g_ptr_array_find_with_equal_func(members, clawt_agent_get_id(agent),
                                              g_str_equal, NULL))
            return FALSE;
    }
    return any_lead;
}

/* A message is a call between two agents, or a meeting of several. */
static void
note_message(ClawtDaemon *self, ClawtEvent *event, gint64 now)
{
    const gchar *room_id = clawt_event_get_subject(event);
    const gchar *from = clawt_event_get_detail(event, "from");
    ClawtRoom *room = self->rooms != NULL && room_id != NULL
        ? clawt_room_manager_get(self->rooms, room_id) : NULL;
    GPtrArray *members;
    gint64 min_members;

    if (room == NULL || from == NULL)
        return;
    members = clawt_room_get_members(room);
    if (members == NULL)
        return;

    min_members = MAX(clawt_config_get_int(self->config, "office.meeting_min_members"), 3);
    if (clawt_room_is_group(room) && members->len >= (guint)min_members) {
        g_autoptr(GPtrArray) names = g_ptr_array_new();
        guint i;

        for (i = 0; i < members->len; i++)
            g_ptr_array_add(names, g_ptr_array_index(members, i));
        g_ptr_array_add(names, NULL);
        clawt_office_presence_note_meeting(self->office, room_id,
                                           (const gchar *const *)names->pdata,
                                           is_town_hall(self, members, from), now);
        return;
    }

    /* two agents in a room of two: a phone call */
    if (members->len == 2 && g_strcmp0(from, "user") != 0) {
        const gchar *a = g_ptr_array_index(members, 0);
        const gchar *b = g_ptr_array_index(members, 1);

        if (g_strcmp0(a, "user") != 0 && g_strcmp0(b, "user") != 0)
            clawt_office_presence_note_call(self->office, from,
                                            g_strcmp0(a, from) == 0 ? b : a, now);
    }
}

static void schedule_flush(ClawtDaemon *self);

static void
on_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    ClawtDaemon *self = user_data;
    const gchar *seen = clawt_event_get_kind(event);
    const gchar *subject = clawt_event_get_subject(event);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    (void)bus;
    if (self->office == NULL || seen == NULL || g_str_has_prefix(seen, "office."))
        return;

    if (g_strcmp0(seen, "agent.typing") == 0) {
        clawt_office_presence_note_turn(self->office, subject,
            clawt_event_get_detail(event, "room"),
            g_strcmp0(clawt_event_get_detail(event, "typing"), "true") == 0, now);
    } else if (g_strcmp0(seen, "turn.step") == 0) {
        clawt_office_presence_note_step(self->office, subject,
            clawt_event_get_detail(event, CLAWT_STEP_MEMBER_ROOM),
            clawt_event_get_detail(event, CLAWT_STEP_MEMBER_TOOL), now);
    } else if (g_strcmp0(seen, "message") == 0) {
        note_message(self, event, now);
    } else if (g_strcmp0(seen, "task.changed") == 0) {
        ClawtTask *task = self->tasks != NULL && subject != NULL
            ? clawt_task_manager_get(self->tasks, subject) : NULL;

        /* handed over: the delegator walks a folder to the assignee */
        if (task != NULL && clawt_task_get_state(task) == CLAWT_TASK_PENDING)
            clawt_office_presence_note_delivery(self->office,
                clawt_task_get_origin(task), clawt_task_get_assignee(task), now);
    } else if (g_strcmp0(seen, "handoff.queued") == 0) {
        clawt_office_presence_note_delivery(self->office,
            clawt_event_get_detail(event, "from"), clawt_event_get_detail(event, "to"), now);
    } else if (g_strcmp0(seen, "decision.asked") == 0) {
        clawt_office_presence_note_waiting(self->office, subject,
            clawt_event_get_detail(event, "decision"), TRUE);
    } else if (g_strcmp0(seen, "decision.settled") == 0) {
        clawt_office_presence_note_waiting(self->office, NULL,
            clawt_event_get_detail(event, "decision"), FALSE);
    } else if (g_strcmp0(seen, "turn.timed_out") == 0 ||
               g_strcmp0(seen, "turn.repeating") == 0) {
        clawt_office_presence_note_error(self->office, subject,
            g_strcmp0(seen, "turn.timed_out") == 0 ? "its turn timed out"
                                                   : "its turn kept repeating", now);
    } else if (g_strcmp0(seen, "incident.opened") == 0) {
        clawt_office_presence_note_error(self->office,
            clawt_event_get_detail(event, "agent"), "a run broke", now);
    } else if (g_strcmp0(seen, "grok.job_updated") == 0) {
        const gchar *bot = clawt_event_get_detail(event, "bot");
        const gchar *state = clawt_event_get_detail(event, "state");
        g_autofree gchar *id = bot != NULL ? g_strdup_printf("grok:%s", bot) : NULL;
        gboolean active = g_strcmp0(state, "accepted") == 0 ||
                          g_strcmp0(state, "working") == 0 ||
                          g_strcmp0(state, "sent") == 0;

        if (id == NULL)
            return;
        /* a remote worker: present while it has a job, at the visitor desk */
        clawt_office_presence_set_participant(self->office, CLAWT_OFFICE_KIND_GROK_BOT,
                                              id, bot, NULL, NULL, TRUE, now);
        clawt_office_presence_note_turn(self->office, id, subject, active, now);
        clawt_office_presence_note_waiting(self->office, id, subject,
                                           g_strcmp0(state, "needs_input") == 0);
    } else if (g_strcmp0(seen, "agent.removed") == 0) {
        ClawtOfficeActivity was = clawt_office_presence_resolve(self->office, subject,
                                                                now, NULL, NULL, NULL);

        /* a helper walks out; anybody else is simply gone */
        if (was != CLAWT_OFFICE_OFFLINE)
            clawt_office_presence_note_leaving(self->office, subject, now);
        else
            clawt_office_presence_remove(self->office, subject);
        sync_roster(self);
    } else if (g_strcmp0(seen, "agent.created") == 0) {
        ClawtAgentConfig *made = clawt_config_get_agent(self->config, subject);
        const gchar *generator = clawt_config_get_string(self->config,
                                                         "office.asset_generator");

        sync_roster(self);
        /*
         * A character for the newcomer, filed as a task when somebody
         * keeps the office and a generator is configured.  Never for a
         * helper (it is gone soon), and a refusal is only logged: the
         * agent exists either way, drawn with its own face.
         */
        if (made != NULL && !clawt_agent_config_is_ephemeral(made) &&
            generator != NULL && *generator != '\0') {
            g_autoptr(GError) why = NULL;
            g_autofree gchar *task = clawt_daemon_office_commission_character(self, subject,
                                                                              &why);

            if (task == NULL)
                g_message("office: no character commissioned for %s: %s", subject,
                          why->message);
        }
    } else if (g_str_has_prefix(seen, "agent.") || g_strcmp0(seen, "team.changed") == 0) {
        sync_roster(self);
    } else {
        return;
    }
    schedule_flush(self);
}

/* ── Publishing the edges ───────────────────────────────────────── */

static void
publish_changes(ClawtDaemon *self)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_autoptr(GPtrArray) changed = NULL;
    guint i;

    if (self->office == NULL)
        return;

    /* a helper that has walked out is taken off the floor */
    {
        g_autoptr(JsonNode) snapshot = clawt_office_presence_snapshot(self->office, now);
        JsonArray *people = json_object_get_array_member(json_node_get_object(snapshot),
                                                         "participants");

        for (i = 0; i < json_array_get_length(people); i++) {
            JsonObject *p = json_array_get_object_element(people, i);
            const gchar *id = json_object_get_string_member(p, "id");

            if (g_strcmp0(json_object_get_string_member(p, "activity"), "leaving") == 0 &&
                now - json_object_get_int_member(p, "since") >= OFFICE_LEAVE_SECONDS &&
                clawt_agent_manager_get(self->agents, id) == NULL) {
                g_autoptr(ClawtEvent) gone = clawt_event_new("office.presence", id);

                clawt_office_presence_remove(self->office, id);
                clawt_event_set_detail(gone, "activity", "gone");
                clawt_event_bus_publish(self->bus, gone);
            }
        }
    }

    changed = clawt_office_presence_changed_since(self->office, now);
    for (i = 0; i < changed->len; i++) {
        const gchar *id = g_ptr_array_index(changed, i);
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(JsonNode) node = NULL;
        g_autoptr(ClawtEvent) event = clawt_event_new("office.presence", id);
        JsonObjectIter iter;
        const gchar *name;
        JsonNode *value;

        if (!clawt_office_presence_add_participant_json(self->office, id, now, builder))
            continue;
        node = json_builder_get_root(builder);
        /* every member as a detail: details are strings on the wire */
        json_object_iter_init(&iter, json_node_get_object(node));
        while (json_object_iter_next(&iter, &name, &value)) {
            if (json_node_get_value_type(value) == G_TYPE_STRING) {
                clawt_event_set_detail(event, name, json_node_get_string(value));
            } else {
                g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT,
                                                         json_node_get_int(value));

                clawt_event_set_detail(event, name, text);
            }
        }
        clawt_event_bus_publish(self->bus, event);
    }
}

static gboolean
on_flush(gpointer user_data)
{
    ClawtDaemon *self = user_data;

    self->office_flush = 0;
    publish_changes(self);
    return G_SOURCE_REMOVE;
}

/*
 * Coalesced: a burst of steps is one look, a quarter of a second later,
 * on the daemon's own context.
 */
static void
schedule_flush(ClawtDaemon *self)
{
    GSource *source;

    if (self->office_flush != 0)
        return;
    source = g_timeout_source_new(250);
    g_source_set_callback(source, on_flush, self, NULL);
    self->office_flush = g_source_attach(source, self->main_context);
    g_source_unref(source);
}

static gboolean
on_tick(gpointer user_data)
{
    publish_changes(user_data);
    return G_SOURCE_CONTINUE;
}

void
clawt_daemon_office_setup(ClawtDaemon *self)
{
    if (self->office != NULL || !clawt_config_get_boolean(self->config, "office.enabled"))
        return;

    self->office = clawt_office_presence_new(
        clawt_daemon_fleet_id(self),
        clawt_config_get_int(self->config, "office.stale_seconds"));
    sync_roster(self);
    self->office_bus_handler = g_signal_connect(self->bus, "event",
                                                G_CALLBACK(on_bus_event), self);
    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);
    self->office_tick = clawt_timeout_add_seconds(OFFICE_TICK_SECONDS, on_tick, self);
    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);
    publish_changes(self);
}

void
clawt_daemon_office_teardown(ClawtDaemon *self)
{
    if (self->bus != NULL && self->office_bus_handler != 0)
        g_signal_handler_disconnect(self->bus, self->office_bus_handler);
    self->office_bus_handler = 0;
    if (self->office_tick != NULL) {
        g_source_destroy(self->office_tick);
        g_clear_pointer(&self->office_tick, g_source_unref);
    }
    if (self->office_flush != 0) {
        GSource *source = g_main_context_find_source_by_id(self->main_context,
                                                           self->office_flush);

        if (source != NULL)
            g_source_destroy(source);
        self->office_flush = 0;
    }
    g_clear_object(&self->office);
}

/* A reload may have turned the office on or off. */
void
clawt_daemon_office_reconfigure(ClawtDaemon *self)
{
    gboolean wanted = clawt_config_get_boolean(self->config, "office.enabled");

    if (wanted && self->office == NULL)
        clawt_daemon_office_setup(self);
    else if (!wanted && self->office != NULL)
        clawt_daemon_office_teardown(self);
    else if (self->office != NULL)
        sync_roster(self);
}

/* ── IPC ────────────────────────────────────────────────────────── */

static ClawtOfficeWorld *
find_world(ClawtDaemon *self, const gchar *id, GError **error)
{
    g_auto(GStrv) dirs = clawt_daemon_office_world_dirs(self);
    g_autoptr(GPtrArray) worlds = clawt_office_worlds_discover((const gchar *const *)dirs,
                                                               NULL);
    const gchar *wanted = id != NULL ? id : clawt_config_get_string(self->config,
                                                                    "office.world");
    guint i;

    for (i = 0; i < worlds->len; i++) {
        ClawtOfficeWorld *world = g_ptr_array_index(worlds, i);

        if (g_strcmp0(clawt_office_world_get_id(world), wanted) == 0)
            return g_object_ref(world);
    }
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                "there is no office world '%s'; `clawtilla office worlds` lists them",
                wanted != NULL ? wanted : "");
    return NULL;
}

/*
 * Who sits where, for every renderer at once: the director's answer over
 * this moment's presence and the configured (or named) world.  One
 * function for office.layout and the custodians' preview, so the preview
 * is the layout and not a description of it.
 */
JsonNode *
clawt_daemon_office_layout(ClawtDaemon *self, const gchar *world_id, GError **error)
{
    g_autoptr(ClawtOfficeWorld) world = NULL;
    g_autoptr(ClawtOfficeChoreography) rules = NULL;
    g_autoptr(JsonNode) snapshot = NULL;
    g_autofree gchar *mine_path = NULL;
    g_autofree gchar *mine = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    JsonNode *layout;

    if (self->office == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "the office is off");
        return NULL;
    }
    world = find_world(self, world_id, error);
    if (world == NULL)
        return NULL;
    rules = clawt_office_choreography_new_default();
    clawt_office_choreography_override(rules,
        clawt_office_world_get_choreography_yaml(world), NULL);
    mine_path = clawt_config_get_path_value(self->config, "office.choreography");
    if (mine_path != NULL && g_file_get_contents(mine_path, &mine, NULL, NULL))
        clawt_office_choreography_override(rules, mine, NULL);

    snapshot = clawt_office_presence_snapshot(self->office, now);
    layout = clawt_office_direct(world, rules, json_node_get_object(snapshot));
    json_object_set_int_member(json_node_get_object(layout), "now", now);
    /* the participants too, so one request draws the whole floor */
    json_object_set_member(json_node_get_object(layout), "participants",
        json_node_copy(json_object_get_member(json_node_get_object(snapshot),
                                              "participants")));
    return layout;
}

static void
add_off(JsonBuilder *builder)
{
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(builder, FALSE);
    json_builder_set_member_name(builder, "why");
    json_builder_add_string_value(builder,
        "the office is off: turn it on from the Office page, or with "
        "`clawtilla office on`");
}

JsonNode *
clawt_daemon_handle_office(ClawtDaemon *self,
                           const gchar *kind,
                           JsonNode    *request,
                           JsonObject  *payload,
                           gboolean    *handled)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GError) error = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    if (kind == NULL || !g_str_has_prefix(kind, "office.")) {
        *handled = FALSE;
        return NULL;
    }
    *handled = TRUE;

    if (g_strcmp0(kind, "office.snapshot") == 0) {
        if (self->office == NULL) {
            json_builder_begin_object(builder);
            add_off(builder);
            json_builder_end_object(builder);
            return clawt_ipc_response_new(request, json_builder_get_root(builder));
        }
        {
            JsonNode *snapshot = clawt_office_presence_snapshot(self->office, now);

            json_object_set_boolean_member(json_node_get_object(snapshot), "enabled", TRUE);
            json_object_set_string_member(json_node_get_object(snapshot), "world",
                clawt_config_get_string(self->config, "office.world"));
            json_object_set_string_member(json_node_get_object(snapshot), "renderer",
                clawt_config_get_string(self->config, "office.renderer"));
            return clawt_ipc_response_new(request, snapshot);
        }
    }

    if (g_strcmp0(kind, "office.worlds") == 0) {
        g_auto(GStrv) dirs = clawt_daemon_office_world_dirs(self);
        g_autoptr(GPtrArray) refused = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GPtrArray) worlds = clawt_office_worlds_discover(
            (const gchar *const *)dirs, refused);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "enabled");
        json_builder_add_boolean_value(builder, self->office != NULL);
        json_builder_set_member_name(builder, "current");
        json_builder_add_string_value(builder,
            clawt_config_get_string(self->config, "office.world"));
        json_builder_set_member_name(builder, "worlds");
        json_builder_begin_array(builder);
        for (i = 0; i < worlds->len; i++) {
            ClawtOfficeWorld *world = g_ptr_array_index(worlds, i);
            g_autofree gchar *two = clawt_office_world_get_renderer(world, "2d");
            g_autofree gchar *three = clawt_office_world_get_renderer(world, "3d");

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, clawt_office_world_get_id(world));
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, clawt_office_world_get_name(world));
            json_builder_set_member_name(builder, "license");
            json_builder_add_string_value(builder, clawt_office_world_get_license(world));
            json_builder_set_member_name(builder, "has_2d");
            json_builder_add_boolean_value(builder, two != NULL);
            json_builder_set_member_name(builder, "has_3d");
            json_builder_add_boolean_value(builder, three != NULL);
            json_builder_set_member_name(builder, "warnings");
            json_builder_add_int_value(builder,
                clawt_office_world_get_warnings(world)->len);
            json_builder_end_object(builder);
        }
        json_builder_end_array(builder);
        json_builder_set_member_name(builder, "refused");
        json_builder_begin_array(builder);
        for (i = 0; i < refused->len; i++)
            json_builder_add_string_value(builder, g_ptr_array_index(refused, i));
        json_builder_end_array(builder);
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * Who sits where, for every renderer at once: the director's answer
     * over this moment's presence and the configured (or named) world.
     */
    if (g_strcmp0(kind, "office.layout") == 0) {
        JsonNode *layout;

        if (self->office == NULL) {
            json_builder_begin_object(builder);
            add_off(builder);
            json_builder_end_object(builder);
            return clawt_ipc_response_new(request, json_builder_get_root(builder));
        }
        layout = clawt_daemon_office_layout(self, clawt_ipc_payload_string(payload, "world"),
                                            &error);
        if (layout == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);
        return clawt_ipc_response_new(request, layout);
    }

    if (g_strcmp0(kind, "office.world") == 0) {
        g_autoptr(ClawtOfficeWorld) world = find_world(self,
            clawt_ipc_payload_string(payload, "world"), &error);
        g_autoptr(ClawtOfficeChoreography) rules = NULL;
        JsonNode *node;
        g_autofree gchar *mine_path = NULL;
        g_autofree gchar *mine = NULL;

        if (world == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        /* built in, then the pack's, then the person's */
        rules = clawt_office_choreography_new_default();
        if (!clawt_office_choreography_override(rules,
                clawt_office_world_get_choreography_yaml(world), &error)) {
            g_warning("office: %s's choreography is ignored: %s",
                      clawt_office_world_get_id(world), error->message);
            g_clear_error(&error);
        }
        mine_path = clawt_config_get_path_value(self->config, "office.choreography");
        if (mine_path != NULL && g_file_get_contents(mine_path, &mine, NULL, NULL) &&
            !clawt_office_choreography_override(rules, mine, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        node = clawt_office_world_to_json(world);
        json_object_set_member(json_node_get_object(node), "choreography",
                               clawt_office_choreography_to_json(rules));
        return clawt_ipc_response_new(request, node);
    }

    /*
     * One file of a pack, by its path inside the pack -- for a client or
     * renderer that is not on the daemon's machine.  Base64, capped, and
     * only a path the pack itself names or its renderer files.
     */
    if (g_strcmp0(kind, "office.file") == 0) {
        g_autoptr(ClawtOfficeWorld) world = find_world(self,
            clawt_ipc_payload_string(payload, "world"), &error);
        const gchar *relative = clawt_ipc_payload_string(payload, "path");
        g_autofree gchar *full = NULL;
        g_autofree gchar *canonical = NULL;
        g_autofree gchar *data = NULL;
        g_autofree gchar *encoded = NULL;
        g_autofree gchar *root = NULL;
        gsize length = 0;

        if (world == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);
        if (relative == NULL || g_path_is_absolute(relative))
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a path inside the pack is required");
        /*
         * Resolved by the kernel, both ends: g_canonicalize_filename() is
         * lexical, so a symbolic link planted in a pack -- packs are
         * written by agents -- would have served any file the daemon can
         * read.  The same rule clawt_office_pack_write_file() applies.
         */
        full = g_build_filename(clawt_office_world_get_directory(world), relative, NULL);
        canonical = realpath(full, NULL);
        {
            g_autofree gchar *pack_real = realpath(clawt_office_world_get_directory(world),
                                                   NULL);

            root = pack_real != NULL ? g_strconcat(pack_real, G_DIR_SEPARATOR_S, NULL) : NULL;
        }
        if (canonical == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "the pack has no such file");
        if (root == NULL || !g_str_has_prefix(canonical, root))
            return clawt_ipc_error_new(request, CLAWT_ERROR_PERMISSION_DENIED,
                                       "that path is outside the pack");
        if (!g_file_get_contents(canonical, &data, &length, &error))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND, error->message);
        if (length > 32 * 1024 * 1024)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "that file is larger than 32 MiB");
        encoded = g_base64_encode((const guchar *)data, length);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "path");
        json_builder_add_string_value(builder, relative);
        json_builder_set_member_name(builder, "size");
        json_builder_add_int_value(builder, (gint64)length);
        json_builder_set_member_name(builder, "base64");
        json_builder_add_string_value(builder, encoded);
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * Turning the office on or off, and choosing its world, from any
     * client: what Settings would do, written to clawtilla.yaml and
     * applied at once, so nobody has to reload by hand.  A world that
     * does not exist is refused here rather than saved and then failing
     * every office.layout.
     */
    if (g_strcmp0(kind, "office.set") == 0) {
        const gchar *world = clawt_ipc_payload_string(payload, "world");
        const gchar *renderer = clawt_ipc_payload_string(payload, "renderer");
        gboolean has_enabled = payload != NULL && json_object_has_member(payload, "enabled");

        if (!has_enabled && world == NULL && renderer == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "nothing to set: send enabled, world or renderer");
        if (renderer != NULL && g_strcmp0(renderer, "auto") != 0 &&
            g_strcmp0(renderer, "2d") != 0 && g_strcmp0(renderer, "3d") != 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "renderer is auto, 2d or 3d");
        if (world != NULL) {
            g_autoptr(ClawtOfficeWorld) found = find_world(self, world, &error);

            if (found == NULL)
                return clawt_ipc_error_new(request, error->code, error->message);
            clawt_config_set_string(self->config, "office.world", world);
        }
        if (renderer != NULL)
            clawt_config_set_string(self->config, "office.renderer", renderer);
        /* a boolean key, set as one: never a string the reader cannot see */
        if (has_enabled)
            clawt_config_set_boolean(self->config, "office.enabled",
                clawt_ipc_payload_boolean(payload, "enabled", FALSE));
        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_daemon_office_reconfigure(self);
        clawt_event_bus_emit(self->bus, "office.changed", NULL);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "enabled");
        json_builder_add_boolean_value(builder, self->office != NULL);
        json_builder_set_member_name(builder, "world");
        json_builder_add_string_value(builder,
            clawt_config_get_string(self->config, "office.world"));
        json_builder_set_member_name(builder, "renderer");
        json_builder_add_string_value(builder,
            clawt_config_get_string(self->config, "office.renderer"));
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * The custodial team, from any client or the CLI: the same install
     * the chief's tool runs, by the operator, who needs no permission.
     */
    if (g_strcmp0(kind, "office.custodians_install") == 0) {
        g_autoptr(GPtrArray) created = g_ptr_array_new_with_free_func(g_free);
        g_autofree gchar *team = NULL;
        guint i;

        if (!clawt_daemon_office_custodians_install(self,
                clawt_ipc_payload_string(payload, "team"),
                clawt_ipc_payload_boolean(payload, "start", TRUE), created, &error))
            return clawt_ipc_error_new(request, error->code, error->message);
        team = clawt_daemon_office_custodial_team(self);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "team");
        json_builder_add_string_value(builder, team);
        json_builder_set_member_name(builder, "agents");
        json_builder_begin_array(builder);
        for (i = 0; i < created->len; i++)
            json_builder_add_string_value(builder, g_ptr_array_index(created, i));
        json_builder_end_array(builder);
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* Who keeps the office, for a client deciding whether to offer the install. */
    if (g_strcmp0(kind, "office.custodians") == 0) {
        g_autofree gchar *team = clawt_daemon_office_custodial_team(self);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "team");
        if (team != NULL)
            json_builder_add_string_value(builder, team);
        else
            json_builder_add_null_value(builder);
        json_builder_set_member_name(builder, "generator");
        json_builder_add_string_value(builder,
            clawt_config_get_string(self->config, "office.asset_generator"));
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * The characters the custodial team has made, for a renderer: each
     * one with a sheet and its layout.  One still being made (no sheet
     * yet) is left out -- the agent keeps the default body until it is
     * whole.
     */
    if (g_strcmp0(kind, "office.characters") == 0) {
        g_autofree gchar *root = clawt_daemon_office_characters_dir(self);
        g_autoptr(GDir) dir = g_dir_open(root, 0, NULL);
        const gchar *name;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "characters");
        json_builder_begin_array(builder);
        while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
            g_autofree gchar *one = g_build_filename(root, name, NULL);
            g_autoptr(ClawtOfficeCharacter) c = clawt_office_character_load(one, NULL);

            /* only a whole one, and only for an agent that exists */
            if (c == NULL || c->sheet == NULL || c->animations->len == 0 ||
                clawt_config_get_agent(self->config, name) == NULL)
                continue;
            g_free(c->agent);
            c->agent = g_strdup(name);
            json_builder_add_value(builder, clawt_office_character_to_json(c));
        }
        json_builder_end_array(builder);
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* One file of a character, by agent and path, resolved as office.file is. */
    if (g_strcmp0(kind, "office.character_file") == 0) {
        const gchar *agent = clawt_ipc_payload_string(payload, "agent");
        const gchar *relative = clawt_ipc_payload_string(payload, "path");
        g_autofree gchar *root = clawt_daemon_office_characters_dir(self);
        g_autofree gchar *dir = NULL;
        g_autofree gchar *full = NULL;
        g_autofree gchar *real = NULL;
        g_autofree gchar *real_dir = NULL;
        g_autofree gchar *prefix = NULL;
        g_autofree gchar *data = NULL;
        g_autofree gchar *encoded = NULL;
        gsize length = 0;

        if (agent == NULL || !clawt_office_pack_path_is_safe(agent) ||
            strchr(agent, '/') != NULL || relative == NULL ||
            !clawt_office_pack_path_is_safe(relative))
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "an agent and a path inside its character are required");
        dir = g_build_filename(root, agent, NULL);
        full = g_build_filename(dir, relative, NULL);
        real = realpath(full, NULL);
        real_dir = realpath(dir, NULL);
        if (real == NULL || real_dir == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "that character has no such file");
        prefix = g_strconcat(real_dir, G_DIR_SEPARATOR_S, NULL);
        if (!g_str_has_prefix(real, prefix))
            return clawt_ipc_error_new(request, CLAWT_ERROR_PERMISSION_DENIED,
                                       "that path is outside the character");
        if (!g_file_get_contents(real, &data, &length, &error))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND, error->message);
        if (length > 32 * 1024 * 1024)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "that file is larger than 32 MiB");
        encoded = g_base64_encode((const guchar *)data, length);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "path");
        json_builder_add_string_value(builder, relative);
        json_builder_set_member_name(builder, "size");
        json_builder_add_int_value(builder, (gint64)length);
        json_builder_set_member_name(builder, "base64");
        json_builder_add_string_value(builder, encoded);
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* A character commissioned again, by hand. */
    if (g_strcmp0(kind, "office.character") == 0) {
        g_autofree gchar *task = clawt_daemon_office_commission_character(self,
            clawt_ipc_payload_string(payload, "agent"), &error);

        if (task == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "task");
        json_builder_add_string_value(builder, task);
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                               "unknown office request");
}
