/*
 * web-agent.c - The inspector
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Built by walking clawt_config_schema_get() rather than from a list of
 * rows, which is the rule the rest of this project already follows for
 * anything that enumerates an option's keys.  The practical difference is
 * that an option added to the schema appears here without anybody editing
 * this file -- and an option somebody forgets to add a row for is exactly
 * how a setting comes to be accepted, reported as saved, and then read
 * from nowhere.
 */

#include "web-pages.h"

#include <string.h>

/* ── Grouping ────────────────────────────────────────────────────── */

/*
 * A friendly heading for a key's first component, and the order the
 * groups appear in.
 *
 * The *set* of groups still comes from the schema -- a prefix with no
 * entry here gets a heading made from its own name rather than being
 * dropped, so a section added later is visible immediately even before
 * anybody gives it a nicer title.
 */
static const struct {
    const gchar *prefix;
    const gchar *title;
    const gchar *summary;
} groups[] = {
    { "",           "Identity",
      "What this agent is called and where it sits in the fleet." },
    { "model",      "Model",
      "Which CLI backend answers, and with what." },
    { "runtime",    "Runtime",
      "How the agent is started, and what happens when it stops." },
    { "persona",    "Persona",
      "The system prompt and the identity files folded into it." },
    { "memory",     "Memory budget",
      "libreclaw's MEMORY.md size limits. Not the searchable store -- "
      "that is memories.*, a different thing with a similar name." },
    { "computer",   "Computer",
      "The machine this agent can reach, and how far it is confined." },
    { "tools",      "Tools",
      "Which orchestration tools the agent is offered." },
    { "workspace",  "Workspace", NULL },
    { "libreclaw",  "Passthrough",
      "Merged verbatim into the rendered libreclaw config." }
};

static const gchar *
group_title(const gchar *prefix)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(groups); i++) {
        if (g_strcmp0(groups[i].prefix, prefix) == 0)
            return groups[i].title;
    }

    return prefix;
}

static const gchar *
group_summary(const gchar *prefix)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(groups); i++) {
        if (g_strcmp0(groups[i].prefix, prefix) == 0)
            return groups[i].summary;
    }

    return NULL;
}

/*
 * The first component of a dotted key, or "" for a bare one.
 */
static gchar *
key_prefix(const gchar *key)
{
    const gchar *dot = strchr(key, '.');

    if (dot == NULL)
        return g_strdup("");

    return g_strndup(key, (gsize)(dot - key));
}

/*
 * A label for a key: its last component, with underscores opened out.
 */
static gchar *
key_label(const gchar *key)
{
    const gchar *last = strrchr(key, '.');
    g_autofree gchar *base = g_strdup(last != NULL ? last + 1 : key);
    gchar *p;

    for (p = base; *p != '\0'; p++) {
        if (*p == '_')
            *p = ' ';
    }

    /*
     * The middle components are kept in front of it, so
     * `computer.vm.ssh_user` reads "vm · ssh user" rather than a bare
     * "ssh user" that could belong to any of three backends.
     */
    if (last != NULL) {
        g_autofree gchar *prefix = g_strndup(key, (gsize)(last - key));
        const gchar *dot = strchr(prefix, '.');

        if (dot != NULL)
            return g_strdup_printf("%s · %s", dot + 1, base);
    }

    return g_steal_pointer(&base);
}

/*
 * The first line of a schema entry's documentation.
 *
 * The whole comment is often a paragraph, which under every field would
 * bury the fields. The first sentence is what a person needs to decide
 * whether this is the setting they meant.
 */
static gchar *
first_line(const gchar *doc)
{
    const gchar *newline;

    if (doc == NULL)
        return NULL;

    newline = strchr(doc, '\n');

    if (newline == NULL)
        return g_strdup(doc);

    return g_strndup(doc, (gsize)(newline - doc));
}

/* ── Controls ────────────────────────────────────────────────────── */

/*
 * The values an enum option accepts, from its own GType.
 */
static gchar **
enum_choices(const ClawtSchemaEntry *entry)
{
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    GType type;

    if (entry->enum_type == NULL)
        return NULL;

    type = entry->enum_type();

    if (G_TYPE_IS_ENUM(type)) {
        g_autoptr(GEnumClass) klass = g_type_class_ref(type);
        guint i;

        for (i = 0; i < klass->n_values; i++)
            g_ptr_array_add(values, g_strdup(klass->values[i].value_nick));
    } else if (G_TYPE_IS_FLAGS(type)) {
        g_autoptr(GFlagsClass) klass = g_type_class_ref(type);
        guint i;

        for (i = 0; i < klass->n_values; i++)
            g_ptr_array_add(values, g_strdup(klass->values[i].value_nick));
    } else {
        return NULL;
    }

    g_ptr_array_add(values, NULL);

    return (gchar **)g_ptr_array_free(g_steal_pointer(&values), FALSE);
}

/*
 * The teams this fleet declares, as ids and as names, with "" first.
 *
 * A team is a string in the schema, so the generic path draws it as a
 * text box -- which asks somebody to know a team's id and to type it
 * without a typo, when the daemon can say what the ids are.  The GTK
 * client has had a combo here since teams were added; this is the same
 * control.
 *
 * Returns %FALSE when the daemon does not answer, and the caller falls
 * back to the text box rather than offering a list with nothing in it.
 */
static gboolean
team_choices(ClawtWebApp *app, GStrv *out_ids, GStrv *out_names)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "team.list", NULL);
    JsonArray *teams = clawt_web_member_array(clawt_web_root(reply), "teams");
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) names = g_ptr_array_new();
    guint i;

    if (reply == NULL)
        return FALSE;

    /*
     * "No team" first, and it is a real choice rather than a prompt: it
     * is how an agent comes off a team, and it is where the chief of
     * staff belongs.
     */
    g_ptr_array_add(ids, g_strdup(""));
    g_ptr_array_add(names, g_strdup("No team"));

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = json_array_get_object_element(teams, i);
        const gchar *id = clawt_web_member(team, "id", NULL);

        if (id == NULL || *id == '\0')
            continue;

        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(names, g_strdup(clawt_web_member(team, "name", id)));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(names, NULL);

    *out_ids = (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);
    *out_names = (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);

    return TRUE;
}

/*
 * The accounts this agent can log in as, as the daemon decides it --
 * the same check agent.set applies, so the list offers only what saving
 * accepts. The GTK inspector builds its combo from the same reply.
 */
static gboolean
account_choices(ClawtWebApp *app, const gchar *agent_id,
                GStrv *out_ids, GStrv *out_names)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *accounts;
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) names = g_ptr_array_new();
    guint i;

    clawt_web_payload_set(payload, "agent", agent_id);
    reply = clawt_web_app_call(app, "account.list",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    if (reply == NULL)
        return FALSE;
    accounts = clawt_web_member_array(clawt_web_root(reply), "accounts");

    g_ptr_array_add(ids, g_strdup(""));
    g_ptr_array_add(names, g_strdup("The CLI's own login"));

    for (i = 0; accounts != NULL && i < json_array_get_length(accounts); i++) {
        JsonObject *account = json_array_get_object_element(accounts, i);
        const gchar *id = clawt_web_member(account, "id", NULL);

        if (id == NULL || !clawt_web_member_bool(account, "usable", FALSE))
            continue;
        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(names, clawt_web_member(account, "description", NULL) != NULL
            ? g_strdup_printf("%s -- %s", id, clawt_web_member(account, "description", ""))
            : g_strdup(id));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(names, NULL);
    *out_ids = (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);
    *out_names = (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
    return TRUE;
}

/*
 * The machines an agent can share, from computer.shared -- the list the
 * GTK inspector's "Use computer" row is built from. A host computer is
 * left out: it cannot be shared, and agent.set refuses it.
 */
static gboolean
use_choices(ClawtWebApp *app, GStrv *out_ids, GStrv *out_names)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *computers;
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) names = g_ptr_array_new();
    guint i;

    reply = clawt_web_app_call(app, "computer.shared", NULL);
    if (reply == NULL)
        return FALSE;
    computers = clawt_web_member_array(clawt_web_root(reply), "computers");

    g_ptr_array_add(ids, g_strdup(""));
    g_ptr_array_add(names, g_strdup("Its own machine"));

    for (i = 0; computers != NULL && i < json_array_get_length(computers); i++) {
        JsonObject *computer = json_array_get_object_element(computers, i);
        const gchar *id = clawt_web_member(computer, "id", NULL);

        if (id == NULL || !clawt_web_member_bool(computer, "shareable", TRUE))
            continue;
        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(names, g_strdup_printf("%s (%s)%s%s", id,
            clawt_web_member(computer, "type", "none"),
            clawt_web_member(computer, "description", NULL) != NULL ? " -- " : "",
            clawt_web_member(computer, "description", "")));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(names, NULL);
    *out_ids = (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);
    *out_names = (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
    return TRUE;
}

/*
 * Whether @key is one of the shared machine's keys rather than the
 * agent's: every computer.* but computer.use, once computer.use is set.
 * The renderer leaves them out and the saver does not send them, with
 * one answer for both.
 */
static gboolean
key_belongs_to_shared(const gchar *key, const gchar *use)
{
    return use != NULL && *use != '\0' &&
           g_str_has_prefix(key, "computer.") && g_strcmp0(key, "computer.use") != 0;
}

/*
 * Whether this agent's network can be chosen: only inside the sandbox,
 * since outside it the CLI could ignore the gate.  Asked of the agent as
 * the daemon reports it, so the page and the refusal agree.
 */
static gboolean
network_editable(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(JsonNode) shown = clawt_web_find_agent(app, agent_id);
    JsonObject *agent = clawt_web_member_object(clawt_web_root(shown),
                                                "agent");

    return g_strcmp0(clawt_web_member(agent, "confine", "none"),
                     "bwrap") == 0;
}

static HtmxElement *
control_for(ClawtWebApp *app, const gchar *agent_id, const ClawtSchemaEntry *entry,
            const gchar *key, const gchar *value)
{
    g_autofree gchar *label = key_label(key);
    g_autofree gchar *doc = first_line(entry->doc);
    g_autofree gchar *field_name = g_strdup_printf("k:%s", key);

    /*
     * The one string whose values the daemon knows.  An id that is not
     * among them -- a team somebody named and never declared -- is added
     * to the list by clawt_web_select_field() rather than dropped, so
     * saving the page cannot silently move the agent somewhere else.
     */
    if (g_strcmp0(key, "team") == 0) {
        g_auto(GStrv) ids = NULL;
        g_auto(GStrv) names = NULL;

        if (team_choices(app, &ids, &names))
            return HTMX_ELEMENT(clawt_web_select_field(
                label, field_name, (const gchar *const *)ids,
                (const gchar *const *)names, value));
    }

    if (g_strcmp0(key, "computer.use") == 0) {
        g_auto(GStrv) ids = NULL;
        g_auto(GStrv) names = NULL;

        if (use_choices(app, &ids, &names))
            return HTMX_ELEMENT(clawt_web_select_field(
                "use computer", field_name, (const gchar *const *)ids,
                (const gchar *const *)names, value));
    }

    /* An account no longer usable stays listed: the select adds the value. */
    if (g_strcmp0(key, "model.account") == 0) {
        g_auto(GStrv) ids = NULL;
        g_auto(GStrv) names = NULL;

        if (account_choices(app, agent_id, &ids, &names))
            return HTMX_ELEMENT(clawt_web_select_field(
                label, field_name, (const gchar *const *)ids,
                (const gchar *const *)names, value));
    }

    /*
     * The agent's own sandbox, named the way all three clients name it
     * rather than "runtime.confine" -- a key spelled as a label is a
     * setting somebody scrolls past.  Still the enum's own values, walked
     * from its type; the sentence and any warning follow it in the card.
     */
    if (g_strcmp0(key, "runtime.confine") == 0) {
        g_auto(GStrv) choices = enum_choices(entry);

        if (choices != NULL)
            return HTMX_ELEMENT(clawt_web_select_field(
                clawt_agent_confine_label(), field_name,
                (const gchar *const *)choices, NULL, value));
    }

    /*
     * Where the agent may connect, with the library's names for the
     * choices ("Through clawtilla's gate") over the enum's own values.
     * Disabled while the sandbox is off -- a disabled select is not
     * posted, so saving cannot write a network the daemon would refuse
     * -- and the reason follows it in the card.
     */
    if (g_strcmp0(key, "runtime.network") == 0) {
        g_auto(GStrv) choices = enum_choices(entry);
        g_autoptr(GPtrArray) labels = g_ptr_array_new();
        HtmxDiv *field;
        GList *l;
        guint i;

        if (choices != NULL) {
            for (i = 0; choices[i] != NULL; i++)
                g_ptr_array_add(labels, (gpointer)
                                clawt_egress_network_choice_label(choices[i]));

            g_ptr_array_add(labels, NULL);
            field = clawt_web_select_field(
                clawt_egress_network_label(), field_name,
                (const gchar *const *)choices,
                (const gchar *const *)labels->pdata, value);

            if (!network_editable(app, agent_id)) {
                for (l = htmx_node_get_children(HTMX_NODE(field)); l != NULL;
                     l = l->next)
                    if (G_TYPE_CHECK_INSTANCE_TYPE(l->data, HTMX_TYPE_SELECT))
                        htmx_element_set_attribute(HTMX_ELEMENT(l->data),
                                                   "disabled", "disabled");
            }

            return HTMX_ELEMENT(field);
        }
    }

    switch (entry->type) {
    case CLAWT_SCHEMA_BOOLEAN:
        return HTMX_ELEMENT(clawt_web_switch_field(
            label, field_name, doc, g_strcmp0(value, "true") == 0));

    case CLAWT_SCHEMA_ENUM: {
        g_auto(GStrv) choices = enum_choices(entry);

        if (choices != NULL)
            return HTMX_ELEMENT(clawt_web_select_field(
                label, field_name, (const gchar *const *)choices, NULL,
                value));

        return HTMX_ELEMENT(clawt_web_field(label, field_name, value, doc));
    }

    case CLAWT_SCHEMA_SECRET:
        /*
         * Never prefilled. The daemon reports only that something is
         * set, and putting a placeholder in the field would mean an
         * untouched form posting that placeholder back as the value.
         */
        return HTMX_ELEMENT(clawt_web_field(
            label, field_name, NULL,
            (g_strcmp0(value, "(set)") == 0)
            ? "set; type to replace" : "a secret reference"));

    case CLAWT_SCHEMA_STRING_LIST:
        return HTMX_ELEMENT(clawt_web_field(
            label, field_name, value,
            doc != NULL ? doc : "comma separated"));

    default:
        break;
    }

    /*
     * A prompt is a paragraph, so it gets a box rather than a line. The
     * schema does not distinguish them -- both are strings -- so the two
     * keys that are genuinely prose are named here.
     */
    if (g_str_has_suffix(key, "system_prompt") ||
        g_str_has_suffix(key, "prompt_suffix"))
        return HTMX_ELEMENT(clawt_web_textarea_field(label, field_name,
                                                     value, 6));

    return HTMX_ELEMENT(clawt_web_field(label, field_name, value, doc));
}

/*
 * Whether this key belongs on this page at all.
 */
static gboolean
skip_key(const gchar *key, const ClawtSchemaEntry *entry)
{
    if (entry->type == CLAWT_SCHEMA_SECTION ||
        entry->type == CLAWT_SCHEMA_MAPPING ||
        entry->type == CLAWT_SCHEMA_LIST_OF)
        return TRUE;

    /* The id names the agent; changing it here would rename nothing. */
    if (g_strcmp0(key, "id") == 0)
        return TRUE;

    /*
     * Integrations have a page of their own, with health checks, a
     * Matrix login and a room picker. A bare text field for
     * `integrations.matrix.access_token` beside them would be a second
     * way to set the same thing, and the worse one.
     */
    if (g_str_has_prefix(key, "integrations."))
        return TRUE;

    /* Mounts are a list, edited on the computer page. */
    if (g_str_has_prefix(key, "computer.mounts"))
        return TRUE;

    /*
     * The fallback list is a list of mappings: the walk yields its child
     * keys, which as three text fields would edit no entry at all.  It
     * has a card of its own, the ordered editor below the form.
     */
    if (g_str_has_prefix(key, "model.fallbacks"))
        return TRUE;

    return FALSE;
}

/* ── The view ────────────────────────────────────────────────────── */

static void
add_summary_card(HtmxElement *parent, JsonObject *agent,
                 const gchar *computer_detail, JsonObject *identity)
{
    g_autoptr(HtmxDiv) card = clawt_web_card("At a glance", NULL);
    HtmxElement *body = clawt_web_card_body(card);
    const gchar *state = clawt_web_member(agent, "state", "stopped");
    const gchar *detail = clawt_web_member(agent, "detail", NULL);

    clawt_web_add(body, clawt_web_row("State", state));

    /*
     * On one of its fallbacks: the badge, and the sentence saying why
     * and until when -- the same words the alert carried.
     */
    {
        HtmxSpan *fallback = clawt_web_failover_badge(agent);

        if (fallback != NULL) {
            g_autoptr(HtmxDiv) note = htmx_div_new();

            htmx_element_add_class(HTMX_ELEMENT(note), "fallback-state");
            clawt_web_add(HTMX_ELEMENT(note), fallback);
            clawt_web_add(HTMX_ELEMENT(note), clawt_web_text(
                clawt_web_member(agent, "failover_message", ""), "small"));
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(note));
        }
    }

    if (detail != NULL)
        clawt_web_add(body, clawt_web_row("Detail", detail));

    clawt_web_add(body, clawt_web_row(
        "Link", clawt_web_member_bool(agent, "connected", FALSE)
                ? "connected" : "not connected"));
    clawt_web_add(body, clawt_web_row(
        "Provider", clawt_web_member(agent, "provider", "—")));
    clawt_web_add(body, clawt_web_row(
        "Model", clawt_web_member(agent, "model", "—")));
    clawt_web_add(body, clawt_web_row(
        "Computer", clawt_web_member(agent, "computer", "none")));
    clawt_web_add(body, clawt_web_row(
        "Team", clawt_web_member(agent, "team", "—")));

    /*
     * What the persona costs, and -- only when it is worth saying --
     * which files account for it.
     *
     * An agent whose identity files outgrow a single command-line
     * argument cannot start a fresh session on a backend that passes the
     * system prompt as one, and the kernel's refusal names neither the
     * files nor the limit.  It is silent right up to the cliff, so the
     * number belongs somewhere a person looks *before* anything fails.
     *
     * The size is shown always and the sentence only past the threshold:
     * a row that appears only in trouble teaches nobody what normal looks
     * like, and a paragraph on every agent is noise.
     */
    if (identity != NULL) {
        gint64 bytes = clawt_web_member_int(identity, "bytes", 0);
        gint64 limit = clawt_web_member_int(identity, "limit", 0);
        const gchar *verdict = clawt_web_member(identity, "verdict", NULL);

        if (bytes > 0) {
            g_autofree gchar *text = g_strdup_printf(
                "%" G_GINT64_FORMAT " bytes of %" G_GINT64_FORMAT,
                bytes, limit);

            clawt_web_add(body, clawt_web_row("Identity", text));
        }

        if (verdict != NULL) {
            g_autoptr(HtmxDiv) note = htmx_div_new();
            g_autofree gchar *sentence = g_strdup_printf("%s.", verdict);

            htmx_element_add_class(HTMX_ELEMENT(note), "clawt-identity-size");
            clawt_web_add(HTMX_ELEMENT(note),
                          clawt_web_badge((bytes >= limit) ? "too large"
                                                           : "filling up",
                                          (bytes >= limit) ? "bad" : "warn"));
            clawt_web_add(HTMX_ELEMENT(note),
                          clawt_web_text(sentence, "small"));

            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(note));
        }
    }

    if (computer_detail != NULL) {
        g_autoptr(HtmxElement) details = HTMX_ELEMENT(htmx_div_new());
        g_autoptr(HtmxElement) pre = HTMX_ELEMENT(htmx_pre_new());

        htmx_element_add_class(pre, "console");
        htmx_node_set_text_content(HTMX_NODE(pre), computer_detail);

        /*
         * What the agent itself is told about its computer, shown as
         * written. It is the text that decides whether an agent goes
         * looking for a tool it does not have -- so somebody debugging
         * "why did it try that" should be able to read the same words.
         */
        clawt_web_add(details, clawt_web_text(
            "What the agent is told about its computer:", "small muted"));
        htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(pre));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(details));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/*
 * Where the agent may connect, what the gate refused it today and why,
 * and which of its keys the broker holds.  The summary is the library's
 * line, so the three clients say "egress: gated (3 refused today)" the
 * same way; each refusal names the address as well as the host, which is
 * what makes a rebinding name legible.
 */
static void
add_egress_card(HtmxElement *parent, JsonObject *egress)
{
    g_autofree gchar *summary = NULL;
    g_autoptr(HtmxDiv) card = NULL;
    HtmxElement *body;
    JsonArray *refusals;
    JsonArray *keys;
    guint i;

    if (egress == NULL)
        return;

    summary = clawt_egress_status_line(
        clawt_web_member(egress, "network", "open"),
        (guint)clawt_web_member_int(egress, "refused_today", 0));
    card = clawt_web_card(clawt_egress_network_label(), summary);
    body = clawt_web_card_body(card);

    refusals = clawt_web_member_array(egress, "refusals");

    for (i = 0; refusals != NULL && i < json_array_get_length(refusals);
         i++) {
        JsonObject *refusal = json_array_get_object_element(refusals, i);
        g_autofree gchar *line = NULL;

        if (refusal == NULL)
            continue;

        line = clawt_egress_refusal_line(
            clawt_web_member(refusal, "host", NULL),
            (guint)clawt_web_member_int(refusal, "port", 0),
            clawt_web_member(refusal, "address", NULL),
            clawt_web_member(refusal, "reason", NULL));
        clawt_web_add(body, clawt_web_text(line, "small mono"));
    }

    keys = clawt_web_member_array(egress, "keys");

    for (i = 0; keys != NULL && i < json_array_get_length(keys); i++) {
        JsonObject *key = json_array_get_object_element(keys, i);
        gboolean brokered;

        if (key == NULL)
            continue;

        brokered = clawt_web_member_bool(key, "brokered", FALSE);
        clawt_web_add(body, clawt_web_row(
            clawt_web_member(key, "name", "?"),
            brokered ? "brokered: the agent holds a token, the daemon "
                       "adds the real key"
                     : clawt_web_member(key, "reason", "not brokered")));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/*
 * The face, and the three ways the web client can change it: choose a
 * file, or drop one on the browser's own file input -- there is no
 * clipboard-paste route here the way there is in GTK, since a browser
 * form has no equivalent of gdk_clipboard_read_texture_async().
 *
 * The `<img>` points at /a/:id/avatar rather than a filesystem path --
 * the whole reason this half of the feature did not already work: the
 * browser may be on a different machine than the daemon entirely, and a
 * path only ever draws on the host that has it.
 */
static void
add_avatar_card(HtmxElement *parent, JsonObject *agent, const gchar *agent_id)
{
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "Profile picture",
        "PNG, JPEG or WebP -- the file's own bytes decide, not its name.");
    HtmxElement *body = clawt_web_card_body(card);
    gboolean has_avatar = clawt_web_member_bool(agent, "has_avatar", FALSE);
    const gchar *configured = clawt_web_member(agent, "avatar", "");
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);

    clawt_web_add(body, clawt_web_text(
        has_avatar
            ? ((configured != NULL && *configured != '\0')
                   ? "Set from agents.avatar."
                   : "Auto-detected: profile-picture.png (or .jpg, .jpeg, "
                     "or .webp) in the agent's own directory.")
            : "No picture yet -- initials are shown instead.",
        "small muted"));

    if (has_avatar) {
        g_autofree gchar *url = g_strdup_printf("/a/%s/avatar", escaped);
        g_autoptr(HtmxImg) picture =
            htmx_img_new_with_src(url, "Profile picture");

        htmx_element_add_class(HTMX_ELEMENT(picture), "avatar-preview");
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(picture));
    }

    /*
     * A form of its own, exactly for the reason the attachment picker in
     * the composer is one: it posts multipart, and the settings form
     * below it does not.
     */
    {
        g_autofree gchar *action =
            g_strdup_printf("/a/%s/avatar/set", escaped);
        g_autoptr(HtmxForm) form = clawt_web_form(action);
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxInput) picker = htmx_input_new(HTMX_INPUT_FILE);
        g_autoptr(HtmxButton) upload = clawt_web_button("Upload", "default");

        htmx_element_set_attribute(HTMX_ELEMENT(form), "enctype",
                                   "multipart/form-data");
        htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-encoding",
                                   "multipart/form-data");

        htmx_input_set_name(picker, "file");
        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(picker));

        htmx_element_set_attribute(HTMX_ELEMENT(upload), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(upload));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    }

    if (has_avatar) {
        g_autofree gchar *action =
            g_strdup_printf("/a/%s/avatar/clear", escaped);
        g_autoptr(HtmxForm) form = clawt_web_form(action);
        g_autoptr(HtmxButton) clear =
            clawt_web_button("Remove picture", "default");

        htmx_element_set_attribute(HTMX_ELEMENT(clear), "type", "submit");
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(clear));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/*
 * What this agent is waiting for: conditions clawtilla checks by code on
 * its behalf, each ending in one notice.  The schedule line is
 * clawt_wait_schedule_label()'s, so it reads as the GTK and libregnum
 * clients draw it.
 */
static void
add_waits_card(
	ClawtWebApp *app,
	HtmxElement *parent,
	const gchar *agent_id
){
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(HtmxDiv) card = clawt_web_card(
		"Waiting for",
		"Conditions clawtilla checks for this agent, by code -- a pending "
		"wait costs nothing. The agent gets one notice when each holds or "
		"expires; cancelling one tells it.");
	HtmxElement *body = clawt_web_card_body(card);
	g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
	g_autofree gchar *action = g_strdup_printf("/a/%s/wait/cancel", escaped);
	JsonArray *waits;
	gint64 now = g_get_real_time();
	guint i;

	clawt_web_payload_set(payload, "agent", agent_id);
	reply = clawt_web_app_call(app, "wait.list",
	                           clawt_web_payload_take(
	                               g_steal_pointer(&payload)));
	waits = clawt_web_member_array(clawt_web_root(reply), "waits");

	if (reply == NULL) {
		g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

		clawt_web_add(body, clawt_web_notice(why, "bad"));
	} else if (waits == NULL || json_array_get_length(waits) == 0) {
		clawt_web_add(body, clawt_web_empty(
			"Nothing",
			"When this agent sets a wait with clawtilla_wait_until, it "
			"shows here until it ends."));
	}

	for (i = 0; waits != NULL && i < json_array_get_length(waits); i++) {
		JsonObject *wait = json_array_get_object_element(waits, i);
		g_autoptr(HtmxDiv) row = htmx_div_new();
		g_autoptr(HtmxDiv) head = htmx_div_new();
		g_autoptr(HtmxSpan) label = htmx_span_new();
		g_autoptr(HtmxForm) form = clawt_web_form(action);
		g_autoptr(HtmxButton) cancel = clawt_web_button("Cancel", "default");
		g_autofree gchar *schedule = clawt_wait_schedule_label(
			clawt_web_member_int(wait, "last_check_at", 0),
			clawt_web_member_int(wait, "next_check_at", 0),
			clawt_web_member_int(wait, "expires_at", 0), now);
		const gchar *seen = clawt_web_member(wait, "last_result", NULL);

		htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
		htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");
		htmx_element_add_class(HTMX_ELEMENT(label), "list-item-title");
		htmx_node_set_text_content(HTMX_NODE(label),
		                           clawt_web_member(wait, "label", "?"));
		htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
		clawt_web_add(head, clawt_web_badge(
			clawt_web_member(wait, "kind", "?"), "info"));
		htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

		clawt_web_add(row, clawt_web_text(
			clawt_web_member(wait, "condition", "?"), "small"));
		clawt_web_add(row, clawt_web_text(schedule, "small muted"));

		if (seen != NULL) {
			g_autofree gchar *line = g_strdup_printf("Last check saw: %s",
			                                         seen);

			clawt_web_add(row, clawt_web_text(line, "small muted"));
		}

		clawt_web_add(form, clawt_web_hidden(
			"wait", clawt_web_member(wait, "id", "")));
		htmx_element_set_attribute(HTMX_ELEMENT(cancel), "type", "submit");
		htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(cancel));
		htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(form));

		htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
	}

	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

static void
add_danger_card(HtmxElement *parent, const gchar *agent_id)
{
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "Removing this agent",
        "Taking it out of the fleet can be undone by adding it back. "
        "Deleting what it wrote cannot.");
    HtmxElement *body = clawt_web_card_body(card);
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    g_autofree gchar *action = g_strdup_printf("/a/%s/remove", escaped);
    g_autoptr(HtmxForm) form = clawt_web_form(action);
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autofree gchar *confirm = NULL;

    clawt_web_add(form, clawt_web_switch_field(
        "Delete its workspace, state and transcripts", "remove_files",
        "Off by default. The files are what the agent wrote; nothing "
        "restores them.", FALSE));
    clawt_web_add(form, clawt_web_switch_field(
        "Destroy its computer", "remove_computer",
        "For a VM this undefines the libvirt domain and removes the disk.",
        FALSE));

    htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");

    {
        g_autoptr(HtmxButton) button = clawt_web_button("Remove agent",
                                                        "danger");

        confirm = g_strdup_printf("Remove %s from the fleet?", agent_id);
        htmx_element_set_hx_confirm(HTMX_ELEMENT(button), confirm);
        htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(button));
    }

    htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

HtmxElement *
clawt_web_agent_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonObject *agent;
    JsonObject *settings;
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;
    g_autofree gchar *action = NULL;
    g_autofree gchar *escaped = NULL;
    g_autoptr(HtmxForm) form = NULL;
    HtmxElement *group_body = NULL;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    if (agent_id == NULL) {
        clawt_web_add(pad, clawt_web_empty("No agent selected", NULL));
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

        return g_steal_pointer(&view);
    }

    reply = clawt_web_find_agent(app, agent_id);
    root = clawt_web_root(reply);
    agent = clawt_web_member_object(root, "agent");
    settings = clawt_web_member_object(root, "settings");

    if (agent == NULL) {
        clawt_web_add(pad, clawt_web_empty(
            "No such agent", clawt_web_app_last_error(app)));
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

        return g_steal_pointer(&view);
    }

    clawt_web_add(pad, clawt_web_section_title(
        clawt_web_member(agent, "name", agent_id)));
    clawt_web_add(pad, clawt_web_text(
        clawt_web_member(agent, "description",
                         "No description. Add one below -- it is what the "
                         "rest of the fleet sees when deciding who to ask."),
        "lede"));

    add_summary_card(HTMX_ELEMENT(pad), agent,
                     clawt_web_member(root, "computer_detail", NULL),
                     clawt_web_member_object(root, "identity"));

    add_egress_card(HTMX_ELEMENT(pad),
                    clawt_web_member_object(root, "egress"));
    add_waits_card(app, HTMX_ELEMENT(pad), agent_id);
    add_avatar_card(HTMX_ELEMENT(pad), agent, agent_id);

    escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    action = g_strdup_printf("/a/%s/set", escaped);
    form = clawt_web_form(action);

    schema = clawt_config_schema_get(&n_entries);

    /*
     * Collected by prefix and then rendered, rather than emitting a
     * heading whenever the prefix changes.
     *
     * That shortcut works for the sidebar because the daemon returns the
     * fleet already grouped. The schema is not: it is in the order the
     * generated YAML wants, which interleaves `persona.*` with the bare
     * `prompt_suffix` -- and the result was two cards both called
     * Identity, with the fields split between them.
     */
    {
        g_autoptr(GHashTable) by_prefix = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free,
            (GDestroyNotify)g_ptr_array_unref);
        g_autoptr(GPtrArray) order = g_ptr_array_new_with_free_func(g_free);
        guint g;

        for (i = 0; i < n_entries; i++) {
            const ClawtSchemaEntry *entry = &schema[i];
            const gchar *key = clawt_config_schema_agent_name(entry);
            g_autofree gchar *prefix = NULL;
            GPtrArray *bucket;

            if (key == NULL || skip_key(key, entry))
                continue;

            prefix = key_prefix(key);
            bucket = g_hash_table_lookup(by_prefix, prefix);

            if (bucket == NULL) {
                bucket = g_ptr_array_new();
                g_hash_table_insert(by_prefix, g_strdup(prefix), bucket);
                g_ptr_array_add(order, g_strdup(prefix));
            }

            g_ptr_array_add(bucket, (gpointer)entry);
        }

        /*
         * The known groups first, in the order the table has them, then
         * anything else in the order the schema mentioned it. A section
         * added later therefore appears -- at the end, with a heading
         * made from its own name -- rather than silently not being
         * editable.
         */
        for (g = 0; g < G_N_ELEMENTS(groups) + order->len; g++) {
            const gchar *prefix;
            GPtrArray *bucket;
            g_autoptr(HtmxDiv) card = NULL;
            guint k;

            if (g < G_N_ELEMENTS(groups)) {
                prefix = groups[g].prefix;
            } else {
                prefix = g_ptr_array_index(order, g - G_N_ELEMENTS(groups));

                if (group_title(prefix) != prefix)
                    continue;   /* already drawn above */
            }

            bucket = g_hash_table_lookup(by_prefix, prefix);

            if (bucket == NULL || bucket->len == 0)
                continue;

            card = clawt_web_card(group_title(prefix),
                                  group_summary(prefix));
            group_body = clawt_web_card_body(card);

            for (k = 0; k < bucket->len; k++) {
                const ClawtSchemaEntry *entry = g_ptr_array_index(bucket, k);
                const gchar *key = clawt_config_schema_agent_name(entry);
                const gchar *value = (settings != NULL)
                                     ? clawt_web_member(settings, key, "") : "";
                const gchar *use = (settings != NULL)
                                   ? clawt_web_member(settings, "computer.use", "") : "";

                if (key_belongs_to_shared(key, use))
                    continue;

                clawt_web_add(group_body,
                              control_for(app, agent_id, entry, key, value));

                /*
                 * What sandboxing does, and -- when the daemon cannot
                 * give it -- that the agent is a shadow because of it,
                 * worded by the library so the three clients agree with
                 * the reason the daemon actually gives.
                 */
                if (g_strcmp0(key, "runtime.confine") == 0) {
                    const gchar *notice = clawt_agent_confine_notice(
                        clawt_web_member(agent, "confine", "none"),
                        clawt_web_member(agent, "runtime_type", "process"),
                        clawt_web_member_bool(agent, "confine_available",
                                              TRUE));

                    clawt_web_add(group_body, clawt_web_text(
                        clawt_agent_confine_explanation(), "small muted"));

                    if (notice != NULL)
                        clawt_web_add(group_body,
                                      clawt_web_notice(notice, "bad"));
                }

                /*
                 * The chosen network's one line, or -- with the sandbox
                 * off -- why it cannot be chosen, in the library's words.
                 */
                if (g_strcmp0(key, "runtime.network") == 0) {
                    const gchar *confine =
                        clawt_web_member(agent, "confine", "none");
                    const gchar *network =
                        clawt_web_member(agent, "network", "open");
                    const gchar *notice =
                        clawt_egress_network_notice(network, confine);

                    if (notice != NULL)
                        clawt_web_add(group_body, clawt_web_notice(
                            notice, g_strcmp0(network, "open") != 0
                                    ? "bad" : ""));
                    else
                        clawt_web_add(group_body, clawt_web_text(
                            clawt_egress_network_choice_explanation(network),
                            "small muted"));
                }

                if (g_strcmp0(key, "computer.use") == 0 && *use != '\0')
                    clawt_web_add(group_body, clawt_web_notice(
                        "This agent is on a shared computer, so the rest of its "
                        "computer settings are that machine's, under computers:. "
                        "Choose \"Its own machine\" and save to give it one of "
                        "its own.", ""));

                /*
                 * An option that hands over real authority says so where
                 * it is set, not only in the documentation. Enabling an
                 * unconfined host should read like a decision.
                 */
                if (entry->flags & CLAWT_SCHEMA_FLAG_DANGEROUS)
                    clawt_web_add(group_body, clawt_web_notice(
                        "This one hands over real authority. Read what it "
                        "does before turning it on.", ""));
            }

            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(card));

            /* Drawn; do not draw it again from the tail pass. */
            g_hash_table_remove(by_prefix, prefix);
        }
    }

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) save = clawt_web_button("Save", "primary");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(form));

    /*
     * The fallback list, right under the form holding the model it falls
     * back from.  A form of its own, because its buttons edit a list and
     * a form cannot nest in another.
     */
    {
        g_autoptr(JsonNode) catalog = clawt_web_app_call(app, "model.list",
                                                         NULL);
        g_autoptr(JsonNode) accounts = clawt_web_app_call(app, "account.list",
                                                          NULL);

        clawt_web_add(pad, clawt_web_fallback_card(
            agent_id,
            settings != NULL ? clawt_web_member(settings, "model.fallbacks", "")
                             : "",
            clawt_web_member(agent, "provider", NULL),
            clawt_web_root(catalog), clawt_web_root(accounts)));
    }

    clawt_web_add_files_card(app, HTMX_ELEMENT(pad), agent_id);
    clawt_web_add_memory_card(app, HTMX_ELEMENT(pad), agent_id, NULL);

    add_danger_card(HTMX_ELEMENT(pad), agent_id);

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── Saving ──────────────────────────────────────────────────────── */

/*
 * Sends only what changed.
 *
 * `agent.set` rewrites every agent's files on each call, so posting a
 * hundred unchanged keys would do that a hundred times for a form
 * somebody opened and closed. Comparing against what the daemon reported
 * also means an option this page does not know how to render is left
 * alone rather than blanked.
 */
static guint
apply_form(ClawtWebApp *app, HtmxRequest *request, const gchar *agent_id,
           JsonObject *settings, gboolean *out_restart, GError **error)
{
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;
    guint changed = 0;

    *out_restart = FALSE;
    schema = clawt_config_schema_get(&n_entries);

    /*
     * The account comes off before the provider changes and goes back on
     * after (the schema walk below reaches it later): the daemon checks
     * the pair, and a codex provider set while the agent still names a
     * Claude login is refused. The GTK inspector saves in the same order.
     */
    {
        const gchar *posted = clawt_web_form_value(request, "k:model.account");
        const gchar *current = settings != NULL
            ? clawt_web_member(settings, "model.account", "") : "";

        if (posted != NULL && g_strcmp0(posted, current) != 0 && *current != '\0') {
            g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
            g_autoptr(JsonNode) reply = NULL;

            clawt_web_payload_set(payload, "agent", agent_id);
            clawt_web_payload_set(payload, "key", "model.account");
            clawt_web_payload_set(payload, "value", "");
            reply = clawt_web_app_request(app, "agent.set",
                                          clawt_web_payload_take(g_steal_pointer(&payload)),
                                          error);
            if (reply == NULL)
                return 0;
            if (settings != NULL)
                json_object_set_string_member(settings, "model.account", "");
        }
    }

    /*
     * Opening the network goes before the schema walk: runtime.confine
     * comes first there, and turning the sandbox off while the network
     * is still gated is refused -- so a save that does both would fail
     * halfway in schema order.
     */
    {
        const gchar *network = clawt_web_form_value(request, "k:runtime.network");
        const gchar *current = (settings != NULL)
            ? clawt_web_member(settings, "runtime.network", "") : "";

        if (g_strcmp0(network, "open") == 0 &&
            g_strcmp0(current, "open") != 0 && *current != '\0') {
            g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
            g_autoptr(JsonNode) reply = NULL;

            clawt_web_payload_set(payload, "agent", agent_id);
            clawt_web_payload_set(payload, "key", "runtime.network");
            clawt_web_payload_set(payload, "value", "open");
            reply = clawt_web_app_request(app, "agent.set",
                                          clawt_web_payload_take(
                                              g_steal_pointer(&payload)),
                                          error);

            if (reply == NULL)
                return 0;

            *out_restart = TRUE;
            json_object_set_string_member(settings, "runtime.network", "open");
        }
    }

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *key = clawt_config_schema_agent_name(entry);
        g_autofree gchar *field = NULL;
        g_autofree gchar *posted = NULL;
        const gchar *current;
        g_autoptr(ClawtWebPayload) payload = NULL;
        g_autoptr(JsonNode) reply = NULL;

        /*
         * The same derivation the renderer uses. Two answers here would
         * mean a field that draws and does not save, or the reverse.
         */
        if (key == NULL || skip_key(key, entry))
            continue;

        /*
         * Moving onto a shared machine in the same save as editing this
         * agent's own computer settings: computer.use comes first in the
         * schema, so the rest would be refused. They are the machine's.
         */
        if (key_belongs_to_shared(key, clawt_web_form_value(request, "k:computer.use")))
            continue;

        field = g_strdup_printf("k:%s", key);

        if (entry->type == CLAWT_SCHEMA_BOOLEAN) {
            if (!clawt_web_form_had(request, field))
                continue;

            posted = g_strdup(clawt_web_form_flag(request, field)
                              ? "true" : "false");
        } else {
            const gchar *raw = clawt_web_form_value(request, field);

            if (raw == NULL)
                continue;

            /*
             * A secret's field is never prefilled, so an empty one means
             * "leave it as it is" rather than "clear it". Clearing one
             * by accident costs an authorization somebody cannot see the
             * reason for.
             */
            if (entry->type == CLAWT_SCHEMA_SECRET && *raw == '\0')
                continue;

            posted = g_strdup(raw);
            g_strstrip(posted);
        }

        current = (settings != NULL) ? clawt_web_member(settings, key, "") : "";

        if (g_strcmp0(posted, current) == 0)
            continue;

        payload = clawt_web_payload_new();
        clawt_web_payload_set(payload, "agent", agent_id);
        clawt_web_payload_set(payload, "key", key);
        clawt_web_payload_set(payload, "value", posted);

        reply = clawt_web_app_request(app, "agent.set",
                                      clawt_web_payload_take(
                                          g_steal_pointer(&payload)),
                                      error);

        if (reply == NULL)
            return changed;

        if (clawt_web_member_bool(clawt_web_root(reply), "restart_required",
                                  FALSE))
            *out_restart = TRUE;

        changed++;
    }

    return changed;
}

static HtmxResponse *
on_save(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(JsonNode) current = clawt_web_find_agent(app, agent_id);
    g_autoptr(GError) error = NULL;
    gboolean restart = FALSE;
    guint changed;
    g_autofree gchar *said = NULL;

    changed = apply_form(app, request, agent_id,
                         clawt_web_member_object(clawt_web_root(current),
                                                 "settings"),
                         &restart, &error);

    if (error != NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_AGENT, error->message);

    if (changed == 0)
        said = g_strdup("Nothing changed.");
    else if (restart)
        /*
         * An AI CLI lists its tools once, when its session starts. A
         * permission granted under a running agent reaches its files and
         * not its session, and the agent then reports -- accurately --
         * not having the tool.
         */
        said = g_strdup_printf(
            "Saved %u setting%s. Restart the agent for it to take effect: "
            "an AI CLI lists its tools once, when its session starts.",
            changed, changed == 1 ? "" : "s");
    else
        said = g_strdup_printf("Saved %u setting%s.", changed,
                               changed == 1 ? "" : "s");

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_PAGE_AGENT, said);
}

/*
 * The fallback list, as the card posted it, with the one edit its button
 * names applied -- then saved whole, in the library's spelling.
 *
 * Every button saves, because the page has no state between requests: a
 * list reordered and not saved would exist nowhere.  An entry missing its
 * provider or its model is refused here rather than dropped, since saving
 * a shorter list than the one on screen would say so nowhere.
 */
static HtmxResponse *
on_fallbacks(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *op = clawt_web_form_value(request, "op");
    const gchar *count_text = clawt_web_form_value(request, "fb_count");
    g_autoptr(GPtrArray) list = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_model_fallback_free);
    g_autoptr(JsonNode) current = NULL;
    g_autoptr(GPtrArray) before = NULL;
    g_autofree gchar *spelled = NULL;
    g_autofree gchar *was = NULL;
    guint64 count = 0;
    guint64 index = 0;
    guint i;

    if (count_text != NULL)
        g_ascii_string_to_unsigned(count_text, 10, 0, 64, &count, NULL);

    for (i = 0; i < (guint)count; i++) {
        g_autofree gchar *provider_name = g_strdup_printf("fb_provider_%u", i);
        g_autofree gchar *model_name = g_strdup_printf("fb_model_%u", i);
        g_autofree gchar *account_name = g_strdup_printf("fb_account_%u", i);
        g_autofree gchar *provider = g_strdup(clawt_web_form_value(request,
                                                                   provider_name));
        g_autofree gchar *model = g_strdup(clawt_web_form_value(request,
                                                                model_name));

        g_ptr_array_add(list, clawt_model_fallback_new(
            provider != NULL ? g_strstrip(provider) : "",
            model != NULL ? g_strstrip(model) : "",
            clawt_web_form_value(request, account_name)));
    }

    if (g_strcmp0(op, "add") == 0) {
        g_autofree gchar *provider = g_strdup(clawt_web_form_value(
            request, "fb_new_provider"));
        g_autofree gchar *model = g_strdup(clawt_web_form_value(
            request, "fb_new_model"));

        g_ptr_array_add(list, clawt_model_fallback_new(
            provider != NULL ? g_strstrip(provider) : "",
            model != NULL ? g_strstrip(model) : "",
            clawt_web_form_value(request, "fb_new_account")));
    } else if (op != NULL && g_str_has_prefix(op, "remove:") &&
               g_ascii_string_to_unsigned(op + 7, 10, 0, G_MAXUINT, &index,
                                          NULL) && index < list->len) {
        g_ptr_array_remove_index(list, (guint)index);
    } else if (op != NULL && g_str_has_prefix(op, "up:") &&
               g_ascii_string_to_unsigned(op + 3, 10, 0, G_MAXUINT, &index,
                                          NULL) &&
               index > 0 && index < list->len) {
        gpointer moved = list->pdata[index];

        list->pdata[index] = list->pdata[index - 1];
        list->pdata[index - 1] = moved;
    } else if (op != NULL && g_str_has_prefix(op, "down:") &&
               g_ascii_string_to_unsigned(op + 5, 10, 0, G_MAXUINT, &index,
                                          NULL) &&
               index + 1 < list->len) {
        gpointer moved = list->pdata[index];

        list->pdata[index] = list->pdata[index + 1];
        list->pdata[index + 1] = moved;
    }

    for (i = 0; i < list->len; i++) {
        ClawtModelFallback *entry = g_ptr_array_index(list, i);

        if (*entry->provider == '\0' || *entry->model == '\0') {
            g_autofree gchar *which = (i + 1 == list->len &&
                                       g_strcmp0(op, "add") == 0)
                ? g_strdup("The new fallback")
                : g_strdup_printf("Fallback %u", i + 1);
            g_autofree gchar *why = g_strdup_printf(
                "%s needs both a provider and a model; nothing was saved.",
                which);

            return clawt_web_error_page(app, request, agent_id,
                                        CLAWT_PAGE_AGENT, why);
        }
    }

    spelled = clawt_model_fallback_list_to_string(list);

    /*
     * Compared in the library's spelling, so a list the daemon reports
     * with other spacing is not "changed" by being posted back.
     */
    current = clawt_web_find_agent(app, agent_id);
    before = clawt_model_fallback_parse_list(
        clawt_web_member(clawt_web_member_object(clawt_web_root(current),
                                                 "settings"),
                         "model.fallbacks", ""), NULL);
    was = (before != NULL) ? clawt_model_fallback_list_to_string(before) : NULL;

    if (g_strcmp0(was, spelled) == 0)
        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_PAGE_AGENT, "Nothing changed.");

    {
        g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
        g_autoptr(JsonNode) reply = NULL;

        clawt_web_payload_set(payload, "agent", agent_id);
        clawt_web_payload_set(payload, "key", "model.fallbacks");
        clawt_web_payload_set(payload, "value", spelled);

        reply = clawt_web_app_call(app, "agent.set",
                                   clawt_web_payload_take(
                                       g_steal_pointer(&payload)));

        /*
         * The daemon's refusal as written: it names the account or the
         * subscription that made the list unacceptable.
         */
        if (reply == NULL) {
            g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

            return clawt_web_error_page(app, request, agent_id,
                                        CLAWT_PAGE_AGENT, why);
        }
    }

    return clawt_web_after_action(
        app, request, agent_id, CLAWT_PAGE_AGENT,
        list->len == 0
            ? "Fallbacks cleared: at a limit the agent waits for the reset."
            : "Fallbacks saved. Restart the agent for them to take effect.");
}

static HtmxResponse *
on_remove(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set_bool(payload, "remove_files",
                               clawt_web_form_flag(request, "remove_files"));
    clawt_web_payload_set_bool(payload, "remove_computer",
                               clawt_web_form_flag(request,
                                                   "remove_computer"));

    reply = clawt_web_app_call(app, "agent.remove",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_AGENT,
                                    clawt_web_app_last_error(app));

    return clawt_web_redirect(request, "/");
}

static HtmxResponse *
on_reset(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    g_autofree gchar *said = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);

    reply = clawt_web_app_call(app, "agent.reset",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_AGENT,
                                    clawt_web_app_last_error(app));

    root = clawt_web_root(reply);
    said = g_strdup_printf(
        "Cleared %" G_GINT64_FORMAT " session%s. The old sessions directory "
        "is kept beside the new one, so the history is still readable.",
        clawt_web_member_int(root, "sessions_cleared", 0),
        clawt_web_member_int(root, "sessions_cleared", 0) == 1 ? "" : "s");

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_PAGE_AGENT, said);
}

/*
 * Cancels one wait as the operator, which the daemon tells the agent
 * about -- it would otherwise go on believing it was waiting.
 */
static HtmxResponse *
on_wait_cancel(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ClawtWebApp *app = user_data;
	g_autofree gchar *agent_id = clawt_web_param(params, "id");
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;

	clawt_web_payload_set(payload, "wait",
	                      clawt_web_form_value(request, "wait"));

	reply = clawt_web_app_call(app, "wait.cancel",
	                           clawt_web_payload_take(
	                               g_steal_pointer(&payload)));

	if (reply == NULL)
		return clawt_web_error_page(app, request, agent_id,
		                            CLAWT_PAGE_AGENT,
		                            clawt_web_app_last_error(app));

	return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_AGENT,
	                              "Cancelled. The agent has been told it is "
	                              "no longer waiting.");
}

/*
 * Streams this agent's picture from the daemon's own bytes.
 *
 * Not a static file: `agent.avatar` is the only place these bytes are
 * kept, and this is the one route that asks for them -- no static-file
 * route reaches an agent's directory, so nothing here can become an
 * arbitrary-file-read primitive by way of a clever id.
 */
static HtmxResponse *
on_avatar_get(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GBytes) bytes = NULL;
    HtmxResponse *response;
    JsonObject *root;
    const gchar *encoded;
    const gchar *mime;
    const gchar *etag;
    guchar *raw;
    gsize length = 0;

    (void)request;

    clawt_web_payload_set(payload, "agent", agent_id);

    reply = clawt_web_app_call(
        app, "agent.avatar",
        clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return htmx_response_not_found();

    root = clawt_web_root(reply);
    encoded = clawt_web_member(root, "base64", NULL);
    mime = clawt_web_member(root, "mime", "application/octet-stream");
    etag = clawt_web_member(root, "etag", NULL);

    if (encoded == NULL)
        return htmx_response_not_found();

    raw = g_base64_decode(encoded, &length);
    bytes = g_bytes_new_take(raw, length);

    response = htmx_response_new();
    htmx_response_set_bytes(response, bytes);

    /*
     * The type the daemon sniffed from the bytes, not a guess from the
     * request path -- there is no extension here to guess from at all.
     */
    htmx_response_set_content_type(response, mime);

    if (etag != NULL)
        htmx_response_add_header(response, "ETag", etag);

    /*
     * A day, not "no-cache": the etag already says when the picture has
     * changed, so a browser holding the bytes that long saves a request
     * on every sidebar redraw without needing agent.changed plumbed
     * through as a cache-buster.
     */
    htmx_response_add_header(response, "Cache-Control",
                             "private, max-age=86400");

    return response;
}

static HtmxResponse *
on_avatar_set(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(GPtrArray) files = NULL;
    g_autoptr(GHashTable) fields = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *encoded = NULL;
    const HtmxUploadedFile *file;

    /*
     * Bytes, never a path -- the daemon's agent.avatar_set has no path
     * parameter at all, which is what stops a client asking it to read
     * an arbitrary file off its own disk and hand the bytes back as this
     * agent's face.
     */
    files = htmx_uploaded_file_parse_multipart(
        htmx_request_get_content_type(request),
        htmx_request_get_body_bytes(request), &fields, &error);

    if (files == NULL || files->len == 0)
        return clawt_web_error_page(
            app, request, agent_id, CLAWT_PAGE_AGENT,
            error != NULL ? error->message : "Choose a file first.");

    file = g_ptr_array_index(files, 0);

    {
        GBytes *bytes = htmx_uploaded_file_get_data(file);
        gsize size = 0;
        const guint8 *data;

        if (bytes == NULL)
            return clawt_web_error_page(app, request, agent_id,
                                        CLAWT_PAGE_AGENT,
                                        "That file is empty.");

        data = g_bytes_get_data(bytes, &size);

        if (data == NULL || size == 0)
            return clawt_web_error_page(app, request, agent_id,
                                        CLAWT_PAGE_AGENT,
                                        "That file is empty.");

        encoded = g_base64_encode(data, size);
    }

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "data", encoded);

    reply = clawt_web_app_call(
        app, "agent.avatar_set",
        clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_AGENT,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_PAGE_AGENT,
                                  "Profile picture updated.");
}

static HtmxResponse *
on_avatar_clear(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    (void)request;

    clawt_web_payload_set(payload, "agent", agent_id);

    reply = clawt_web_app_call(
        app, "agent.avatar_clear",
        clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_AGENT,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_PAGE_AGENT,
                                  "Profile picture removed.");
}

void
clawt_web_register_agent(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_post(router, "/a/:id/set", on_save, app);
    htmx_router_post(router, "/a/:id/fallbacks", on_fallbacks, app);
    htmx_router_post(router, "/a/:id/remove", on_remove, app);
    htmx_router_post(router, "/a/:id/reset", on_reset, app);
    htmx_router_post(router, "/a/:id/wait/cancel", on_wait_cancel, app);
    htmx_router_get(router, "/a/:id/avatar", on_avatar_get, app);
    htmx_router_post(router, "/a/:id/avatar/set", on_avatar_set, app);
    htmx_router_post(router, "/a/:id/avatar/clear", on_avatar_clear, app);
}
