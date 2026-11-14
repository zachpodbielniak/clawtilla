/*
 * lrg-work-model.c - Decisions behind the Work and Automation pages
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * See clawt-lrg-work-model.h.  No libregnum header is included here, on
 * purpose: tests/test-lrg-models-work.c #includes this file and runs it
 * with no display.
 */

#include "clawt-lrg-work-model.h"

#include <string.h>

/* "—", spelled once */
#define DASH " \342\200\224 "

/* ── Schema-driven fields ─────────────────────────────────────────── */

static const ClawtSchemaEntry *
schema_entry(const gchar *prefix, const gchar *leaf)
{
    const ClawtSchemaEntry *entries;
    gsize n_entries = 0;
    gsize i;
    gsize prefix_len = strlen(prefix);

    entries = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        if (strncmp(entries[i].key, prefix, prefix_len) == 0 &&
            g_strcmp0(entries[i].key + prefix_len, leaf) == 0)
            return &entries[i];
    }

    return NULL;
}

gchar *
clawt_lrg_work_first_paragraph(const gchar *doc)
{
    const gchar *end;
    g_autofree gchar *para = NULL;
    gchar **lines;
    gchar *joined;

    if (doc == NULL)
        return g_strdup("");

    /*
     * The schema's documentation is hard-wrapped for the generated YAML,
     * and a hint under a control is one paragraph that the label wraps
     * itself: re-joined, or it breaks mid-sentence at column 72.
     */
    end = strstr(doc, "\n\n");
    para = (end != NULL) ? g_strndup(doc, (gsize)(end - doc)) : g_strdup(doc);
    lines = g_strsplit(para, "\n", -1);
    joined = g_strjoinv(" ", lines);
    g_strfreev(lines);

    return joined;
}

static void
schema_field_free(gpointer data)
{
    ClawtLrgSchemaField *field = data;

    g_free(field->hint);
    g_free(field);
}

GPtrArray *
clawt_lrg_work_extra_fields(const gchar *prefix, const gchar *const *handled)
{
    GPtrArray *fields = g_ptr_array_new_with_free_func(schema_field_free);
    const ClawtSchemaEntry *entries;
    gsize n_entries = 0;
    gsize prefix_len = strlen(prefix);
    gsize i;

    entries = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const gchar *leaf;
        ClawtLrgFieldKind kind;
        ClawtLrgSchemaField *field;

        if (strncmp(entries[i].key, prefix, prefix_len) != 0)
            continue;

        leaf = entries[i].key + prefix_len;

        /* a nested key belongs to a sub-table, and `id` is the name */
        if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0)
            continue;

        if (handled != NULL && g_strv_contains(handled, leaf))
            continue;

        switch (entries[i].type) {
        case CLAWT_SCHEMA_BOOLEAN:
            kind = CLAWT_LRG_FIELD_SWITCH;
            break;

        case CLAWT_SCHEMA_INT:
            kind = CLAWT_LRG_FIELD_NUMBER;
            break;

        case CLAWT_SCHEMA_STRING_LIST:
            kind = CLAWT_LRG_FIELD_LIST;
            break;

        case CLAWT_SCHEMA_STRING:
        case CLAWT_SCHEMA_PATH:
        case CLAWT_SCHEMA_ENUM:
        case CLAWT_SCHEMA_DOUBLE:
            kind = CLAWT_LRG_FIELD_TEXT;
            break;

        case CLAWT_SCHEMA_SECRET:
        case CLAWT_SCHEMA_MAPPING:
        default:
            /*
             * A secret is minted by the daemon and posting one is
             * refused; a mapping has no one-line spelling.  Neither is
             * a field.
             */
            continue;
        }

        field = g_new0(ClawtLrgSchemaField, 1);
        field->leaf = leaf;
        field->kind = kind;
        field->hint = clawt_lrg_work_first_paragraph(entries[i].doc);
        g_ptr_array_add(fields, field);
    }

    return fields;
}

gboolean
clawt_lrg_work_boolean(JsonObject *object, const gchar *prefix,
                       const gchar *leaf)
{
    const ClawtSchemaEntry *entry = schema_entry(prefix, leaf);
    gboolean fallback = entry != NULL &&
                        g_strcmp0(entry->default_value, "true") == 0;

    if (object == NULL)
        return fallback;

    return clawt_ipc_payload_boolean(object, leaf, fallback);
}

gchar *
clawt_lrg_work_joined(JsonObject *object, const gchar *member)
{
    g_auto(GStrv) values = NULL;

    if (object == NULL)
        return g_strdup("");

    values = clawt_ipc_payload_strv(object, member);

    if (values == NULL)
        return g_strdup("");

    return g_strjoinv(", ", values);
}

static JsonNode *list_node(const gchar *text);

gchar *
clawt_lrg_work_field_label(const gchar *leaf)
{
    gchar *label = g_strdup((leaf != NULL) ? leaf : "");
    gchar *p;

    for (p = label; *p != '\0'; p++) {
        if (*p == '_')
            *p = ' ';
    }

    label[0] = g_ascii_toupper(label[0]);
    return label;
}

gchar *
clawt_lrg_work_field_text(JsonObject *object, const gchar *leaf,
                          ClawtLrgFieldKind kind)
{
    const gchar *text;

    if (object == NULL || !json_object_has_member(object, leaf))
        return g_strdup("");

    switch (kind) {
    case CLAWT_LRG_FIELD_NUMBER:
        return g_strdup_printf("%" G_GINT64_FORMAT,
                               clawt_ipc_payload_int(object, leaf, 0));

    case CLAWT_LRG_FIELD_LIST:
        return clawt_lrg_work_joined(object, leaf);

    case CLAWT_LRG_FIELD_SWITCH:
        return g_strdup(clawt_ipc_payload_boolean(object, leaf, FALSE)
                        ? "true" : "false");

    case CLAWT_LRG_FIELD_TEXT:
        break;
    }

    text = clawt_ipc_payload_string(object, leaf);
    return g_strdup((text != NULL) ? text : "");
}

JsonNode *
clawt_lrg_work_field_value(ClawtLrgFieldKind kind, const gchar *text,
                           gboolean active, GError **error)
{
    JsonNode *node;

    switch (kind) {
    case CLAWT_LRG_FIELD_SWITCH:
        node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, active);
        return node;

    case CLAWT_LRG_FIELD_NUMBER: {
        gchar *end = NULL;
        gint64 value;

        if (text == NULL || *text == '\0') {
            value = 0;
        } else {
            value = g_ascii_strtoll(text, &end, 10);

            if (end == NULL || *end != '\0') {
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "\"%s\" is not a whole number.", text);
                return NULL;
            }
        }

        node = json_node_new(JSON_NODE_VALUE);
        json_node_set_int(node, value);
        return node;
    }

    case CLAWT_LRG_FIELD_LIST:
        return list_node(text);

    case CLAWT_LRG_FIELD_TEXT:
        break;
    }

    node = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(node, (text != NULL) ? text : "");
    return node;
}

void
clawt_lrg_work_agent_choices(JsonNode *agents, GStrv *ids, GStrv *labels)
{
    JsonObject *root = clawt_ipc_reply_object(agents);
    JsonArray *array = NULL;
    GPtrArray *id_array = g_ptr_array_new();
    GPtrArray *label_array = g_ptr_array_new();
    guint i;

    if (root != NULL && json_object_has_member(root, "agents") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "agents")))
        array = json_object_get_array_member(root, "agents");

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        JsonObject *agent = json_array_get_object_element(array, i);
        const gchar *id = (agent != NULL)
            ? clawt_ipc_payload_string(agent, "id") : NULL;
        const gchar *name;

        if (id == NULL)
            continue;

        name = clawt_ipc_payload_string(agent, "name");
        g_ptr_array_add(id_array, g_strdup(id));
        g_ptr_array_add(label_array, g_strdup((name != NULL) ? name : id));
    }

    g_ptr_array_add(id_array, NULL);
    g_ptr_array_add(label_array, NULL);

    *ids = (GStrv)g_ptr_array_free(id_array, FALSE);
    *labels = (GStrv)g_ptr_array_free(label_array, FALSE);
}

/* Splits "a, b,,c" into ["a", "b", "c"] as a JSON array. */
static JsonNode *
list_node(const gchar *text)
{
    JsonArray *array = json_array_new();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    g_auto(GStrv) parts = g_strsplit((text != NULL) ? text : "", ",", -1);
    guint i;

    for (i = 0; parts[i] != NULL; i++) {
        g_strstrip(parts[i]);

        if (*parts[i] != '\0')
            json_array_add_string_element(array, parts[i]);
    }

    json_node_take_array(node, array);
    return node;
}

static gboolean
blank(const gchar *text)
{
    const gchar *p;

    if (text == NULL)
        return TRUE;

    for (p = text; *p != '\0'; p++) {
        if (!g_ascii_isspace(*p))
            return FALSE;
    }

    return TRUE;
}

/* Copies every member of @extras into @builder's open object. */
static void
add_extras(JsonBuilder *builder, JsonObject *extras)
{
    GList *members;
    GList *l;

    if (extras == NULL)
        return;

    members = json_object_get_members(extras);

    for (l = members; l != NULL; l = l->next) {
        json_builder_set_member_name(builder, l->data);
        json_builder_add_value(builder,
                               json_node_copy(json_object_get_member(
                                   extras, l->data)));
    }

    g_list_free(members);
}

static void
add_string(JsonBuilder *builder, const gchar *name, const gchar *value)
{
    json_builder_set_member_name(builder, name);
    json_builder_add_string_value(builder, (value != NULL) ? value : "");
}

static void
add_boolean(JsonBuilder *builder, const gchar *name, gboolean value)
{
    json_builder_set_member_name(builder, name);
    json_builder_add_boolean_value(builder, value);
}

/* ── Routines ─────────────────────────────────────────────────────── */

void
clawt_lrg_schedule_choices(GStrv *ids, GStrv *labels)
{
    GEnumClass *klass = g_type_class_ref(CLAWT_TYPE_SCHEDULE);
    GPtrArray *id_array = g_ptr_array_new();
    GPtrArray *label_array = g_ptr_array_new();
    guint i;

    for (i = 0; i < klass->n_values; i++) {
        const gchar *nick = klass->values[i].value_nick;
        gchar *label = g_strdup(nick);

        label[0] = g_ascii_toupper(label[0]);
        g_ptr_array_add(id_array, g_strdup(nick));
        g_ptr_array_add(label_array, label);
    }

    g_ptr_array_add(id_array, NULL);
    g_ptr_array_add(label_array, NULL);
    g_type_class_unref(klass);

    *ids = (GStrv)g_ptr_array_free(id_array, FALSE);
    *labels = (GStrv)g_ptr_array_free(label_array, FALSE);
}

void
clawt_lrg_weekday_choices(GStrv *ids, GStrv *labels)
{
    static const gchar *const days[] = {
        "sunday", "monday", "tuesday", "wednesday", "thursday", "friday",
        "saturday"
    };
    GPtrArray *id_array = g_ptr_array_new();
    GPtrArray *label_array = g_ptr_array_new();
    guint i;

    for (i = 0; i < G_N_ELEMENTS(days); i++) {
        /* 2023-01-01 was a Sunday: day i of that week names day i */
        g_autoptr(GDateTime) day =
            g_date_time_new_utc(2023, 1, 1 + (gint)i, 12, 0, 0.0);

        g_ptr_array_add(id_array, g_strdup(days[i]));
        g_ptr_array_add(label_array, g_date_time_format(day, "%A"));
    }

    g_ptr_array_add(id_array, NULL);
    g_ptr_array_add(label_array, NULL);

    *ids = (GStrv)g_ptr_array_free(id_array, FALSE);
    *labels = (GStrv)g_ptr_array_free(label_array, FALSE);
}

ClawtLrgScheduleUses
clawt_lrg_schedule_uses(const gchar *schedule)
{
    ClawtLrgScheduleUses uses = { FALSE, FALSE, FALSE, FALSE };
    gint value = 0;

    if (schedule == NULL ||
        !clawt_enum_from_nick(CLAWT_TYPE_SCHEDULE, schedule, &value))
        return uses;

    /* No `default:` -- -Wswitch names a schedule added to the enum. */
    switch ((ClawtSchedule)value) {
    case CLAWT_SCHEDULE_MANUAL:
        break;

    case CLAWT_SCHEDULE_HOURLY:
        uses.at = TRUE;
        uses.minute_only = TRUE;
        break;

    case CLAWT_SCHEDULE_DAILY:
    case CLAWT_SCHEDULE_WEEKDAYS:
        uses.at = TRUE;
        break;

    case CLAWT_SCHEDULE_WEEKLY:
        uses.at = TRUE;
        uses.weekday = TRUE;
        break;

    case CLAWT_SCHEDULE_CUSTOM:
        uses.cron = TRUE;
        break;
    }

    return uses;
}

gboolean
clawt_lrg_parse_at(const gchar *at, gint *hour, gint *minute)
{
    gchar *end = NULL;
    gint64 h;
    gint64 m;

    *hour = 0;
    *minute = 0;

    if (at == NULL || !g_ascii_isdigit(at[0]))
        return FALSE;

    h = g_ascii_strtoll(at, &end, 10);

    if (end == NULL || *end != ':' || (end - at) > 2)
        return FALSE;

    at = end + 1;

    if (!g_ascii_isdigit(at[0]) || !g_ascii_isdigit(at[1]) || at[2] != '\0')
        return FALSE;

    m = g_ascii_strtoll(at, NULL, 10);

    if (h < 0 || h > 23 || m < 0 || m > 59)
        return FALSE;

    *hour = (gint)h;
    *minute = (gint)m;
    return TRUE;
}

gchar *
clawt_lrg_schedule_preview(const gchar *schedule, const gchar *at,
                           const gchar *weekday, const gchar *cron,
                           GDateTime *after, gboolean *ok)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *expression = NULL;
    g_autoptr(ClawtCron) parsed = NULL;
    g_autoptr(GDateTime) cursor = NULL;
    GString *text;
    guint i;

    if (ok != NULL)
        *ok = FALSE;

    expression = clawt_cron_from_preset(schedule, at, weekday, cron, &error);

    if (expression == NULL) {
        /* `manual` is the one preset with no expression and no error */
        if (error == NULL) {
            if (ok != NULL)
                *ok = TRUE;

            return g_strdup("Only when you ask.");
        }

        return g_strdup_printf("Not a schedule the daemon will take: %s",
                               error->message);
    }

    parsed = clawt_cron_parse(expression, &error);

    if (parsed == NULL)
        return g_strdup_printf("Not a schedule the daemon will take: %s",
                               error->message);

    text = g_string_new(NULL);
    cursor = g_date_time_ref(after);

    for (i = 0; i < 3; i++) {
        g_autoptr(GDateTime) next = clawt_cron_next(parsed, cursor);
        g_autofree gchar *stamp = NULL;

        if (next == NULL)
            break;

        stamp = g_date_time_format(next, "%a %-d %b %H:%M");
        g_string_append(text, (i == 0) ? "Next: "
                                       : (i == 1) ? ", then " : " and ");
        g_string_append(text, stamp);

        g_date_time_unref(cursor);
        cursor = g_steal_pointer(&next);
    }

    /*
     * An expression that parses and never fires -- 30 February -- is a
     * routine that will never run, and is said as one rather than
     * previewed as an empty list.
     */
    if (text->len == 0) {
        g_string_free(text, TRUE);
        return g_strdup_printf("%s never comes round: it will never run.",
                               expression);
    }

    g_string_append_printf(text, ".  (%s)", expression);

    if (ok != NULL)
        *ok = TRUE;

    return g_string_free(text, FALSE);
}

const gchar *
clawt_lrg_routine_title(JsonObject *routine)
{
    const gchar *description = clawt_ipc_payload_string(routine,
                                                        "description");

    if (description != NULL && *description != '\0')
        return description;

    description = clawt_ipc_payload_string(routine, "id");

    return (description != NULL) ? description : "?";
}

gchar *
clawt_lrg_routine_when(JsonObject *routine)
{
    const gchar *next = clawt_ipc_payload_string(routine, "next_run");
    const gchar *problem = clawt_ipc_payload_string(routine, "problem");

    if (next != NULL) {
        g_autoptr(GDateTime) parsed = g_date_time_new_from_iso8601(next,
                                                                   NULL);
        g_autoptr(GDateTime) local = (parsed != NULL)
            ? g_date_time_to_local(parsed) : NULL;

        return (local != NULL)
            ? g_date_time_format(local, "next %a %-d %b at %H:%M")
            : g_strdup_printf("next %s", next);
    }

    /*
     * A schedule that cannot be read is said, not hidden: drawing "only
     * when you ask" for it, as GTK once did, is a wrong answer that
     * looks like a working configuration.
     */
    if (problem != NULL)
        return g_strdup_printf("will never run" DASH "%s", problem);

    if (clawt_lrg_work_boolean(routine, "routines.", "enabled"))
        return g_strdup("only when you ask");

    return g_strdup("off");
}

gchar *
clawt_lrg_routine_last_run(JsonObject *routine, gint64 now_us,
                           const gchar **tone)
{
    const gchar *nick = clawt_ipc_payload_string(routine, "last_state");
    gint64 last = clawt_ipc_payload_int(routine, "last_run", 0);
    g_autofree gchar *ago = NULL;
    gint value = 0;
    const gchar *chosen = "neutral";

    if (tone != NULL)
        *tone = chosen;

    if (nick == NULL ||
        !clawt_enum_from_nick(CLAWT_TYPE_RUN_STATE, nick, &value))
        return NULL;

    switch ((ClawtRunState)value) {
    case CLAWT_RUN_NEVER:
        return NULL;

    case CLAWT_RUN_OK:
        chosen = "good";
        break;

    case CLAWT_RUN_FAILED:
        chosen = "bad";
        break;

    case CLAWT_RUN_MISSED:
        chosen = "warn";
        break;
    }

    if (tone != NULL)
        *tone = chosen;

    /* `last_run` is Unix seconds; the label takes microseconds */
    if (last <= 0)
        return g_strdup(nick);

    ago = clawt_time_ago_label(last * G_USEC_PER_SEC, now_us);
    return g_strdup_printf("%s %s", nick, ago);
}

JsonNode *
clawt_lrg_routine_payload(const ClawtLrgRoutineForm *form, GError **error)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) cron_error = NULL;
    g_autofree gchar *expression = NULL;
    gchar *end = NULL;
    gint64 jitter;

    if (blank(form->id)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "It needs a name.");
        return NULL;
    }

    if (blank(form->instructions) && !form->proactive) {
        /*
         * Refused rather than saved empty: a routine with no
         * instructions fires on schedule and asks an agent for nothing,
         * which costs a turn and produces a puzzled reply.  A proactive
         * one is the exception -- the schema says it is told what to
         * look for when it has no instructions of its own.
         */
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "It needs instructions -- that is the whole of "
                            "what the agent is asked.");
        return NULL;
    }

    if (blank(form->agent)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "There is no agent to run it.");
        return NULL;
    }

    jitter = g_ascii_strtoll((form->jitter != NULL) ? form->jitter : "0",
                             &end, 10);

    if (end == NULL || *end != '\0' || jitter < 0 ||
        (form->jitter != NULL && *form->jitter == '\0')) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "Jitter is a whole number of seconds, zero or "
                            "more.");
        return NULL;
    }

    expression = clawt_cron_from_preset(form->schedule, form->at,
                                        form->weekday, form->cron,
                                        &cron_error);

    if (expression == NULL && cron_error != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "That schedule would be refused: %s",
                    cron_error->message);
        return NULL;
    }

    builder = json_builder_new();
    json_builder_begin_object(builder);
    add_string(builder, "id", form->id);
    add_string(builder, "agent", form->agent);
    add_string(builder, "description", form->description);
    add_string(builder, "instructions", form->instructions);
    add_string(builder, "schedule", form->schedule);
    add_string(builder, "at", form->at);
    add_string(builder, "weekday", form->weekday);
    add_string(builder, "cron", form->cron);
    add_string(builder, "directory", form->directory);
    add_boolean(builder, "worktree", form->worktree);
    add_boolean(builder, "enabled", form->enabled);
    add_boolean(builder, "catch_up", form->catch_up);
    add_boolean(builder, "read_only", form->read_only);
    add_boolean(builder, "proactive", form->proactive);
    json_builder_set_member_name(builder, "jitter_seconds");
    json_builder_add_int_value(builder, jitter);
    add_extras(builder, form->extras);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* ── Triggers ─────────────────────────────────────────────────────── */

void
clawt_lrg_provider_choices(GStrv *ids, GStrv *labels)
{
    guint n = clawt_trigger_provider_count();
    guint i;

    *ids = g_new0(gchar *, n + 1);
    *labels = g_new0(gchar *, n + 1);

    for (i = 0; i < n; i++) {
        (*ids)[i] = g_strdup(clawt_trigger_provider_nth_nick(i));
        (*labels)[i] = g_strdup(clawt_trigger_provider_nth_label(i));
    }
}

void
clawt_lrg_trigger_mode_choices(GStrv *ids, GStrv *labels)
{
	guint n = clawt_trigger_mode_count();
	guint i;

	*ids = g_new0(gchar *, n + 1);
	*labels = g_new0(gchar *, n + 1);

	for (i = 0; i < n; i++) {
		(*ids)[i] = g_strdup(clawt_trigger_mode_nth_nick(i));
		(*labels)[i] = g_strdup(clawt_trigger_mode_nth_label(i));
	}
}

gchar *
clawt_lrg_trigger_mode_hints(void)
{
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; i < clawt_trigger_mode_count(); i++) {
		if (text->len > 0)
			g_string_append_c(text, ' ');

		g_string_append_printf(text, "%s: %s",
		                       clawt_trigger_mode_nth_label(i),
		                       clawt_trigger_mode_nth_hint(i));
	}

	return g_string_free(text, FALSE);
}

JsonNode *
clawt_lrg_match_node(const gchar *const *paths, const gchar *const *values)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	guint i;

	json_builder_begin_array(builder);

	for (i = 0; paths != NULL && paths[i] != NULL; i++) {
		const gchar *value = (values != NULL) ? values[i] : NULL;
		g_autofree gchar *path = g_strstrip(g_strdup(paths[i]));

		/* A row left wholly blank is no condition. */
		if (*path == '\0' && blank(value))
			continue;

		json_builder_begin_object(builder);
		add_string(builder, "path", path);
		add_string(builder, "value", value);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}

gchar *
clawt_lrg_match_report(JsonObject *reply)
{
	GString *text = g_string_new(NULL);
	JsonArray *results = NULL;
	guint i;

	if (reply == NULL || clawt_ipc_payload_string(reply, "payload") == NULL) {
		const gchar *note = (reply != NULL)
			? clawt_ipc_payload_string(reply, "note") : NULL;

		g_string_append(text, (note != NULL) ? note
		                                     : "Nothing has arrived yet.");
		return g_string_free(text, FALSE);
	}

	if (clawt_ipc_payload_boolean(reply, "matches", FALSE)) {
		g_string_append(text, "The last delivery would have been let "
		                      "through.");
	} else {
		const gchar *reason = clawt_ipc_payload_string(reply, "reason");

		g_string_append_printf(text, "The last delivery would have been "
		                       "ignored: %s",
		                       (reason != NULL) ? reason : "no reason given");
	}

	if (json_object_has_member(reply, "results") &&
	    JSON_NODE_HOLDS_ARRAY(json_object_get_member(reply, "results")))
		results = json_object_get_array_member(reply, "results");

	for (i = 0; results != NULL && i < json_array_get_length(results); i++) {
		JsonObject *row = json_array_get_object_element(results, i);
		const gchar *actual = clawt_ipc_payload_string(row, "actual");

		g_string_append_printf(
			text, "\n%s %s contains \"%s\"%s%s%s",
			clawt_ipc_payload_boolean(row, "matched", FALSE) ? "yes" : "no ",
			clawt_ipc_payload_string(row, "path"),
			clawt_ipc_payload_string(row, "value"),
			(actual != NULL) ? DASH "it is \"" : DASH "nothing is there",
			(actual != NULL) ? actual : "",
			(actual != NULL) ? "\"" : "");
	}

	return g_string_free(text, FALSE);
}

const gchar *
clawt_lrg_trigger_state(JsonObject *trigger, const gchar **tone)
{
    const gchar *state;
    const gchar *chosen;

    if (!clawt_ipc_payload_boolean(trigger, "has_secret", FALSE)) {
        state = "no secret -- it refuses everything";
        chosen = "bad";
    } else if (clawt_ipc_payload_boolean(trigger, "pending_verification",
                                         FALSE)) {
        state = "waiting for its first delivery";
        chosen = "warn";
    } else if (!clawt_lrg_work_boolean(trigger, "triggers.", "enabled")) {
        state = "off";
        chosen = "neutral";
    } else {
        state = "on";
        chosen = "good";
    }

    if (tone != NULL)
        *tone = chosen;

    return state;
}

gchar *
clawt_lrg_trigger_subtitle(JsonObject *trigger)
{
    const gchar *agent = clawt_ipc_payload_string(trigger, "agent");
    const gchar *provider = clawt_ipc_payload_string(trigger, "provider");
    g_autofree gchar *others = clawt_lrg_work_joined(trigger, "agents");
    gint64 unfinished = clawt_ipc_payload_int(trigger, "unfinished", 0);
    GString *text = g_string_new(NULL);

    g_string_append_printf(text, "%s" DASH "%s",
                           (agent != NULL) ? agent : "?",
                           (provider != NULL) ? provider : "generic");

    if (*others != '\0')
        g_string_append_printf(text, " (and %s)", others);

    if (unfinished > 0)
        g_string_append_printf(text, DASH "%" G_GINT64_FORMAT
                               " run%s still going", unfinished,
                               unfinished == 1 ? "" : "s");

    return g_string_free(text, FALSE);
}

gchar *
clawt_lrg_trigger_address(const gchar *endpoint, JsonArray *listening)
{
    const gchar *base = NULL;

    if (endpoint == NULL || *endpoint == '\0')
        return NULL;

    if (listening != NULL && json_array_get_length(listening) > 0)
        base = json_array_get_string_element(listening, 0);

    return g_strdup_printf("%s/hooks/%s", (base != NULL) ? base : "",
                           endpoint);
}

gchar *
clawt_lrg_trigger_secret_body(JsonObject *reply, const gchar *lead,
                              JsonArray *listening)
{
    const gchar *secret = clawt_ipc_payload_string(reply, "secret");
    g_autofree gchar *address = clawt_lrg_trigger_address(
        clawt_ipc_payload_string(reply, "endpoint"), listening);
    GString *text;

    if (secret == NULL)
        return NULL;

    text = g_string_new(lead);

    if (address != NULL)
        g_string_append_printf(text, "\n\nThe address: %s", address);

    /*
     * Said because the daemon said so, not because this client assumes
     * it: if a later daemon made a secret re-readable, a hardcoded "this
     * is the only time" would be telling somebody to do something
     * irreversible for no reason.
     */
    if (clawt_ipc_payload_boolean(reply, "secret_shown_once", FALSE))
        g_string_append(text,
                        "\n\nThis is the only time the secret is shown. It "
                        "is not in the listing, the log or an event. If you "
                        "lose it, rotate the trigger -- that makes a new "
                        "secret and a new address, and retires both of "
                        "these.");

    return g_string_free(text, FALSE);
}

const gchar *
clawt_lrg_trigger_report(JsonObject *reply)
{
    const gchar *report = clawt_ipc_payload_string(reply, "report");

    if (report == NULL)
        report = clawt_ipc_payload_string(reply, "prompt");

    return (report != NULL) ? report : "";
}

gchar *
clawt_lrg_delivery_title(JsonObject *delivery)
{
    const gchar *trigger = clawt_ipc_payload_string(delivery, "trigger");
    const gchar *outcome = clawt_ipc_payload_string(delivery, "outcome");

    return g_strdup_printf("%s" DASH "%s", (trigger != NULL) ? trigger : "?",
                           (outcome != NULL) ? outcome : "?");
}

gchar *
clawt_lrg_delivery_subtitle(JsonObject *delivery)
{
    const gchar *at = clawt_ipc_payload_string(delivery, "at");
    const gchar *event = clawt_ipc_payload_string(delivery, "event");
    const gchar *detail = clawt_ipc_payload_string(delivery, "detail");
    gint64 seconds = (at != NULL) ? g_ascii_strtoll(at, NULL, 10) : 0;
    GString *text = g_string_new(NULL);

    if (seconds > 0) {
        g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(seconds);

        if (when != NULL) {
            g_autofree gchar *stamp = g_date_time_format(when,
                                                         "%a %-d %b %H:%M");

            g_string_append(text, stamp);
            g_string_append(text, DASH);
        }
    }

    g_string_append(text, (event != NULL) ? event : "-");

    if (detail != NULL && *detail != '\0') {
        g_string_append(text, DASH);
        g_string_append(text, detail);
    }

    return g_string_free(text, FALSE);
}

JsonNode *
clawt_lrg_trigger_payload(const ClawtLrgTriggerForm *form, GError **error)
{
    g_autoptr(JsonBuilder) builder = NULL;
    gint value = 0;

    if (blank(form->id)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "It needs a name.");
        return NULL;
    }

    if (blank(form->instructions)) {
        /*
         * As a routine's: a trigger with no instructions wakes an agent
         * and asks it for nothing -- and here it happens whenever
         * somebody else pushes.
         */
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "It needs instructions -- that is the whole of "
                            "what the agent is asked.");
        return NULL;
    }

    if (blank(form->agent)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "There is no agent to run it.");
        return NULL;
    }

    if (form->provider == NULL ||
        !clawt_enum_from_nick(CLAWT_TYPE_TRIGGER_PROVIDER, form->provider,
                              &value)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "\"%s\" is not a sender this daemon understands.",
                    (form->provider != NULL) ? form->provider : "");
        return NULL;
    }

    /*
     * Refused here too, with the form still open: the daemon would refuse
     * each, and a refusal that arrives after the dialog closed is the
     * dearer one.
     */
    if (form->mode != NULL &&
        !clawt_enum_from_nick(CLAWT_TYPE_TRIGGER_MODE, form->mode, &value)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "\"%s\" is not a mode this daemon understands.",
                    form->mode);
        return NULL;
    }

    if (form->mode != NULL && value == (gint)CLAWT_TRIGGER_MODE_POST &&
        blank(form->room)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "Just posting needs a room to post into.");
        return NULL;
    }

    /* Zero is "not given", so a form that never had the field sends none. */
    if (form->max_pending != 0 &&
        (form->max_pending < CLAWT_TRIGGER_MAX_PENDING_MIN ||
         form->max_pending > CLAWT_TRIGGER_MAX_PENDING_MAX)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "Runs at once must be %d to %d.",
                    CLAWT_TRIGGER_MAX_PENDING_MIN,
                    CLAWT_TRIGGER_MAX_PENDING_MAX);
        return NULL;
    }

    builder = json_builder_new();
    json_builder_begin_object(builder);
    add_string(builder, "id", form->id);
    add_string(builder, "agent", form->agent);
    add_string(builder, "description", form->description);
    add_string(builder, "instructions", form->instructions);
    add_string(builder, "room", form->room);
    add_string(builder, "provider", form->provider);
    json_builder_set_member_name(builder, "events");
    json_builder_add_value(builder, list_node(form->events));
    json_builder_set_member_name(builder, "agents");
    json_builder_add_value(builder, list_node(form->agents));
    add_string(builder, "repo", form->repo);
    add_string(builder, "branch", form->branch);
    add_string(builder, "header", form->header);
    add_string(builder, "directory", form->directory);
    add_boolean(builder, "worktree", form->worktree);
    add_boolean(builder, "isolate", form->isolate);

    if (!form->creating)
        add_boolean(builder, "enabled", form->enabled);

    if (form->mode != NULL)
        add_string(builder, "mode", form->mode);

    if (form->max_pending != 0) {
        json_builder_set_member_name(builder, "max_pending");
        json_builder_add_int_value(builder, form->max_pending);
    }

    if (form->match != NULL) {
        json_builder_set_member_name(builder, "match");
        json_builder_add_value(builder, json_node_copy(form->match));
    }

    add_extras(builder, form->extras);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* ── Tasks ────────────────────────────────────────────────────────── */

static void
task_row_free(gpointer data)
{
    ClawtLrgTaskRow *row = data;

    json_object_unref(row->task);
    g_free(row);
}

static void
task_tree_walk(GPtrArray *rows, GHashTable *children, GHashTable *placed,
               JsonObject *task, guint depth)
{
    const gchar *id = clawt_ipc_payload_string(task, "id");
    GPtrArray *kids;
    ClawtLrgTaskRow *row;
    guint i;

    if (id != NULL) {
        if (g_hash_table_contains(placed, id))
            return;

        g_hash_table_add(placed, (gpointer)id);
    }

    row = g_new0(ClawtLrgTaskRow, 1);
    row->task = json_object_ref(task);
    row->depth = depth;
    g_ptr_array_add(rows, row);

    kids = (id != NULL) ? g_hash_table_lookup(children, id) : NULL;

    for (i = 0; kids != NULL && i < kids->len; i++)
        task_tree_walk(rows, children, placed, g_ptr_array_index(kids, i),
                       depth + 1);
}

GPtrArray *
clawt_lrg_task_tree(JsonNode *reply)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(task_row_free);
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *tasks = NULL;
    g_autoptr(GHashTable) ids = NULL;
    g_autoptr(GHashTable) children = NULL;
    g_autoptr(GHashTable) placed = NULL;
    guint n;
    guint i;

    if (root != NULL && json_object_has_member(root, "tasks") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "tasks")))
        tasks = json_object_get_array_member(root, "tasks");

    if (tasks == NULL)
        return rows;

    n = json_array_get_length(tasks);
    ids = g_hash_table_new(g_str_hash, g_str_equal);
    children = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                     (GDestroyNotify)g_ptr_array_unref);
    placed = g_hash_table_new(g_str_hash, g_str_equal);

    for (i = 0; i < n; i++) {
        JsonObject *task = json_array_get_object_element(tasks, i);
        const gchar *id = (task != NULL)
            ? clawt_ipc_payload_string(task, "id") : NULL;

        if (id != NULL)
            g_hash_table_add(ids, (gpointer)id);
    }

    /* the member is `parent`, as clawt_daemon_add_task_object() writes it */
    for (i = 0; i < n; i++) {
        JsonObject *task = json_array_get_object_element(tasks, i);
        const gchar *parent = (task != NULL)
            ? clawt_ipc_payload_string(task, "parent") : NULL;
        GPtrArray *kids;

        if (parent == NULL || !g_hash_table_contains(ids, parent))
            continue;

        kids = g_hash_table_lookup(children, parent);

        if (kids == NULL) {
            kids = g_ptr_array_new();
            g_hash_table_insert(children, (gpointer)parent, kids);
        }

        g_ptr_array_add(kids, task);
    }

    for (i = 0; i < n; i++) {
        JsonObject *task = json_array_get_object_element(tasks, i);
        const gchar *parent;

        if (task == NULL)
            continue;

        parent = clawt_ipc_payload_string(task, "parent");

        if (parent == NULL || !g_hash_table_contains(ids, parent))
            task_tree_walk(rows, children, placed, task, 0);
    }

    /*
     * Anything still unplaced is in a cycle, which no root reaches.  It
     * is drawn as a root rather than dropped: a task that vanishes from
     * the list is worse than one drawn in the wrong place.
     */
    for (i = 0; i < n; i++) {
        JsonObject *task = json_array_get_object_element(tasks, i);
        const gchar *id = (task != NULL)
            ? clawt_ipc_payload_string(task, "id") : NULL;

        if (task != NULL && (id == NULL || !g_hash_table_contains(placed, id)))
            task_tree_walk(rows, children, placed, task, 0);
    }

    return rows;
}

gchar *
clawt_lrg_task_title(JsonObject *task)
{
    const gchar *origin = clawt_ipc_payload_string(task, "origin");
    const gchar *assignee = clawt_ipc_payload_string(task, "assignee");

    return g_strdup_printf("%s \342\206\222 %s",
                           (origin != NULL) ? origin : "?",
                           (assignee != NULL) ? assignee : "?");
}

const gchar *
clawt_lrg_task_tone(const gchar *state)
{
    gint value = 0;

    if (state == NULL ||
        !clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &value))
        return "neutral";

    return clawt_task_state_tone((ClawtTaskState)value);
}

gboolean
clawt_lrg_task_cancellable(const gchar *state)
{
    gint value = 0;

    if (state == NULL ||
        !clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &value))
        return FALSE;

    /* No `default:` -- a state added later is decided, not defaulted. */
    switch ((ClawtTaskState)value) {
    case CLAWT_TASK_PENDING:
    case CLAWT_TASK_RUNNING:
        return TRUE;

    case CLAWT_TASK_COMPLETED:
    case CLAWT_TASK_FAILED:
    case CLAWT_TASK_CANCELLED:
    case CLAWT_TASK_STALLED:
        return FALSE;
    }

    return FALSE;
}

const gchar *
clawt_lrg_task_note(JsonObject *task)
{
    const gchar *note = clawt_ipc_payload_string(task, "progress_note");

    if (note != NULL && *note != '\0')
        return note;

    if (clawt_ipc_payload_boolean(task, "result_inferred", FALSE))
        return "nobody reported this finished -- it is the last thing the "
               "assignee wrote";

    return NULL;
}

/* ── Decisions ────────────────────────────────────────────────────── */

JsonArray *
clawt_lrg_decision_items(JsonNode *reply, guint *open)
{
    JsonObject *root = clawt_ipc_reply_object(reply);

    if (open != NULL)
        *open = (root != NULL)
            ? (guint)MAX(clawt_ipc_payload_int(root, "open", 0), 0) : 0;

    if (root == NULL || !json_object_has_member(root, "decisions") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "decisions")))
        return NULL;

    return json_object_get_array_member(root, "decisions");
}

gchar *
clawt_lrg_decision_going(JsonObject *decision)
{
    const gchar *fallback = clawt_ipc_payload_string(decision, "default");
    const gchar *reason = clawt_ipc_payload_string(decision,
                                                   "default_reason");

    if (fallback == NULL || *fallback == '\0')
        return NULL;

    if (reason != NULL && *reason != '\0')
        return g_strdup_printf("Going ahead with: %s" DASH "%s", fallback,
                               reason);

    return g_strdup_printf("Going ahead with: %s", fallback);
}

GStrv
clawt_lrg_decision_options(JsonObject *decision)
{
    g_auto(GStrv) all = clawt_ipc_payload_strv(decision, "options");
    GPtrArray *kept = g_ptr_array_new();
    guint i;

    for (i = 0; all != NULL && all[i] != NULL; i++) {
        if (*all[i] != '\0')
            g_ptr_array_add(kept, g_strdup(all[i]));
    }

    g_ptr_array_add(kept, NULL);
    return (GStrv)g_ptr_array_free(kept, FALSE);
}

gchar *
clawt_lrg_decision_meta(JsonObject *decision, gint64 now_us)
{
    const gchar *agent = clawt_ipc_payload_string(decision, "agent");
    gint64 created = clawt_ipc_payload_int(decision, "created_at", 0);
    g_autofree gchar *ago = NULL;

    if (created <= 0)
        return g_strdup_printf("asked by %s", (agent != NULL) ? agent : "?");

    ago = clawt_time_ago_label(created * G_USEC_PER_SEC, now_us);
    return g_strdup_printf("asked by %s \302\267 %s",
                           (agent != NULL) ? agent : "?", ago);
}

const gchar *
clawt_lrg_decision_answer(const gchar *option, const gchar *typed)
{
    if (!blank(typed))
        return typed;

    if (!blank(option))
        return option;

    return NULL;
}

/* ── Go to… ───────────────────────────────────────────────────────── */

static void
nav_target_free(gpointer data)
{
    ClawtLrgNavTarget *target = data;

    g_free(target->name);
    g_free(target->detail);
    g_free(target->target);
    g_free(target->search);
    g_free(target);
}

static void
nav_add(GPtrArray *targets, ClawtLrgNavKind kind, const gchar *name,
        const gchar *detail, const gchar *target)
{
    ClawtLrgNavTarget *t = g_new0(ClawtLrgNavTarget, 1);

    t->kind = kind;
    t->name = g_strdup(name);
    t->detail = g_strdup(detail);
    t->target = g_strdup(target);
    t->search = g_strdup_printf("%s %s %s", name, detail, target);
    g_ptr_array_add(targets, t);
}

static JsonArray *
reply_array(JsonNode *reply, const gchar *member)
{
    JsonObject *root = clawt_ipc_reply_object(reply);

    if (root == NULL || !json_object_has_member(root, member) ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, member)))
        return NULL;

    return json_object_get_array_member(root, member);
}

GPtrArray *
clawt_lrg_nav_targets(JsonNode *agents, JsonNode *rooms,
                      gboolean agent_selected)
{
    GPtrArray *targets = g_ptr_array_new_with_free_func(nav_target_free);
    JsonArray *array;
    guint n_sections = clawt_section_count();
    guint i;

    array = reply_array(agents, "agents");

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        JsonObject *agent = json_array_get_object_element(array, i);
        const gchar *id = clawt_ipc_payload_string(agent, "id");
        const gchar *name = clawt_ipc_payload_string(agent, "name");
        const gchar *team = clawt_ipc_payload_string(agent, "team");
        g_autofree gchar *detail = NULL;

        if (id == NULL)
            continue;

        detail = g_strdup_printf("Agent \302\267 %s \302\267 %s", id,
                                 (team != NULL) ? team : "");
        nav_add(targets, CLAWT_LRG_NAV_AGENT, (name != NULL) ? name : id,
                detail, id);
    }

    /*
     * Declared rooms only: a fleet accumulates a direct room per pair,
     * and those are reached through the agent they belong to.
     */
    array = reply_array(rooms, "rooms");

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        JsonObject *room = json_array_get_object_element(array, i);
        const gchar *id = clawt_ipc_payload_string(room, "id");
        const gchar *name = clawt_ipc_payload_string(room, "name");
        const gchar *team = clawt_ipc_payload_string(room, "team");
        g_autofree gchar *detail = NULL;

        if (id == NULL || !clawt_room_is_listed(id))
            continue;

        detail = g_strdup_printf("Room \302\267 %s \302\267 %s", id,
                                 (team != NULL) ? team : "");
        nav_add(targets, CLAWT_LRG_NAV_ROOM, (name != NULL) ? name : id,
                detail, id);
    }

    for (i = 0; i < n_sections; i++) {
        ClawtSection section = clawt_section_nth(i);
        guint n_pages = clawt_section_page_count(section);
        guint j;

        for (j = 0; j < n_pages; j++) {
            ClawtPage page = clawt_section_page_nth(section, j);

            if (!agent_selected && page != CLAWT_PAGE_MEMORY)
                continue;

            nav_add(targets, CLAWT_LRG_NAV_PAGE, clawt_page_label(page),
                    clawt_section_label(section), clawt_page_nick(page));
        }
    }

    return targets;
}

GArray *
clawt_lrg_nav_filter(GPtrArray *targets, const gchar *query)
{
    GArray *kept = g_array_new(FALSE, FALSE, sizeof(guint));
    guint i;

    for (i = 0; targets != NULL && i < targets->len; i++) {
        ClawtLrgNavTarget *target = g_ptr_array_index(targets, i);

        if (clawt_navigation_matches(query, target->search))
            g_array_append_val(kept, i);
    }

    return kept;
}

guint
clawt_lrg_nav_move(guint cursor, gint delta, guint n)
{
    gint64 moved;

    if (n == 0)
        return 0;

    moved = (gint64)MIN(cursor, n - 1) + delta;

    if (moved < 0)
        return 0;

    if (moved >= (gint64)n)
        return n - 1;

    return (guint)moved;
}
