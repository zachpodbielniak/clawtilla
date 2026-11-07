/*
 * lrg-settings-model.c - What the Settings dialog and the create
 *                        dialogs decide
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * No libregnum here: see clawt-lrg-settings-model.h for why.  Every
 * member read off a reply below was checked against the
 * json_builder_set_member_name() that produces it in src/core/, and
 * booleans and integers are read with the typed readers -- reading a
 * JSON boolean with the string reader is the bug that silently wrote
 * `enabled: false` back over every routine somebody opened.
 */

#include "clawt-lrg-settings-model.h"

#include <string.h>

/* ── Shared ───────────────────────────────────────────────────────── */

JsonArray *
clawt_lrg_settings_member_array(JsonObject *object, const gchar *member)
{
    JsonNode *node;

    if (object == NULL || member == NULL ||
        !json_object_has_member(object, member))
        return NULL;

    node = json_object_get_member(object, member);

    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
        return NULL;

    return json_node_get_array(node);
}

gchar *
clawt_lrg_settings_join_strings(JsonObject *object, const gchar *member,
                                const gchar *separator)
{
    JsonArray *array = clawt_lrg_settings_member_array(object, member);
    GString *out = g_string_new(NULL);
    guint i;

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        JsonNode *node = json_array_get_element(array, i);

        if (!JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING)
            continue;

        if (out->len > 0)
            g_string_append(out, separator);

        g_string_append(out, json_node_get_string(node));
    }

    return g_string_free(out, FALSE);
}

GStrv
clawt_lrg_settings_split_list(const gchar *text)
{
    GPtrArray *out = g_ptr_array_new();
    g_auto(GStrv) parts = NULL;
    guint i;

    if (text != NULL && *text != '\0') {
        parts = g_strsplit(text, ",", -1);

        for (i = 0; parts[i] != NULL; i++) {
            g_strstrip(parts[i]);

            if (*parts[i] != '\0')
                g_ptr_array_add(out, g_strdup(parts[i]));
        }
    }

    g_ptr_array_add(out, NULL);
    return (GStrv)g_ptr_array_free(out, FALSE);
}

/* A string member, or NULL; never the empty string. */
static const gchar *
nonempty(JsonObject *object, const gchar *member)
{
    const gchar *value = clawt_ipc_payload_string(object, member);

    return (value != NULL && *value != '\0') ? value : NULL;
}

/* Text a person typed, or NULL when they left it blank. */
static const gchar *
typed(const gchar *text)
{
    return (text != NULL && *text != '\0') ? text : NULL;
}

static void
add_string(JsonBuilder *builder, const gchar *member, const gchar *value)
{
    json_builder_set_member_name(builder, member);
    json_builder_add_string_value(builder, value);
}

static void
add_string_array(JsonBuilder *builder, const gchar *member,
                 const gchar *const *items)
{
    guint i;

    json_builder_set_member_name(builder, member);
    json_builder_begin_array(builder);

    for (i = 0; items != NULL && items[i] != NULL; i++)
        json_builder_add_string_value(builder, items[i]);

    json_builder_end_array(builder);
}

static gboolean
refuse(GError **error, const gchar *message)
{
    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        message);
    return FALSE;
}

/* ── Cloud images ─────────────────────────────────────────────────── */

gchar *
clawt_lrg_settings_human_size(gint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        return g_strdup_printf("%.1f GB",
                               (gdouble)bytes / (1024.0 * 1024.0 * 1024.0));

    if (bytes >= 1024 * 1024)
        return g_strdup_printf("%.0f MB", (gdouble)bytes / (1024.0 * 1024.0));

    if (bytes >= 1024)
        return g_strdup_printf("%.0f kB", (gdouble)bytes / 1024.0);

    return g_strdup_printf("%" G_GINT64_FORMAT " B", MAX(bytes, 0));
}

gboolean
clawt_lrg_settings_image_downloading(JsonObject *image)
{
    return clawt_ipc_payload_boolean(image, "downloading", FALSE);
}

gchar *
clawt_lrg_settings_progress_text(gint64 done, gint64 total)
{
    g_autofree gchar *done_text = clawt_lrg_settings_human_size(done);
    g_autofree gchar *total_text = NULL;

    /* a total of nothing is a server that did not say, not an empty file */
    if (total <= 0)
        return g_strdup_printf("%s so far", done_text);

    total_text = clawt_lrg_settings_human_size(total);
    return g_strdup_printf("%s of %s", done_text, total_text);
}

gdouble
clawt_lrg_settings_progress_fraction(gint64 done, gint64 total)
{
    if (total <= 0)
        return -1.0;

    return CLAMP((gdouble)done / (gdouble)total, 0.0, 1.0);
}

gchar *
clawt_lrg_settings_image_subtitle(JsonObject *image)
{
    gint64 bytes = clawt_ipc_payload_int(image, "bytes", 0);
    const gchar *url = nonempty(image, "url");
    g_autofree gchar *size = NULL;

    if (clawt_lrg_settings_image_downloading(image))
        return clawt_lrg_settings_progress_text(
            bytes, clawt_ipc_payload_int(image, "total", 0));

    size = clawt_lrg_settings_human_size(bytes);

    if (url == NULL)
        return g_steal_pointer(&size);

    return g_strdup_printf("%s  \342\200\224  %s", size, url);
}

gchar *
clawt_lrg_settings_image_failed_text(const gchar *name, const gchar *failure)
{
    if (failure == NULL || *failure == '\0')
        return NULL;

    return g_strdup_printf("%s could not be downloaded: %s",
                           (name != NULL) ? name : "An image", failure);
}

/* ── Spending ─────────────────────────────────────────────────────── */

typedef struct {
    const gchar *label;
    gint         days;      /* 0 = everything, -1 = since local midnight */
} SpendingPeriod;

static const SpendingPeriod spending_periods[] = {
    { "All time", 0 },
    { "Today", -1 },
    { "Last 7 days", 7 },
    { "Last 30 days", 30 }
};

guint
clawt_lrg_settings_period_count(void)
{
    return G_N_ELEMENTS(spending_periods);
}

const gchar *
clawt_lrg_settings_period_label(guint n)
{
    return spending_periods[MIN(n, G_N_ELEMENTS(spending_periods) - 1)].label;
}

gint64
clawt_lrg_settings_period_since(guint n, GDateTime *now)
{
    gint days;

    if (n >= G_N_ELEMENTS(spending_periods) || now == NULL)
        return 0;

    days = spending_periods[n].days;

    if (days == 0)
        return 0;

    if (days < 0) {
        g_autoptr(GDateTime) midnight = g_date_time_new(
            g_date_time_get_timezone(now), g_date_time_get_year(now),
            g_date_time_get_month(now), g_date_time_get_day_of_month(now),
            0, 0, 0.0);

        return g_date_time_to_unix(midnight);
    }

    return g_date_time_to_unix(now) - (gint64)days * 86400;
}

JsonNode *
clawt_lrg_settings_usage_payload(gint64 since)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    /*
     * An integer, not a string: the daemon reads `since` as a number,
     * and clawt_ipc_payload_new() can only build strings.
     */
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "since");
    json_builder_add_int_value(builder, since);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

gchar *
clawt_lrg_settings_spend_subtitle(JsonObject *agent)
{
    gint64 turns = clawt_ipc_payload_int(agent, "turns", 0);

    if (turns == 0)
        return g_strdup("nothing recorded in this period");

    return g_strdup_printf("%" G_GINT64_FORMAT " turn%s, %" G_GINT64_FORMAT
                           " tokens out",
                           turns, turns == 1 ? "" : "s",
                           clawt_ipc_payload_int(agent, "output_tokens", 0));
}

gchar *
clawt_lrg_settings_spend_total_subtitle(JsonObject *total)
{
    return g_strdup_printf("%" G_GINT64_FORMAT " turns across the fleet",
                           clawt_ipc_payload_int(total, "turns", 0));
}

gchar *
clawt_lrg_settings_spend_cost(JsonObject *object)
{
    return clawt_usage_format_cost(
        clawt_ipc_payload_int(object, "cost_micros", 0));
}

/* ── Teams ────────────────────────────────────────────────────────── */

gchar *
clawt_lrg_settings_team_subtitle(JsonObject *team)
{
    const gchar *description = nonempty(team, "description");
    const gchar *lead = nonempty(team, "lead");

    return g_strdup_printf(
        "%s \342\200\224 %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT
        " running, lead: %s",
        (description != NULL) ? description
            : "no description, so nothing will be sent here",
        clawt_ipc_payload_int(team, "running", 0),
        clawt_ipc_payload_int(team, "total", 0),
        (lead != NULL) ? lead : "nobody");
}

gchar *
clawt_lrg_settings_team_removed_text(gint64 orphaned)
{
    /*
     * Said rather than left to be discovered: the agents still run, on a
     * team that is no longer declared, which is where hand-offs quietly
     * stop working.
     */
    if (orphaned <= 0)
        return g_strdup("Team removed.");

    return g_strdup_printf("Team removed. %" G_GINT64_FORMAT " agent%s "
                           "still name it -- put them on another team.",
                           orphaned, orphaned == 1 ? "" : "s");
}

/* ── Shared folders ───────────────────────────────────────────────── */

gchar *
clawt_lrg_settings_folder_scope_text(JsonObject *mount)
{
    const gchar *scope = clawt_ipc_payload_string(mount, "scope");
    static const gchar *const keys[] = { "agents", "teams", NULL };
    g_autoptr(GString) out = g_string_new(NULL);
    guint k;

    if (scope == NULL || g_strcmp0(scope, "all") == 0)
        return g_strdup("every agent");

    if (g_strcmp0(scope, "none") == 0)
        return g_strdup("nobody");

    for (k = 0; keys[k] != NULL; k++) {
        JsonArray *items = clawt_lrg_settings_member_array(mount, keys[k]);
        guint i;

        for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
            if (out->len > 0)
                g_string_append(out, ", ");

            g_string_append_printf(out, "%s %s",
                                   (k == 1) ? "team" : "agent",
                                   json_array_get_string_element(items, i));
        }
    }

    /* a `selected` scope naming nothing reaches nobody, and says so */
    if (out->len == 0)
        return g_strdup("nobody (no list)");

    return g_strdup(out->str);
}

const gchar *
clawt_lrg_settings_relabel_label(const gchar *nick)
{
    guint i;

    if (nick == NULL || *nick == '\0')
        return "The backend's default";

    for (i = 0; i < clawt_relabel_count(); i++) {
        if (g_strcmp0(clawt_relabel_nth_nick(i), nick) == 0)
            return clawt_relabel_nth_label(i);
    }

    return nick;
}

gchar *
clawt_lrg_settings_folder_subtitle(JsonObject *mount)
{
    const gchar *target = clawt_ipc_payload_string(mount, "target");
    const gchar *mode = clawt_ipc_payload_string(mount, "mode");
    const gchar *relabel = nonempty(mount, "relabel");
    g_autofree gchar *who = clawt_lrg_settings_folder_scope_text(mount);
    GString *out = g_string_new(NULL);

    g_string_append_printf(out, "%s inside, %s \302\267 %s",
                           (target != NULL) ? target : "?",
                           g_strcmp0(mode, "ro") == 0 ? "read-only"
                                                      : "writable",
                           who);

    /*
     * The relabel only when it is not the backend's own choice.  It is
     * work podman does at every start -- and refuses outright on files
     * the daemon's user does not own -- so a folder with one set is
     * worth being able to see.
     */
    if (relabel != NULL && g_strcmp0(relabel, clawt_relabel_nth_nick(0)) != 0)
        g_string_append_printf(out, " \302\267 relabel: %s",
                               clawt_lrg_settings_relabel_label(relabel));

    return g_string_free(out, FALSE);
}

JsonNode *
clawt_lrg_settings_folder_add_payload(const gchar *source,
                                      const gchar *target,
                                      gboolean writable, const gchar *who,
                                      const gchar *relabel, GError **error)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_auto(GStrv) names = NULL;

    if (typed(source) == NULL) {
        refuse(error, "A shared folder needs a path to share.");
        return NULL;
    }

    /*
     * An empty target means the same path inside, which is what people
     * mean nine times out of ten: a note about a file is then one either
     * side can follow.
     */
    if (typed(target) == NULL)
        target = source;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    add_string(builder, "source", source);
    add_string(builder, "target", target);
    add_string(builder, "mode", writable ? "rw" : "ro");

    names = clawt_lrg_settings_split_list(who);

    if (names[0] != NULL)
        add_string_array(builder, "who", (const gchar *const *)names);

    if (typed(relabel) != NULL)
        add_string(builder, "relabel", relabel);

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* ── Integrations ─────────────────────────────────────────────────── */

guint
clawt_lrg_settings_scope_count(void)
{
    GEnumClass *klass = g_type_class_ref(CLAWT_TYPE_SCOPE);
    guint n = klass->n_values;

    g_type_class_unref(klass);
    return n;
}

const gchar *
clawt_lrg_settings_scope_nth_nick(guint n)
{
    GEnumClass *klass = g_type_class_ref(CLAWT_TYPE_SCOPE);
    const gchar *nick;

    /*
     * Widest first, which is the reverse of the enum's order (none is 0
     * so that an unset scope reaches nobody rather than everybody).  The
     * nick is static storage in the registered enum, so it outlives the
     * class reference.
     */
    n = MIN(n, klass->n_values - 1);
    nick = klass->values[klass->n_values - 1 - n].value_nick;

    g_type_class_unref(klass);
    return nick;
}

const gchar *
clawt_lrg_settings_scope_label(const gchar *nick)
{
    gint value = -1;

    if (!clawt_enum_from_nick(CLAWT_TYPE_SCOPE, nick, &value))
        return (nick != NULL) ? nick : "";

    switch ((ClawtScope)value) {
    case CLAWT_SCOPE_ALL:      return "Every agent";
    case CLAWT_SCOPE_SELECTED: return "These agents";
    case CLAWT_SCOPE_NONE:     return "Nobody";
    }

    return nick;
}

gchar *
clawt_lrg_settings_integration_subtitle(JsonObject *integration)
{
    const gchar *scope = clawt_ipc_payload_string(integration, "scope");
    const gchar *type = clawt_ipc_payload_string(integration, "type");
    JsonArray *effective =
        clawt_lrg_settings_member_array(integration, "effective_agents");
    guint n = (effective != NULL) ? json_array_get_length(effective) : 0;
    g_autofree gchar *reach = NULL;

    if (g_strcmp0(scope, "all") == 0)
        reach = g_strdup_printf("every agent (%u)", n);
    else if (n == 0)
        reach = g_strdup("nobody yet");
    else
        reach = clawt_lrg_settings_join_strings(integration,
                                                "effective_agents", ", ");

    return g_strdup_printf(
        "%s \342\200\224 %s%s",
        clawt_integration_type_label((type != NULL) ? type : "?"), reach,
        clawt_ipc_payload_boolean(integration, "enabled", TRUE)
            ? "" : " (off)");
}

gchar *
clawt_lrg_settings_integration_type_text(const gchar *type_id,
                                         const gchar *summary)
{
    g_autofree gchar *needs = clawt_integration_needs_summary(type_id);

    return g_strdup_printf("%s%s%s", (summary != NULL) ? summary : "",
                           (needs != NULL && summary != NULL &&
                            *summary != '\0') ? "\n\n" : "",
                           (needs != NULL) ? needs : "");
}

JsonObject *
clawt_lrg_settings_find_integration(JsonNode *reply, const gchar *name)
{
    JsonArray *integrations = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(reply), "integrations");
    guint i;

    for (i = 0; integrations != NULL &&
                i < json_array_get_length(integrations); i++) {
        JsonObject *integration =
            json_array_get_object_element(integrations, i);

        if (g_strcmp0(clawt_ipc_payload_string(integration, "name"),
                      name) == 0)
            return integration;
    }

    return NULL;
}

const gchar *
clawt_lrg_settings_field_slot(const ClawtIntegrationField *field)
{
    return (field->when_key != NULL) ? field->label : field->key;
}

gchar *
clawt_lrg_settings_flag_slot(const ClawtIntegrationField *field,
                             const gchar *choice)
{
    return g_strdup_printf("%s.%s", field->key, choice);
}

gchar *
clawt_lrg_settings_field_initial(const ClawtIntegrationField *field,
                                 JsonObject *integration)
{
    gboolean present = integration != NULL &&
                       json_object_has_member(integration, field->key);

    switch (field->kind) {
    case CLAWT_FIELD_BOOLEAN:
        /* on unless the instance says otherwise, as GTK draws it */
        return g_strdup(clawt_ipc_payload_boolean(integration, field->key,
                                                  TRUE) ? "true" : "false");

    case CLAWT_FIELD_CHOICE: {
        const gchar *chosen = clawt_ipc_payload_string(integration,
                                                       field->key);

        return g_strdup((chosen != NULL) ? chosen
                        : clawt_integration_field_default(field));
    }

    case CLAWT_FIELD_LIST:
        return clawt_lrg_settings_join_strings(integration, field->key, ", ");

    case CLAWT_FIELD_INT:
        if (!present)
            return g_strdup("");

        return g_strdup_printf("%" G_GINT64_FORMAT,
                               clawt_ipc_payload_int(integration, field->key,
                                                     0));

    case CLAWT_FIELD_SECRET:
        /* never the value: there is no path that puts one in the config */
        return g_strdup("");

    case CLAWT_FIELD_FLAGS:
        return clawt_lrg_settings_join_strings(integration, field->key, ",");

    case CLAWT_FIELD_TEXT:
        break;
    }

    {
        const gchar *text = clawt_ipc_payload_string(integration, field->key);

        return g_strdup((text != NULL) ? text : "");
    }
}

gboolean
clawt_lrg_settings_flag_initial(const ClawtIntegrationField *field,
                                const gchar *choice, JsonObject *integration)
{
    JsonArray *chosen = clawt_lrg_settings_member_array(integration,
                                                        field->key);
    guint i;

    /*
     * No list of its own is the schema default, question and error --
     * an empty form would show switches off that the daemon has on.
     */
    if (chosen == NULL || json_array_get_length(chosen) == 0)
        return g_strcmp0(choice, "question") == 0 ||
               g_strcmp0(choice, "error") == 0;

    for (i = 0; i < json_array_get_length(chosen); i++) {
        if (g_strcmp0(json_array_get_string_element(chosen, i), choice) == 0)
            return TRUE;
    }

    return FALSE;
}

/* Whether @value is one of a choice field's choices. */
static gboolean
is_choice(const ClawtIntegrationField *field, const gchar *value)
{
    guint c;

    for (c = 0; field->choices != NULL && field->choices[c] != NULL; c++) {
        if (g_strcmp0(field->choices[c], value) == 0)
            return TRUE;
    }

    return FALSE;
}

const gchar *
clawt_lrg_settings_when_value(const gchar *type_id, GHashTable *values,
                              const gchar *when_key)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;

    if (when_key == NULL)
        return NULL;

    list = clawt_integration_fields(type_id, &n);

    for (i = 0; i < n; i++) {
        const gchar *value;

        if (g_strcmp0(list[i].key, when_key) != 0)
            continue;

        value = (values != NULL)
            ? g_hash_table_lookup(values,
                                  clawt_lrg_settings_field_slot(&list[i]))
            : NULL;

        if (list[i].kind == CLAWT_FIELD_CHOICE && is_choice(&list[i], value))
            return value;

        return clawt_integration_field_default(&list[i]);
    }

    return NULL;
}

/* One field's member, or FALSE (with @error) when the form is refused. */
static gboolean
add_field(JsonBuilder *builder, const ClawtIntegrationField *field,
          GHashTable *values, GError **error)
{
    const gchar *value;

    if (field->kind == CLAWT_FIELD_FLAGS) {
        guint c;

        json_builder_set_member_name(builder, field->key);
        json_builder_begin_array(builder);

        for (c = 0; field->choices != NULL && field->choices[c] != NULL;
             c++) {
            g_autofree gchar *slot =
                clawt_lrg_settings_flag_slot(field, field->choices[c]);

            if (g_strcmp0(g_hash_table_lookup(values, slot), "true") == 0)
                json_builder_add_string_value(builder, field->choices[c]);
        }

        json_builder_end_array(builder);
        return TRUE;
    }

    value = g_hash_table_lookup(values, clawt_lrg_settings_field_slot(field));

    /* a field the form has no control for is a field it did not offer */
    if (value == NULL)
        return TRUE;

    switch (field->kind) {
    case CLAWT_FIELD_BOOLEAN:
        json_builder_set_member_name(builder, field->key);
        json_builder_add_boolean_value(builder,
                                       g_strcmp0(value, "true") == 0);
        return TRUE;

    case CLAWT_FIELD_CHOICE:
        add_string(builder, field->key,
                   is_choice(field, value) ? value
                       : clawt_integration_field_default(field));
        return TRUE;

    case CLAWT_FIELD_LIST: {
        g_auto(GStrv) items = clawt_lrg_settings_split_list(value);

        add_string_array(builder, field->key, (const gchar *const *)items);
        return TRUE;
    }

    case CLAWT_FIELD_INT: {
        gint64 number = 0;

        if (*value != '\0' &&
            !g_ascii_string_to_signed(value, 10, G_MININT64, G_MAXINT64,
                                      &number, NULL)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "%s is a number.", field->label);
            return FALSE;
        }

        json_builder_set_member_name(builder, field->key);
        json_builder_add_int_value(builder, number);
        return TRUE;
    }

    case CLAWT_FIELD_SECRET: {
        g_auto(GStrv) parts = NULL;

        /* empty keeps what is set: clearing one by accident costs an
         * authorization nobody can see the reason for */
        if (*value == '\0')
            return TRUE;

        parts = g_strsplit(value, ":", 2);

        if (parts[1] == NULL || *parts[1] == '\0')
            return refuse(error, "A secret is a reference: env:NAME, "
                                 "file:PATH or command:... -- not the "
                                 "value itself.");

        add_string(builder, "secret_key", field->key);
        add_string(builder, "secret_backend", parts[0]);
        add_string(builder, "secret_locator", parts[1]);
        return TRUE;
    }

    case CLAWT_FIELD_TEXT:
    case CLAWT_FIELD_FLAGS:
        break;
    }

    add_string(builder, field->key, value);
    return TRUE;
}

JsonNode *
clawt_lrg_settings_integration_payload(const ClawtLrgIntegrationForm *form,
                                       GError **error)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;

    g_return_val_if_fail(form != NULL && form->name != NULL, NULL);

    json_builder_begin_object(builder);
    add_string(builder, "name", form->name);
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(builder, form->enabled);
    add_string(builder, "description",
               (form->description != NULL) ? form->description : "");
    add_string(builder, "scope",
               (form->scope != NULL) ? form->scope : "selected");
    add_string_array(builder, "agents", form->agents);

    list = clawt_integration_fields(form->type_id, &n);

    /* the same table and predicate the form was drawn from */
    for (i = 0; i < n; i++) {
        const gchar *when = clawt_lrg_settings_when_value(
            form->type_id, form->values, list[i].when_key);

        if (!clawt_integration_field_applies(&list[i], when))
            continue;

        if (!add_field(builder, &list[i], form->values, error))
            return NULL;
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

const gchar *
clawt_lrg_settings_health_text(JsonObject *check)
{
    const gchar *failure;

    if (clawt_ipc_payload_boolean(check, "ok", FALSE))
        return "It answered.";

    failure = nonempty(check, "error");
    return (failure != NULL) ? failure : "It did not answer.";
}

/* ── Connectors ───────────────────────────────────────────────────── */

ClawtConnectorState
clawt_lrg_settings_connector_state(JsonObject *entry, gint64 now)
{
    return clawt_connector_state(
        clawt_ipc_payload_boolean(entry, "connected", FALSE),
        clawt_ipc_payload_int(entry, "expires_at", 0),
        clawt_ipc_payload_boolean(entry, "renewable", FALSE), now);
}

const gchar *
clawt_lrg_settings_connector_tone(ClawtConnectorState state)
{
    /*
     * Seeded so -Wmaybe-uninitialized has nothing to say, with no
     * default: below, so -Wswitch names a state nobody classified.
     */
    const gchar *tone = "neutral";

    switch (state) {
    case CLAWT_CONNECTOR_UNAUTHORISED:
        tone = "neutral";
        break;

    case CLAWT_CONNECTOR_EXPIRED:
        tone = "warn";
        break;

    case CLAWT_CONNECTOR_READY:
        tone = "good";
        break;
    }

    return tone;
}

gchar *
clawt_lrg_settings_connector_subtitle(JsonObject *entry)
{
    const gchar *provider = clawt_ipc_payload_string(entry, "provider");
    const gchar *account = nonempty(entry, "account");

    if (account != NULL)
        return g_strdup_printf("%s -- %s",
                               (provider != NULL) ? provider : "", account);

    return g_strdup((provider != NULL) ? provider : "");
}

gchar *
clawt_lrg_settings_catalog_label(JsonObject *entry)
{
    const gchar *id = clawt_ipc_payload_string(entry, "id");
    const gchar *name = nonempty(entry, "name");
    const gchar *auth = nonempty(entry, "auth");

    if (name == NULL)
        name = (id != NULL) ? id : "?";

    if (auth == NULL)
        return g_strdup(name);

    return g_strdup_printf("%s (%s)", name, auth);
}

JsonNode *
clawt_lrg_settings_connector_payload(const gchar *name,
                                     const gchar *provider,
                                     const gchar *scope,
                                     const gchar *account,
                                     const gchar *client_id,
                                     const gchar *instance,
                                     const gchar *agents, GError **error)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_auto(GStrv) ids = NULL;

    if (typed(name) == NULL) {
        refuse(error, "It needs a name.");
        return NULL;
    }

    if (typed(provider) == NULL) {
        refuse(error, "Choose a service.");
        return NULL;
    }

    builder = json_builder_new();
    json_builder_begin_object(builder);
    add_string(builder, "name", name);
    add_string(builder, "type", "connector");
    add_string(builder, "provider", provider);
    add_string(builder, "scope", (typed(scope) != NULL) ? scope : "selected");

    if (typed(account) != NULL)
        add_string(builder, "account", account);

    if (typed(client_id) != NULL)
        add_string(builder, "client_id", client_id);

    if (typed(instance) != NULL)
        add_string(builder, "instance", instance);

    ids = clawt_lrg_settings_split_list(agents);

    if (ids[0] != NULL)
        add_string_array(builder, "agents", (const gchar *const *)ids);

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

const gchar *
clawt_lrg_settings_revoked_text(JsonObject *reply)
{
    if (clawt_ipc_payload_boolean(reply, "told_provider", FALSE))
        return "Revoked, and the provider was told.";

    return "Forgotten here -- withdraw it in the provider's settings to "
           "finish.";
}

gchar *
clawt_lrg_settings_imported_text(gint64 imported)
{
    return g_strdup_printf("Imported %" G_GINT64_FORMAT " connector%s from "
                           "the registry.",
                           imported, imported == 1 ? "" : "s");
}

/* ── Connections ──────────────────────────────────────────────────── */

const gchar *
clawt_lrg_settings_reach_tone(ClawtReachability reach)
{
    const gchar *tone = "neutral";

    /*
     * Refused and unreachable share a tone and keep their words: the
     * word (from clawt_reachability_word()) is what says whether to
     * check the token or the network.
     */
    switch (reach) {
    case CLAWT_REACH_UNKNOWN:
        tone = "neutral";
        break;

    case CLAWT_REACH_REACHABLE:
        tone = "good";
        break;

    case CLAWT_REACH_REFUSED:
    case CLAWT_REACH_UNREACHABLE:
        tone = "bad";
        break;
    }

    return tone;
}

gchar *
clawt_lrg_settings_reach_detail(const ClawtConnectionStatus *status)
{
    g_autofree gchar *mismatch = NULL;

    if (status == NULL || status->reach == CLAWT_REACH_UNKNOWN)
        return NULL;

    if (status->reach != CLAWT_REACH_REACHABLE)
        return (status->detail != NULL) ? g_strdup(status->detail) : NULL;

    mismatch = clawt_version_mismatch_text(status->version);

    return g_strdup_printf("clawtillad %s, %u agent%s%s%s",
                           (status->version != NULL) ? status->version : "?",
                           status->agents, status->agents == 1 ? "" : "s",
                           (mismatch != NULL) ? " \302\267 " : "",
                           (mismatch != NULL) ? mismatch : "");
}

ClawtConnection *
clawt_lrg_settings_connection_from_form(GPtrArray *existing,
                                        const gchar *name,
                                        const gchar *host,
                                        const gchar *port_text,
                                        const gchar *token, gboolean tls,
                                        gboolean accept_unknown,
                                        GError **error)
{
    guint64 port = CLAWT_DEFAULT_TCP_PORT;
    ClawtConnection *connection;

    if (typed(host) == NULL) {
        refuse(error, "A connection needs a host.");
        return NULL;
    }

    if (typed(port_text) != NULL &&
        (!g_ascii_string_to_unsigned(port_text, 10, 1, G_MAXUINT16, &port,
                                     NULL))) {
        refuse(error, "That is not a port.");
        return NULL;
    }

    if (typed(name) == NULL)
        name = host;

    if (existing != NULL && clawt_connection_list_find(existing, name) != NULL) {
        refuse(error, "There is already a connection with that name.");
        return NULL;
    }

    connection = clawt_connection_new_remote(name, host, (guint16)port,
                                             typed(token));
    clawt_connection_set_tls(connection, tls, tls && accept_unknown);

    return connection;
}

gchar *
clawt_lrg_settings_daemon_version(JsonNode *status, gchar **out_update)
{
    JsonObject *payload = clawt_ipc_reply_object(status);
    JsonObject *update = NULL;

    if (out_update != NULL)
        *out_update = NULL;

    if (payload == NULL)
        return NULL;

    if (json_object_has_member(payload, "update") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(payload, "update")))
        update = json_object_get_object_member(payload, "update");

    /*
     * Only when the daemon says one is available: `latest` on its own is
     * also sent when it is the version already running.
     */
    if (out_update != NULL && update != NULL &&
        clawt_ipc_payload_boolean(update, "available", FALSE))
        *out_update = g_strdup(nonempty(update, "latest"));

    return g_strdup(nonempty(payload, "version"));
}

/* ── Appearance ───────────────────────────────────────────────────── */

gdouble
clawt_lrg_settings_scale_step(gdouble current, gdouble proposed)
{
    if (proposed <= 0.0)
        return 0.0;

    /*
     * Below the minimum there is nothing to choose: moving down out of
     * the range follows the monitor, anything else lands on the floor.
     */
    if (proposed < CLAWT_LRG_SCALE_MIN)
        return (current > 0.0 && proposed < current) ? 0.0
                                                     : CLAWT_LRG_SCALE_MIN;

    return MIN(proposed, CLAWT_LRG_SCALE_MAX);
}

gchar *
clawt_lrg_settings_scale_text(gdouble scale)
{
    if (scale <= 0.0)
        return g_strdup("Follows the monitor");

    return g_strdup_printf("%.2f\303\227", scale);
}

gchar *
clawt_lrg_settings_palette_note(ClawtAppearance *appearance)
{
    const gchar *css = clawt_appearance_get_palette_css(appearance);
    g_auto(GStrv) missing = NULL;
    g_autofree gchar *names = NULL;
    g_autofree gchar *dir = NULL;
    guint n;

    if (css == NULL)
        return NULL;

    missing = clawt_appearance_palette_missing(css);
    n = (missing != NULL) ? g_strv_length(missing) : 0;
    dir = clawt_appearance_palette_dir();

    if (n == 0)
        return g_strdup_printf("A palette from %s, complete.", dir);

    names = g_strjoinv(", ", missing);

    return g_strdup_printf("A palette from %s. It leaves %u colour%s out, "
                           "which keep the stock value: %s",
                           dir, n, n == 1 ? "" : "s", names);
}

const gchar *
clawt_lrg_settings_font_text(const gchar *family, gboolean monospace)
{
    if (family != NULL && *family != '\0')
        return family;

    return monospace ? "Whatever the desktop uses for monospace"
                     : "Whatever the desktop uses";
}

/* ── New agent ────────────────────────────────────────────────────── */

JsonArray *
clawt_lrg_create_filter_providers(JsonNode *catalog, const gchar *require,
                                  const gchar *want_provider)
{
    JsonArray *all = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(catalog), "providers");
    JsonArray *kept;
    guint i;

    if (all == NULL)
        return NULL;

    kept = json_array_new();

    for (i = 0; i < json_array_get_length(all); i++) {
        JsonNode *node = json_array_get_element(all, i);
        JsonObject *provider;

        if (!JSON_NODE_HOLDS_OBJECT(node))
            continue;

        provider = json_node_get_object(node);

        if (require == NULL ||
            !json_object_has_member(provider, require) ||
            clawt_ipc_payload_boolean(provider, require, TRUE) ||
            (want_provider != NULL &&
             g_strcmp0(clawt_ipc_payload_string(provider, "id"),
                       want_provider) == 0))
            json_array_add_element(kept, json_node_copy(node));
    }

    return kept;
}

gchar *
clawt_lrg_create_model_label(JsonObject *model)
{
    const gchar *label = nonempty(model, "label");
    const gchar *note = nonempty(model, "note");

    if (label == NULL)
        label = nonempty(model, "id");

    if (label == NULL)
        label = "?";

    if (note == NULL)
        return g_strdup(label);

    return g_strdup_printf("%s - %s", label, note);
}

/* The index of the entry whose @member is @value, or the length. */
static guint
catalog_find(JsonArray *items, const gchar *member, const gchar *value)
{
    guint n = (items != NULL) ? json_array_get_length(items) : 0;
    guint i;

    for (i = 0; i < n; i++) {
        JsonObject *item = json_array_get_object_element(items, i);

        if (g_strcmp0(clawt_ipc_payload_string(item, member), value) == 0)
            return i;
    }

    return n;
}

guint
clawt_lrg_create_catalog_index(JsonArray *items, const gchar *member,
                               const gchar *want, const gchar *fallback)
{
    guint n = (items != NULL) ? json_array_get_length(items) : 0;
    guint found;

    /* a value of its own is kept, as Other when it is not on the list */
    if (typed(want) != NULL)
        return catalog_find(items, member, want);

    if (typed(fallback) != NULL) {
        found = catalog_find(items, member, fallback);

        if (found < n)
            return found;
    }

    /* nothing asked for: the first entry, or Other when there is none */
    return (n > 0) ? 0 : n;
}

gchar *
clawt_lrg_create_image_note(JsonObject *image)
{
    const gchar *reference;
    const gchar *note;

    if (image == NULL)
        return g_strdup("any reference podman can pull");

    reference = clawt_ipc_payload_string(image, "reference");
    note = nonempty(image, "note");

    if (note == NULL)
        return g_strdup((reference != NULL) ? reference : "");

    return g_strdup_printf("%s -- %s", (reference != NULL) ? reference : "",
                           note);
}

const gchar *
clawt_lrg_create_disk_note(JsonObject *image, guint n_images)
{
    const gchar *path;

    if (image != NULL) {
        path = clawt_ipc_payload_string(image, "path");
        return (path != NULL) ? path : "";
    }

    return (n_images == 0)
        ? "No cloud images yet \342\200\224 fetch one from Settings"
        : "A path to a qcow2 on this machine";
}

void
clawt_lrg_create_team_choices(JsonNode *teams, const gchar *current,
                              GPtrArray *ids, GPtrArray *labels)
{
    JsonArray *list = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(teams), "teams");
    gboolean known = FALSE;
    guint i;

    g_ptr_array_add(ids, g_strdup(""));
    g_ptr_array_add(labels, g_strdup("No team"));

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *team = json_array_get_object_element(list, i);
        const gchar *id = clawt_ipc_payload_string(team, "id");
        const gchar *name = nonempty(team, "name");

        if (id == NULL)
            continue;

        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(labels, g_strdup((name != NULL) ? name : id));

        if (g_strcmp0(id, current) == 0)
            known = TRUE;
    }

    if (typed(current) != NULL && !known) {
        g_ptr_array_add(ids, g_strdup(current));
        g_ptr_array_add(labels, g_strdup_printf("%s (not declared)", current));
    }
}

gchar *
clawt_lrg_create_preview_text(JsonObject *result)
{
    JsonArray *files = clawt_lrg_settings_member_array(result, "files");
    const gchar *yaml = clawt_ipc_payload_string(result, "yaml");
    const gchar *id = clawt_ipc_payload_string(result, "id");
    GString *body = g_string_new((yaml != NULL) ? yaml : "");
    guint i;

    if (files == NULL || json_array_get_length(files) == 0)
        return g_string_free(body, FALSE);

    g_string_append(body, "\n\nIt also wrote:\n");

    for (i = 0; i < json_array_get_length(files); i++) {
        JsonObject *file = json_array_get_object_element(files, i);
        const gchar *content = clawt_ipc_payload_string(file, "content");
        const gchar *name = clawt_ipc_payload_string(file, "name");
        gsize lines = 0;
        const gchar *p;

        for (p = content; p != NULL && *p != '\0'; p++) {
            if (*p == '\n')
                lines++;
        }

        g_string_append_printf(body, "  %s  (%" G_GSIZE_FORMAT " lines)\n",
                               (name != NULL) ? name : "?", lines);
    }

    g_string_append_printf(body, "\nEdit them after with: clawtilla agent "
                                 "edit %s", (id != NULL) ? id : "");

    return g_string_free(body, FALSE);
}

gchar *
clawt_lrg_create_created_text(JsonObject *reply, const gchar *agent_id)
{
    const gchar *failure = nonempty(reply, "start_error");
    const gchar *id = typed(agent_id);

    if (id == NULL)
        id = nonempty(reply, "id");

    if (id == NULL)
        id = "The agent";

    if (failure != NULL)
        return g_strdup_printf("%s was created but did not start: %s", id,
                               failure);

    if (clawt_ipc_payload_boolean(reply, "started", FALSE))
        return g_strdup_printf("%s created and started.", id);

    /* an older daemon sends neither, and "created" is what happened */
    return g_strdup("Agent created.");
}

/* ── Import ───────────────────────────────────────────────────────── */

const gchar *
clawt_lrg_create_found_subtitle(JsonObject *entry)
{
    const gchar *holds = nonempty(entry, "holds");
    const gchar *path = clawt_ipc_payload_string(entry, "path");

    if (holds != NULL)
        return holds;

    return (path != NULL) ? path : "";
}

JsonNode *
clawt_lrg_create_import_payload(const gchar *agent_id, ClawtImportMode mode,
                                const gchar *url, const gchar *path,
                                gboolean keep_git, GError **error)
{
    const gchar *source;
    guint i;
    const gchar *nick = NULL;

    if (typed(agent_id) == NULL) {
        refuse(error, "An imported agent needs an id.");
        return NULL;
    }

    if (clawt_import_mode_takes_url(mode)) {
        source = typed(url);

        if (source == NULL) {
            refuse(error, "A git import needs a repository URL.");
            return NULL;
        }
    } else {
        source = typed(path);

        if (source == NULL) {
            refuse(error, "Choose the directory to import from.");
            return NULL;
        }
    }

    for (i = 0; i < clawt_import_mode_count(); i++) {
        if (clawt_import_mode_nth(i) == mode)
            nick = clawt_import_mode_nth_nick(i);
    }

    /*
     * keep_git as the string the daemon's boolean reader accepts, the
     * way every client sends it; only a copy reads the source's .git.
     */
    return clawt_ipc_payload_new("id", agent_id, "from", source,
                                 "mode", nick,
                                 "keep_git",
                                 (keep_git && mode == CLAWT_IMPORT_COPY)
                                     ? "true" : "false",
                                 NULL);
}

/* ── New room ─────────────────────────────────────────────────────── */

JsonNode *
clawt_lrg_create_room_payload(const gchar *room_id, const gchar *name,
                              const gchar *const *members, GError **error)
{
    g_autofree gchar *joined = NULL;

    if (typed(room_id) == NULL) {
        refuse(error, "A room needs a name to be addressed by.");
        return NULL;
    }

    if (members == NULL || members[0] == NULL || members[1] == NULL) {
        refuse(error, "Pick at least two agents -- a room with one is the "
                      "conversation you already have with it.");
        return NULL;
    }

    joined = g_strjoinv(",", (gchar **)members);

    return clawt_ipc_payload_new("room", room_id,
                                 "name", (typed(name) != NULL) ? name
                                                               : room_id,
                                 "members", joined, NULL);
}

const gchar *
clawt_lrg_create_room_created_text(guint n_members)
{
    if (n_members > 2)
        return "Created. A message there reaches only the members you name "
               "with @their-id -- everyone can still read everything.";

    return "Created.";
}
