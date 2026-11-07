/*
 * test-lrg-models-settings.c - The Settings and create dialogs' decisions
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: no daemon, no display, no libregnum.  The model is included
 * whole, as the other lrg model tests do, so a change to it relinks this.
 *
 * Every reply below is written the way the daemon writes it -- booleans
 * as JSON booleans, sizes and ports as JSON integers -- because the bugs
 * these functions exist to avoid were all a reader of the wrong type
 * answering with its fallback.
 */

#include "../clients/lrg/lrg-settings-model.c"

static JsonNode *
parse(const gchar *json)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;

    g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
    g_assert_no_error(error);

    return json_node_copy(json_parser_get_root(parser));
}

static gchar *
to_json(JsonNode *node)
{
    return json_to_string(node, FALSE);
}

/* ── Shared ───────────────────────────────────────────────────────── */

static void
test_split_list(void)
{
    g_auto(GStrv) none = clawt_lrg_settings_split_list(NULL);
    g_auto(GStrv) blank = clawt_lrg_settings_split_list(" , ,");
    g_auto(GStrv) some = clawt_lrg_settings_split_list(" a, b ,,c ");

    g_assert_cmpuint(g_strv_length(none), ==, 0);
    g_assert_cmpuint(g_strv_length(blank), ==, 0);
    g_assert_cmpuint(g_strv_length(some), ==, 3);
    g_assert_cmpstr(some[0], ==, "a");
    g_assert_cmpstr(some[1], ==, "b");
    g_assert_cmpstr(some[2], ==, "c");
}

static void
test_member_array_wrong_type(void)
{
    g_autoptr(JsonNode) node = parse("{\"agents\": \"kudu\"}");

    /* a string where an array was expected is NULL, not an assertion */
    g_assert_null(clawt_lrg_settings_member_array(
        json_node_get_object(node), "agents"));
    g_assert_null(clawt_lrg_settings_member_array(NULL, "agents"));
}

/* ── Cloud images ─────────────────────────────────────────────────── */

static void
test_human_size(void)
{
    g_autofree gchar *b = clawt_lrg_settings_human_size(12);
    g_autofree gchar *k = clawt_lrg_settings_human_size(16 * 1024);
    g_autofree gchar *m = clawt_lrg_settings_human_size(557LL * 1024 * 1024);
    g_autofree gchar *g = clawt_lrg_settings_human_size(
        (gint64)(1.5 * 1024 * 1024 * 1024));

    g_assert_cmpstr(b, ==, "12 B");
    g_assert_cmpstr(k, ==, "16 kB");
    g_assert_cmpstr(m, ==, "557 MB");
    g_assert_cmpstr(g, ==, "1.5 GB");
}

static void
test_image_subtitle(void)
{
    g_autoptr(JsonNode) busy = parse(
        "{\"name\": \"fedora\", \"downloading\": true, "
        "\"bytes\": 16384, \"total\": 584056832}");
    g_autoptr(JsonNode) done = parse(
        "{\"name\": \"fedora\", \"downloading\": false, "
        "\"bytes\": 584056832, \"url\": \"https://x/f.qcow2\"}");
    g_autoptr(JsonNode) bare = parse("{\"name\": \"local\", \"bytes\": 2048}");
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;
    g_autofree gchar *c = NULL;

    g_assert_true(clawt_lrg_settings_image_downloading(
        json_node_get_object(busy)));
    g_assert_false(clawt_lrg_settings_image_downloading(
        json_node_get_object(done)));

    a = clawt_lrg_settings_image_subtitle(json_node_get_object(busy));
    b = clawt_lrg_settings_image_subtitle(json_node_get_object(done));
    c = clawt_lrg_settings_image_subtitle(json_node_get_object(bare));

    g_assert_cmpstr(a, ==, "16 kB of 557 MB");
    g_assert_cmpstr(b, ==, "557 MB  \342\200\224  https://x/f.qcow2");
    g_assert_cmpstr(c, ==, "2 kB");
}

static void
test_progress(void)
{
    g_autofree gchar *unknown = clawt_lrg_settings_progress_text(2048, 0);

    g_assert_cmpfloat(clawt_lrg_settings_progress_fraction(1, 4), ==, 0.25);
    g_assert_cmpfloat(clawt_lrg_settings_progress_fraction(9, 4), ==, 1.0);
    g_assert_cmpfloat(clawt_lrg_settings_progress_fraction(1, 0), <, 0.0);
    g_assert_cmpstr(unknown, ==, "2 kB so far");
}

static void
test_image_failed(void)
{
    g_autofree gchar *text =
        clawt_lrg_settings_image_failed_text("fedora", "404");

    g_assert_null(clawt_lrg_settings_image_failed_text("fedora", NULL));
    g_assert_null(clawt_lrg_settings_image_failed_text("fedora", ""));
    g_assert_cmpstr(text, ==, "fedora could not be downloaded: 404");
}

/* ── Spending ─────────────────────────────────────────────────────── */

static void
test_periods(void)
{
    g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
    g_autoptr(GDateTime) now = g_date_time_new(utc, 2026, 11, 7, 15, 30, 0);
    gint64 unix_now = g_date_time_to_unix(now);
    g_autoptr(JsonNode) payload = clawt_lrg_settings_usage_payload(1234);
    g_autofree gchar *json = to_json(payload);

    g_assert_cmpuint(clawt_lrg_settings_period_count(), ==, 4);
    g_assert_cmpstr(clawt_lrg_settings_period_label(0), ==, "All time");

    /* all time is 0; today is local midnight; seven days is a week back */
    g_assert_cmpint(clawt_lrg_settings_period_since(0, now), ==, 0);
    g_assert_cmpint(clawt_lrg_settings_period_since(1, now), ==,
                    unix_now - (15 * 3600 + 30 * 60));
    g_assert_cmpint(clawt_lrg_settings_period_since(2, now), ==,
                    unix_now - 7 * 86400);
    g_assert_cmpint(clawt_lrg_settings_period_since(99, now), ==, 0);

    /* since is a JSON integer, in seconds */
    g_assert_cmpstr(json, ==, "{\"since\":1234}");
}

static void
test_spend_rows(void)
{
    g_autoptr(JsonNode) busy = parse(
        "{\"id\": \"kudu\", \"turns\": 3, \"output_tokens\": 1200, "
        "\"cost_micros\": 1500000}");
    g_autoptr(JsonNode) idle = parse("{\"id\": \"oryx\", \"turns\": 0}");
    g_autoptr(JsonNode) one = parse("{\"turns\": 1, \"output_tokens\": 5}");
    g_autoptr(JsonNode) total = parse("{\"turns\": 12}");
    g_autofree gchar *a =
        clawt_lrg_settings_spend_subtitle(json_node_get_object(busy));
    g_autofree gchar *b =
        clawt_lrg_settings_spend_subtitle(json_node_get_object(idle));
    g_autofree gchar *c =
        clawt_lrg_settings_spend_subtitle(json_node_get_object(one));
    g_autofree gchar *t =
        clawt_lrg_settings_spend_total_subtitle(json_node_get_object(total));
    g_autofree gchar *cost =
        clawt_lrg_settings_spend_cost(json_node_get_object(busy));
    g_autofree gchar *expected = clawt_usage_format_cost(1500000);

    g_assert_cmpstr(a, ==, "3 turns, 1200 tokens out");
    g_assert_cmpstr(b, ==, "nothing recorded in this period");
    g_assert_cmpstr(c, ==, "1 turn, 5 tokens out");
    g_assert_cmpstr(t, ==, "12 turns across the fleet");
    g_assert_cmpstr(cost, ==, expected);
}

/* ── Teams ────────────────────────────────────────────────────────── */

static void
test_team_subtitle(void)
{
    g_autoptr(JsonNode) good = parse(
        "{\"id\": \"backend\", \"description\": \"APIs\", \"lead\": \"kudu\","
        " \"running\": 2, \"total\": 3}");
    g_autoptr(JsonNode) blank = parse(
        "{\"id\": \"ops\", \"description\": \"\", \"running\": 0, "
        "\"total\": 1}");
    g_autofree gchar *a =
        clawt_lrg_settings_team_subtitle(json_node_get_object(good));
    g_autofree gchar *b =
        clawt_lrg_settings_team_subtitle(json_node_get_object(blank));
    g_autofree gchar *none = clawt_lrg_settings_team_removed_text(0);
    g_autofree gchar *one = clawt_lrg_settings_team_removed_text(1);
    g_autofree gchar *two = clawt_lrg_settings_team_removed_text(2);

    g_assert_cmpstr(a, ==, "APIs \342\200\224 2 of 3 running, lead: kudu");
    g_assert_cmpstr(b, ==, "no description, so nothing will be sent here "
                           "\342\200\224 0 of 1 running, lead: nobody");
    g_assert_cmpstr(none, ==, "Team removed.");
    g_assert_true(g_str_has_prefix(one, "Team removed. 1 agent still"));
    g_assert_true(g_str_has_prefix(two, "Team removed. 2 agents still"));
}

/* ── Shared folders ───────────────────────────────────────────────── */

static void
test_folder_scope(void)
{
    g_autoptr(JsonNode) all = parse("{\"scope\": \"all\"}");
    g_autoptr(JsonNode) none = parse("{\"scope\": \"none\"}");
    g_autoptr(JsonNode) both = parse(
        "{\"scope\": \"selected\", \"agents\": [\"kudu\"], "
        "\"teams\": [\"backend\"]}");
    g_autoptr(JsonNode) empty = parse("{\"scope\": \"selected\"}");
    g_autofree gchar *a =
        clawt_lrg_settings_folder_scope_text(json_node_get_object(all));
    g_autofree gchar *b =
        clawt_lrg_settings_folder_scope_text(json_node_get_object(none));
    g_autofree gchar *c =
        clawt_lrg_settings_folder_scope_text(json_node_get_object(both));
    g_autofree gchar *d =
        clawt_lrg_settings_folder_scope_text(json_node_get_object(empty));

    g_assert_cmpstr(a, ==, "every agent");
    g_assert_cmpstr(b, ==, "nobody");
    g_assert_cmpstr(c, ==, "agent kudu, team backend");
    g_assert_cmpstr(d, ==, "nobody (no list)");
}

static void
test_folder_subtitle(void)
{
    g_autofree gchar *json = g_strdup_printf(
        "{\"source\": \"/s\", \"target\": \"/t\", \"mode\": \"ro\", "
        "\"scope\": \"all\", \"relabel\": \"%s\"}",
        clawt_relabel_nth_nick(clawt_relabel_count() - 1));
    g_autoptr(JsonNode) labelled = parse(json);
    g_autofree gchar *plain_json = g_strdup_printf(
        "{\"target\": \"/t\", \"mode\": \"rw\", \"relabel\": \"%s\"}",
        clawt_relabel_nth_nick(0));
    g_autoptr(JsonNode) plain = parse(plain_json);
    g_autofree gchar *a =
        clawt_lrg_settings_folder_subtitle(json_node_get_object(labelled));
    g_autofree gchar *b =
        clawt_lrg_settings_folder_subtitle(json_node_get_object(plain));

    g_assert_true(g_str_has_prefix(a, "/t inside, read-only"));
    g_assert_nonnull(strstr(a, clawt_relabel_nth_label(
        clawt_relabel_count() - 1)));
    g_assert_cmpstr(b, ==, "/t inside, writable \302\267 every agent");
}

static void
test_relabel_label_walked(void)
{
    guint i;

    for (i = 0; i < clawt_relabel_count(); i++)
        g_assert_cmpstr(
            clawt_lrg_settings_relabel_label(clawt_relabel_nth_nick(i)), ==,
            clawt_relabel_nth_label(i));

    g_assert_cmpstr(clawt_lrg_settings_relabel_label(NULL), ==,
                    "The backend's default");
}

static void
test_folder_payload(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) same = NULL;
    g_autoptr(JsonNode) full = NULL;
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;

    g_assert_null(clawt_lrg_settings_folder_add_payload("", NULL, TRUE, NULL,
                                                        NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    /* an empty target is the same path inside; no who, no relabel */
    same = clawt_lrg_settings_folder_add_payload("~/src", "", TRUE, " ", "",
                                                 &error);
    g_assert_no_error(error);
    a = to_json(same);
    g_assert_cmpstr(a, ==, "{\"source\":\"~/src\",\"target\":\"~/src\","
                           "\"mode\":\"rw\"}");

    full = clawt_lrg_settings_folder_add_payload("/s", "/t", FALSE,
                                                 "backend, kudu", "none",
                                                 &error);
    g_assert_no_error(error);
    b = to_json(full);
    g_assert_cmpstr(b, ==, "{\"source\":\"/s\",\"target\":\"/t\","
                           "\"mode\":\"ro\",\"who\":[\"backend\",\"kudu\"],"
                           "\"relabel\":\"none\"}");
}

/* ── Integrations ─────────────────────────────────────────────────── */

static void
test_scopes_walked(void)
{
    guint n = clawt_lrg_settings_scope_count();
    guint i;

    g_assert_cmpuint(n, ==, 3);
    g_assert_cmpstr(clawt_lrg_settings_scope_nth_nick(0), ==, "all");
    g_assert_cmpstr(clawt_lrg_settings_scope_nth_nick(n - 1), ==, "none");

    for (i = 0; i < n; i++) {
        const gchar *nick = clawt_lrg_settings_scope_nth_nick(i);

        /* every scope has wording; none falls through to its nick */
        g_assert_cmpstr(clawt_lrg_settings_scope_label(nick), !=, nick);
    }

    g_assert_cmpstr(clawt_lrg_settings_scope_label("bogus"), ==, "bogus");
}

static void
test_integration_subtitle(void)
{
    g_autoptr(JsonNode) all = parse(
        "{\"name\": \"m\", \"type\": \"matrix\", \"scope\": \"all\", "
        "\"enabled\": true, \"effective_agents\": [\"a\", \"b\"]}");
    g_autoptr(JsonNode) off = parse(
        "{\"name\": \"n\", \"type\": \"notify\", \"scope\": \"selected\", "
        "\"enabled\": false, \"effective_agents\": []}");
    g_autoptr(JsonNode) some = parse(
        "{\"name\": \"x\", \"type\": \"mcp\", \"scope\": \"selected\", "
        "\"enabled\": true, \"effective_agents\": [\"kudu\", \"oryx\"]}");
    g_autofree gchar *a =
        clawt_lrg_settings_integration_subtitle(json_node_get_object(all));
    g_autofree gchar *b =
        clawt_lrg_settings_integration_subtitle(json_node_get_object(off));
    g_autofree gchar *c =
        clawt_lrg_settings_integration_subtitle(json_node_get_object(some));
    g_autofree gchar *want_a = g_strdup_printf(
        "%s \342\200\224 every agent (2)",
        clawt_integration_type_label("matrix"));
    g_autofree gchar *want_b = g_strdup_printf(
        "%s \342\200\224 nobody yet (off)",
        clawt_integration_type_label("notify"));
    g_autofree gchar *want_c = g_strdup_printf(
        "%s \342\200\224 kudu, oryx", clawt_integration_type_label("mcp"));

    g_assert_cmpstr(a, ==, want_a);
    g_assert_cmpstr(b, ==, want_b);
    g_assert_cmpstr(c, ==, want_c);
}

static void
test_find_integration(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"integrations\": [{\"name\": \"a\"}, {\"name\": \"b\"}]}");

    g_assert_nonnull(clawt_lrg_settings_find_integration(reply, "b"));
    g_assert_null(clawt_lrg_settings_find_integration(reply, "c"));
    g_assert_null(clawt_lrg_settings_find_integration(NULL, "a"));
}

/* The first field of @type with @kind, or NULL. */
static const ClawtIntegrationField *
field_of_kind(const gchar *type, ClawtFieldKind kind)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;

    list = clawt_integration_fields(type, &n);

    for (i = 0; i < n; i++) {
        if (list[i].kind == kind)
            return &list[i];
    }

    return NULL;
}

/* Any type's first field of @kind, searching the types the daemon knows. */
static const ClawtIntegrationField *
any_field_of_kind(ClawtFieldKind kind)
{
    static const gchar *const types[] = {
        "matrix", "email", "notify", "mcp", "webhook", "connector",
        "telegram", "slack", "discord", NULL
    };
    guint t;

    for (t = 0; types[t] != NULL; t++) {
        const ClawtIntegrationField *field = field_of_kind(types[t], kind);

        if (field != NULL)
            return field;
    }

    return NULL;
}

static void
test_field_initial(void)
{
    const ClawtIntegrationField *integer = any_field_of_kind(CLAWT_FIELD_INT);
    const ClawtIntegrationField *secret =
        any_field_of_kind(CLAWT_FIELD_SECRET);
    const ClawtIntegrationField *boolean =
        any_field_of_kind(CLAWT_FIELD_BOOLEAN);
    const ClawtIntegrationField *list = any_field_of_kind(CLAWT_FIELD_LIST);

    if (integer != NULL) {
        g_autofree gchar *json = g_strdup_printf("{\"%s\": 993}",
                                                 integer->key);
        g_autoptr(JsonNode) node = parse(json);
        g_autofree gchar *value = clawt_lrg_settings_field_initial(
            integer, json_node_get_object(node));
        g_autofree gchar *missing =
            clawt_lrg_settings_field_initial(integer, NULL);

        /* a JSON integer read as a number, not as a string's fallback */
        g_assert_cmpstr(value, ==, "993");
        g_assert_cmpstr(missing, ==, "");
    }

    if (secret != NULL) {
        g_autofree gchar *json = g_strdup_printf("{\"%s\": \"env:TOKEN\"}",
                                                 secret->key);
        g_autoptr(JsonNode) node = parse(json);
        g_autofree gchar *value = clawt_lrg_settings_field_initial(
            secret, json_node_get_object(node));

        /* never the value, nor even the reference */
        g_assert_cmpstr(value, ==, "");
    }

    if (boolean != NULL) {
        g_autofree gchar *json = g_strdup_printf("{\"%s\": false}",
                                                 boolean->key);
        g_autoptr(JsonNode) node = parse(json);
        g_autofree gchar *off = clawt_lrg_settings_field_initial(
            boolean, json_node_get_object(node));
        g_autofree gchar *unset =
            clawt_lrg_settings_field_initial(boolean, NULL);

        g_assert_cmpstr(off, ==, "false");
        g_assert_cmpstr(unset, ==, "true");
    }

    if (list != NULL) {
        g_autofree gchar *json = g_strdup_printf("{\"%s\": [\"a\", \"b\"]}",
                                                 list->key);
        g_autoptr(JsonNode) node = parse(json);
        g_autofree gchar *value = clawt_lrg_settings_field_initial(
            list, json_node_get_object(node));

        g_assert_cmpstr(value, ==, "a, b");
    }
}

static void
test_flag_initial(void)
{
    const ClawtIntegrationField *flags = any_field_of_kind(CLAWT_FIELD_FLAGS);
    g_autoptr(JsonNode) chosen = NULL;
    g_autofree gchar *json = NULL;
    const gchar *first;

    if (flags == NULL) {
        g_test_skip("no integration type has a flags field");
        return;
    }

    first = flags->choices[0];

    /* no list of its own is the default: question and error */
    g_assert_cmpint(clawt_lrg_settings_flag_initial(flags, "question", NULL),
                    ==, TRUE);
    g_assert_cmpint(clawt_lrg_settings_flag_initial(flags, "error", NULL),
                    ==, TRUE);

    json = g_strdup_printf("{\"%s\": [\"%s\"]}", flags->key, first);
    chosen = parse(json);
    g_assert_true(clawt_lrg_settings_flag_initial(
        flags, first, json_node_get_object(chosen)));

    if (flags->choices[1] != NULL)
        g_assert_false(clawt_lrg_settings_flag_initial(
            flags, flags->choices[1], json_node_get_object(chosen)));
}

/*
 * Every field a type has, given a value, survives the trip through the
 * payload with the member type the daemon reads -- and a conditional
 * field that does not apply is left out.
 */
static void
test_integration_payload_walks_table(void)
{
    static const gchar *const types[] = {
        "matrix", "email", "notify", "mcp", "webhook", NULL
    };
    guint t;

    for (t = 0; types[t] != NULL; t++) {
        const ClawtIntegrationField *list;
        g_autoptr(GHashTable) values =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_autoptr(JsonNode) payload = NULL;
        g_autoptr(GError) error = NULL;
        const gchar *agents[] = { "kudu", NULL };
        ClawtLrgIntegrationForm form = { 0 };
        JsonObject *object;
        gsize n = 0;
        gsize i;

        list = clawt_integration_fields(types[t], &n);

        for (i = 0; i < n; i++) {
            const ClawtIntegrationField *field = &list[i];
            const gchar *value = "x";
            guint c;

            switch (field->kind) {
            case CLAWT_FIELD_BOOLEAN: value = "false"; break;
            case CLAWT_FIELD_INT:     value = "42"; break;
            case CLAWT_FIELD_LIST:    value = "a, b"; break;
            case CLAWT_FIELD_SECRET:  value = "env:SECRET_NAME"; break;
            case CLAWT_FIELD_CHOICE:
                value = clawt_integration_field_default(field);
                break;
            case CLAWT_FIELD_FLAGS:
                for (c = 0; field->choices[c] != NULL; c++)
                    g_hash_table_insert(
                        values,
                        clawt_lrg_settings_flag_slot(field,
                                                     field->choices[c]),
                        g_strdup(c == 0 ? "true" : "false"));
                continue;
            case CLAWT_FIELD_TEXT:
                break;
            }

            g_hash_table_insert(values,
                                g_strdup(clawt_lrg_settings_field_slot(field)),
                                g_strdup(value));
        }

        form.name = "it";
        form.type_id = types[t];
        form.enabled = FALSE;
        form.description = "for things";
        form.scope = "selected";
        form.agents = agents;
        form.values = values;

        payload = clawt_lrg_settings_integration_payload(&form, &error);
        g_assert_no_error(error);
        g_assert_nonnull(payload);
        object = json_node_get_object(payload);

        g_assert_false(clawt_ipc_payload_boolean(object, "enabled", TRUE));
        g_assert_cmpstr(clawt_ipc_payload_string(object, "scope"), ==,
                        "selected");

        for (i = 0; i < n; i++) {
            const ClawtIntegrationField *field = &list[i];
            const gchar *when = clawt_lrg_settings_when_value(
                types[t], values, field->when_key);
            JsonNode *member;

            if (!clawt_integration_field_applies(field, when)) {
                /* not shown, so not saved -- unless a sibling with the
                 * same key applies and wrote it */
                continue;
            }

            if (field->kind == CLAWT_FIELD_SECRET) {
                g_assert_cmpstr(clawt_ipc_payload_string(object,
                                                         "secret_backend"),
                                ==, "env");
                g_assert_cmpstr(clawt_ipc_payload_string(object,
                                                         "secret_locator"),
                                ==, "SECRET_NAME");
                continue;
            }

            g_assert_true(json_object_has_member(object, field->key));
            member = json_object_get_member(object, field->key);

            switch (field->kind) {
            case CLAWT_FIELD_BOOLEAN:
                g_assert_cmpint(json_node_get_value_type(member), ==,
                                G_TYPE_BOOLEAN);
                g_assert_false(json_node_get_boolean(member));
                break;
            case CLAWT_FIELD_INT:
                g_assert_cmpint(json_node_get_value_type(member), ==,
                                G_TYPE_INT64);
                g_assert_cmpint(json_node_get_int(member), ==, 42);
                break;
            case CLAWT_FIELD_LIST:
                g_assert_true(JSON_NODE_HOLDS_ARRAY(member));
                g_assert_cmpuint(json_array_get_length(
                    json_node_get_array(member)), ==, 2);
                break;
            case CLAWT_FIELD_FLAGS:
                g_assert_true(JSON_NODE_HOLDS_ARRAY(member));
                g_assert_cmpuint(json_array_get_length(
                    json_node_get_array(member)), ==, 1);
                break;
            case CLAWT_FIELD_CHOICE:
            case CLAWT_FIELD_TEXT:
            case CLAWT_FIELD_SECRET:
                g_assert_cmpint(json_node_get_value_type(member), ==,
                                G_TYPE_STRING);
                break;
            }
        }
    }
}

static void
test_integration_payload_refuses_secret_value(void)
{
    const ClawtIntegrationField *secret =
        any_field_of_kind(CLAWT_FIELD_SECRET);
    g_autoptr(GHashTable) values =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(GError) error = NULL;
    ClawtLrgIntegrationForm form = { 0 };

    g_assert_nonnull(secret);

    /* a bare token is not a reference, and must not be sent */
    g_hash_table_insert(values,
                        g_strdup(clawt_lrg_settings_field_slot(secret)),
                        g_strdup("ghp_actualtokenvalue"));

    form.name = "it";
    form.type_id = secret->type;
    form.enabled = TRUE;
    form.scope = "all";
    form.values = values;

    /*
     * Only refused if the secret field applies with this form's values;
     * conditional secrets are under a backend that has a default.
     */
    if (clawt_integration_field_applies(
            secret, clawt_lrg_settings_when_value(secret->type, values,
                                                  secret->when_key))) {
        g_assert_null(clawt_lrg_settings_integration_payload(&form, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    }
}

static void
test_when_value_defaults(void)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;

    list = clawt_integration_fields("notify", &n);

    for (i = 0; i < n; i++) {
        g_autoptr(GHashTable) values = NULL;
        const gchar *other;

        if (list[i].when_key == NULL)
            continue;

        /* nothing chosen: the deciding field's default, never NULL */
        g_assert_nonnull(clawt_lrg_settings_when_value("notify", NULL,
                                                       list[i].when_key));

        /* an unknown value is not a value */
        values = g_hash_table_new(g_str_hash, g_str_equal);
        g_hash_table_insert(values, (gpointer)list[i].when_key, "bogus");
        other = clawt_lrg_settings_when_value("notify", values,
                                              list[i].when_key);
        g_assert_cmpstr(other, !=, "bogus");
        return;
    }

    g_test_skip("notify has no conditional field");
}

static void
test_health_text(void)
{
    g_autoptr(JsonNode) ok = parse("{\"ok\": true}");
    g_autoptr(JsonNode) bad = parse("{\"ok\": false, \"error\": \"401\"}");
    g_autoptr(JsonNode) mute = parse("{\"ok\": false}");

    g_assert_cmpstr(clawt_lrg_settings_health_text(json_node_get_object(ok)),
                    ==, "It answered.");
    g_assert_cmpstr(clawt_lrg_settings_health_text(json_node_get_object(bad)),
                    ==, "401");
    g_assert_cmpstr(clawt_lrg_settings_health_text(
                        json_node_get_object(mute)), ==, "It did not answer.");
}

/* ── Connectors ───────────────────────────────────────────────────── */

static void
test_connector_state(void)
{
    g_autoptr(JsonNode) ready = parse(
        "{\"connected\": true, \"expires_at\": 2000, \"renewable\": false}");
    g_autoptr(JsonNode) expired = parse(
        "{\"connected\": true, \"expires_at\": 500, \"renewable\": false}");
    g_autoptr(JsonNode) renews = parse(
        "{\"connected\": true, \"expires_at\": 500, \"renewable\": true}");
    g_autoptr(JsonNode) none = parse("{\"connected\": false}");

    g_assert_cmpint(clawt_lrg_settings_connector_state(
                        json_node_get_object(ready), 1000), ==,
                    CLAWT_CONNECTOR_READY);
    g_assert_cmpint(clawt_lrg_settings_connector_state(
                        json_node_get_object(expired), 1000), ==,
                    CLAWT_CONNECTOR_EXPIRED);
    /* expired but renewable is working normally */
    g_assert_cmpint(clawt_lrg_settings_connector_state(
                        json_node_get_object(renews), 1000), ==,
                    CLAWT_CONNECTOR_READY);
    g_assert_cmpint(clawt_lrg_settings_connector_state(
                        json_node_get_object(none), 1000), ==,
                    CLAWT_CONNECTOR_UNAUTHORISED);

    g_assert_cmpstr(clawt_lrg_settings_connector_tone(CLAWT_CONNECTOR_READY),
                    ==, "good");
    g_assert_cmpstr(clawt_lrg_settings_connector_tone(
                        CLAWT_CONNECTOR_EXPIRED), ==, "warn");
    g_assert_cmpstr(clawt_lrg_settings_connector_tone(
                        CLAWT_CONNECTOR_UNAUTHORISED), ==, "neutral");
}

static void
test_connector_labels(void)
{
    g_autoptr(JsonNode) entry = parse(
        "{\"provider\": \"github\", \"account\": \"work\"}");
    g_autoptr(JsonNode) bare = parse("{\"provider\": \"github\"}");
    g_autoptr(JsonNode) cat = parse(
        "{\"id\": \"github\", \"name\": \"GitHub\", \"auth\": \"device\"}");
    g_autoptr(JsonNode) told = parse("{\"told_provider\": true}");
    g_autofree gchar *a =
        clawt_lrg_settings_connector_subtitle(json_node_get_object(entry));
    g_autofree gchar *b =
        clawt_lrg_settings_connector_subtitle(json_node_get_object(bare));
    g_autofree gchar *c =
        clawt_lrg_settings_catalog_label(json_node_get_object(cat));
    g_autofree gchar *one = clawt_lrg_settings_imported_text(1);
    g_autofree gchar *many = clawt_lrg_settings_imported_text(4);

    g_assert_cmpstr(a, ==, "github -- work");
    g_assert_cmpstr(b, ==, "github");
    g_assert_cmpstr(c, ==, "GitHub (device)");
    g_assert_cmpstr(one, ==, "Imported 1 connector from the registry.");
    g_assert_cmpstr(many, ==, "Imported 4 connectors from the registry.");
    g_assert_true(g_str_has_prefix(
        clawt_lrg_settings_revoked_text(json_node_get_object(told)),
        "Revoked"));
    g_assert_true(g_str_has_prefix(clawt_lrg_settings_revoked_text(NULL),
                                   "Forgotten here"));
}

static void
test_connector_payload(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) payload = NULL;
    g_autofree gchar *json = NULL;

    g_assert_null(clawt_lrg_settings_connector_payload(
        "", "github", "all", NULL, NULL, NULL, NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    payload = clawt_lrg_settings_connector_payload(
        "gh", "github", NULL, "work", "", NULL, "kudu, oryx", &error);
    g_assert_no_error(error);
    json = to_json(payload);
    g_assert_cmpstr(json, ==, "{\"name\":\"gh\",\"type\":\"connector\","
                              "\"provider\":\"github\",\"scope\":\"selected\","
                              "\"account\":\"work\","
                              "\"agents\":[\"kudu\",\"oryx\"]}");
}

/* ── Connections ──────────────────────────────────────────────────── */

static void
test_reach(void)
{
    ClawtConnectionStatus up = { CLAWT_REACH_REACHABLE, NULL, 1, NULL };
    ClawtConnectionStatus down = { CLAWT_REACH_UNREACHABLE, NULL, 0,
                                   (gchar *)"timed out" };
    ClawtConnectionStatus asked = { CLAWT_REACH_UNKNOWN, NULL, 0, NULL };
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;

    up.version = (gchar *)CLAWT_VERSION_STRING;
    a = clawt_lrg_settings_reach_detail(&up);
    b = clawt_lrg_settings_reach_detail(&down);

    g_assert_cmpstr(clawt_lrg_settings_reach_tone(CLAWT_REACH_REACHABLE), ==,
                    "good");
    g_assert_cmpstr(clawt_lrg_settings_reach_tone(CLAWT_REACH_REFUSED), ==,
                    "bad");
    g_assert_cmpstr(clawt_lrg_settings_reach_tone(CLAWT_REACH_UNKNOWN), ==,
                    "neutral");

    /* the same version says nothing about a mismatch */
    g_assert_true(g_str_has_suffix(a, ", 1 agent"));
    g_assert_cmpstr(b, ==, "timed out");
    g_assert_null(clawt_lrg_settings_reach_detail(&asked));
    g_assert_null(clawt_lrg_settings_reach_detail(NULL));
}

static void
test_reach_mismatch(void)
{
    ClawtConnectionStatus old = { CLAWT_REACH_REACHABLE, (gchar *)"0.0.1", 2,
                                  NULL };
    g_autofree gchar *line = clawt_lrg_settings_reach_detail(&old);
    g_autofree gchar *mismatch = clawt_version_mismatch_text("0.0.1");

    g_assert_nonnull(mismatch);
    g_assert_nonnull(strstr(line, mismatch));
}

static void
test_connection_form(void)
{
    g_autoptr(GPtrArray) existing = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_connection_free);
    g_autoptr(GError) error = NULL;
    ClawtConnection *made;

    g_ptr_array_add(existing, clawt_connection_new_remote("box", "h", 1,
                                                          NULL));

    g_assert_null(clawt_lrg_settings_connection_from_form(
        existing, "x", "", NULL, NULL, FALSE, FALSE, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    g_assert_null(clawt_lrg_settings_connection_from_form(
        existing, "x", "host", "70000", NULL, FALSE, FALSE, &error));
    g_assert_nonnull(strstr(error->message, "port"));
    g_clear_error(&error);

    g_assert_null(clawt_lrg_settings_connection_from_form(
        existing, "x", "host", "0", NULL, FALSE, FALSE, &error));
    g_clear_error(&error);

    /* the name defaults to the host, and two cannot share one */
    g_assert_null(clawt_lrg_settings_connection_from_form(
        existing, "", "box", NULL, NULL, FALSE, FALSE, &error));
    g_assert_nonnull(strstr(error->message, "already"));
    g_clear_error(&error);

    made = clawt_lrg_settings_connection_from_form(
        existing, "", "laptop", "", "tok", TRUE, TRUE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(clawt_connection_get_name(made), ==, "laptop");
    g_assert_cmpuint(clawt_connection_get_port(made), ==,
                     CLAWT_DEFAULT_TCP_PORT);
    g_assert_cmpstr(clawt_connection_get_token(made), ==, "tok");
    g_assert_true(clawt_connection_get_tls(made));
    g_assert_true(clawt_connection_get_accept_unknown_certificate(made));
    clawt_connection_free(made);

    /* accepting an unknown certificate means nothing without TLS */
    made = clawt_lrg_settings_connection_from_form(
        existing, "plain", "laptop", "9000", "", FALSE, TRUE, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(clawt_connection_get_port(made), ==, 9000);
    g_assert_null(clawt_connection_get_token(made));
    g_assert_false(clawt_connection_get_accept_unknown_certificate(made));
    clawt_connection_free(made);
}

static void
test_daemon_version(void)
{
    g_autoptr(JsonNode) newer = parse(
        "{\"version\": \"0.6.2\", \"update\": {\"available\": true, "
        "\"latest\": \"0.7.0\"}}");
    g_autoptr(JsonNode) same = parse(
        "{\"version\": \"0.6.2\", \"update\": {\"available\": false, "
        "\"latest\": \"0.6.2\"}}");
    g_autofree gchar *update = NULL;
    g_autofree gchar *none = NULL;
    g_autofree gchar *v1 = clawt_lrg_settings_daemon_version(newer, &update);
    g_autofree gchar *v2 = clawt_lrg_settings_daemon_version(same, &none);

    g_assert_cmpstr(v1, ==, "0.6.2");
    g_assert_cmpstr(update, ==, "0.7.0");
    g_assert_cmpstr(v2, ==, "0.6.2");
    /* latest on its own is the version already running */
    g_assert_null(none);
    g_assert_null(clawt_lrg_settings_daemon_version(NULL, NULL));
}

/* ── Appearance ───────────────────────────────────────────────────── */

static void
test_scale_step(void)
{
    /* down out of the range follows the monitor; up from 0 is the floor */
    g_assert_cmpfloat(clawt_lrg_settings_scale_step(CLAWT_LRG_SCALE_MIN,
                                                    CLAWT_LRG_SCALE_MIN - 0.05),
                      ==, 0.0);
    g_assert_cmpfloat(clawt_lrg_settings_scale_step(0.0, 0.05), ==,
                      CLAWT_LRG_SCALE_MIN);
    g_assert_cmpfloat(clawt_lrg_settings_scale_step(1.0, 1.25), ==, 1.25);
    g_assert_cmpfloat(clawt_lrg_settings_scale_step(1.0, 9.0), ==,
                      CLAWT_LRG_SCALE_MAX);
    g_assert_cmpfloat(clawt_lrg_settings_scale_step(1.0, 0.0), ==, 0.0);
}

static void
test_scale_text(void)
{
    g_autofree gchar *follow = clawt_lrg_settings_scale_text(0.0);
    g_autofree gchar *set = clawt_lrg_settings_scale_text(1.25);

    g_assert_cmpstr(follow, ==, "Follows the monitor");
    g_assert_cmpstr(set, ==, "1.25\303\227");
}

static void
test_palette_note_builtin(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();

    /* a built-in scheme has no palette file to complain about */
    g_assert_null(clawt_lrg_settings_palette_note(appearance));
    g_assert_cmpstr(clawt_lrg_settings_font_text(NULL, FALSE), ==,
                    "Whatever the desktop uses");
    g_assert_cmpstr(clawt_lrg_settings_font_text("", TRUE), ==,
                    "Whatever the desktop uses for monospace");
    g_assert_cmpstr(clawt_lrg_settings_font_text("Inter", TRUE), ==,
                    "Inter");
}

/* ── New agent ────────────────────────────────────────────────────── */

static void
test_filter_providers(void)
{
    g_autoptr(JsonNode) catalog = parse(
        "{\"providers\": ["
        " {\"id\": \"claude-code\", \"agent\": true, \"tools\": false},"
        " {\"id\": \"openai\", \"agent\": false, \"tools\": true},"
        " {\"id\": \"old\"}"
        "]}");
    g_autoptr(JsonArray) agents =
        clawt_lrg_create_filter_providers(catalog, "agent", NULL);
    g_autoptr(JsonArray) tools =
        clawt_lrg_create_filter_providers(catalog, "tools", NULL);
    g_autoptr(JsonArray) kept =
        clawt_lrg_create_filter_providers(catalog, "agent", "openai");
    g_autoptr(JsonArray) all =
        clawt_lrg_create_filter_providers(catalog, NULL, NULL);

    /* a provider with no flag is an older daemon's, and kept */
    g_assert_cmpuint(json_array_get_length(agents), ==, 2);
    g_assert_cmpuint(json_array_get_length(tools), ==, 2);
    g_assert_cmpuint(json_array_get_length(kept), ==, 3);
    g_assert_cmpuint(json_array_get_length(all), ==, 3);
    g_assert_null(clawt_lrg_create_filter_providers(NULL, "agent", NULL));
}

static void
test_model_label(void)
{
    g_autoptr(JsonNode) noted = parse(
        "{\"id\": \"s\", \"label\": \"Sonnet\", \"note\": \"fast\"}");
    g_autoptr(JsonNode) bare = parse("{\"id\": \"haiku\"}");
    g_autofree gchar *a =
        clawt_lrg_create_model_label(json_node_get_object(noted));
    g_autofree gchar *b =
        clawt_lrg_create_model_label(json_node_get_object(bare));

    g_assert_cmpstr(a, ==, "Sonnet - fast");
    g_assert_cmpstr(b, ==, "haiku");
}

static void
test_catalog_index(void)
{
    g_autoptr(JsonNode) node = parse(
        "[{\"reference\": \"a\"}, {\"reference\": \"b\"}]");
    JsonArray *items = json_node_get_array(node);
    g_autoptr(JsonArray) empty = json_array_new();

    g_assert_cmpuint(clawt_lrg_create_catalog_index(items, "reference", "b",
                                                    "a"), ==, 1);
    /* a value of its own that is not listed is Other, never the first */
    g_assert_cmpuint(clawt_lrg_create_catalog_index(items, "reference", "z",
                                                    "a"), ==, 2);
    g_assert_cmpuint(clawt_lrg_create_catalog_index(items, "reference", NULL,
                                                    "b"), ==, 1);
    g_assert_cmpuint(clawt_lrg_create_catalog_index(items, "reference", NULL,
                                                    "gone"), ==, 0);
    g_assert_cmpuint(clawt_lrg_create_catalog_index(empty, "path", NULL,
                                                    NULL), ==, 0);
    g_assert_cmpuint(clawt_lrg_create_catalog_index(NULL, "path", "x",
                                                    NULL), ==, 0);
}

static void
test_image_and_disk_notes(void)
{
    g_autoptr(JsonNode) noted = parse(
        "{\"reference\": \"quay.io/fedora:44\", \"note\": \"toolbox\"}");
    g_autoptr(JsonNode) disk = parse("{\"path\": \"/var/i.qcow2\"}");
    g_autofree gchar *a =
        clawt_lrg_create_image_note(json_node_get_object(noted));
    g_autofree gchar *other = clawt_lrg_create_image_note(NULL);

    g_assert_cmpstr(a, ==, "quay.io/fedora:44 -- toolbox");
    g_assert_cmpstr(other, ==, "any reference podman can pull");
    g_assert_cmpstr(clawt_lrg_create_disk_note(json_node_get_object(disk), 1),
                    ==, "/var/i.qcow2");
    g_assert_true(g_str_has_prefix(clawt_lrg_create_disk_note(NULL, 0),
                                   "No cloud images yet"));
    g_assert_cmpstr(clawt_lrg_create_disk_note(NULL, 2), ==,
                    "A path to a qcow2 on this machine");
}

static void
test_team_choices(void)
{
    g_autoptr(JsonNode) teams = parse(
        "{\"teams\": [{\"id\": \"backend\", \"name\": \"Backend\"}, "
        "{\"id\": \"ops\"}]}");
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) ids2 = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels2 = g_ptr_array_new_with_free_func(g_free);

    clawt_lrg_create_team_choices(teams, NULL, ids, labels);
    g_assert_cmpuint(ids->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(ids, 0), ==, "");
    g_assert_cmpstr(g_ptr_array_index(labels, 1), ==, "Backend");
    g_assert_cmpstr(g_ptr_array_index(labels, 2), ==, "ops");

    /* an undeclared team the agent names is kept, marked */
    clawt_lrg_create_team_choices(teams, "ghost", ids2, labels2);
    g_assert_cmpuint(ids2->len, ==, 4);
    g_assert_cmpstr(g_ptr_array_index(ids2, 3), ==, "ghost");
    g_assert_cmpstr(g_ptr_array_index(labels2, 3), ==, "ghost (not declared)");
}

static void
test_preview_text(void)
{
    g_autoptr(JsonNode) bare = parse("{\"yaml\": \"id: a\\n\", \"id\": \"a\"}");
    g_autoptr(JsonNode) files = parse(
        "{\"yaml\": \"id: a\\n\", \"id\": \"a\", \"draft\": \"d1\", "
        "\"files\": [{\"name\": \"SOUL.org\", \"content\": \"x\\ny\\n\"}]}");
    g_autofree gchar *a =
        clawt_lrg_create_preview_text(json_node_get_object(bare));
    g_autofree gchar *b =
        clawt_lrg_create_preview_text(json_node_get_object(files));

    g_assert_cmpstr(a, ==, "id: a\n");
    g_assert_nonnull(strstr(b, "  SOUL.org  (2 lines)\n"));
    g_assert_true(g_str_has_suffix(b, "clawtilla agent edit a"));
}

static void
test_created_text(void)
{
    g_autoptr(JsonNode) started = parse("{\"started\": true}");
    g_autoptr(JsonNode) failed = parse(
        "{\"started\": false, \"start_error\": \"no disk\"}");
    g_autoptr(JsonNode) old = parse("{}");
    g_autoptr(JsonNode) designed = parse("{\"id\": \"kudu\", "
                                         "\"started\": true}");
    g_autofree gchar *a =
        clawt_lrg_create_created_text(json_node_get_object(started), "kudu");
    g_autofree gchar *b =
        clawt_lrg_create_created_text(json_node_get_object(failed), "kudu");
    g_autofree gchar *c =
        clawt_lrg_create_created_text(json_node_get_object(old), "kudu");
    g_autofree gchar *d =
        clawt_lrg_create_created_text(json_node_get_object(designed), NULL);

    g_assert_cmpstr(a, ==, "kudu created and started.");
    g_assert_cmpstr(b, ==, "kudu was created but did not start: no disk");
    g_assert_cmpstr(c, ==, "Agent created.");
    g_assert_cmpstr(d, ==, "kudu created and started.");
}

/* ── Import ───────────────────────────────────────────────────────── */

static void
test_import_payload(void)
{
    g_autoptr(GError) error = NULL;
    guint i;

    g_assert_null(clawt_lrg_create_import_payload("", CLAWT_IMPORT_COPY,
                                                  NULL, "/p", FALSE, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    /* every mode the library has: a URL where it takes one, else a path */
    for (i = 0; i < clawt_import_mode_count(); i++) {
        ClawtImportMode mode = clawt_import_mode_nth(i);
        gboolean url = clawt_import_mode_takes_url(mode);
        g_autoptr(JsonNode) refused = NULL;
        g_autoptr(JsonNode) payload = NULL;
        JsonObject *object;

        refused = clawt_lrg_create_import_payload(
            "kudu", mode, url ? NULL : "https://x/r.git",
            url ? "/p" : NULL, TRUE, &error);
        g_assert_null(refused);
        g_assert_nonnull(error);
        g_clear_error(&error);

        payload = clawt_lrg_create_import_payload(
            "kudu", mode, "https://x/r.git", "/p", TRUE, &error);
        g_assert_no_error(error);
        object = json_node_get_object(payload);

        g_assert_cmpstr(clawt_ipc_payload_string(object, "mode"), ==,
                        clawt_import_mode_nth_nick(i));
        g_assert_cmpstr(clawt_ipc_payload_string(object, "from"), ==,
                        url ? "https://x/r.git" : "/p");
        /* only a copy reads the source's .git */
        g_assert_cmpstr(clawt_ipc_payload_string(object, "keep_git"), ==,
                        mode == CLAWT_IMPORT_COPY ? "true" : "false");
    }
}

static void
test_found_subtitle(void)
{
    g_autoptr(JsonNode) holds = parse(
        "{\"id\": \"a\", \"holds\": \"a mailbox\", \"path\": \"/p\"}");
    g_autoptr(JsonNode) path = parse(
        "{\"id\": \"a\", \"holds\": \"\", \"path\": \"/p\"}");

    g_assert_cmpstr(clawt_lrg_create_found_subtitle(
                        json_node_get_object(holds)), ==, "a mailbox");
    g_assert_cmpstr(clawt_lrg_create_found_subtitle(
                        json_node_get_object(path)), ==, "/p");
}

/* ── New room ─────────────────────────────────────────────────────── */

static void
test_room_payload(void)
{
    const gchar *one[] = { "kudu", NULL };
    const gchar *three[] = { "kudu", "oryx", "ibex", NULL };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) payload = NULL;
    g_autofree gchar *json = NULL;

    g_assert_null(clawt_lrg_create_room_payload("", NULL, three, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    g_assert_null(clawt_lrg_create_room_payload("r", NULL, one, &error));
    g_assert_nonnull(strstr(error->message, "two"));
    g_clear_error(&error);

    payload = clawt_lrg_create_room_payload("r", "", three, &error);
    g_assert_no_error(error);
    json = to_json(payload);

    /* the name falls back to the id */
    g_assert_cmpstr(json, ==, "{\"room\":\"r\",\"name\":\"r\","
                              "\"members\":\"kudu,oryx,ibex\"}");
    g_assert_cmpstr(clawt_lrg_create_room_created_text(2), ==, "Created.");
    g_assert_nonnull(strstr(clawt_lrg_create_room_created_text(3),
                            "@their-id"));
}

gint
main(gint argc, gchar **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/lrg-settings/split-list", test_split_list);
    g_test_add_func("/lrg-settings/member-array-wrong-type",
                    test_member_array_wrong_type);
    g_test_add_func("/lrg-settings/images/human-size", test_human_size);
    g_test_add_func("/lrg-settings/images/subtitle", test_image_subtitle);
    g_test_add_func("/lrg-settings/images/progress", test_progress);
    g_test_add_func("/lrg-settings/images/failed", test_image_failed);
    g_test_add_func("/lrg-settings/spending/periods", test_periods);
    g_test_add_func("/lrg-settings/spending/rows", test_spend_rows);
    g_test_add_func("/lrg-settings/teams/subtitle", test_team_subtitle);
    g_test_add_func("/lrg-settings/folders/scope", test_folder_scope);
    g_test_add_func("/lrg-settings/folders/subtitle", test_folder_subtitle);
    g_test_add_func("/lrg-settings/folders/relabel-walked",
                    test_relabel_label_walked);
    g_test_add_func("/lrg-settings/folders/payload", test_folder_payload);
    g_test_add_func("/lrg-settings/integrations/scopes", test_scopes_walked);
    g_test_add_func("/lrg-settings/integrations/subtitle",
                    test_integration_subtitle);
    g_test_add_func("/lrg-settings/integrations/find", test_find_integration);
    g_test_add_func("/lrg-settings/integrations/field-initial",
                    test_field_initial);
    g_test_add_func("/lrg-settings/integrations/flag-initial",
                    test_flag_initial);
    g_test_add_func("/lrg-settings/integrations/payload-walks-table",
                    test_integration_payload_walks_table);
    g_test_add_func("/lrg-settings/integrations/refuses-secret-value",
                    test_integration_payload_refuses_secret_value);
    g_test_add_func("/lrg-settings/integrations/when-defaults",
                    test_when_value_defaults);
    g_test_add_func("/lrg-settings/integrations/health", test_health_text);
    g_test_add_func("/lrg-settings/connectors/state", test_connector_state);
    g_test_add_func("/lrg-settings/connectors/labels", test_connector_labels);
    g_test_add_func("/lrg-settings/connectors/payload",
                    test_connector_payload);
    g_test_add_func("/lrg-settings/connections/reach", test_reach);
    g_test_add_func("/lrg-settings/connections/reach-mismatch",
                    test_reach_mismatch);
    g_test_add_func("/lrg-settings/connections/form", test_connection_form);
    g_test_add_func("/lrg-settings/connections/daemon-version",
                    test_daemon_version);
    g_test_add_func("/lrg-settings/appearance/scale-step", test_scale_step);
    g_test_add_func("/lrg-settings/appearance/scale-text", test_scale_text);
    g_test_add_func("/lrg-settings/appearance/palette-note",
                    test_palette_note_builtin);
    g_test_add_func("/lrg-create/filter-providers", test_filter_providers);
    g_test_add_func("/lrg-create/model-label", test_model_label);
    g_test_add_func("/lrg-create/catalog-index", test_catalog_index);
    g_test_add_func("/lrg-create/image-and-disk-notes",
                    test_image_and_disk_notes);
    g_test_add_func("/lrg-create/team-choices", test_team_choices);
    g_test_add_func("/lrg-create/preview-text", test_preview_text);
    g_test_add_func("/lrg-create/created-text", test_created_text);
    g_test_add_func("/lrg-create/import-payload", test_import_payload);
    g_test_add_func("/lrg-create/found-subtitle", test_found_subtitle);
    g_test_add_func("/lrg-create/room-payload", test_room_payload);

    return g_test_run();
}
