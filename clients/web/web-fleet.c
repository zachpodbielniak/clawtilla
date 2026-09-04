/*
 * web-fleet.c - The sidebar, the topbar, and everything that acts on an agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "web-pages.h"

#include <string.h>

/* ── Shared helpers ──────────────────────────────────────────────── */

gchar *
clawt_web_param(GHashTable *params, const gchar *name)
{
    const gchar *raw;

    if (params == NULL)
        return NULL;

    raw = g_hash_table_lookup(params, name);

    if (raw == NULL)
        return NULL;

    return g_uri_unescape_string(raw, NULL);
}

const gchar *
clawt_web_form_value(HtmxRequest *request, const gchar *name)
{
    const gchar *value = htmx_request_get_form_value(request, name);

    if (value == NULL)
        return NULL;

    return value;
}

gboolean
clawt_web_form_had(HtmxRequest *request, const gchar *name)
{
    g_autofree gchar *marker = g_strdup_printf("%s__present", name);

    return htmx_request_get_form_value(request, marker) != NULL;
}

gboolean
clawt_web_form_flag(HtmxRequest *request, const gchar *name)
{
    const gchar *value = htmx_request_get_form_value(request, name);

    return value != NULL && (g_strcmp0(value, "true") == 0 ||
                             g_strcmp0(value, "on") == 0 ||
                             g_strcmp0(value, "1") == 0);
}

HtmxDiv *
clawt_web_notice(const gchar *text, const gchar *tone)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();

    htmx_element_add_class(HTMX_ELEMENT(box), "notice");

    if (tone != NULL && *tone != '\0') {
        g_autofree gchar *css = g_strdup_printf("notice-%s", tone);

        htmx_element_add_class(HTMX_ELEMENT(box), css);
    }

    htmx_node_set_text_content(HTMX_NODE(box), text != NULL ? text : "");

    return (HtmxDiv *)g_steal_pointer(&box);
}

guint
clawt_web_warnings(HtmxElement *parent, JsonNode *reply)
{
    JsonArray *warnings = clawt_web_member_array(clawt_web_root(reply),
                                                 "warnings");
    guint count = 0;
    guint i;

    if (warnings == NULL)
        return 0;

    for (i = 0; i < json_array_get_length(warnings); i++) {
        const gchar *text = json_array_get_string_element(warnings, i);

        if (text == NULL)
            continue;

        clawt_web_add(parent, clawt_web_notice(text, ""));
        count++;
    }

    return count;
}

gchar *
clawt_web_first_agent(ClawtWebApp *app)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "agent.list", NULL);
    JsonArray *agents = clawt_web_member_array(clawt_web_root(reply), "agents");

    if (agents == NULL || json_array_get_length(agents) == 0)
        return NULL;

    return g_strdup(clawt_web_member(json_array_get_object_element(agents, 0),
                                     "id", NULL));
}

JsonNode *
clawt_web_find_agent(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(ClawtWebPayload) payload = NULL;

    if (agent_id == NULL)
        return NULL;

    payload = clawt_web_payload_new();
    clawt_web_payload_set(payload, "agent", agent_id);

    return clawt_web_app_call(app, "agent.show",
                              clawt_web_payload_take(g_steal_pointer(&payload)));
}

/* ── The sidebar ─────────────────────────────────────────────────── */

static HtmxElement *
agent_row(JsonObject *agent, const gchar *selected, ClawtPage view,
          guint unread, gboolean show_description)
{
    const gchar *id = clawt_web_member(agent, "id", "?");
    const gchar *name = clawt_web_member(agent, "name", id);
    const gchar *state = clawt_web_member(agent, "state", "stopped");
    const gchar *caps = clawt_web_member(agent, "caps", "");
    gint64 depth = clawt_web_member_int(agent, "mailbox_depth", 0);
    g_autofree gchar *url = clawt_web_agent_url(id, view);
    g_autoptr(HtmxA) row = htmx_a_new_with_href(url);
    g_autoptr(HtmxDiv) line = htmx_div_new();
    g_autoptr(HtmxDiv) meta = htmx_div_new();
    g_autoptr(HtmxSpan) dot = htmx_span_new();
    g_autoptr(HtmxSpan) label = htmx_span_new();
    g_autofree gchar *dot_class = NULL;

    htmx_element_add_class(HTMX_ELEMENT(row), "agent-row");

    if (g_strcmp0(id, selected) == 0)
        htmx_element_add_class(HTMX_ELEMENT(row), "selected");

    htmx_element_add_class(HTMX_ELEMENT(line), "agent-line");

    /*
     * The face, drawn through the one builder the transcript also
     * calls -- clawt_web_avatar() -- so picking an agent out of the
     * sidebar and reading its turns cannot disagree about what it
     * looks like.
     */
    clawt_web_add(
        line,
        clawt_web_avatar(name, id,
                         clawt_web_member_bool(agent, "has_avatar", FALSE),
                         clawt_web_member(agent, "color", NULL),
                         "agent-face"));

    dot_class = g_strdup_printf("dot-%s", clawt_web_state_tone(state));
    htmx_element_add_class(HTMX_ELEMENT(dot), "dot");
    htmx_element_add_class(HTMX_ELEMENT(dot), dot_class);
    htmx_node_add_child(HTMX_NODE(line), HTMX_NODE(dot));

    htmx_element_add_class(HTMX_ELEMENT(label), "agent-name");
    htmx_node_set_text_content(HTMX_NODE(label), name);
    htmx_node_add_child(HTMX_NODE(line), HTMX_NODE(label));

    /*
     * Who may put work on somebody's list.
     *
     * Through the library, which is where the "never both, the chief
     * wins" rule lives -- the GTK sidebar draws the same pair, and two
     * answers to "is this agent a lead" would differ exactly once and
     * report it to nobody, since a badge that is not drawn looks like
     * an agent that does not have the role.
     *
     * No `default:`: a standing added to the enum draws a -Wswitch
     * warning here rather than quietly going unmarked.
     */
    switch (clawt_team_badge_for(
                clawt_web_member_bool(agent, "chief_of_staff", FALSE),
                clawt_web_member(agent, "team_role", NULL))) {
    case CLAWT_TEAM_BADGE_CHIEF:
        clawt_web_add(line, clawt_web_badge("chief", "info"));
        break;
    case CLAWT_TEAM_BADGE_LEAD:
        clawt_web_add(line, clawt_web_badge("lead", "good"));
        break;
    case CLAWT_TEAM_BADGE_NONE:
        break;
    }

    /*
     * What has arrived from this agent since its conversation was last
     * opened -- and *not* the queue depth below, which is the opposite
     * thing: work waiting for the agent to read rather than for you.
     *
     * A filled pill, because everything else in this row is an outlined
     * badge: filled means for you, outlined means about the agent.  The
     * row also gains a class that bolds the name, so colour is never the
     * only signal.
     */
    if (unread > 0) {
        g_autoptr(HtmxSpan) pill = htmx_span_new();
        g_autofree gchar *text = g_strdup_printf("%u", unread);

        htmx_element_add_class(HTMX_ELEMENT(row), "is-unread");
        htmx_element_add_class(HTMX_ELEMENT(pill), "unread-badge");
        htmx_element_set_attribute(HTMX_ELEMENT(pill), "title",
                                   unread == 1
                                       ? "1 message you have not read"
                                       : "messages you have not read");
        htmx_node_set_text_content(HTMX_NODE(pill), text);
        htmx_node_add_child(HTMX_NODE(line), HTMX_NODE(pill));
    }

    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(line));

    htmx_element_add_class(HTMX_ELEMENT(meta), "agent-meta");
    clawt_web_add(meta, clawt_web_badge(state, clawt_web_state_tone(state)));

    /*
     * The queue depth, because a stopped agent with work waiting for it
     * is the case somebody most needs to see from a list -- it is the
     * difference between an agent nobody is using and one that is
     * holding up whatever asked it for something.
     */
    if (depth > 0) {
        g_autofree gchar *text =
            g_strdup_printf("%" G_GINT64_FORMAT " waiting", depth);

        clawt_web_add(meta, clawt_web_badge(text, "warn"));
    }

    /*
     * A HOST agent runs on the real machine. It is the one capability
     * worth stating in a list rather than in an inspector somebody has
     * to open.
     */
    if (caps != NULL && strstr(caps, "host-control") != NULL)
        clawt_web_add(meta, clawt_web_badge("host", "bad"));

    /*
     * What it is doing, through the same rule the GTK sidebar and the
     * CLI use.  This badge said "busy" and dropped `peer` on the floor,
     * so a page that had the answer on the wire could not say who an
     * agent was working for.
     */
    {
        g_autofree gchar *work = clawt_agent_activity_label(
            clawt_web_member_bool(agent, "busy", FALSE),
            clawt_web_member(agent, "peer", NULL));
        g_autofree gchar *hold = clawt_hold_label(
            clawt_web_member_bool(agent, "held", FALSE),
            clawt_web_member_bool(agent, "draining", FALSE));

        /*
         * The hold first and in a colour that is not the working one: a
         * draining agent is finishing its last turn and will not start
         * another, which is the opposite of what "busy" usually means
         * about whether more is coming.
         */
        if (hold != NULL)
            clawt_web_add(meta, clawt_web_badge(hold, "warn"));

        if (work != NULL)
            clawt_web_add(meta, clawt_web_badge(work, "info"));
    }

    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(meta));

    /*
     * What the agent is for, under its name.
     *
     * New here.  The GTK sidebar has written the description under
     * every row since it was first drawn and this client never did, so
     * the two answered "which of these do I ask about the build?"
     * differently -- and `make parity` could not see it, because a
     * subtitle sends no frame, answers no command and spells out no
     * library value.  It is drawn from the same `description` the GTK
     * row reads and clipped by the stylesheet rather than here, so the
     * two cannot disagree about where a sentence stops.
     *
     * Off, it becomes the row's `title` rather than disappearing: the
     * complaint this setting answers is that ten paragraphs do not fit
     * on a screen, not that the paragraphs are unwanted.
     */
    {
        const gchar *description = clawt_web_member(agent, "description",
                                                    "");

        if (*description != '\0') {
            if (show_description) {
                g_autoptr(HtmxDiv) about = htmx_div_new();

                htmx_element_add_class(HTMX_ELEMENT(about), "agent-desc");
                htmx_node_set_text_content(HTMX_NODE(about), description);
                htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(about));
            } else {
                htmx_element_set_attribute(HTMX_ELEMENT(row), "title",
                                           description);
            }
        }
    }

    return HTMX_ELEMENT(g_steal_pointer(&row));
}


/*
 * A room's label, from its members rather than its id.
 *
 * How a room is named is the daemon's business, and a client that takes
 * `dm:a:b` apart is a client that breaks when that changes.
 *
 * Returns: (transfer full): the label
 */
static gchar *
room_label(JsonObject *room)
{
    const gchar *id = clawt_web_member(room, "id", "");
    const gchar *name = clawt_web_member(room, "name", NULL);
    JsonArray *members;
    g_autoptr(GString) out = NULL;
    guint i;

    if (name != NULL && *name != '\0' && g_strcmp0(name, id) != 0)
        return g_strdup(name);

    members = clawt_web_member_array(room, "members");

    if (members == NULL)
        return g_strdup(id);

    out = g_string_new(NULL);

    for (i = 0; i < json_array_get_length(members); i++) {
        const gchar *member = json_array_get_string_element(members, i);

        if (g_strcmp0(member, "user") == 0)
            continue;

        if (out->len > 0)
            g_string_append(out, ", ");

        g_string_append(out, member);
    }

    if (out->len == 0)
        return g_strdup(id);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * One room in the sidebar, beside the agents it concerns.
 *
 * A room has exactly one view -- every other page is scoped to an agent
 * and means nothing here -- so the href carries no view segment, and
 * clawt_web_view_body()'s switch is left alone rather than gaining a
 * page only one kind of entry can answer.
 */
static HtmxElement *
room_row(JsonObject *room, const gchar *selected_room, guint unread)
{
    const gchar *id = clawt_web_member(room, "id", "");
    g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
    g_autofree gchar *url = g_strdup_printf("/r/%s", escaped);
    g_autofree gchar *label = room_label(room);
    g_autofree gchar *meta_text = NULL;
    JsonArray *members = clawt_web_member_array(room, "members");
    guint count = (members != NULL) ? json_array_get_length(members) : 0;
    g_autoptr(HtmxA) row = htmx_a_new_with_href(url);
    g_autoptr(HtmxDiv) line = htmx_div_new();
    g_autoptr(HtmxDiv) meta = htmx_div_new();
    g_autoptr(HtmxSpan) name = htmx_span_new();

    htmx_element_add_class(HTMX_ELEMENT(row), "agent-row");

    if (g_strcmp0(id, selected_room) == 0)
        htmx_element_add_class(HTMX_ELEMENT(row), "selected");

    htmx_element_add_class(HTMX_ELEMENT(line), "agent-line");
    htmx_element_add_class(HTMX_ELEMENT(name), "agent-name");
    htmx_node_set_text_content(HTMX_NODE(name), label);
    htmx_node_add_child(HTMX_NODE(line), HTMX_NODE(g_steal_pointer(&name)));

    if (unread > 0) {
        g_autofree gchar *badge = g_strdup_printf("%u", unread);

        clawt_web_add(line, clawt_web_badge(badge, "accent"));
    }

    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(g_steal_pointer(&line)));

    /*
     * How many are in it, and whether being in it means answering
     * everything -- the second being what somebody needs to know about a
     * room before they type in it.
     */
    meta_text = clawt_web_member_bool(room, "require_mention", FALSE)
        ? g_strdup_printf("%u members \xc2\xb7 answers when named", count)
        : g_strdup_printf("%u members \xc2\xb7 everyone answers", count);

    htmx_element_add_class(HTMX_ELEMENT(meta), "agent-meta");
    htmx_node_set_text_content(HTMX_NODE(meta), meta_text);
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(g_steal_pointer(&meta)));

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

/*
 * Every room in @team's group, after that team's agents.
 *
 * Rooms sit with the agents they concern rather than in a section of
 * their own, which is what "move it wherever you like" has to mean if a
 * team's group is somewhere one can be put.
 */
static void
append_rooms_for_team(ClawtWebApp *app, HtmxDiv *scroll, JsonArray *rooms,
                      const gchar *team, const gchar *selected_room,
                      GHashTable *emitted)
{
    guint i;

    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);
        const gchar *id = clawt_web_member(room, "id", "");

        /*
         * Only rooms somebody made.  A direct conversation is already
         * the agent's own row, and a routine's or a trigger's belongs to
         * that routine or trigger.
         */
        if (!clawt_web_member_bool(room, "declared", FALSE))
            continue;

        if (g_strcmp0(clawt_web_member(room, "team", ""),
                      team != NULL ? team : "") != 0)
            continue;

        if (g_hash_table_contains(emitted, id))
            continue;

        g_hash_table_add(emitted, g_strdup(id));

        clawt_web_add(scroll,
                      room_row(room, selected_room,
                               clawt_web_app_unread(app, id)));

        /*
         * Move controls for the row somebody is looking at, in the same
         * strip an agent gets.  The GTK client drags; a button is the
         * same capability on a page that has to work without
         * JavaScript, and both write the same key.
         */
        if (g_strcmp0(id, selected_room) == 0) {
            g_autoptr(HtmxDiv) move = htmx_div_new();
            g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
            g_autofree gchar *up =
                g_strdup_printf("/r/%s/reorder?direction=up", escaped);
            g_autofree gchar *down =
                g_strdup_printf("/r/%s/reorder?direction=down", escaped);

            htmx_element_add_class(HTMX_ELEMENT(move), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(move), "style",
                                       "padding:4px 20px 8px");
            clawt_web_add(move, clawt_web_post_button("Move up", up,
                                                      "default", NULL));
            clawt_web_add(move, clawt_web_post_button("Move down", down,
                                                      "default", NULL));
            htmx_node_add_child(HTMX_NODE(scroll), HTMX_NODE(move));
        }
    }
}

static const gchar *
team_display_name(JsonArray *teams, const gchar *team_id)
{
    guint i;

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = json_array_get_object_element(teams, i);

        if (g_strcmp0(clawt_web_member(team, "id", ""), team_id) == 0)
            return clawt_web_member(team, "name", team_id);
    }

    /*
     * An agent on a team nobody declared still gets a heading, under the
     * id it named. Hiding it is how a typo in `agents.team` survives
     * being looked at.
     */
    return team_id;
}

static HtmxElement *
team_header(JsonArray *teams, JsonArray *agents, const gchar *team_id)
{
    g_autoptr(HtmxDiv) head = htmx_div_new();
    g_autoptr(HtmxSpan) name = htmx_span_new();
    g_autoptr(HtmxSpan) tally = htmx_span_new();
    g_autofree gchar *text = NULL;
    guint total = 0;
    guint running = 0;
    guint busy = 0;

    clawt_team_tally(agents, team_id, &total, &running, &busy);

    htmx_element_add_class(HTMX_ELEMENT(head), "team-head");

    htmx_element_add_class(HTMX_ELEMENT(name), "team-name");
    htmx_node_set_text_content(HTMX_NODE(name),
                               (team_id != NULL && *team_id != '\0')
                               ? team_display_name(teams, team_id)
                               : "No team");
    htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(name));

    /*
     * A busy badge when anybody on the team is mid-turn, which is what
     * the agent rows underneath already draw.  The GTK client spins
     * here; this one badges, because that is what each of them says on
     * a row -- the two clients read as one language by matching their
     * own rows, not by borrowing each other's widgets.
     *
     * It matters most on a heading.  A folded team hides exactly the
     * rows that would have shown it.
     */
    if (busy > 0) {
        HtmxSpan *mark = clawt_web_badge("working", "info");

        /*
         * Named so `make parity` can see it, the same as the GTK
         * spinner's class.  "working" alone is not a marker for this
         * badge -- the tally string beside it contains the same word,
         * so the check would pass with the badge deleted.
         */
        htmx_element_add_class(HTMX_ELEMENT(mark), "clawt-team-busy");
        clawt_web_add(head, mark);
    }

    /*
     * How many of those are working, when any are.  One busy out of
     * seven is a different fleet state from seven out of seven, and a
     * bare badge collapses the two.  Left off entirely at zero rather
     * than drawn as "0 working", which is a count nobody is looking for
     * taking space on every idle heading.
     */
    text = (busy > 0) ? g_strdup_printf("%u working \302\267 %u/%u",
                                        busy, running, total)
                      : g_strdup_printf("%u/%u", running, total);
    htmx_element_add_class(HTMX_ELEMENT(tally), "team-tally");
    htmx_node_set_text_content(HTMX_NODE(tally), text);
    htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(tally));

    return HTMX_ELEMENT(g_steal_pointer(&head));
}

/*
 * The headings for declared teams that sort before @next and have nobody
 * in them.
 *
 * The order is the daemon's rather than a second opinion about it:
 * teamless first, then the declared teams in the order team.list gives
 * them, which is the array the daemon's own group_position() indexes
 * into.  @next of %NULL means "everything that is left", which is how
 * the teams nobody is on reach the bottom.
 *
 * The teamless group is never emitted *here*, because it is never
 * empty in the sense this function means: it has no declared team
 * behind it, so there is nothing to draw a heading for until an agent
 * turns up in it.  The heading it does get is drawn by the loop below,
 * beside the first agent that is in no team.
 *
 * That sentence used to read "this sidebar has never given it one --
 * teamless agents simply come first, unlabelled", which described the
 * behaviour rather than arguing for it, and stopped being true in the
 * same change that added the heading.
 */
static void
emit_empty_headers_before(HtmxDiv *scroll, JsonArray *teams,
                          JsonArray *agents, const gchar *next,
                          GHashTable *shown)
{
    gboolean to_the_end = (next == NULL);
    guint i;

    /*
     * Nothing sorts before the teamless group, and its id is "" -- which
     * no declared team has, so without this the loop would run to the
     * end and draw every heading above the first teamless agent.  The
     * GTK sidebar had the identical bug from the identical cause, and
     * there it was visible: four headings in a row above the fleet.
     */
    if (!to_the_end && *next == '\0')
        return;

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = json_array_get_object_element(teams, i);
        const gchar *id = clawt_web_member(team, "id", "");

        if (*id == '\0')
            continue;

        if (!to_the_end && g_strcmp0(id, next) == 0)
            return;

        if (g_hash_table_contains(shown, id))
            continue;

        clawt_web_add(scroll, team_header(teams, agents, id));
        g_hash_table_add(shown, g_strdup(id));
    }
}

/*
 * "Move to <team>" for one agent, as a select and a button.
 *
 * Every team the fleet declares, plus "No team" -- which is a choice
 * rather than a prompt, because it is how an agent comes off a team.
 * A team an agent names that nobody declared is kept by
 * clawt_web_select_field(), so pressing Move without touching the list
 * cannot quietly reassign it.
 */
static HtmxElement *
team_picker(JsonArray *teams, JsonObject *agent)
{
    const gchar *id = clawt_web_member(agent, "id", "");
    const gchar *current = clawt_web_member(agent, "team", "");
    g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
    g_autofree gchar *action = g_strdup_printf("/a/%s/team", escaped);
    g_autoptr(HtmxForm) form = clawt_web_form(action);
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    guint i;

    g_ptr_array_add(ids, g_strdup(""));
    g_ptr_array_add(names, g_strdup("No team"));

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = json_array_get_object_element(teams, i);
        const gchar *team_id = clawt_web_member(team, "id", NULL);

        if (team_id == NULL || *team_id == '\0')
            continue;

        g_ptr_array_add(ids, g_strdup(team_id));
        g_ptr_array_add(names, g_strdup(clawt_web_member(team, "name",
                                                         team_id)));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(names, NULL);

    htmx_element_set_attribute(HTMX_ELEMENT(form), "style",
                               "padding:0 20px 12px");

    clawt_web_add(form, clawt_web_select_field(
        "Team", "team", (const gchar *const *)ids->pdata,
        (const gchar *const *)names->pdata, current));

    htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");

    {
        g_autoptr(HtmxButton) go = clawt_web_button("Move", "default");

        htmx_element_set_attribute(HTMX_ELEMENT(go), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(go));
    }

    htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));

    return HTMX_ELEMENT(g_steal_pointer(&form));
}

HtmxElement *
clawt_web_sidebar(ClawtWebApp *app, const gchar *selected, ClawtPage view,
                  const ClawtWebLook *look)
{
    /*
     * A NULL look is the shipped behaviour, not "hide": this is
     * reachable from a request built with no message at all, and the
     * field's zero already means show.
     */
    gboolean show_description = (look == NULL || !look->hide_descriptions);

    g_autoptr(HtmxElement) aside = HTMX_ELEMENT(htmx_aside_new());
    g_autoptr(HtmxDiv) head = htmx_div_new();
    g_autoptr(HtmxDiv) scroll = htmx_div_new();
    g_autoptr(HtmxDiv) foot = htmx_div_new();
    g_autoptr(HtmxHeading) wordmark = htmx_heading_new(1);
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonNode) teams_reply = NULL;
    g_autoptr(JsonNode) rooms_reply = NULL;
    JsonArray *agents;
    JsonArray *teams;
    JsonArray *rooms = NULL;

    /*
     * Which sidebar entry is a room rather than an agent.  The two are
     * exclusive: a room is a first-class entry, not one of a selected
     * agent's conversations, and `selected` names an agent.
     */
    const gchar *selected_room = clawt_web_app_get_viewing_room(app);
    const gchar *current_team = NULL;
    g_autoptr(GHashTable) shown = g_hash_table_new_full(g_str_hash,
                                                        g_str_equal,
                                                        g_free, NULL);
    g_autoptr(GHashTable) rooms_shown = g_hash_table_new_full(g_str_hash,
                                                              g_str_equal,
                                                              g_free, NULL);
    gboolean first = TRUE;
    guint i;

    htmx_element_add_class(aside, "sidebar");
    htmx_element_set_id(aside, "sidebar");

    /*
     * Re-fetched whenever the daemon says anything happened. The sidebar
     * shows state, queue depth and busy for every agent at once, so it is
     * the surface that goes stale fastest and the one somebody is most
     * likely to be looking at while it does.
     */
    htmx_element_set_attribute(aside, "hx-get", "/f/sidebar");
    htmx_element_set_attribute(aside, "hx-trigger", "sse:fleet");
    htmx_element_set_attribute(aside, "hx-swap", "outerHTML");

    htmx_element_add_class(HTMX_ELEMENT(head), "sidebar-head");
    htmx_element_add_class(HTMX_ELEMENT(wordmark), "wordmark");
    htmx_node_set_text_content(HTMX_NODE(wordmark), "clawtilla");
    htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(wordmark));

    /*
     * Beside Settings, where this client already puts what is about the
     * whole fleet rather than one agent.  Recall searches every room and
     * the operator profile is the same for all of them, so neither
     * belongs under an agent's tabs.
     */
    {
        g_autoptr(HtmxA) memory = htmx_a_new_with_href("/memory");

        htmx_element_add_class(HTMX_ELEMENT(memory), "tab");
        htmx_node_set_text_content(HTMX_NODE(memory), "Memory");
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(memory));
    }

    {
        g_autoptr(HtmxA) settings = htmx_a_new_with_href("/settings");

        htmx_element_add_class(HTMX_ELEMENT(settings), "tab");
        htmx_node_set_text_content(HTMX_NODE(settings), "Settings");
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(settings));
    }

    /*
     * And what has happened, with a count.
     *
     * The GTK client's bell sits in the header bar; here it sits beside
     * Settings, which is where this client already puts the things that
     * are about the whole fleet rather than about one agent.
     */
    {
        g_autoptr(HtmxA) alerts = htmx_a_new_with_href("/alerts");
        guint waiting = clawt_web_app_alert_count(app);

        htmx_element_add_class(HTMX_ELEMENT(alerts), "tab");
        htmx_node_set_text_content(HTMX_NODE(alerts), "Alerts");

        if (waiting > 0) {
            g_autoptr(HtmxSpan) pill = htmx_span_new();
            /*
             * Capped at 9+ here, unlike the sidebar's unread count: this
             * sits on a small control rather than at the end of a row.
             */
            g_autofree gchar *text = (waiting > 9)
                ? g_strdup("9+") : g_strdup_printf("%u", waiting);

            htmx_element_add_class(HTMX_ELEMENT(pill), "unread-badge");
            htmx_node_set_text_content(HTMX_NODE(pill), text);
            htmx_node_add_child(HTMX_NODE(alerts), HTMX_NODE(pill));
        }

        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(alerts));
    }

    htmx_node_add_child(HTMX_NODE(aside), HTMX_NODE(head));

    htmx_element_add_class(HTMX_ELEMENT(scroll), "sidebar-scroll");

    reply = clawt_web_app_call(app, "agent.list", NULL);
    teams_reply = clawt_web_app_call(app, "team.list", NULL);

    /*
     * And the rooms, already ordered by the daemon.  Sorting them here
     * would be a second answer to what order the sidebar is in, and the
     * two clients would differ exactly once.
     */
    rooms_reply = clawt_web_app_call(app, "room.list", NULL);

    agents = clawt_web_member_array(clawt_web_root(reply), "agents");
    teams = clawt_web_member_array(clawt_web_root(teams_reply), "teams");
    rooms = clawt_web_member_array(clawt_web_root(rooms_reply), "rooms");

    /*
     * Which room is whose conversation, learned here because this is
     * where the fleet is listed anyway.  The event handler cannot ask
     * the daemon itself: a request from there would run while a page
     * render is blocked inside its own request on the same context.
     */
    clawt_web_app_note_fleet(app, agents);

    if (agents == NULL) {
        const gchar *why = clawt_web_app_last_error(app);

        clawt_web_add(scroll,
                      clawt_web_empty("The daemon is not answering",
                                      why != NULL ? why : NULL));
    } else if (json_array_get_length(agents) == 0) {
        clawt_web_add(scroll,
                      clawt_web_empty("No agents yet",
                                      "Add one below, or import a workspace "
                                      "that already exists."));
    }

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *team = clawt_web_member(agent, "team", "");

        /*
         * A heading is emitted when the team changes, which works only
         * because the daemon returns the fleet already grouped. Teamless
         * agents come first -- that is where the chief of staff lives,
         * and putting them last would bury the agent somebody talks to
         * most under every team in the fleet.
         */
        if (first || g_strcmp0(team, current_team) != 0) {
            /*
             * The outgoing team's rooms, before the next heading takes
             * the floor -- so a room sits under the team it was put in
             * rather than at the end of the list.
             */
            if (!first)
                append_rooms_for_team(app, scroll, rooms, current_team,
                                      selected_room, rooms_shown);

            emit_empty_headers_before(scroll, teams, agents,
                                      team != NULL ? team : "", shown);

            /*
             * The teamless group gets a heading too, as it already does
             * in the GTK sidebar.  It was the one group without one --
             * and it is where the chief of staff lives, so "is anybody
             * working" is at least as worth saying there as on a team.
             *
             * It is also the only change here that adds a row to a
             * sidebar rather than decorating one that was already
             * drawn.
             */
            clawt_web_add(scroll,
                          team_header(teams, agents,
                                      team != NULL ? team : ""));
            g_hash_table_add(shown, g_strdup(team != NULL ? team : ""));

            current_team = team;
            first = FALSE;
        }

        clawt_web_add(scroll,
                      agent_row(agent, selected, view,
                                clawt_web_app_unread(
                                    app,
                                    clawt_web_member(agent, "dm_room", "")),
                                show_description));

        /*
         * Shown only for the row somebody is looking at, so the sidebar
         * is a list of agents rather than a list of buttons. Ordering
         * lives in clawtilla.yaml, so an agent moved here is moved in
         * the GTK client too.
         */
        if (g_strcmp0(clawt_web_member(agent, "id", ""), selected) == 0) {
            g_autoptr(HtmxDiv) move = htmx_div_new();
            g_autofree gchar *escaped = g_uri_escape_string(
                clawt_web_member(agent, "id", ""), NULL, FALSE);
            g_autofree gchar *up = g_strdup_printf(
                "/a/%s/reorder?direction=up", escaped);
            g_autofree gchar *down = g_strdup_printf(
                "/a/%s/reorder?direction=down", escaped);

            htmx_element_add_class(HTMX_ELEMENT(move), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(move), "style",
                                       "padding:4px 20px 8px");
            clawt_web_add(move, clawt_web_post_button("Move up", up,
                                                      "default", NULL));
            clawt_web_add(move, clawt_web_post_button("Move down", down,
                                                      "default", NULL));
            htmx_node_add_child(HTMX_NODE(scroll), HTMX_NODE(move));

            /*
             * And which team it is on, in the same strip. The GTK client
             * drags an agent onto a heading; a select and a button is
             * the same operation for a page that has to work without
             * JavaScript, and it writes the same `agents.team`.
             */
            clawt_web_add(scroll, team_picker(teams, agent));
        }
    }

    /*
     * And whatever the fleet declares that nobody is on yet.  A team
     * created in Settings and then nowhere in the sidebar reads as a
     * team that was not created -- which is exactly when somebody goes
     * looking for it, because filling it is the next thing they meant
     * to do.
     */
    /* The last team's rooms, which no following heading will flush. */
    append_rooms_for_team(app, scroll, rooms, current_team, selected_room,
                          rooms_shown);

    if (agents != NULL)
        emit_empty_headers_before(scroll, teams, agents, NULL, shown);

    /*
     * And any room whose team holds no agents, which the loop above
     * never reaches: it walks agents, so a team with none of them is a
     * heading it never stands under.  A room that vanished because the
     * last agent left its team would read as a room that was deleted.
     */
    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++)
        append_rooms_for_team(
            app, scroll, rooms,
            clawt_web_member(json_array_get_object_element(rooms, i),
                             "team", ""),
            selected_room, rooms_shown);

    htmx_node_add_child(HTMX_NODE(aside), HTMX_NODE(scroll));

    htmx_element_add_class(HTMX_ELEMENT(foot), "sidebar-foot");

    {
        g_autoptr(HtmxA) add = htmx_a_new_with_href("/new");
        g_autoptr(HtmxA) import = htmx_a_new_with_href("/import");

        htmx_element_add_class(HTMX_ELEMENT(add), "btn");
        htmx_element_add_class(HTMX_ELEMENT(add), "btn-primary");
        htmx_node_set_text_content(HTMX_NODE(add), "New agent");
        htmx_node_add_child(HTMX_NODE(foot), HTMX_NODE(add));

        htmx_element_add_class(HTMX_ELEMENT(import), "btn");
        htmx_node_set_text_content(HTMX_NODE(import), "Import");
        htmx_node_add_child(HTMX_NODE(foot), HTMX_NODE(import));
    }

    htmx_node_add_child(HTMX_NODE(aside), HTMX_NODE(foot));

    return g_steal_pointer(&aside);
}

/* ── The topbar ──────────────────────────────────────────────────── */

static void
add_lifecycle_buttons(HtmxElement *parent, const gchar *agent_id,
                      const gchar *state)
{
    g_autofree gchar *base = g_uri_escape_string(agent_id, NULL, FALSE);
    gboolean stopped = (g_strcmp0(state, "stopped") == 0 ||
                        g_strcmp0(state, "error") == 0);

    if (stopped) {
        g_autofree gchar *action = g_strdup_printf("/a/%s/start", base);

        clawt_web_add(parent,
                      clawt_web_post_button("Start", action, "primary", NULL));
    } else {
        g_autofree gchar *stop = g_strdup_printf("/a/%s/stop", base);
        g_autofree gchar *restart = g_strdup_printf("/a/%s/restart", base);

        clawt_web_add(parent,
                      clawt_web_post_button("Stop", stop, "default", NULL));
        clawt_web_add(parent,
                      clawt_web_post_button("Restart", restart, "default",
                                            NULL));
    }
}

/*
 * The pill a section tab carries.
 *
 * One builder for both, because two would be two spellings of the same
 * class and the second would be the one that stopped matching the
 * stylesheet.
 */
static void
add_tab_badge(HtmxElement *tab, guint count)
{
    g_autoptr(HtmxSpan) pill = htmx_span_new();
    g_autofree gchar *text = g_strdup_printf("%u", count);

    htmx_element_add_class(HTMX_ELEMENT(pill), "unread-badge");
    htmx_node_set_text_content(HTMX_NODE(pill), text);
    htmx_node_add_child(HTMX_NODE(tab), HTMX_NODE(pill));
}

HtmxElement *
clawt_web_topbar(ClawtWebApp *app, const gchar *agent_id, ClawtPage view)
{
    g_autoptr(HtmxElement) bar = HTMX_ELEMENT(htmx_header_new());
    g_autoptr(HtmxHeading) title = htmx_heading_new(2);
    g_autoptr(HtmxElement) tabs = HTMX_ELEMENT(htmx_nav_new());
    g_autoptr(HtmxDiv) spacer = htmx_div_new();
    g_autoptr(HtmxDiv) actions = htmx_div_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *agent = NULL;
    guint i;

    htmx_element_add_class(bar, "topbar");

    /*
     * Shown only at phone width, where the sidebar is a drawer.  It is
     * built on every page because the topbar is what every view has in
     * common; the stylesheet decides whether it is visible, so there is
     * no width to guess at on the server.
     */
    {
        g_autoptr(HtmxLabel) nav = htmx_label_new_for("nav-open");

        htmx_element_add_class(HTMX_ELEMENT(nav), "nav-button");
        htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label",
                                   "Show the agent list");
        /* U+2630 TRIGRAM FOR HEAVEN, as octal bytes: gnu89 has no \u. */
        htmx_node_set_text_content(HTMX_NODE(nav), "\342\230\260");
        htmx_node_add_child(HTMX_NODE(bar), HTMX_NODE(nav));
    }

    if (agent_id == NULL) {
        htmx_element_add_class(HTMX_ELEMENT(title), "topbar-title");
        htmx_node_set_text_content(HTMX_NODE(title), "clawtilla");
        htmx_node_add_child(HTMX_NODE(bar), HTMX_NODE(title));

        return g_steal_pointer(&bar);
    }

    reply = clawt_web_find_agent(app, agent_id);
    agent = clawt_web_member_object(clawt_web_root(reply), "agent");

    htmx_element_add_class(HTMX_ELEMENT(title), "topbar-title");
    htmx_node_set_text_content(HTMX_NODE(title),
                               clawt_web_member(agent, "name", agent_id));
    htmx_node_add_child(HTMX_NODE(bar), HTMX_NODE(title));

    htmx_element_add_class(tabs, "tabs");

    /*
     * Six section tabs, not eleven page tabs.
     *
     * Walked out of clawt_section_count() rather than listed, so the
     * browser cannot come to file a page under a different heading than
     * the window does -- and so a page added to the library appears in
     * both or in neither.  The row of pages inside a section is drawn by
     * clawt_web_section_subnav(), under the bar rather than in it.
     */
    for (i = 0; i < clawt_section_count(); i++) {
        ClawtSection section = clawt_section_nth(i);
        g_autofree gchar *url =
            clawt_web_agent_url(agent_id,
                                clawt_section_default_page(section));
        g_autoptr(HtmxA) tab = htmx_a_new_with_href(url);

        htmx_element_add_class(HTMX_ELEMENT(tab), "tab");
        htmx_node_set_text_content(HTMX_NODE(tab),
                                   clawt_section_nth_label(i));

        if (section == clawt_page_section(view))
            htmx_element_set_attribute(HTMX_ELEMENT(tab), "aria-current",
                                       "page");

        /*
         * The fleet-wide total on Chat, and not while Chat is what you
         * are reading: the sidebar beside it is saying exactly *which*
         * agent, which is strictly more information, and a badge on the
         * page you are looking at is noise.
         */
        if (section == CLAWT_SECTION_CHAT && view != CLAWT_PAGE_CHAT) {
            guint total = clawt_web_app_unread_total(app);

            if (total > 0)
                add_tab_badge(HTMX_ELEMENT(tab), total);
        }

        /*
         * And how many decisions are waiting, on the section that holds
         * them.
         *
         * A badge on the page itself would be invisible until somebody
         * opened Work, which for this number defeats the point of having
         * one: an agent waiting on a person has to be visible without
         * anybody thinking to look.  The GTK client sums its section's
         * pages for the same reason.
         *
         * Drawn while Work is open too, unlike the unread total: the
         * sidebar does not say how many decisions there are, so nothing
         * else on screen carries it.
         */
        if (section == CLAWT_SECTION_WORK) {
            guint waiting = clawt_web_app_open_decisions(app);

            if (waiting > 0)
                add_tab_badge(HTMX_ELEMENT(tab), waiting);
        }

        htmx_node_add_child(HTMX_NODE(tabs), HTMX_NODE(tab));
    }

    htmx_node_add_child(HTMX_NODE(bar), HTMX_NODE(tabs));

    htmx_element_add_class(HTMX_ELEMENT(spacer), "topbar-spacer");
    htmx_node_add_child(HTMX_NODE(bar), HTMX_NODE(spacer));

    htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
    add_lifecycle_buttons(HTMX_ELEMENT(actions), agent_id,
                          clawt_web_member(agent, "state", "stopped"));
    htmx_node_add_child(HTMX_NODE(bar), HTMX_NODE(actions));

    return g_steal_pointer(&bar);
}

/* ── Dispatch ────────────────────────────────────────────────────── */
HtmxElement *
clawt_web_view_body(ClawtWebApp *app, const gchar *agent_id, ClawtPage view)
{
    /*
     * No `default:`, so -Wswitch names a page added to the library that
     * this client does not draw.  It used to fall through to the chat
     * page, which meant a tab the GTK client had and this one did not
     * answered 200 with somebody else's conversation.
     */
    switch (view) {
    case CLAWT_PAGE_CHAT:
        return clawt_web_chat_body(app, agent_id);
    case CLAWT_PAGE_AGENT:
        return clawt_web_agent_body(app, agent_id);
    case CLAWT_PAGE_MAILBOX:
        return clawt_web_mailbox_body(app, agent_id);
    case CLAWT_PAGE_COMPUTER:
        return clawt_web_computer_body(app, agent_id);
    case CLAWT_PAGE_ROUTINES:
        return clawt_web_routines_body(app, agent_id);
    case CLAWT_PAGE_TRIGGERS:
        return clawt_web_triggers_body(app, agent_id);
    case CLAWT_PAGE_TASKS:
        return clawt_web_tasks_body(app, agent_id);
    case CLAWT_PAGE_DECISIONS:
        return clawt_web_decisions_body(app, agent_id);
    case CLAWT_PAGE_FLOW:
        return clawt_web_flow_body(app, agent_id);
    case CLAWT_PAGE_SKILLS:
        return clawt_web_skills_body(app, agent_id);
    case CLAWT_PAGE_MEMORY:
        return clawt_web_memory_body(app, agent_id);
    }

    g_return_val_if_reached(clawt_web_chat_body(app, agent_id));
}

/* ── Responses after an action ───────────────────────────────────── */

static HtmxResponse *
page_with_banner(ClawtWebApp *app, HtmxRequest *request, const gchar *agent_id,
                 ClawtPage view, const gchar *text, const gchar *tone)
{
    /*
     * Copied before anything else happens.
     *
     * Callers pass clawt_web_app_last_error(), which points at a string
     * the app object owns -- and building the body below makes half a
     * dozen more daemon calls, each of which frees it and writes a new
     * one. The banner then rendered whatever happened to be at that
     * address: a refusal about a path traversal came out as "Which CLI
     * backend answers, and with what", which is a static string from the
     * inspector's group table.
     */
    g_autofree gchar *said = g_strdup(text);
    g_autoptr(HtmxElement) body = NULL;
    g_autoptr(HtmxDiv) wrap = htmx_div_new();
    g_autofree gchar *html = NULL;

    body = clawt_web_view_body(app, agent_id, view);

    if (said != NULL) {
        g_autoptr(HtmxDiv) toast = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(toast), "toast");

        if (g_strcmp0(tone, "bad") == 0)
            htmx_element_add_class(HTMX_ELEMENT(toast), "notice-bad");

        htmx_node_set_text_content(HTMX_NODE(toast), said);
        htmx_node_add_child(HTMX_NODE(wrap), HTMX_NODE(toast));
    }

    htmx_node_add_child(HTMX_NODE(wrap), HTMX_NODE(body));

    html = clawt_web_page(app, agent_id, view, HTMX_ELEMENT(wrap), request);

    return clawt_web_html_response(html);
}

HtmxResponse *
clawt_web_after_action(ClawtWebApp *app, HtmxRequest *request,
                       const gchar *agent_id, ClawtPage view,
                       const gchar *toast)
{
    /*
     * Copied before anything renders.  page_with_banner() asks the daemon
     * for the sidebar and the topbar, and every one of those calls frees
     * what the app is holding -- the same trap the last_error rule in
     * CLAUDE.md exists for, one field along.
     */
    g_autofree gchar *refused = g_strdup(clawt_web_app_last_refusal(app));

    if (refused != NULL) {
        /*
         * The action did happen, so the note still says so -- but an
         * agent whose files were not written is the part that needs
         * doing something about, so it goes first and the whole banner
         * turns.  Reported here rather than at each of the dozen call
         * sites, for the same reason the GTK client checks in its one
         * request wrapper: a handler that starts carrying refusals later
         * is covered without anybody remembering to look.
         */
        g_autofree gchar *both = (toast != NULL)
            ? g_strdup_printf("%s\n%s", refused, toast)
            : g_strdup(refused);

        return page_with_banner(app, request, agent_id, view, both, "bad");
    }

    return page_with_banner(app, request, agent_id, view, toast, NULL);
}

HtmxResponse *
clawt_web_error_page(ClawtWebApp *app, HtmxRequest *request,
                     const gchar *agent_id, ClawtPage view,
                     const gchar *message)
{
    return page_with_banner(app, request, agent_id, view, message, "bad");
}

/* ── Routes ──────────────────────────────────────────────────────── */

static HtmxResponse *
on_index(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *first = clawt_web_first_agent(app);

    (void)params;

    if (first != NULL) {
        g_autofree gchar *url = clawt_web_agent_url(first,
                                                    CLAWT_PAGE_CHAT);

        return clawt_web_redirect(request, url);
    }

    {
        g_autoptr(HtmxDiv) view = htmx_div_new();
        g_autoptr(HtmxDiv) pad = htmx_div_new();
        g_autofree gchar *html = NULL;

        htmx_element_add_class(HTMX_ELEMENT(view), "view");
        htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

        clawt_web_add(pad, clawt_web_section_title("No agents yet"));
        clawt_web_add(pad, clawt_web_text(
            "A fleet starts with one agent. Create one, or import a "
            "workspace that already exists on this machine.", "lede"));

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxA) add = htmx_a_new_with_href("/new");
            g_autoptr(HtmxA) import = htmx_a_new_with_href("/import");

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_add_class(HTMX_ELEMENT(add), "btn");
            htmx_element_add_class(HTMX_ELEMENT(add), "btn-primary");
            htmx_node_set_text_content(HTMX_NODE(add), "New agent");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(add));

            htmx_element_add_class(HTMX_ELEMENT(import), "btn");
            htmx_node_set_text_content(HTMX_NODE(import), "Import a workspace");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(import));

            htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

        html = clawt_web_page(app, NULL, CLAWT_PAGE_CHAT,
                              HTMX_ELEMENT(view), request);

        return clawt_web_html_response(html);
    }
}

static HtmxResponse *
on_agent_page(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autofree gchar *slug = clawt_web_param(params, "view");
    ClawtPage view = clawt_page_from_nick(slug);
    g_autoptr(HtmxElement) body = NULL;
    g_autofree gchar *html = NULL;

    if (view == CLAWT_PAGE_CHAT) {
        /*
         * `with` names one of this agent's peer conversations. Read here
         * rather than in a route of its own, because this handler
         * already has the request -- and a new route under an agent
         * would have to be registered before /a/:id/:view, which
         * swallows everything.
         */
        const gchar *peer = htmx_request_get_query_param(request, "with");

        body = clawt_web_chat_body_full(
            app, agent_id,
            htmx_request_get_query_param(request, "clear") != NULL, peer);
    } else {
        body = clawt_web_view_body(app, agent_id, view);
    }

    html = clawt_web_page(app, agent_id, view, body, request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_agent_root(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autofree gchar *url = clawt_web_agent_url(agent_id,
                                                CLAWT_PAGE_CHAT);

    (void)user_data;

    return clawt_web_redirect(request, url);
}

static HtmxResponse *
on_sidebar_fragment(HtmxRequest *request, GHashTable *params,
                    gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *selected = htmx_request_get_query_param(request, "agent");
    const gchar *slug = htmx_request_get_query_param(request, "view");
    g_autoptr(ClawtWebLook) look = clawt_web_look_from_request(request);
    g_autoptr(HtmxElement) sidebar = NULL;

    (void)params;

    /*
     * The look is read here as well as in clawt_web_page(), because
     * this fragment replaces the sidebar on every fleet event and
     * nothing about that swap goes through the page renderer.  Without
     * it the setting would hold until the first event arrived and then
     * silently revert -- which reads as the setting not sticking.
     */
    sidebar = clawt_web_sidebar(app, selected,
                                clawt_page_from_nick(slug), look);

    return clawt_web_fragment_response(sidebar);
}

/*
 * One handler for start, stop, restart and reset.
 *
 * They differ only in the frame they send and what they say afterwards,
 * and four near-identical handlers is four places for one of them to
 * forget to report the daemon's refusal.
 */
typedef struct {
    ClawtWebApp *app;
    const gchar *kind;
    const gchar *done;
} LifecycleAction;

static HtmxResponse *
on_lifecycle(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    LifecycleAction *action = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);

    reply = clawt_web_app_call(action->app, action->kind,
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(action->app, request, agent_id,
                                    CLAWT_PAGE_CHAT,
                                    clawt_web_app_last_error(action->app));

    return clawt_web_after_action(action->app, request, agent_id,
                                  CLAWT_PAGE_CHAT, action->done);
}

static LifecycleAction *
lifecycle_new(ClawtWebApp *app, const gchar *kind, const gchar *done)
{
    LifecycleAction *action = g_new0(LifecycleAction, 1);

    action->app = app;
    action->kind = kind;
    action->done = done;

    return action;
}

/*
 * A group room's page.
 *
 * `/r/:id` with no view segment, because a room has exactly one view --
 * every other page is scoped to an agent and means nothing here.  That
 * also keeps clawt_web_view_body()'s switch untouched rather than
 * adding a ClawtPage only one kind of entry can answer.
 */
/*
 * The sidebar as one ordered list of typed entries.
 *
 * Both move buttons build the same list, because what they send is a
 * *renumbering* of the whole sidebar: a frame carrying only the agents
 * would renumber them from ten while the rooms kept whatever numbers
 * they had, and the two scales would interleave in a way nobody asked
 * for.  One helper, so an agent moved and a room moved cannot disagree
 * about what the list is.
 *
 * Returns: (transfer full) (element-type utf8): the entries
 */
GPtrArray *
clawt_web_sidebar_entries(ClawtWebApp *app, const gchar *find, gint *index_out)
{
    g_autoptr(JsonNode) agent_list = clawt_web_app_call(app, "agent.list",
                                                        NULL);
    g_autoptr(JsonNode) room_list = clawt_web_app_call(app, "room.list",
                                                       NULL);
    JsonArray *agents = clawt_web_member_array(clawt_web_root(agent_list),
                                               "agents");
    JsonArray *rooms = clawt_web_member_array(clawt_web_root(room_list),
                                              "rooms");
    GPtrArray *entries = g_ptr_array_new_with_free_func(g_free);
    guint i;

    *index_out = -1;

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        const gchar *id = clawt_web_member(
            json_array_get_object_element(agents, i), "id", NULL);

        if (id == NULL)
            continue;

        if (g_strcmp0(id, find) == 0)
            *index_out = (gint)entries->len;

        g_ptr_array_add(entries, g_strconcat("a:", id, NULL));
    }

    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);
        const gchar *id = clawt_web_member(room, "id", NULL);

        if (id == NULL || !clawt_web_member_bool(room, "declared", FALSE))
            continue;

        if (g_strcmp0(id, find) == 0)
            *index_out = (gint)entries->len;

        g_ptr_array_add(entries, g_strconcat("r:", id, NULL));
    }

    return entries;
}

/*
 * Swaps @index with its neighbour and sends the whole arrangement.
 *
 * Returns: (transfer full) (nullable): a message for the banner, or
 *   %NULL when the frame was refused
 */
gboolean
clawt_web_move_entry(ClawtWebApp *app, GPtrArray *entries, gint index,
           const gchar *direction, const gchar **message)
{
    g_autoptr(ClawtWebPayload) payload = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *joined = NULL;
    gint swap_with = (g_strcmp0(direction, "up") == 0) ? index - 1
                                                        : index + 1;

    if (swap_with < 0 || swap_with >= (gint)entries->len) {
        *message = "Already at the end.";
        return TRUE;
    }

    {
        gpointer moved = g_ptr_array_index(entries, (guint)index);

        entries->pdata[index] = entries->pdata[swap_with];
        entries->pdata[swap_with] = moved;
    }

    g_ptr_array_add(entries, NULL);
    joined = g_strjoinv(",", (gchar **)entries->pdata);

    payload = clawt_web_payload_new();
    clawt_web_payload_set(payload, "entries", joined);

    reply = clawt_web_app_call(app, "fleet.reorder",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return FALSE;

    *message = "Moved.";

    return TRUE;
}

static HtmxResponse *
on_room_page(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *room_id = clawt_web_param(params, "id");
    g_autoptr(HtmxElement) body = NULL;
    g_autofree gchar *html = NULL;

    clawt_web_app_set_viewing_room(app, room_id);

    body = clawt_web_room_body(app, room_id);
    html = clawt_web_page(app, NULL, CLAWT_PAGE_CHAT, body, request);

    return clawt_web_html_response(html);
}

/*
 * Moves a room one place in the sidebar.
 *
 * The GTK client drags; a pair of buttons is the same capability on a
 * page that has to work without JavaScript, and both send the same
 * `fleet.reorder` frame describing the whole arrangement -- a client
 * whose view was a moment stale cannot produce a half-applied reorder.
 */
static HtmxResponse *
on_room_reorder(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *room_id = clawt_web_param(params, "id");
    gint index = -1;
    g_autoptr(GPtrArray) entries = clawt_web_sidebar_entries(app, room_id, &index);
    const gchar *message = NULL;

    if (index < 0)
        return clawt_web_after_action(app, request, NULL, CLAWT_PAGE_CHAT,
                                      NULL);

    if (!clawt_web_move_entry(app, entries, index,
                    htmx_request_get_query_param(request, "direction"),
                    &message))
        return clawt_web_error_page(app, request, NULL, CLAWT_PAGE_CHAT,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, NULL, CLAWT_PAGE_CHAT,
                                  message);
}

/*
 * Puts a room in a team's group, which is presentation and nothing else
 * -- it changes neither who is in the room nor who a message reaches.
 */
static HtmxResponse *
on_room_team(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *room_id = clawt_web_param(params, "id");
    const gchar *team = clawt_web_form_value(request, "team");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "room", room_id);
    clawt_web_payload_set(payload, "team", team != NULL ? team : "");

    reply = clawt_web_app_call(app, "room.set",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, NULL, CLAWT_PAGE_CHAT,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, NULL, CLAWT_PAGE_CHAT,
                                  "Moved.");
}

void
clawt_web_register_fleet(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/", on_index, app);
    htmx_router_get(router, "/a/:id", on_agent_root, app);
    htmx_router_get(router, "/f/sidebar", on_sidebar_fragment, app);

    /*
     * The room routes live in their own space rather than under /a/, so
     * they are not affected by the catch-all `/a/:id/:view` -- which
     * matches everything under an agent and has swallowed a route once
     * already.
     */
    htmx_router_get(router, "/r/:id", on_room_page, app);
    htmx_router_post(router, "/r/:id/reorder", on_room_reorder, app);
    htmx_router_post(router, "/r/:id/team", on_room_team, app);

    htmx_router_post(router, "/a/:id/start", on_lifecycle,
                     lifecycle_new(app, "agent.start", "Starting."));
    htmx_router_post(router, "/a/:id/stop", on_lifecycle,
                     lifecycle_new(app, "agent.stop", "Stopped."));
    htmx_router_post(router, "/a/:id/restart", on_lifecycle,
                     lifecycle_new(app, "agent.restart", "Restarting."));
}

/*
 * The view route, registered last on purpose.
 *
 * The router takes the first pattern that matches, and "/a/:id/:view"
 * matches everything under an agent -- so registered with the rest of
 * the fleet it swallowed /a/x/export, /a/x/copy and /a/x/file. The bug
 * was invisible because an unrecognised view falls back to chat, so
 * every one of those quietly rendered the chat page and returned 200.
 */
void
clawt_web_register_views(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/a/:id/:view", on_agent_page, app);
}

/* ── Creating an agent ───────────────────────────────────────────── */

/*
 * What a new agent can be given, from what actually exists.
 *
 * Only providers whose `agent` flag is set. The HTTP ones belong to
 * ai-glib and are for the designer, which needs tool calls -- the exact
 * thing the CLI backends cannot do. An agent configured for one of them
 * is silently rewritten to claude-code with a warning, so offering it
 * here would be offering a choice that does not do what it says.
 */
static void
add_provider_choices(ClawtWebApp *app, HtmxElement *form,
                     const gchar *current)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "model.list", NULL);
    JsonArray *providers = clawt_web_member_array(clawt_web_root(reply),
                                                  "providers");
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) models = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; providers != NULL && i < json_array_get_length(providers);
         i++) {
        JsonObject *provider = json_array_get_object_element(providers, i);
        const gchar *id = clawt_web_member(provider, "id", NULL);
        JsonArray *provider_models;
        guint m;

        if (id == NULL || !clawt_web_member_bool(provider, "agent", FALSE))
            continue;

        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(labels, g_strdup_printf(
            "%s — %s", clawt_web_member(provider, "label", id),
            clawt_web_member(provider, "note", "")));

        provider_models = clawt_web_member_array(provider, "models");

        for (m = 0; provider_models != NULL &&
                    m < json_array_get_length(provider_models); m++) {
            JsonObject *model = json_array_get_object_element(provider_models,
                                                              m);
            const gchar *model_id = clawt_web_member(model, "id", NULL);
            guint k;
            gboolean seen = FALSE;

            if (model_id == NULL)
                continue;

            for (k = 0; k < models->len; k++) {
                if (g_strcmp0(g_ptr_array_index(models, k), model_id) == 0)
                    seen = TRUE;
            }

            if (!seen)
                g_ptr_array_add(models, g_strdup(model_id));
        }
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);
    g_ptr_array_add(models, NULL);

    clawt_web_add(form, clawt_web_select_field(
        "Provider", "provider", (const gchar *const *)ids->pdata,
        (const gchar *const *)labels->pdata, current));
    clawt_web_add(form, clawt_web_select_field(
        "Model", "model", (const gchar *const *)models->pdata, NULL, NULL));
}

static void
add_image_choices(ClawtWebApp *app, HtmxElement *form)
{
    g_autoptr(JsonNode) containers = clawt_web_app_call(app, "image.list",
                                                        NULL);
    g_autoptr(JsonNode) disks = clawt_web_app_call(app, "image.vm_list", NULL);
    JsonArray *list = clawt_web_member_array(clawt_web_root(containers),
                                             "images");
    JsonArray *cached = clawt_web_member_array(clawt_web_root(disks),
                                               "images");
    g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) vm_names = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *image = json_array_get_object_element(list, i);
        const gchar *reference = clawt_web_member(image, "reference", NULL);

        if (reference == NULL)
            continue;

        g_ptr_array_add(refs, g_strdup(reference));
        g_ptr_array_add(labels, g_strdup_printf(
            "%s — %s", clawt_web_member(image, "label", reference),
            clawt_web_member(image, "note", "")));
    }

    g_ptr_array_add(refs, NULL);
    g_ptr_array_add(labels, NULL);

    clawt_web_add(form, clawt_web_select_field(
        "Container image", "image", (const gchar *const *)refs->pdata,
        (const gchar *const *)labels->pdata,
        clawt_web_member(clawt_web_root(containers), "default", NULL)));

    /*
     * Only images that have finished downloading. clawtilla ships no
     * disk and downloads none on its own, so a VM whose image is a name
     * nobody fetched defines, starts, and boots nothing -- three
     * symptoms and one cause.
     */
    for (i = 0; cached != NULL && i < json_array_get_length(cached); i++) {
        JsonObject *image = json_array_get_object_element(cached, i);

        if (clawt_web_member_bool(image, "downloading", FALSE))
            continue;

        g_ptr_array_add(vm_names, g_strdup(clawt_web_member(image, "name",
                                                            "?")));
    }

    g_ptr_array_add(vm_names, NULL);

    if (vm_names->len > 1) {
        clawt_web_add(form, clawt_web_select_field(
            "VM disk image", "vm_image",
            (const gchar *const *)vm_names->pdata, NULL, NULL));
    } else {
        clawt_web_add(form, clawt_web_text(
            "No VM disk images are downloaded, so a VM agent cannot be "
            "created yet. Fetch one under Settings → VM images.",
            "small muted"));
    }
}

static HtmxResponse *
on_new_agent_page(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    /*
     * Walked from the library rather than named here. This was one of
     * five hand-written copies of the type list -- four in the GTK
     * client and this one -- so a type added to the fleet reached
     * whichever of them somebody remembered. The labels come with it,
     * because two clients describing the same choice differently is the
     * same drift one layer down.
     */
    g_autoptr(GPtrArray) computers = g_ptr_array_new();
    g_autoptr(GPtrArray) computer_labels = g_ptr_array_new();
    guint computer_i;
    g_autoptr(HtmxDiv) view = htmx_div_new();
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(HtmxDiv) card = clawt_web_card("New agent", NULL);
    HtmxElement *body = clawt_web_card_body(card);
    g_autoptr(HtmxForm) form = clawt_web_form("/new");
    g_autofree gchar *html = NULL;

    (void)params;
    (void)request;

    for (computer_i = 0; computer_i < clawt_computer_type_count();
         computer_i++) {
        g_ptr_array_add(computers,
                        (gpointer)clawt_computer_type_nth_nick(computer_i));
        g_ptr_array_add(computer_labels,
                        (gpointer)clawt_computer_type_nth_label(computer_i));
    }

    g_ptr_array_add(computers, NULL);
    g_ptr_array_add(computer_labels, NULL);

    htmx_element_add_class(HTMX_ELEMENT(view), "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("New agent"));
    clawt_web_add(pad, clawt_web_text(
        "Creating an agent also builds its computer and starts it. A VM "
        "needs a disk image before it can be created at all.", "lede"));

    clawt_web_add(form, clawt_web_field("Id", "id", NULL, "researcher"));
    clawt_web_add(form, clawt_web_field("Name", "name", NULL, "Researcher"));
    clawt_web_add(form, clawt_web_field(
        "Description", "description", NULL,
        "What it is for. The rest of the fleet reads this when deciding "
        "who to ask."));

    add_provider_choices(app, HTMX_ELEMENT(form), NULL);

    clawt_web_add(form, clawt_web_select_field(
        "Computer", "computer", (const gchar *const *)computers->pdata,
        (const gchar *const *)computer_labels->pdata, "none"));

    add_image_choices(app, HTMX_ELEMENT(form));

    clawt_web_add(form, clawt_web_switch_field(
        "Start it now", "start",
        "A computer is built when the agent starts, never when it is "
        "created -- so declining leaves a VM agent with no machine yet.",
        TRUE));

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) create = clawt_web_button("Create", "primary");
        g_autoptr(HtmxA) cancel = htmx_a_new_with_href("/");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(create), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(create));

        htmx_element_add_class(HTMX_ELEMENT(cancel), "btn");
        htmx_node_set_text_content(HTMX_NODE(cancel), "Cancel");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(cancel));

        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));

    {
        g_autoptr(HtmxDiv) designer = clawt_web_card(
            "Design one instead",
            "Describe what you want and let a model work out the "
            "settings. It cannot choose a disk image -- the ones that "
            "exist are the ones somebody fetched -- so a VM agent has to "
            "be made above.");
        HtmxElement *designer_body = clawt_web_card_body(designer);
        g_autoptr(HtmxForm) design_form = clawt_web_form("/design");

        clawt_web_add(design_form, clawt_web_field("Id", "id", NULL,
                                                   "researcher"));
        clawt_web_add(design_form, clawt_web_field("Name", "name", NULL,
                                                   "Researcher"));
        clawt_web_add(design_form, clawt_web_textarea_field(
            "What should it do?", "description", NULL, 4));

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) go = clawt_web_button("Design", "default");

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(go), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(go));
            htmx_node_add_child(HTMX_NODE(design_form), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(designer_body), HTMX_NODE(design_form));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(designer));
    }

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    html = clawt_web_shell_page(app, "New agent", HTMX_ELEMENT(view), request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_create_agent(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *id = clawt_web_form_value(request, "id");
    const gchar *computer = clawt_web_form_value(request, "computer");
    JsonObject *root;

    (void)params;

    if (id == NULL || *id == '\0') {
        g_autofree gchar *first = clawt_web_first_agent(app);

        return clawt_web_error_page(app, request, first, CLAWT_PAGE_CHAT,
                                    "An agent needs an id.");
    }

    clawt_web_payload_set(payload, "id", id);
    clawt_web_payload_set(payload, "name",
                          clawt_web_form_value(request, "name"));
    clawt_web_payload_set(payload, "description",
                          clawt_web_form_value(request, "description"));
    clawt_web_payload_set(payload, "provider",
                          clawt_web_form_value(request, "provider"));
    clawt_web_payload_set(payload, "model",
                          clawt_web_form_value(request, "model"));
    clawt_web_payload_set(payload, "computer", computer);

    {
        gint type = CLAWT_COMPUTER_NONE;

        clawt_enum_from_nick(CLAWT_TYPE_COMPUTER_TYPE, computer, &type);

        if (clawt_computer_type_takes_image((ClawtComputerType)type))
            clawt_web_payload_set(payload, "image",
                                  clawt_web_form_value(request, "image"));
    }

    if (g_strcmp0(computer, "vm") == 0)
        clawt_web_payload_set(payload, "vm_image",
                              clawt_web_form_value(request, "vm_image"));

    clawt_web_payload_set_bool(payload, "start",
                               clawt_web_form_flag(request, "start"));

    reply = clawt_web_app_call(app, "agent.create",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL) {
        /*
         * Copied before anything else asks the daemon anything.
         * clawt_web_first_agent() makes a call of its own, and every call
         * replaces the app's last error -- so reporting it afterwards
         * reports the wrong failure, or worse, a freed pointer.
         */
        g_autofree gchar *failure = g_strdup(clawt_web_app_last_error(app));
        g_autofree gchar *first = clawt_web_first_agent(app);

        return clawt_web_error_page(app, request, first, CLAWT_PAGE_CHAT,
                                    failure);
    }

    root = clawt_web_root(reply);

    /*
     * A failure to start never undoes the creation -- rolling back
     * because a hypervisor was busy would discard everything the person
     * typed -- so the two are reported separately.
     */
    if (clawt_web_member(root, "start_error", NULL) != NULL) {
        g_autofree gchar *said = g_strdup_printf(
            "Created, but it did not start: %s",
            clawt_web_member(root, "start_error", ""));

        return clawt_web_error_page(app, request, id, CLAWT_PAGE_CHAT,
                                    said);
    }

    {
        g_autofree gchar *url = clawt_web_agent_url(id, CLAWT_PAGE_CHAT);

        return clawt_web_redirect(request, url);
    }
}

static HtmxResponse *
on_design(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *id = clawt_web_form_value(request, "id");
    JsonObject *root;

    (void)params;

    clawt_web_payload_set(payload, "id", id);
    clawt_web_payload_set(payload, "name",
                          clawt_web_form_value(request, "name"));
    clawt_web_payload_set(payload, "description",
                          clawt_web_form_value(request, "description"));

    reply = clawt_web_app_call(app, "design.agent",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL) {
        /*
         * Copied before anything else asks the daemon anything.
         * clawt_web_first_agent() makes a call of its own, and every call
         * replaces the app's last error -- so reporting it afterwards
         * reports the wrong failure, or worse, a freed pointer.
         */
        g_autofree gchar *failure = g_strdup(clawt_web_app_last_error(app));
        g_autofree gchar *first = clawt_web_first_agent(app);

        return clawt_web_error_page(app, request, first, CLAWT_PAGE_CHAT,
                                    failure);
    }

    root = clawt_web_root(reply);

    {
        g_autoptr(HtmxDiv) view = htmx_div_new();
        g_autoptr(HtmxDiv) pad = htmx_div_new();
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "What the designer came up with",
            "Nothing exists yet. Committing creates the agent through the "
            "same path as creating one by hand.");
        HtmxElement *body = clawt_web_card_body(card);
        const gchar *draft = clawt_web_member(root, "draft", NULL);
        const gchar *preview = clawt_web_member(root, "preview", NULL);
        g_autofree gchar *html = NULL;

        htmx_element_add_class(HTMX_ELEMENT(view), "view");
        htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

        if (preview != NULL) {
            g_autoptr(HtmxElement) pre = HTMX_ELEMENT(htmx_pre_new());

            htmx_element_add_class(pre, "console");
            htmx_node_set_text_content(HTMX_NODE(pre), preview);
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(pre));
        }

        if (draft != NULL) {
            g_autoptr(HtmxForm) commit = clawt_web_form("/design/commit");
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) go = clawt_web_button("Create it",
                                                        "primary");

            clawt_web_add(commit, clawt_web_hidden("draft", draft));
            clawt_web_add(commit, clawt_web_switch_field(
                "Start it now", "start", NULL, TRUE));

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(go), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(go));

            {
                g_autoptr(HtmxForm) discard = clawt_web_form("/design/discard");
                g_autoptr(HtmxButton) drop = clawt_web_button("Discard",
                                                              "default");

                clawt_web_add(discard, clawt_web_hidden("draft", draft));
                htmx_element_set_attribute(HTMX_ELEMENT(drop), "type",
                                           "submit");
                htmx_node_add_child(HTMX_NODE(discard), HTMX_NODE(drop));
                htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(discard));
            }

            htmx_node_add_child(HTMX_NODE(commit), HTMX_NODE(row));
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(commit));
        }

        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

        html = clawt_web_shell_page(app, "Design", HTMX_ELEMENT(view), request);

        return clawt_web_html_response(html);
    }
}

static HtmxResponse *
on_design_commit(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *created = NULL;

    (void)params;

    clawt_web_payload_set(payload, "draft",
                          clawt_web_form_value(request, "draft"));
    clawt_web_payload_set_bool(payload, "start",
                               clawt_web_form_flag(request, "start"));

    reply = clawt_web_app_call(app, "design.commit",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL) {
        /*
         * Copied before anything else asks the daemon anything.
         * clawt_web_first_agent() makes a call of its own, and every call
         * replaces the app's last error -- so reporting it afterwards
         * reports the wrong failure, or worse, a freed pointer.
         */
        g_autofree gchar *failure = g_strdup(clawt_web_app_last_error(app));
        g_autofree gchar *first = clawt_web_first_agent(app);

        return clawt_web_error_page(app, request, first, CLAWT_PAGE_CHAT,
                                    failure);
    }

    /*
     * Copied out of the reply rather than borrowed from it. Everything
     * from clawt_web_member() points into the node, and the node goes at
     * the end of this scope -- which is how the CLI's designer path came
     * to print an id it no longer owned.
     */
    created = g_strdup(clawt_web_member(clawt_web_root(reply), "id", NULL));

    {
        g_autofree gchar *url = clawt_web_agent_url(created,
                                                    CLAWT_PAGE_CHAT);

        return clawt_web_redirect(request, url);
    }
}

static HtmxResponse *
on_design_discard(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    (void)params;

    clawt_web_payload_set(payload, "draft",
                          clawt_web_form_value(request, "draft"));

    reply = clawt_web_app_call(app, "design.discard",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    (void)reply;

    return clawt_web_redirect(request, "/new");
}

/* ── Importing one ───────────────────────────────────────────────── */

static HtmxResponse *
on_import_page(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "agent.discover",
                                                   NULL);
    JsonArray *found = clawt_web_member_array(clawt_web_root(reply), "found");
    g_autoptr(HtmxDiv) view = htmx_div_new();
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autofree gchar *html = NULL;
    guint i;

    (void)params;
    (void)request;

    htmx_element_add_class(HTMX_ELEMENT(view), "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Import an agent"));
    clawt_web_add(pad, clawt_web_text(
        "A libreclaw workspace that already exists on this machine can be "
        "adopted rather than rebuilt.", "lede"));

    {
        g_autoptr(HtmxDiv) card = clawt_web_card("Found nearby", NULL);
        HtmxElement *body = clawt_web_card_body(card);

        if (found == NULL || json_array_get_length(found) == 0)
            clawt_web_add(body, clawt_web_empty(
                "Nothing found", "Point at a directory below instead."));

        for (i = 0; found != NULL && i < json_array_get_length(found); i++) {
            JsonObject *entry = json_array_get_object_element(found, i);
            const gchar *path = clawt_web_member(entry, "path", "?");
            const gchar *id = clawt_web_member(entry, "id", NULL);
            g_autoptr(HtmxForm) form = clawt_web_form("/import");
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) adopt = clawt_web_button("Adopt",
                                                           "primary");

            clawt_web_add(form, clawt_web_hidden("from", path));
            clawt_web_add(form, clawt_web_field(
                "Id", "id", id != NULL ? id : "", NULL));
            clawt_web_add(form, clawt_web_text(path, "small muted mono"));
            clawt_web_add(form, clawt_web_switch_field(
                "Keep its git history", "keep_git", NULL, TRUE));

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(adopt), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(adopt));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        }

        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    }

    {
        g_autoptr(HtmxDiv) card = clawt_web_card("From a directory", NULL);
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/import");
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) go = clawt_web_button("Import", "primary");

        g_autoptr(GPtrArray) modes = g_ptr_array_new();
        g_autoptr(GPtrArray) mode_labels = g_ptr_array_new();
        guint mode_i;

        for (mode_i = 0; mode_i < clawt_import_mode_count(); mode_i++) {
            g_ptr_array_add(modes,
                            (gpointer)clawt_import_mode_nth_nick(mode_i));
            g_ptr_array_add(mode_labels,
                            (gpointer)clawt_import_mode_nth_label(mode_i));
        }

        g_ptr_array_add(modes, NULL);
        g_ptr_array_add(mode_labels, NULL);

        clawt_web_add(form, clawt_web_field(
            "Path or git URL", "from", NULL, "~/agents/some-bot"));
        clawt_web_add(form, clawt_web_field("Id", "id", NULL, "some-bot"));
        clawt_web_add(form, clawt_web_select_field(
            "How", "mode", (const gchar *const *)modes->pdata,
            (const gchar *const *)mode_labels->pdata, "copy"));
        clawt_web_add(form, clawt_web_switch_field(
            "Keep its git history", "keep_git", NULL, TRUE));
        clawt_web_add(form, clawt_web_text(
            "Copying forks: edits afterwards go to one of two diverging "
            "directories. Linking keeps one, which is what you want for a "
            "workspace you already maintain somewhere.", "small muted"));

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(go), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(go));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    }

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    html = clawt_web_shell_page(app, "Import", HTMX_ELEMENT(view), request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_import(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *id = clawt_web_form_value(request, "id");

    (void)params;

    clawt_web_payload_set(payload, "from",
                          clawt_web_form_value(request, "from"));
    clawt_web_payload_set(payload, "id", id);
    clawt_web_payload_set(payload, "mode",
                          clawt_web_form_value(request, "mode"));
    clawt_web_payload_set_bool(payload, "keep_git",
                               clawt_web_form_flag(request, "keep_git"));

    reply = clawt_web_app_call(app, "agent.import",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL) {
        /*
         * Copied before anything else asks the daemon anything.
         * clawt_web_first_agent() makes a call of its own, and every call
         * replaces the app's last error -- so reporting it afterwards
         * reports the wrong failure, or worse, a freed pointer.
         */
        g_autofree gchar *failure = g_strdup(clawt_web_app_last_error(app));
        g_autofree gchar *first = clawt_web_first_agent(app);

        return clawt_web_error_page(app, request, first, CLAWT_PAGE_CHAT,
                                    failure);
    }

    {
        g_autofree gchar *url = clawt_web_agent_url(id, CLAWT_PAGE_CHAT);

        return clawt_web_redirect(request, url);
    }
}

void
clawt_web_register_creation(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/new", on_new_agent_page, app);
    htmx_router_post(router, "/new", on_create_agent, app);
    htmx_router_post(router, "/design", on_design, app);
    htmx_router_post(router, "/design/commit", on_design_commit, app);
    htmx_router_post(router, "/design/discard", on_design_discard, app);
    htmx_router_get(router, "/import", on_import_page, app);
    htmx_router_post(router, "/import", on_import, app);
}
