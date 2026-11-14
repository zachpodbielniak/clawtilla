/*
 * web-triggers.c - The triggers page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Work an agent does because something happened elsewhere.  It sits
 * beside routines because a routine is a clock and a trigger is an
 * event, and both end in the same queued run against the same agent.
 *
 * A file of its own rather than a fourth view in web-work.c: the secret
 * is shown exactly once, and a page that also has to render it needs
 * enough care that it should not be sharing a file with three other
 * views' row builders.
 */

#include "clawt-web.h"
#include "web-pages.h"
#include "web-ui.h"

#include <string.h>

/*
 * A trigger's state in one word, in priority order.
 *
 * "Why is this not firing" wants one answer, not three flags a reader
 * has to combine -- and the first thing that is true is always the
 * answer, because a trigger with no secret refuses everything whatever
 * else is set.
 */
static const gchar *
trigger_state(JsonObject *trigger, const gchar **out_tone)
{
    if (!clawt_web_member_bool(trigger, "has_secret", FALSE)) {
        *out_tone = "bad";
        return "No secret, so it refuses every delivery. Rotate it.";
    }

    if (clawt_web_member_bool(trigger, "pending_verification", TRUE)) {
        *out_tone = "warn";
        return "Waiting for its first delivery. The next one that "
               "authenticates is held here for you to read rather than "
               "run.";
    }

    if (!clawt_web_member_bool(trigger, "enabled", FALSE)) {
        *out_tone = "warn";
        return "Switched off. Its address answers as though it does not "
               "exist.";
    }

    *out_tone = "good";
    return "On.";
}

/*
 * The URL a forge should be pointed at, built from what is actually
 * listening.
 *
 * Assembled here rather than asked of the operator: the endpoint and the
 * bound addresses are two things the daemon knows and a person would
 * otherwise have to join by hand, and getting it wrong produces
 * deliveries that never arrive with nothing anywhere saying why.
 */
static void
add_endpoint_rows(HtmxElement *body, JsonObject *trigger)
{
    const gchar *endpoint = clawt_web_member(trigger, "endpoint", NULL);
    JsonArray *listening = clawt_web_member_array(trigger, "listening");
    guint i;

    if (endpoint == NULL)
        return;

    if (listening == NULL || json_array_get_length(listening) == 0) {
        g_autofree gchar *path = g_strdup_printf("/hooks/%s", endpoint);

        clawt_web_add(body, clawt_web_row("Path", path));
        clawt_web_add(body, clawt_web_notice(
            "Nothing is listening. Set daemon.webhook_enabled and restart "
            "the daemon, or no delivery can arrive.", "warn"));
        return;
    }

    for (i = 0; i < json_array_get_length(listening); i++) {
        g_autofree gchar *url = g_strdup_printf(
            "%s/hooks/%s", json_array_get_string_element(listening, i),
            endpoint);

        clawt_web_add(body, clawt_web_row(i == 0 ? "URL" : "also", url));
    }
}

/* The id of the datalist a trigger's path boxes complete from. */
static gchar *
paths_list_id(const gchar *trigger_id)
{
	g_autofree gchar *safe = g_strdup(trigger_id != NULL ? trigger_id : "new");

	/* An id is a word in an attribute; fold anything that is not one. */
	g_strcanon(safe, "abcdefghijklmnopqrstuvwxyz0123456789-_", '_');

	return g_strdup_printf("cond-paths-%s", safe);
}

/*
 * The mode, as a choice with the library's two sentences under it.
 *
 * Both sentences rather than the selected one: the page is
 * form-and-redraw, so a line that changed with the selection would need
 * a script, and the two together are the comparison somebody choosing
 * is making anyway.
 */
static void
add_mode_field(HtmxElement *form, JsonObject *existing)
{
	g_autofree const gchar **values = NULL;
	g_autofree const gchar **labels = NULL;
	g_autoptr(GString) hints = g_string_new(NULL);
	guint n = clawt_trigger_mode_count();
	guint i;

	values = g_new0(const gchar *, n + 1);
	labels = g_new0(const gchar *, n + 1);

	for (i = 0; i < n; i++) {
		values[i] = clawt_trigger_mode_nth_nick(i);
		labels[i] = clawt_trigger_mode_nth_label(i);

		if (hints->len > 0)
			g_string_append_c(hints, ' ');

		g_string_append_printf(hints, "%s: %s", labels[i],
		                       clawt_trigger_mode_nth_hint(i));
	}

	clawt_web_add(form, clawt_web_select_field(
		"When it fires", "mode", values, labels,
		clawt_web_member(existing, "mode", "run")));
	clawt_web_add(form, clawt_web_text(hints->str, "check-sub"));
}

/* The pending cap as a number box that cannot hold a refused value. */
static void
add_pending_field(HtmxElement *form, JsonObject *existing,
                  const ClawtSchemaEntry *entry)
{
	g_autoptr(HtmxDiv) field = htmx_div_new();
	g_autoptr(HtmxLabel) tag = htmx_label_new_with_text("Runs at once");
	g_autoptr(HtmxInput) input = htmx_input_new(HTMX_INPUT_NUMBER);
	g_autoptr(HtmxP) hint = htmx_p_new();
	g_autofree gchar *value = NULL;
	g_autofree gchar *low = g_strdup_printf("%d", CLAWT_TRIGGER_MAX_PENDING_MIN);
	g_autofree gchar *high = g_strdup_printf("%d", CLAWT_TRIGGER_MAX_PENDING_MAX);

	value = g_strdup_printf("%" G_GINT64_FORMAT,
		clawt_web_member_int(existing, "max_pending",
		                     entry->default_value != NULL
		                         ? g_ascii_strtoll(entry->default_value,
		                                           NULL, 10)
		                         : CLAWT_TRIGGER_MAX_PENDING_MIN));

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_input_set_name(input, "max_pending");
	htmx_input_set_value(input, value);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "min", low);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "max", high);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "step", "1");
	htmx_element_add_class(HTMX_ELEMENT(hint), "check-sub");
	htmx_node_set_text_content(
		HTMX_NODE(hint),
		"How many runs may be unfinished together, 1 to 50. A delivery "
		"past this is answered \"try later\" and recorded as dropped. A "
		"post holds no slot.");

	htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(tag));
	htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(input));
	htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(hint));
	clawt_web_add(form, g_steal_pointer(&field));
}

/* One condition line: path, "contains", value. */
static HtmxDiv *
condition_line(guint n, const gchar *path, const gchar *value,
               const gchar *list_id)
{
	g_autoptr(HtmxDiv) line = htmx_div_new();
	g_autofree gchar *path_name = g_strdup_printf("match_path_%u", n);
	g_autofree gchar *value_name = g_strdup_printf("match_value_%u", n);
	g_autoptr(HtmxInput) path_box = htmx_input_new_text(path_name);
	g_autoptr(HtmxInput) value_box = htmx_input_new_text(value_name);
	g_autoptr(HtmxSpan) word = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(line), "cond-line");

	if (path != NULL)
		htmx_input_set_value(path_box, path);

	if (value != NULL)
		htmx_input_set_value(value_box, value);

	htmx_input_set_placeholder(path_box, "pull_request.title");
	htmx_input_set_placeholder(value_box, "release");
	htmx_element_set_attribute(HTMX_ELEMENT(path_box), "list", list_id);
	htmx_element_set_attribute(HTMX_ELEMENT(path_box), "aria-label",
	                           "Field path");
	htmx_element_set_attribute(HTMX_ELEMENT(value_box), "aria-label",
	                           "Text it must contain");
	htmx_element_add_class(HTMX_ELEMENT(word), "cond-word");
	htmx_node_set_text_content(HTMX_NODE(word), "contains");

	htmx_node_add_child(HTMX_NODE(line), HTMX_NODE(path_box));
	htmx_node_add_child(HTMX_NODE(line), HTMX_NODE(word));
	htmx_node_add_child(HTMX_NODE(line), HTMX_NODE(value_box));

	return (HtmxDiv *)g_steal_pointer(&line);
}

/*
 * The payload conditions: what is set, plus empty lines up to the limit
 * of ten -- three at a time, so the form is not ten boxes tall for a
 * trigger that filters on one field.  Clearing both boxes of a line
 * removes it; saving with more lines filled shows more empty ones next
 * time.  The check button posts the lines as they stand, unsaved, and
 * its answer fills the datalist the path boxes complete from.
 */
static void
add_match_editor(HtmxElement *form, JsonObject *existing, const gchar *id,
                 const gchar *escaped)
{
	g_autoptr(HtmxDiv) field = htmx_div_new();
	g_autoptr(HtmxLabel) tag = htmx_label_new_with_text(
		"Only when the body contains");
	g_autoptr(HtmxDiv) out = htmx_div_new();
	g_autofree gchar *list_id = paths_list_id(id);
	g_autofree gchar *out_id = g_strdup_printf("%s-out", list_id);
	g_autofree gchar *target = g_strdup_printf("#%s", out_id);
	JsonArray *pairs = clawt_web_member_array(existing, "match");
	guint have = pairs != NULL ? json_array_get_length(pairs) : 0;
	guint shown;
	guint n;

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(tag));

	/*
	 * Said rather than drawn as no lines: saving an empty editor over a
	 * filter the daemon could not read would delete what was written.
	 */
	if (clawt_web_member(existing, "match_error", NULL) != NULL) {
		g_autofree gchar *said = g_strdup_printf(
			"The filter as written cannot be read, so the trigger refuses "
			"every delivery: %s. It is left as it is unless you fill in "
			"lines here to replace it.",
			clawt_web_member(existing, "match_error", ""));

		clawt_web_add(field, clawt_web_notice(said, "bad"));
		clawt_web_add(field, clawt_web_hidden("match__unreadable", "1"));
	}

	clawt_web_add(field, clawt_web_text(
		"A dotted path into the JSON body and text it must contain, "
		"ignoring case; every line must hold. A miss is recorded as "
		"ignored, naming the line, and starts nothing. A name on a list, "
		"such as labels.name, looks in every item. Clear both boxes to "
		"remove a line.", "check-sub"));

	shown = MIN(have + 3, (guint)CLAWT_TRIGGER_MATCH_MAX);

	for (n = 0; n < shown; n++) {
		JsonObject *pair = n < have
			? json_array_get_object_element(pairs, n) : NULL;

		clawt_web_add(field, condition_line(
			n, clawt_web_member(pair, "path", NULL),
			clawt_web_member(pair, "value", NULL), list_id));
	}

	/* Always posted, so a form whose lines are all empty clears the filter. */
	clawt_web_add(field, clawt_web_hidden("match__present", "1"));

	htmx_element_set_id(HTMX_ELEMENT(out), out_id);
	htmx_element_add_class(HTMX_ELEMENT(out), "cond-out");

	if (id != NULL) {
		g_autofree gchar *check = g_strdup_printf("/f/triggers/%s/match",
		                                          escaped);
		g_autoptr(HtmxButton) button = clawt_web_button(
			"Check against the last delivery", "default");

		/*
		 * Inside the form, so htmx sends the lines as they stand; a
		 * button rather than a submit, so it does not save them.
		 */
		htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "button");
		htmx_element_set_hx_post(HTMX_ELEMENT(button), check);
		htmx_element_set_hx_target(HTMX_ELEMENT(button), target);
		htmx_element_set_attribute(HTMX_ELEMENT(button), "hx-swap",
		                           "innerHTML");
		htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(button));
	}

	htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(out));
	clawt_web_add(form, g_steal_pointer(&field));
}

static void
add_trigger_form(HtmxElement *parent, JsonObject *existing)
{
    const gchar *id = clawt_web_member(existing, "id", NULL);
    g_autofree gchar *action = NULL;
    g_autofree gchar *escaped = NULL;
    g_autoptr(HtmxForm) form = NULL;
    g_autoptr(HtmxDiv) card = NULL;
    HtmxElement *body;
    g_autofree const gchar **provider_values = NULL;
    g_autofree const gchar **provider_labels = NULL;
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;

    if (id != NULL) {
        escaped = g_uri_escape_string(id, NULL, FALSE);
        action = g_strdup_printf("/triggers/%s/save", escaped);
    } else {
        action = g_strdup("/triggers/add");
    }

    card = clawt_web_card(
        id != NULL ? id : "New trigger",
        id != NULL ? NULL
                   : "Work that happens because somebody else's server "
                     "called, rather than because you asked.");
    body = clawt_web_card_body(card);

    if (id != NULL) {
        const gchar *tone = "good";
        const gchar *state = trigger_state(existing, &tone);

        clawt_web_add(body, clawt_web_notice(state, tone));
        add_endpoint_rows(body, existing);
    }

    form = clawt_web_form(action);

    if (id == NULL)
        clawt_web_add(form, clawt_web_field("Id", "id", NULL,
                                            "ci-failed"));

    /*
     * The providers come from the library's own enumeration, never a
     * list written here. A list in this client and a list in
     * clawt-enums.c would drift, and the drift is a provider the page
     * offers that the daemon does not understand -- which reads as a
     * trigger that refuses every delivery.
     */
    provider_values = g_new0(const gchar *,
                             clawt_trigger_provider_count() + 1);
    provider_labels = g_new0(const gchar *,
                             clawt_trigger_provider_count() + 1);

    for (i = 0; i < clawt_trigger_provider_count(); i++) {
        provider_values[i] = clawt_trigger_provider_nth_nick((guint)i);
        provider_labels[i] = clawt_trigger_provider_nth_label((guint)i);
    }

    /*
     * The fields come from the schema, the same table `trigger.add`
     * walks on the other side. Two lists of a trigger's keys would
     * differ the first time one was added, and the symptom is a field
     * that is accepted, reported as saved, and never read.
     */
    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *leaf;
        const gchar *value;

        if (!g_str_has_prefix(entry->key, "triggers."))
            continue;

        leaf = entry->key + strlen("triggers.");

        if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0)
            continue;

        /* The one list of mappings a trigger holds has its own editor. */
        if (entry->type == CLAWT_SCHEMA_LIST_OF &&
            g_strcmp0(leaf, "match") == 0) {
            add_match_editor(HTMX_ELEMENT(form), existing, id, escaped);
            continue;
        }

        if (entry->type == CLAWT_SCHEMA_SECTION ||
            entry->type == CLAWT_SCHEMA_MAPPING ||
            entry->type == CLAWT_SCHEMA_LIST_OF)
            continue;

        /*
         * A secret is never a form field. It is generated by the daemon
         * and shown once; a field that could send one would put it in a
         * POST body, a browser's autofill and quite possibly a log.
         */
        if (entry->type == CLAWT_SCHEMA_SECRET)
            continue;

        value = clawt_web_member(existing, leaf, NULL);

        if (g_strcmp0(leaf, "provider") == 0) {
            clawt_web_add(form, clawt_web_select_field(
                "Sent by", leaf, provider_values, provider_labels,
                value != NULL ? value : "generic"));
            continue;
        }

        if (g_strcmp0(leaf, "mode") == 0) {
            add_mode_field(HTMX_ELEMENT(form), existing);
            continue;
        }

        if (g_strcmp0(leaf, "max_pending") == 0) {
            add_pending_field(HTMX_ELEMENT(form), existing, entry);
            continue;
        }

        if (entry->type == CLAWT_SCHEMA_BOOLEAN) {
            clawt_web_add(form, clawt_web_switch_field(
                leaf, leaf, entry->doc,
                clawt_web_schema_flag(existing, leaf)));
            continue;
        }

        if (entry->type == CLAWT_SCHEMA_STRING_LIST) {
            JsonArray *array = clawt_web_member_array(existing, leaf);
            g_autoptr(GString) joined = g_string_new(NULL);
            guint e;

            for (e = 0; array != NULL && e < json_array_get_length(array);
                 e++) {
                if (joined->len > 0)
                    g_string_append(joined, ", ");

                g_string_append(joined,
                                json_array_get_string_element(array, e));
            }

            clawt_web_add(form, clawt_web_field(leaf, leaf, joined->str,
                                                entry->doc));
            continue;
        }

        if (g_strcmp0(leaf, "instructions") == 0) {
            clawt_web_add(form, clawt_web_textarea_field(
                "Instructions", leaf, value, 6));
            continue;
        }

        /*
         * An integer read with the integer reader.  The string reader
         * answers its fallback for a JSON number, so the field would be
         * drawn empty and the next save would write 0 over whatever the
         * file said -- silently, on every edit of an unrelated field.
         */
        if (entry->type == CLAWT_SCHEMA_INT) {
            g_autofree gchar *number = g_strdup_printf(
                "%" G_GINT64_FORMAT,
                clawt_web_member_int(existing, leaf,
                                     entry->default_value != NULL
                                         ? g_ascii_strtoll(
                                               entry->default_value, NULL,
                                               10)
                                         : 0));

            clawt_web_add(form, clawt_web_field(leaf, leaf, number,
                                                entry->doc));
            continue;
        }

        clawt_web_add(form, clawt_web_field(leaf, leaf, value, entry->doc));
    }

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) save = clawt_web_button(
            id != NULL ? "Save" : "Add trigger", "primary");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));

        if (id != NULL) {
            g_autofree gchar *test = g_strdup_printf("/triggers/%s/test",
                                                     escaped);
            g_autofree gchar *capture =
                g_strdup_printf("/triggers/%s/capture", escaped);
            g_autofree gchar *replay =
                g_strdup_printf("/triggers/%s/replay", escaped);
            clawt_web_add(row, clawt_web_post_button("Explain latest delivery",
                                                     replay, "default", NULL));
            g_autofree gchar *rotate = g_strdup_printf("/triggers/%s/rotate",
                                                       escaped);
            g_autofree gchar *remove = g_strdup_printf("/triggers/%s/remove",
                                                       escaped);

            clawt_web_add(row, clawt_web_post_button("Preview prompt", test,
                                                     "default", NULL));
            clawt_web_add(row, clawt_web_post_button("First delivery",
                                                     capture, "default",
                                                     NULL));
            clawt_web_add(row, clawt_web_post_button(
                "Rotate secret", rotate, "danger",
                "Rotate? The old secret and address stop working at once, "
                "and the webhook needs both new ones."));
            clawt_web_add(row, clawt_web_post_button(
                "Remove", remove, "danger", "Remove this trigger?"));
        }

        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/*
 * The receipts, which are how "nothing happened" is answerable.
 *
 * Four things produce no run -- the endpoint was wrong, the secret was
 * wrong, the event was filtered out, or the run failed -- and without
 * this they are indistinguishable from each other and from a forge that
 * never called.
 */
static void
add_deliveries(ClawtWebApp *app, HtmxElement *pad)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "trigger.deliveries",
                                                   NULL);
    JsonArray *deliveries;
    g_autoptr(HtmxDiv) card = clawt_web_card("Recent deliveries", NULL);
    HtmxElement *body = clawt_web_card_body(card);
    guint i;

    deliveries = clawt_web_member_array(clawt_web_root(reply), "deliveries");

    if (deliveries == NULL || json_array_get_length(deliveries) == 0) {
        clawt_web_add(body, clawt_web_empty(
            "Nothing yet",
            clawt_web_member(clawt_web_root(reply), "note", NULL)));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
        return;
    }

    for (i = 0; i < json_array_get_length(deliveries); i++) {
        JsonObject *row = json_array_get_object_element(deliveries, i);
        const gchar *detail = clawt_web_member(row, "detail", NULL);
        g_autofree gchar *value = g_strdup_printf(
            "%s \342\200\224 %s%s%s",
            clawt_web_member(row, "event", "-"),
            clawt_web_member(row, "outcome", "?"),
            detail != NULL ? ": " : "", detail != NULL ? detail : "");

        clawt_web_add(body, clawt_web_row(clawt_web_member(row, "trigger",
                                                           "?"),
                                          value));
    }

    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
}

HtmxElement *
clawt_web_triggers_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "trigger.list", NULL);
    JsonObject *root = clawt_web_root(reply);
    JsonArray *triggers;
    guint i;

    /*
     * Discarded, as the routines page discards it: triggers are
     * fleet-wide, and the agent is only here so the sidebar and the
     * topbar keep their selection.
     */
    (void)agent_id;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Triggers"));
    clawt_web_add(pad, clawt_web_text(
        "A routine is a clock; a trigger is an event. The receiver is its "
        "own server on its own port and serves nothing but /health and "
        "the secret /hooks paths, so putting it behind a tunnel exposes "
        "no other part of the machine.", "lede"));

    if (root != NULL && !clawt_web_member_bool(root, "receiving", FALSE))
        clawt_web_add(pad, clawt_web_notice(
            "The receiver is not running, so nothing can arrive. Set "
            "daemon.webhook_enabled.", "warn"));

    triggers = clawt_web_member_array(root, "triggers");

    if (triggers == NULL || json_array_get_length(triggers) == 0)
        clawt_web_add(pad, clawt_web_empty("No triggers", NULL));

    for (i = 0; triggers != NULL && i < json_array_get_length(triggers); i++)
        add_trigger_form(HTMX_ELEMENT(pad),
                         json_array_get_object_element(triggers, i));

    add_trigger_form(HTMX_ELEMENT(pad), NULL);
    add_deliveries(app, HTMX_ELEMENT(pad));

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── Handlers ────────────────────────────────────────────────────── */

static HtmxResponse *
triggers_page(ClawtWebApp *app, HtmxRequest *request, const gchar *toast,
              gboolean failed)
{
    g_autofree gchar *first = clawt_web_first_agent(app);

    if (failed)
        return clawt_web_error_page(app, request, first,
                                    CLAWT_PAGE_TRIGGERS, toast);

    return clawt_web_after_action(app, request, first,
                                  CLAWT_PAGE_TRIGGERS, toast);
}

/*
 * Reads the posted form, dispatching on what the schema says each key
 * is.
 *
 * A list written as a scalar is accepted, echoed back, saved, and read
 * as the default -- and the default for `events` is empty, which means
 * *every* event. So the failure would not be an ignored filter; it would
 * be the narrowest instruction somebody can give turning into the
 * widest.
 */
/*
 * The condition lines of a posted form, as `path=value` strings -- the
 * spelling the daemon reads from a form, split at the first '=' because
 * a path cannot hold one.  A line with both boxes empty is no line; one
 * with only a value is sent, so the daemon can say the path is missing
 * rather than the value silently vanishing.
 */
static GStrv
match_lines(HtmxRequest *request)
{
	g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
	guint n;

	for (n = 0; n < CLAWT_TRIGGER_MATCH_MAX; n++) {
		g_autofree gchar *path_name = g_strdup_printf("match_path_%u", n);
		g_autofree gchar *value_name = g_strdup_printf("match_value_%u", n);
		const gchar *path = clawt_web_form_value(request, path_name);
		const gchar *value = clawt_web_form_value(request, value_name);
		g_autofree gchar *trimmed = g_strstrip(g_strdup(path != NULL ? path : ""));

		if (*trimmed == '\0' && (value == NULL || *value == '\0'))
			continue;

		g_ptr_array_add(out, g_strdup_printf("%s=%s", trimmed,
		                                     value != NULL ? value : ""));
	}

	g_ptr_array_add(out, NULL);

	return (GStrv)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

static JsonNode *
trigger_payload(HtmxRequest *request, const gchar *id)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;

    clawt_web_payload_set(payload, "id", id);

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *leaf;
        const gchar *value;

        if (!g_str_has_prefix(entry->key, "triggers."))
            continue;

        leaf = entry->key + strlen("triggers.");

        if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0)
            continue;

        /* Never posted, never accepted. See add_trigger_form(). */
        if (entry->type == CLAWT_SCHEMA_SECRET)
            continue;

        if (entry->type == CLAWT_SCHEMA_LIST_OF) {
            if (g_strcmp0(leaf, "match") == 0 &&
                clawt_web_form_had(request, "match__present")) {
                g_auto(GStrv) lines = match_lines(request);

                /*
                 * No lines over a filter the daemon could not read is the
                 * page drawing nothing, not somebody clearing it.
                 */
                if (lines[0] == NULL &&
                    clawt_web_form_had(request, "match__unreadable"))
                    continue;

                clawt_web_payload_set_list(payload, leaf,
                                           (const gchar *const *)lines);
            }
            continue;
        }

        if (entry->type == CLAWT_SCHEMA_BOOLEAN) {
            if (clawt_web_form_had(request, leaf))
                clawt_web_payload_set_bool(payload, leaf,
                                           clawt_web_form_flag(request,
                                                               leaf));
            continue;
        }

        value = clawt_web_form_value(request, leaf);

        if (value == NULL)
            continue;

        if (entry->type == CLAWT_SCHEMA_INT) {
            clawt_web_payload_set_int(payload, leaf,
                                      g_ascii_strtoll(value, NULL, 10));
            continue;
        }

        /*
         * A list goes over as the comma-separated string the field
         * holds; the daemon splits it, because it is the side that
         * knows the key is a list.
         */
        clawt_web_payload_set(payload, leaf, value);
    }

    return clawt_web_payload_take(g_steal_pointer(&payload));
}

static HtmxResponse *
on_trigger_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *id = clawt_web_form_value(request, "id");
    g_autoptr(JsonNode) reply = NULL;

    (void)params;

    if (id == NULL || *id == '\0')
        return triggers_page(app, request, "A trigger needs a name.", TRUE);

    reply = clawt_web_app_call(app, "trigger.add",
                               trigger_payload(request, id));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    /*
     * The one and only time the secret is rendered.
     *
     * It is put in the banner rather than a card, because the banner is
     * what somebody is already looking at after a submit -- and because
     * a card would be re-rendered on the next refresh, at which point
     * there would be no secret to put in it and the page would silently
     * lose the value somebody had not copied yet.
     */
    {
        JsonObject *root = clawt_web_root(reply);
        const gchar *secret = clawt_web_member(root, "secret", NULL);
        const gchar *endpoint = clawt_web_member(root, "endpoint", NULL);
        g_autofree gchar *told = NULL;

        /*
         * The sentence is printed because the daemon said the value is
         * shown once, not because this client assumes so -- the same
         * reading the GTK client takes, for the same reason.
         */
        told = g_strdup_printf(
            "Added, switched off. Path /hooks/%s, secret %s.%s The first "
            "delivery that authenticates is held for you to read.",
            endpoint != NULL ? endpoint : "?",
            secret != NULL ? secret : "?",
            clawt_web_member_bool(root, "secret_shown_once", FALSE)
                ? " This is the only time the secret is shown; if you"
                  " lose it, rotate."
                : "");

        return triggers_page(app, request, told, FALSE);
    }
}

static HtmxResponse *
on_trigger_save(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_web_app_call(app, "trigger.update",
                               trigger_payload(request, id));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    return triggers_page(app, request, "Saved.", FALSE);
}

static HtmxResponse *
on_trigger_rotate(HtmxRequest *request, GHashTable *params,
                  gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "id", id);
    reply = clawt_web_app_call(app, "trigger.rotate",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    {
        JsonObject *root = clawt_web_root(reply);
        g_autofree gchar *told = g_strdup_printf(
            "Rotated. Path /hooks/%s, secret %s. The old secret and the "
            "old address stopped working just now, so the webhook needs "
            "both of these.%s",
            clawt_web_member(root, "endpoint", "?"),
            clawt_web_member(root, "secret", "?"),
            clawt_web_member_bool(root, "secret_shown_once", FALSE)
                ? " This is the only time the secret is shown."
                : "");

        return triggers_page(app, request, told, FALSE);
    }
}

static HtmxResponse *
on_trigger_remove(HtmxRequest *request, GHashTable *params,
                  gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "id", id);
    reply = clawt_web_app_call(app, "trigger.remove",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    return triggers_page(app, request, "Removed.", FALSE);
}

/*
 * Shows what the agent would be asked, without asking it.
 *
 * The banner carries the whole expanded prompt, fence and all: seeing
 * the untrusted-payload block is most of the point, because it is the
 * thing that stops a webhook body reading as an instruction.
 */
static HtmxResponse *
on_trigger_test(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "id", id);
    reply = clawt_web_app_call(app, "trigger.test",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    return triggers_page(app, request,
                         clawt_web_member(clawt_web_root(reply), "report",
                             clawt_web_member(clawt_web_root(reply), "prompt", "")),
                         FALSE);
}

/* The persisted snapshot uses the same filters as a live delivery;
 * this action only explains it. Execution requires the CLI's --run. */
static HtmxResponse *
on_trigger_replay(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "id", id);
    reply = clawt_web_app_call(app, "trigger.replay",
        clawt_web_payload_take(g_steal_pointer(&payload)));
    if (reply == NULL)
        return triggers_page(app, request, "No replayable delivery is available.", TRUE);
    return triggers_page(app, request,
        clawt_web_member(clawt_web_root(reply), "report", ""), FALSE);
}

static HtmxResponse *
on_trigger_capture(HtmxRequest *request, GHashTable *params,
                   gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *captured;

    clawt_web_payload_set(payload, "id", id);
    reply = clawt_web_app_call(app, "trigger.capture",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    captured = clawt_web_member(clawt_web_root(reply), "payload", NULL);

    if (captured == NULL)
        return triggers_page(app, request,
                             "Nothing has arrived yet. Send it a test "
                             "delivery from the forge.", FALSE);

    return triggers_page(app, request, captured, FALSE);
}

/*
 * The lines as they stand against the newest body, as a fragment for the
 * box under them: a verdict, each line's result, every field in that
 * body, and the datalist the path boxes complete from -- so after one
 * check a path is picked from the forge's real shape rather than guessed.
 */
static HtmxResponse *
on_trigger_match_check(HtmxRequest *request, GHashTable *params,
                       gpointer user_data)
{
	ClawtWebApp *app = user_data;
	g_autofree gchar *id = clawt_web_param(params, "trigger");
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(HtmxDiv) box = htmx_div_new();
	g_auto(GStrv) lines = match_lines(request);
	g_autofree gchar *list_id = paths_list_id(id);
	JsonObject *root;
	JsonArray *results;
	JsonArray *paths;
	guint i;

	clawt_web_payload_set(payload, "id", id);
	clawt_web_payload_set_list(payload, "match", (const gchar *const *)lines);
	reply = clawt_web_app_call(app, "trigger.match_test",
	                           clawt_web_payload_take(g_steal_pointer(&payload)));

	if (reply == NULL) {
		g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

		clawt_web_add(box, clawt_web_notice(why, "bad"));
		return clawt_web_fragment_response(HTMX_ELEMENT(box));
	}

	root = clawt_web_root(reply);

	if (clawt_web_member(root, "payload", NULL) == NULL) {
		clawt_web_add(box, clawt_web_notice(
			clawt_web_member(root, "note", "Nothing has arrived yet."),
			"info"));
		return clawt_web_fragment_response(HTMX_ELEMENT(box));
	}

	{
		g_autofree gchar *verdict = clawt_web_member_bool(root, "matches", FALSE)
			? g_strdup("The last delivery would have been let through.")
			: g_strdup_printf("The last delivery would have been ignored: %s",
			                  clawt_web_member(root, "reason", "no reason given"));

		clawt_web_add(box, clawt_web_notice(
			verdict, clawt_web_member_bool(root, "matches", FALSE)
				? "info" : "warn"));
	}

	results = clawt_web_member_array(root, "results");

	for (i = 0; results != NULL && i < json_array_get_length(results); i++) {
		JsonObject *row = json_array_get_object_element(results, i);
		g_autofree gchar *title = g_strdup_printf(
			"%s contains \342\200\234%s\342\200\235",
			clawt_web_member(row, "path", "?"),
			clawt_web_member(row, "value", ""));
		g_autofree gchar *said = !clawt_web_member_bool(row, "found", FALSE)
			? g_strdup("no: nothing is there")
			: g_strdup_printf("%s: it is \342\200\234%s\342\200\235",
			                  clawt_web_member_bool(row, "matched", FALSE)
			                      ? "yes" : "no",
			                  clawt_web_member(row, "actual", ""));

		clawt_web_add(box, clawt_web_row(title, said));
	}

	paths = clawt_web_member_array(root, "paths");

	if (paths != NULL && json_array_get_length(paths) > 0) {
		g_autoptr(HtmxDatalist) list = htmx_datalist_new_with_id(list_id);

		clawt_web_add(box, clawt_web_text(
			"Fields in it -- the path boxes above now offer these:",
			"check-sub"));

		for (i = 0; i < json_array_get_length(paths); i++) {
			JsonObject *row = json_array_get_object_element(paths, i);
			const gchar *path = clawt_web_member(row, "path", "");
			const gchar *value = clawt_web_member(row, "value", "");
			g_autoptr(HtmxOption) option = htmx_option_new_with_value(path,
			                                                         value);

			htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(option));
			clawt_web_add(box, clawt_web_row(path, value));
		}

		htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(list));
	}

	return clawt_web_fragment_response(HTMX_ELEMENT(box));
}

void
clawt_web_register_triggers(HtmxRouter *router, ClawtWebApp *app)
{
    /*
     * Under /triggers, never under /a/:id -- and registered before
     * clawt_web_register_views() in any case, since "/a/:id/:view"
     * matches everything below an agent and renders the chat page with
     * a 200 for anything after it.
     */
    htmx_router_post(router, "/triggers/add", on_trigger_add, app);
    htmx_router_post(router, "/triggers/:trigger/save", on_trigger_save, app);
    htmx_router_post(router, "/triggers/:trigger/rotate", on_trigger_rotate,
                     app);
    htmx_router_post(router, "/triggers/:trigger/remove", on_trigger_remove,
                     app);
    htmx_router_post(router, "/triggers/:trigger/test", on_trigger_test, app);
    htmx_router_post(router, "/triggers/:trigger/replay", on_trigger_replay, app);
    htmx_router_post(router, "/triggers/:trigger/capture", on_trigger_capture,
                     app);
    htmx_router_post(router, "/f/triggers/:trigger/match",
                     on_trigger_match_check, app);
}
