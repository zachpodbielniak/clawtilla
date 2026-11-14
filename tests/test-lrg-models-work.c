/*
 * test-lrg-models-work.c - The Work and Automation pages' decisions
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: no daemon, no display, no network.  The model is #included
 * so its static helpers are reachable and the Makefile's
 * test-lrg-models% rule relinks this binary when the model changes.
 *
 * Every reply below is shaped by reading the daemon code that produces
 * it, not by the name that sounds right: `parent` (not `parent_id`),
 * `created_at` in seconds, `at` as a string, booleans as booleans.
 */

#include <clawtilla.h>

#include "../clients/lrg/lrg-work-model.c"

static JsonNode *
parse(const gchar *text)
{
    g_autoptr(GError) error = NULL;
    JsonNode *node = json_from_string(text, &error);

    g_assert_no_error(error);
    g_assert_nonnull(node);
    return node;
}

static JsonObject *
object_of(JsonNode *node)
{
    return json_node_get_object(node);
}

/* ── Schema fields ────────────────────────────────────────────────── */

static void
test_extra_fields_walk_schema(void)
{
    static const gchar *const handled[] = {
        "description", "instructions", "agent", "enabled", "schedule", "at",
        "weekday", "cron", "directory", "worktree", "read_only", "proactive",
        "catch_up", "jitter_seconds", NULL
    };
    g_autoptr(GPtrArray) fields =
        clawt_lrg_work_extra_fields("routines.", handled);
    gboolean saw_isolate = FALSE;
    guint i;

    /* `isolate` is in the schema and in no hand-written editor */
    for (i = 0; i < fields->len; i++) {
        ClawtLrgSchemaField *field = g_ptr_array_index(fields, i);

        g_assert_false(g_strv_contains(handled, field->leaf));
        g_assert_cmpstr(field->leaf, !=, "id");

        if (g_strcmp0(field->leaf, "isolate") == 0) {
            saw_isolate = TRUE;
            g_assert_cmpint(field->kind, ==, CLAWT_LRG_FIELD_SWITCH);
            g_assert_cmpstr(field->hint, ==,
                            "Give this routine a conversation of its own.");
        }
    }

    g_assert_true(saw_isolate);
}

static void
test_extra_fields_never_offer_secret(void)
{
    g_autoptr(GPtrArray) fields = clawt_lrg_work_extra_fields("triggers.",
                                                              NULL);
    guint i;

    g_assert_cmpuint(fields->len, >, 0);

    for (i = 0; i < fields->len; i++) {
        ClawtLrgSchemaField *field = g_ptr_array_index(fields, i);

        g_assert_cmpstr(field->leaf, !=, "secret");

        if (g_strcmp0(field->leaf, "events") == 0)
            g_assert_cmpint(field->kind, ==, CLAWT_LRG_FIELD_LIST);
    }
}

static void
test_boolean_reads_booleans_with_schema_default(void)
{
    g_autoptr(JsonNode) off = parse("{\"enabled\": false}");
    g_autoptr(JsonNode) on = parse("{\"enabled\": true}");
    g_autoptr(JsonNode) silent = parse("{}");

    g_assert_false(clawt_lrg_work_boolean(object_of(off), "routines.",
                                          "enabled"));
    g_assert_true(clawt_lrg_work_boolean(object_of(on), "routines.",
                                         "enabled"));

    /* the schema says `enabled` defaults true and `catch_up` false */
    g_assert_true(clawt_lrg_work_boolean(object_of(silent), "routines.",
                                         "enabled"));
    g_assert_true(clawt_lrg_work_boolean(NULL, "routines.", "enabled"));
    g_assert_false(clawt_lrg_work_boolean(NULL, "routines.", "catch_up"));
    g_assert_false(clawt_lrg_work_boolean(NULL, "routines.", "no_such"));
}

static void
test_first_paragraph_rejoins(void)
{
    g_autofree gchar *a = clawt_lrg_work_first_paragraph("one\ntwo\n\nthree");
    g_autofree gchar *b = clawt_lrg_work_first_paragraph(NULL);

    g_assert_cmpstr(a, ==, "one two");
    g_assert_cmpstr(b, ==, "");
}

static void
test_field_text_and_value(void)
{
    g_autoptr(JsonNode) routine = parse(
        "{\"jitter_seconds\": 30, \"events\": [\"push\", \"tag\"],"
        " \"directory\": \"/srv\", \"isolate\": true}");
    g_autofree gchar *number = clawt_lrg_work_field_text(
        object_of(routine), "jitter_seconds", CLAWT_LRG_FIELD_NUMBER);
    g_autofree gchar *list = clawt_lrg_work_field_text(
        object_of(routine), "events", CLAWT_LRG_FIELD_LIST);
    g_autofree gchar *text = clawt_lrg_work_field_text(
        object_of(routine), "directory", CLAWT_LRG_FIELD_TEXT);
    g_autofree gchar *absent = clawt_lrg_work_field_text(
        object_of(routine), "cron", CLAWT_LRG_FIELD_TEXT);
    g_autofree gchar *label = clawt_lrg_work_field_label("catch_up");
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) as_bool = NULL;
    g_autoptr(JsonNode) as_int = NULL;
    g_autoptr(JsonNode) as_list = NULL;
    JsonNode *refused;

    /* a number read with the int reader, not the string one */
    g_assert_cmpstr(number, ==, "30");
    g_assert_cmpstr(list, ==, "push, tag");
    g_assert_cmpstr(text, ==, "/srv");
    g_assert_cmpstr(absent, ==, "");
    g_assert_cmpstr(label, ==, "Catch up");

    as_bool = clawt_lrg_work_field_value(CLAWT_LRG_FIELD_SWITCH, NULL, TRUE,
                                         &error);
    g_assert_cmpint(json_node_get_value_type(as_bool), ==, G_TYPE_BOOLEAN);
    g_assert_true(json_node_get_boolean(as_bool));
    as_int = clawt_lrg_work_field_value(CLAWT_LRG_FIELD_NUMBER, "12", FALSE,
                                        &error);
    g_assert_cmpint(json_node_get_int(as_int), ==, 12);
    as_list = clawt_lrg_work_field_value(CLAWT_LRG_FIELD_LIST, "a, b", FALSE,
                                         &error);
    g_assert_true(JSON_NODE_HOLDS_ARRAY(as_list));
    g_assert_cmpuint(json_array_get_length(json_node_get_array(as_list)), ==,
                     2);
    g_assert_no_error(error);

    refused = clawt_lrg_work_field_value(CLAWT_LRG_FIELD_NUMBER, "1.5",
                                         FALSE, &error);
    g_assert_null(refused);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
}

static void
test_agent_choices(void)
{
    g_autoptr(JsonNode) agents = parse(
        "{\"agents\":[{\"id\":\"kudu\",\"name\":\"Kudu\"},"
        "{\"id\":\"oryx\"},{\"name\":\"no id\"}]}");
    g_auto(GStrv) ids = NULL;
    g_auto(GStrv) labels = NULL;
    g_auto(GStrv) none_ids = NULL;
    g_auto(GStrv) none_labels = NULL;

    clawt_lrg_work_agent_choices(agents, &ids, &labels);
    g_assert_cmpuint(g_strv_length(ids), ==, 2);
    g_assert_cmpstr(labels[0], ==, "Kudu");
    g_assert_cmpstr(labels[1], ==, "oryx");

    clawt_lrg_work_agent_choices(NULL, &none_ids, &none_labels);
    g_assert_cmpuint(g_strv_length(none_ids), ==, 0);
}

/* ── Routines ─────────────────────────────────────────────────────── */

static void
test_schedule_choices_walk_enum(void)
{
    g_auto(GStrv) ids = NULL;
    g_auto(GStrv) labels = NULL;
    GEnumClass *klass = g_type_class_ref(CLAWT_TYPE_SCHEDULE);

    clawt_lrg_schedule_choices(&ids, &labels);
    g_assert_cmpuint(g_strv_length(ids), ==, klass->n_values);
    g_assert_cmpuint(g_strv_length(labels), ==, klass->n_values);
    g_assert_true(g_strv_contains((const gchar *const *)ids, "weekdays"));
    g_assert_true(g_strv_contains((const gchar *const *)labels, "Custom"));
    g_type_class_unref(klass);
}

static void
test_weekday_choices(void)
{
    g_auto(GStrv) ids = NULL;
    g_auto(GStrv) labels = NULL;

    clawt_lrg_weekday_choices(&ids, &labels);
    g_assert_cmpuint(g_strv_length(ids), ==, 7);
    g_assert_cmpstr(ids[0], ==, "sunday");
    g_assert_cmpstr(ids[6], ==, "saturday");
    g_assert_cmpuint(g_strv_length(labels), ==, 7);
}

static void
test_schedule_uses(void)
{
    ClawtLrgScheduleUses uses;

    uses = clawt_lrg_schedule_uses("weekly");
    g_assert_true(uses.at && uses.weekday && !uses.cron && !uses.minute_only);
    uses = clawt_lrg_schedule_uses("hourly");
    g_assert_true(uses.at && uses.minute_only && !uses.weekday);
    uses = clawt_lrg_schedule_uses("custom");
    g_assert_true(uses.cron && !uses.at);
    uses = clawt_lrg_schedule_uses("manual");
    g_assert_false(uses.at || uses.cron || uses.weekday);
    uses = clawt_lrg_schedule_uses("fortnightly");
    g_assert_false(uses.at || uses.cron || uses.weekday);
}

static void
test_parse_at(void)
{
    gint h = -1;
    gint m = -1;

    g_assert_true(clawt_lrg_parse_at("09:00", &h, &m));
    g_assert_cmpint(h, ==, 9);
    g_assert_cmpint(m, ==, 0);
    g_assert_true(clawt_lrg_parse_at("7:45", &h, &m));
    g_assert_cmpint(h, ==, 7);
    g_assert_cmpint(m, ==, 45);

    /* values a picker would silently rewrite are refused */
    g_assert_false(clawt_lrg_parse_at("24:00", &h, &m));
    g_assert_false(clawt_lrg_parse_at("9", &h, &m));
    g_assert_false(clawt_lrg_parse_at("09:5", &h, &m));
    g_assert_false(clawt_lrg_parse_at("09:00pm", &h, &m));
    g_assert_false(clawt_lrg_parse_at("", &h, &m));
    g_assert_false(clawt_lrg_parse_at(NULL, &h, &m));
}

static void
test_schedule_preview(void)
{
    /* Saturday 2 August 2025, noon (TZ is pinned to UTC in main) */
    g_autoptr(GDateTime) after = g_date_time_new_local(2025, 8, 2, 12, 0, 0);
    g_autofree gchar *weekdays = NULL;
    g_autofree gchar *manual = NULL;
    g_autofree gchar *broken = NULL;
    g_autofree gchar *never = NULL;
    gboolean ok = FALSE;

    weekdays = clawt_lrg_schedule_preview("weekdays", "09:00", NULL, NULL,
                                          after, &ok);
    g_assert_true(ok);
    g_assert_cmpstr(weekdays, ==,
                    "Next: Mon 4 Aug 09:00, then Tue 5 Aug 09:00 and "
                    "Wed 6 Aug 09:00.  (0 9 * * 1-5)");

    manual = clawt_lrg_schedule_preview("manual", NULL, NULL, NULL, after,
                                        &ok);
    g_assert_true(ok);
    g_assert_cmpstr(manual, ==, "Only when you ask.");

    broken = clawt_lrg_schedule_preview("custom", NULL, NULL, "61 * * * *",
                                        after, &ok);
    g_assert_false(ok);
    g_assert_true(g_str_has_prefix(broken,
                                   "Not a schedule the daemon will take"));

    never = clawt_lrg_schedule_preview("custom", NULL, NULL, "0 0 30 2 *",
                                       after, &ok);
    g_assert_false(ok);
    g_assert_true(strstr(never, "will never run") != NULL);
}

static void
test_routine_when(void)
{
    g_autoptr(JsonNode) next = parse(
        "{\"id\":\"brief\",\"next_run\":\"2025-08-04T09:00:00Z\","
        "\"enabled\":true}");
    g_autoptr(JsonNode) broken = parse(
        "{\"id\":\"b\",\"problem\":\"bad minute\",\"enabled\":true}");
    g_autoptr(JsonNode) manual = parse("{\"id\":\"m\",\"enabled\":true}");
    g_autoptr(JsonNode) off = parse("{\"id\":\"o\",\"enabled\":false}");
    g_autofree gchar *a = clawt_lrg_routine_when(object_of(next));
    g_autofree gchar *b = clawt_lrg_routine_when(object_of(broken));
    g_autofree gchar *c = clawt_lrg_routine_when(object_of(manual));
    g_autofree gchar *d = clawt_lrg_routine_when(object_of(off));

    g_assert_cmpstr(a, ==, "next Mon 4 Aug at 09:00");
    g_assert_cmpstr(b, ==, "will never run \342\200\224 bad minute");
    g_assert_cmpstr(c, ==, "only when you ask");
    g_assert_cmpstr(d, ==, "off");
}

static void
test_routine_title_and_last_run(void)
{
    g_autoptr(JsonNode) described = parse(
        "{\"id\":\"r\",\"description\":\"Morning brief\","
        "\"last_state\":\"failed\",\"last_run\":1000}");
    g_autoptr(JsonNode) bare = parse(
        "{\"id\":\"r2\",\"last_state\":\"never\",\"last_run\":0}");
    g_autofree gchar *failed = NULL;
    g_autofree gchar *never = NULL;
    const gchar *tone = NULL;

    g_assert_cmpstr(clawt_lrg_routine_title(object_of(described)), ==,
                    "Morning brief");
    g_assert_cmpstr(clawt_lrg_routine_title(object_of(bare)), ==, "r2");

    /* seconds on the wire: 1000 s plus two hours is "2h ago" */
    failed = clawt_lrg_routine_last_run(object_of(described),
                                        (gint64)(1000 + 7200) * G_USEC_PER_SEC,
                                        &tone);
    g_assert_cmpstr(failed, ==, "failed 2h ago");
    g_assert_cmpstr(tone, ==, "bad");

    never = clawt_lrg_routine_last_run(object_of(bare), 0, &tone);
    g_assert_null(never);
    g_assert_cmpstr(tone, ==, "neutral");
}

static ClawtLrgRoutineForm
good_routine_form(void)
{
    ClawtLrgRoutineForm form = {
        TRUE, "brief", "Morning brief", "Summarise the night.", "kudu", "",
        "weekly", "09:30", "friday", "", FALSE, TRUE, FALSE, FALSE, FALSE,
        "15", NULL
    };

    return form;
}

static void
test_routine_payload(void)
{
    ClawtLrgRoutineForm form = good_routine_form();
    g_autoptr(JsonNode) extras_node = parse("{\"isolate\": true}");
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) payload = NULL;
    JsonObject *object;

    form.extras = object_of(extras_node);
    payload = clawt_lrg_routine_payload(&form, &error);
    g_assert_no_error(error);
    object = object_of(payload);

    g_assert_cmpstr(json_object_get_string_member(object, "id"), ==, "brief");
    g_assert_cmpstr(json_object_get_string_member(object, "schedule"), ==,
                    "weekly");
    g_assert_cmpstr(json_object_get_string_member(object, "weekday"), ==,
                    "friday");

    /* booleans go as booleans, jitter as a number */
    g_assert_cmpint(json_node_get_value_type(json_object_get_member(
                        object, "enabled")), ==, G_TYPE_BOOLEAN);
    g_assert_true(json_object_get_boolean_member(object, "enabled"));
    g_assert_false(json_object_get_boolean_member(object, "catch_up"));
    g_assert_cmpint(json_object_get_int_member(object, "jitter_seconds"), ==,
                    15);
    g_assert_true(json_object_get_boolean_member(object, "isolate"));
}

static void
test_routine_payload_refusals(void)
{
    ClawtLrgRoutineForm form;
    g_autoptr(GError) error = NULL;
    JsonNode *payload;

    form = good_routine_form();
    form.id = "  ";
    payload = clawt_lrg_routine_payload(&form, &error);
    g_assert_null(payload);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    form = good_routine_form();
    form.instructions = "";
    g_assert_null(clawt_lrg_routine_payload(&form, &error));
    g_assert_nonnull(strstr(error->message, "instructions"));
    g_clear_error(&error);

    /* a proactive routine is told what to do when it has nothing */
    form.proactive = TRUE;
    payload = clawt_lrg_routine_payload(&form, &error);
    g_assert_no_error(error);
    json_node_unref(payload);

    form = good_routine_form();
    form.agent = NULL;
    g_assert_null(clawt_lrg_routine_payload(&form, &error));
    g_clear_error(&error);

    form = good_routine_form();
    form.jitter = "ten";
    g_assert_null(clawt_lrg_routine_payload(&form, &error));
    g_assert_nonnull(strstr(error->message, "Jitter"));
    g_clear_error(&error);

    form.jitter = "-1";
    g_assert_null(clawt_lrg_routine_payload(&form, &error));
    g_clear_error(&error);

    form = good_routine_form();
    form.schedule = "custom";
    form.cron = "not cron";
    g_assert_null(clawt_lrg_routine_payload(&form, &error));
    g_assert_nonnull(strstr(error->message, "schedule"));
}

/* ── Triggers ─────────────────────────────────────────────────────── */

static void
test_provider_choices_walk_library(void)
{
    g_auto(GStrv) ids = NULL;
    g_auto(GStrv) labels = NULL;
    guint i;

    clawt_lrg_provider_choices(&ids, &labels);
    g_assert_cmpuint(g_strv_length(ids), ==, clawt_trigger_provider_count());

    for (i = 0; ids[i] != NULL; i++) {
        g_assert_cmpstr(ids[i], ==, clawt_trigger_provider_nth_nick(i));
        g_assert_cmpstr(labels[i], ==, clawt_trigger_provider_nth_label(i));
    }
}

static void
test_trigger_state_priority(void)
{
    g_autoptr(JsonNode) none = parse(
        "{\"has_secret\":false,\"pending_verification\":true,"
        "\"enabled\":true}");
    g_autoptr(JsonNode) pending = parse(
        "{\"has_secret\":true,\"pending_verification\":true,"
        "\"enabled\":false}");
    g_autoptr(JsonNode) off = parse(
        "{\"has_secret\":true,\"pending_verification\":false,"
        "\"enabled\":false}");
    g_autoptr(JsonNode) on = parse(
        "{\"has_secret\":true,\"pending_verification\":false,"
        "\"enabled\":true}");
    const gchar *tone = NULL;

    g_assert_cmpstr(clawt_lrg_trigger_state(object_of(none), &tone), ==,
                    "no secret -- it refuses everything");
    g_assert_cmpstr(tone, ==, "bad");
    g_assert_cmpstr(clawt_lrg_trigger_state(object_of(pending), &tone), ==,
                    "waiting for its first delivery");
    g_assert_cmpstr(tone, ==, "warn");
    g_assert_cmpstr(clawt_lrg_trigger_state(object_of(off), &tone), ==,
                    "off");
    g_assert_cmpstr(clawt_lrg_trigger_state(object_of(on), &tone), ==, "on");
    g_assert_cmpstr(tone, ==, "good");
}

static void
test_trigger_subtitle_and_address(void)
{
    g_autoptr(JsonNode) trigger = parse(
        "{\"agent\":\"kudu\",\"provider\":\"forgejo\","
        "\"agents\":[\"oryx\",\"ibex\"],\"unfinished\":2,"
        "\"listening\":[\"http://100.64.0.1:8787\"]}");
    g_autoptr(JsonNode) plain = parse("{\"agent\":\"kudu\",\"agents\":[]}");
    g_autofree gchar *subtitle = clawt_lrg_trigger_subtitle(
        object_of(trigger));
    g_autofree gchar *simple = clawt_lrg_trigger_subtitle(object_of(plain));
    g_autofree gchar *full = clawt_lrg_trigger_address(
        "abc", json_object_get_array_member(object_of(trigger), "listening"));
    g_autofree gchar *path = clawt_lrg_trigger_address("abc", NULL);

    g_assert_cmpstr(subtitle, ==,
                    "kudu \342\200\224 forgejo (and oryx, ibex) "
                    "\342\200\224 2 runs still going");
    g_assert_cmpstr(simple, ==, "kudu \342\200\224 generic");
    g_assert_cmpstr(full, ==, "http://100.64.0.1:8787/hooks/abc");
    g_assert_cmpstr(path, ==, "/hooks/abc");
    g_assert_null(clawt_lrg_trigger_address(NULL, NULL));
}

static void
test_trigger_secret_body(void)
{
    g_autoptr(JsonNode) once = parse(
        "{\"id\":\"t\",\"secret\":\"s3\",\"secret_shown_once\":true,"
        "\"endpoint\":\"e1\"}");
    g_autoptr(JsonNode) again = parse(
        "{\"id\":\"t\",\"secret\":\"s3\",\"secret_shown_once\":false}");
    g_autoptr(JsonNode) update = parse("{\"id\":\"t\",\"endpoint\":\"e1\"}");
    g_autofree gchar *a = clawt_lrg_trigger_secret_body(object_of(once),
                                                        "Lead.", NULL);
    g_autofree gchar *b = clawt_lrg_trigger_secret_body(object_of(again),
                                                        "Lead.", NULL);

    g_assert_true(g_str_has_prefix(a, "Lead.\n\nThe address: /hooks/e1"));
    g_assert_nonnull(strstr(a, "only time the secret is shown"));

    /* the sentence follows the daemon, not an assumption */
    g_assert_null(strstr(b, "only time"));
    g_assert_null(clawt_lrg_trigger_secret_body(object_of(update), "Lead.",
                                                NULL));
}

static void
test_trigger_report_and_deliveries(void)
{
    g_autoptr(JsonNode) both = parse("{\"report\":\"R\",\"prompt\":\"P\"}");
    g_autoptr(JsonNode) prompt = parse("{\"prompt\":\"P\"}");
    g_autoptr(JsonNode) none = parse("{}");
    g_autoptr(JsonNode) delivery = parse(
        "{\"trigger\":\"push-review\",\"outcome\":\"ran\","
        "\"event\":\"push\",\"detail\":\"task t1\",\"at\":\"1754298000\"}");
    g_autofree gchar *title = clawt_lrg_delivery_title(object_of(delivery));
    g_autofree gchar *subtitle =
        clawt_lrg_delivery_subtitle(object_of(delivery));

    g_assert_cmpstr(clawt_lrg_trigger_report(object_of(both)), ==, "R");
    g_assert_cmpstr(clawt_lrg_trigger_report(object_of(prompt)), ==, "P");
    g_assert_cmpstr(clawt_lrg_trigger_report(object_of(none)), ==, "");
    g_assert_cmpstr(title, ==, "push-review \342\200\224 ran");

    /* 1754298000 is Mon 4 Aug 2025 09:00 UTC */
    g_assert_cmpstr(subtitle, ==,
                    "Mon 4 Aug 09:00 \342\200\224 push \342\200\224 task t1");
}

static void
test_trigger_payload(void)
{
    ClawtLrgTriggerForm form = {
        TRUE, "push-review", "", "Review {{ref}}.", "kudu", " oryx, ,ibex ",
        "", "forgejo", "push, pull_request", "zach/clawtilla", "master", "",
        "", TRUE, FALSE, TRUE, NULL, NULL, 0, NULL
    };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) created = clawt_lrg_trigger_payload(&form, &error);
    g_autoptr(JsonNode) edited = NULL;
    JsonArray *agents;

    g_assert_no_error(error);

    /* a new trigger always starts off: `enabled` is not claimed */
    g_assert_false(json_object_has_member(object_of(created), "enabled"));
    agents = json_object_get_array_member(object_of(created), "agents");
    g_assert_cmpuint(json_array_get_length(agents), ==, 2);
    g_assert_cmpstr(json_array_get_string_element(agents, 1), ==, "ibex");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
                         object_of(created), "events")), ==, 2);

    form.creating = FALSE;
    edited = clawt_lrg_trigger_payload(&form, &error);
    g_assert_true(json_object_get_boolean_member(object_of(edited),
                                                 "enabled"));

    form.provider = "bitbucket";
    g_assert_null(clawt_lrg_trigger_payload(&form, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    form.provider = "generic";
    form.instructions = "\n";
    g_assert_null(clawt_lrg_trigger_payload(&form, &error));
}

/*
 * The mode, cap and conditions go as the daemon reads them, and what the
 * daemon would refuse is refused with the form still open.
 */
static void
test_trigger_payload_mode_cap_and_match(void)
{
    const gchar *paths[] = { "action", "  ", "labels.name", NULL };
    const gchar *values[] = { "opened", "", "deploy", NULL };
    g_autoptr(JsonNode) match = clawt_lrg_match_node(paths, values);
    ClawtLrgTriggerForm form = {
        FALSE, "deploys", "", "Deployed {{repo}}", "kudu", "", "ops",
        "generic", "", "", "", "", "", FALSE, FALSE, TRUE, NULL,
        "post", 7, NULL
    };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) payload = NULL;
    g_autofree gchar *hints = clawt_lrg_trigger_mode_hints();
    g_auto(GStrv) ids = NULL;
    g_auto(GStrv) labels = NULL;
    JsonArray *pairs;

    /* The blank line is no condition; the others keep their order. */
    pairs = json_node_get_array(match);
    g_assert_cmpuint(json_array_get_length(pairs), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(
        json_array_get_object_element(pairs, 1), "path"), ==, "labels.name");

    form.match = match;
    payload = clawt_lrg_trigger_payload(&form, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(json_object_get_string_member(object_of(payload), "mode"),
                    ==, "post");
    g_assert_cmpint(json_object_get_int_member(object_of(payload),
                                               "max_pending"), ==, 7);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
        object_of(payload), "match")), ==, 2);

    /* A post with nowhere to land, a cap out of range, a made-up mode. */
    form.room = " ";
    g_assert_null(clawt_lrg_trigger_payload(&form, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    form.room = "ops";
    form.max_pending = 51;
    g_assert_null(clawt_lrg_trigger_payload(&form, &error));
    g_clear_error(&error);
    form.max_pending = 4;
    form.mode = "sometimes";
    g_assert_null(clawt_lrg_trigger_payload(&form, &error));
    g_clear_error(&error);

    /* The choices and their sentences are the library's. */
    clawt_lrg_trigger_mode_choices(&ids, &labels);
    g_assert_cmpuint(g_strv_length(ids), ==, clawt_trigger_mode_count());
    g_assert_cmpstr(ids[0], ==, "run");
    g_assert_nonnull(strstr(hints, clawt_trigger_mode_nth_hint(1)));
}

/* The check dialog's text says the verdict and each line's result. */
static void
test_match_report(void)
{
    g_autoptr(JsonNode) empty = parse("{\"note\": \"nothing yet\"}");
    g_autoptr(JsonNode) checked = parse(
        "{\"payload\": \"{}\", \"matches\": false,"
        " \"reason\": \"match pair 1 of 1 failed\","
        " \"results\": [{\"path\": \"action\", \"value\": \"opened\","
        " \"found\": true, \"matched\": false, \"actual\": \"closed\"}]}");
    g_autofree gchar *none = clawt_lrg_match_report(object_of(empty));
    g_autofree gchar *report = clawt_lrg_match_report(object_of(checked));

    g_assert_cmpstr(none, ==, "nothing yet");
    g_assert_nonnull(strstr(report, "would have been ignored"));
    g_assert_nonnull(strstr(report, "action contains \"opened\""));
    g_assert_nonnull(strstr(report, "it is \"closed\""));
}

/* ── Tasks ────────────────────────────────────────────────────────── */

static void
test_task_tree_follows_parent(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"tasks\":["
        " {\"id\":\"c\",\"parent\":\"a\",\"state\":\"running\"},"
        " {\"id\":\"a\",\"state\":\"running\"},"
        " {\"id\":\"d\",\"parent\":\"c\",\"state\":\"pending\"},"
        " {\"id\":\"b\",\"state\":\"completed\"},"
        " {\"id\":\"e\",\"parent\":\"gone\",\"state\":\"failed\"},"
        " {\"id\":\"x\",\"parent\":\"y\"},"
        " {\"id\":\"y\",\"parent\":\"x\"}"
        "]}");
    g_autoptr(GPtrArray) rows = clawt_lrg_task_tree(reply);
    static const gchar *const order[] = { "a", "c", "d", "b", "e", "x", "y" };
    static const guint depth[] = { 0, 1, 2, 0, 0, 0, 1 };
    guint i;

    g_assert_cmpuint(rows->len, ==, G_N_ELEMENTS(order));

    for (i = 0; i < rows->len; i++) {
        ClawtLrgTaskRow *row = g_ptr_array_index(rows, i);

        g_assert_cmpstr(clawt_ipc_payload_string(row->task, "id"), ==,
                        order[i]);
        g_assert_cmpuint(row->depth, ==, depth[i]);
    }
}

static void
test_task_tree_empty_and_failed(void)
{
    g_autoptr(JsonNode) empty = parse("{\"tasks\":[]}");
    g_autoptr(JsonNode) wrong = parse("{\"tasks\":\"no\"}");
    g_autoptr(GPtrArray) a = clawt_lrg_task_tree(empty);
    g_autoptr(GPtrArray) b = clawt_lrg_task_tree(wrong);
    g_autoptr(GPtrArray) c = clawt_lrg_task_tree(NULL);

    g_assert_cmpuint(a->len, ==, 0);
    g_assert_cmpuint(b->len, ==, 0);
    g_assert_cmpuint(c->len, ==, 0);
}

static void
test_task_tone_and_cancel(void)
{
    GEnumClass *klass = g_type_class_ref(CLAWT_TYPE_TASK_STATE);
    guint i;

    g_assert_cmpstr(clawt_lrg_task_tone("completed"), ==, "good");
    g_assert_cmpstr(clawt_lrg_task_tone("failed"), ==, "bad");
    g_assert_cmpstr(clawt_lrg_task_tone("stalled"), ==, "warn");
    g_assert_cmpstr(clawt_lrg_task_tone("running"), ==, "info");
    g_assert_cmpstr(clawt_lrg_task_tone("from-the-future"), ==, "neutral");
    g_assert_cmpstr(clawt_lrg_task_tone(NULL), ==, "neutral");

    g_assert_true(clawt_lrg_task_cancellable("running"));
    g_assert_true(clawt_lrg_task_cancellable("pending"));
    g_assert_false(clawt_lrg_task_cancellable("completed"));
    g_assert_false(clawt_lrg_task_cancellable("stalled"));
    g_assert_false(clawt_lrg_task_cancellable("from-the-future"));

    /* every state the enum has gets a tone the theme paints */
    for (i = 0; i < klass->n_values; i++) {
        const gchar *tone = clawt_lrg_task_tone(klass->values[i].value_nick);

        g_assert_true(g_strcmp0(tone, "good") == 0 ||
                      g_strcmp0(tone, "bad") == 0 ||
                      g_strcmp0(tone, "warn") == 0 ||
                      g_strcmp0(tone, "info") == 0 ||
                      g_strcmp0(tone, "neutral") == 0);
    }

    g_type_class_unref(klass);
}

static void
test_task_title_and_note(void)
{
    g_autoptr(JsonNode) progress = parse(
        "{\"origin\":\"chief\",\"assignee\":\"kudu\","
        "\"progress_note\":\"halfway\",\"result_inferred\":true}");
    g_autoptr(JsonNode) inferred = parse(
        "{\"origin\":\"chief\",\"assignee\":\"kudu\","
        "\"result_inferred\":true}");
    g_autoptr(JsonNode) plain = parse("{\"state\":\"completed\"}");
    g_autofree gchar *title = clawt_lrg_task_title(object_of(progress));
    g_autofree gchar *unknown = clawt_lrg_task_title(object_of(plain));

    g_assert_cmpstr(title, ==, "chief \342\206\222 kudu");
    g_assert_cmpstr(unknown, ==, "? \342\206\222 ?");
    g_assert_cmpstr(clawt_lrg_task_note(object_of(progress)), ==, "halfway");
    g_assert_nonnull(strstr(clawt_lrg_task_note(object_of(inferred)),
                            "nobody reported this finished"));
    g_assert_null(clawt_lrg_task_note(object_of(plain)));
}

/* ── Decisions ────────────────────────────────────────────────────── */

static void
test_decision_items(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"decisions\":[{\"id\":\"d1\"},{\"id\":\"d2\"}],\"open\":2}");
    g_autoptr(JsonNode) empty = parse("{}");
    guint open = 99;
    JsonArray *items = clawt_lrg_decision_items(reply, &open);

    g_assert_cmpuint(json_array_get_length(items), ==, 2);
    g_assert_cmpuint(open, ==, 2);
    g_assert_null(clawt_lrg_decision_items(empty, &open));
    g_assert_cmpuint(open, ==, 0);
    g_assert_null(clawt_lrg_decision_items(NULL, &open));
}

static void
test_decision_going_options_meta(void)
{
    g_autoptr(JsonNode) policy = parse(
        "{\"id\":\"policy-1\",\"agent\":\"kudu\","
        "\"question\":\"kudu wants to run: rm -rf build\\n\\nThe action rule "
        "'deletes' asks first.\","
        "\"options\":[\"approve\",\"\",\"deny\"],\"default\":\"deny\","
        "\"default_reason\":\"nobody answered in time\","
        "\"created_at\":1000,\"urgent\":true}");
    g_autoptr(JsonNode) bare = parse(
        "{\"agent\":\"kudu\",\"options\":[],\"default\":\"\"}");
    g_autofree gchar *going = clawt_lrg_decision_going(object_of(policy));
    g_auto(GStrv) options = clawt_lrg_decision_options(object_of(policy));
    g_auto(GStrv) none = clawt_lrg_decision_options(object_of(bare));
    g_autofree gchar *meta = NULL;
    g_autofree gchar *meta_bare = NULL;

    g_assert_cmpstr(going, ==,
                    "Going ahead with: deny \342\200\224 nobody answered in "
                    "time");
    g_assert_null(clawt_lrg_decision_going(object_of(bare)));
    g_assert_cmpuint(g_strv_length(options), ==, 2);
    g_assert_cmpstr(options[1], ==, "deny");
    g_assert_cmpuint(g_strv_length(none), ==, 0);

    /* created_at is seconds: five minutes later is "5m ago", not 20694d */
    meta = clawt_lrg_decision_meta(object_of(policy),
                                   (gint64)(1000 + 300) * G_USEC_PER_SEC);
    g_assert_cmpstr(meta, ==, "asked by kudu \302\267 5m ago");
    meta_bare = clawt_lrg_decision_meta(object_of(bare), 0);
    g_assert_cmpstr(meta_bare, ==, "asked by kudu");
}

static void
test_decision_answer_typed_wins(void)
{
    g_assert_cmpstr(clawt_lrg_decision_answer("approve", "neither, do X"), ==,
                    "neither, do X");
    g_assert_cmpstr(clawt_lrg_decision_answer("approve", "  "), ==,
                    "approve");
    g_assert_cmpstr(clawt_lrg_decision_answer(NULL, "typed"), ==, "typed");
    g_assert_null(clawt_lrg_decision_answer(NULL, ""));
    g_assert_null(clawt_lrg_decision_answer("", NULL));
}

/* ── Go to… ───────────────────────────────────────────────────────── */

static void
test_nav_targets(void)
{
    g_autoptr(JsonNode) agents = parse(
        "{\"agents\":[{\"id\":\"kudu\",\"name\":\"Kudu\",\"team\":\"ops\"},"
        "{\"id\":\"oryx\",\"name\":\"Oryx\"}]}");
    g_autoptr(JsonNode) rooms = parse(
        "{\"rooms\":[{\"id\":\"war-room\",\"name\":\"War room\","
        "\"declared\":true},{\"id\":\"dm:kudu:oryx\",\"declared\":false}]}");
    g_autoptr(GPtrArray) with_agent = clawt_lrg_nav_targets(agents, rooms,
                                                            TRUE);
    g_autoptr(GPtrArray) without = clawt_lrg_nav_targets(agents, rooms,
                                                         FALSE);
    ClawtLrgNavTarget *first = g_ptr_array_index(with_agent, 0);
    ClawtLrgNavTarget *room = g_ptr_array_index(with_agent, 2);
    ClawtLrgNavTarget *page = g_ptr_array_index(without, 3);

    /* two agents, one declared room, then every page */
    g_assert_cmpuint(with_agent->len, ==, 3 + clawt_page_count());
    g_assert_cmpint(first->kind, ==, CLAWT_LRG_NAV_AGENT);
    g_assert_cmpstr(first->name, ==, "Kudu");
    g_assert_cmpstr(first->target, ==, "kudu");
    g_assert_cmpint(room->kind, ==, CLAWT_LRG_NAV_ROOM);
    g_assert_cmpstr(room->target, ==, "war-room");

    /* no agent selected: only the page that needs none */
    g_assert_cmpuint(without->len, ==, 4);
    g_assert_cmpint(page->kind, ==, CLAWT_LRG_NAV_PAGE);
    g_assert_cmpstr(page->target, ==, clawt_page_nick(CLAWT_PAGE_MEMORY));
}

static void
test_nav_filter_and_move(void)
{
    g_autoptr(JsonNode) agents = parse(
        "{\"agents\":[{\"id\":\"kudu\",\"name\":\"Kudu\",\"team\":\"ops\"},"
        "{\"id\":\"oryx\",\"name\":\"Oryx\",\"team\":\"research\"}]}");
    g_autoptr(GPtrArray) targets = clawt_lrg_nav_targets(agents, NULL, TRUE);
    g_autoptr(GArray) all = clawt_lrg_nav_filter(targets, "");
    g_autoptr(GArray) ops = clawt_lrg_nav_filter(targets, "ops");
    g_autoptr(GArray) nothing = clawt_lrg_nav_filter(targets, "zzzz");

    g_assert_cmpuint(all->len, ==, targets->len);
    g_assert_cmpuint(ops->len, ==, 1);
    g_assert_cmpuint(g_array_index(ops, guint, 0), ==, 0);
    g_assert_cmpuint(nothing->len, ==, 0);

    g_assert_cmpuint(clawt_lrg_nav_move(0, -1, 5), ==, 0);
    g_assert_cmpuint(clawt_lrg_nav_move(0, 1, 5), ==, 1);
    g_assert_cmpuint(clawt_lrg_nav_move(4, 1, 5), ==, 4);
    g_assert_cmpuint(clawt_lrg_nav_move(9, -1, 5), ==, 3);
    g_assert_cmpuint(clawt_lrg_nav_move(3, 1, 0), ==, 0);
}

int
main(int argc, char *argv[])
{
    /* local time is part of what is checked; pin it before GLib caches it */
    g_setenv("TZ", "UTC", TRUE);
    g_setenv("LC_ALL", "C", TRUE);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/lrg-work/schema/extra-fields",
                    test_extra_fields_walk_schema);
    g_test_add_func("/lrg-work/schema/no-secret",
                    test_extra_fields_never_offer_secret);
    g_test_add_func("/lrg-work/schema/boolean",
                    test_boolean_reads_booleans_with_schema_default);
    g_test_add_func("/lrg-work/schema/first-paragraph",
                    test_first_paragraph_rejoins);
    g_test_add_func("/lrg-work/schema/field-text-value",
                    test_field_text_and_value);
    g_test_add_func("/lrg-work/schema/agent-choices", test_agent_choices);
    g_test_add_func("/lrg-work/routine/schedule-choices",
                    test_schedule_choices_walk_enum);
    g_test_add_func("/lrg-work/routine/weekday-choices",
                    test_weekday_choices);
    g_test_add_func("/lrg-work/routine/schedule-uses", test_schedule_uses);
    g_test_add_func("/lrg-work/routine/parse-at", test_parse_at);
    g_test_add_func("/lrg-work/routine/preview", test_schedule_preview);
    g_test_add_func("/lrg-work/routine/when", test_routine_when);
    g_test_add_func("/lrg-work/routine/title-last-run",
                    test_routine_title_and_last_run);
    g_test_add_func("/lrg-work/routine/payload", test_routine_payload);
    g_test_add_func("/lrg-work/routine/refusals",
                    test_routine_payload_refusals);
    g_test_add_func("/lrg-work/trigger/providers",
                    test_provider_choices_walk_library);
    g_test_add_func("/lrg-work/trigger/state", test_trigger_state_priority);
    g_test_add_func("/lrg-work/trigger/subtitle-address",
                    test_trigger_subtitle_and_address);
    g_test_add_func("/lrg-work/trigger/secret", test_trigger_secret_body);
    g_test_add_func("/lrg-work/trigger/report-deliveries",
                    test_trigger_report_and_deliveries);
    g_test_add_func("/lrg-work/trigger/payload", test_trigger_payload);
    g_test_add_func("/lrg-work/task/tree", test_task_tree_follows_parent);
    g_test_add_func("/lrg-work/task/tree-empty",
                    test_task_tree_empty_and_failed);
    g_test_add_func("/lrg-work/task/tone-cancel", test_task_tone_and_cancel);
    g_test_add_func("/lrg-work/task/title-note", test_task_title_and_note);
    g_test_add_func("/lrg-work/decision/items", test_decision_items);
    g_test_add_func("/lrg-work/decision/going-options-meta",
                    test_decision_going_options_meta);
    g_test_add_func("/lrg-work/decision/answer",
                    test_decision_answer_typed_wins);
    g_test_add_func("/lrg-work/trigger/mode-cap-match",
                    test_trigger_payload_mode_cap_and_match);
    g_test_add_func("/lrg-work/trigger/match-report", test_match_report);
    g_test_add_func("/lrg-work/nav/targets", test_nav_targets);
    g_test_add_func("/lrg-work/nav/filter-move", test_nav_filter_and_move);

    return g_test_run();
}
