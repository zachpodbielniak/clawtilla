/*
 * clawt-provider-usage.c - What each provider login has left of its plan
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <math.h>
#include <string.h>

/*
 * A member that is present *and* a number.  ai-glib writes every key on
 * every entry and sets the unknown ones to JSON null, so "has the
 * member" is not "knows the value" -- json_object_get_double_member()
 * on a null answers 0, which is the exact lie the report's schema exists
 * to prevent.
 */
static gboolean
member_number(JsonObject *object, const gchar *name, gdouble *out)
{
    JsonNode *node;
    GType type;

    if (object == NULL || !json_object_has_member(object, name))
        return FALSE;
    node = json_object_get_member(object, name);
    if (node == NULL || JSON_NODE_TYPE(node) != JSON_NODE_VALUE)
        return FALSE;

    type = json_node_get_value_type(node);
    if (type == G_TYPE_INT64) {
        if (out != NULL)
            *out = (gdouble)json_node_get_int(node);
        return TRUE;
    }
    if (type == G_TYPE_DOUBLE) {
        gdouble value = json_node_get_double(node);

        if (!isfinite(value))
            return FALSE;
        if (out != NULL)
            *out = value;
        return TRUE;
    }
    return FALSE;
}

/* A string member that is present, a string and not empty. */
static const gchar *
member_text(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;
    node = json_object_get_member(object, name);
    if (node == NULL || JSON_NODE_TYPE(node) != JSON_NODE_VALUE ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;
    if (json_node_get_string(node) == NULL || *json_node_get_string(node) == '\0')
        return NULL;
    return json_node_get_string(node);
}

static gboolean
member_bool(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return FALSE;
    node = json_object_get_member(object, name);
    return node != NULL && JSON_NODE_TYPE(node) == JSON_NODE_VALUE &&
           json_node_get_value_type(node) == G_TYPE_BOOLEAN &&
           json_node_get_boolean(node);
}

/*
 * An exact instant, or nothing.  The report's dates may be date-only or
 * month-only ("2026-12" for a billing period); those are not instants,
 * so they become the window's reset text rather than midnight UTC,
 * which in most timezones is the wrong day.
 */
static gboolean
parse_instant(const gchar *text, gint64 *out)
{
    g_autoptr(GDateTime) when = NULL;

    if (text == NULL || strchr(text, 'T') == NULL)
        return FALSE;
    when = g_date_time_new_from_iso8601(text, NULL);
    if (when == NULL)
        return FALSE;
    *out = g_date_time_to_unix(when);
    return TRUE;
}

static gdouble
clamp_percent(gdouble value)
{
    return CLAMP(value, 0.0, 100.0);
}

/*
 * One report entry -> one window.  The direction rule lives here and
 * nowhere else: remaining_percent when the source said "left", 100 minus
 * used_percent when it said "used", and remaining/limit when it gave
 * amounts in a unit.  A bare `percent` keeps its own member, uncoloured.
 */
static void
add_window(JsonBuilder *builder, JsonObject *entry)
{
    const gchar *label = member_text(entry, "label");
    const gchar *unit = member_text(entry, "unit");
    const gchar *reset_text = member_text(entry, "reset_text");
    const gchar *reset_at = member_text(entry, "reset_at");
    const gchar *model = member_text(entry, "model");
    gdouble value = 0.0;
    gdouble used = 0.0;
    gdouble limit = 0.0;
    gdouble remaining = 0.0;
    gboolean has_used = member_number(entry, "used", &used);
    gboolean has_limit = member_number(entry, "limit", &limit);
    gboolean has_remaining = member_number(entry, "remaining", &remaining);
    gboolean has_left = FALSE;
    gdouble left = 0.0;
    gint64 instant = 0;

    if (member_number(entry, "remaining_percent", &value)) {
        has_left = TRUE;
        left = clamp_percent(value);
    } else if (member_number(entry, "used_percent", &value)) {
        has_left = TRUE;
        left = clamp_percent(100.0 - value);
    } else if (has_limit && limit > 0.0 && (has_remaining || has_used)) {
        has_left = TRUE;
        left = clamp_percent(100.0 * (has_remaining ? remaining : limit - used) / limit);
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "label");
    json_builder_add_string_value(builder, label != NULL ? label : "Allowance");
    json_builder_set_member_name(builder, "unit");
    json_builder_add_string_value(builder, unit != NULL ? unit : "percent");

    if (has_left) {
        json_builder_set_member_name(builder, "remaining_percent");
        json_builder_add_double_value(builder, left);
    } else if (member_number(entry, "percent", &value)) {
        json_builder_set_member_name(builder, "percent");
        json_builder_add_double_value(builder, clamp_percent(value));
    }

    if (has_used) {
        json_builder_set_member_name(builder, "used");
        json_builder_add_double_value(builder, used);
    }
    if (has_limit) {
        json_builder_set_member_name(builder, "limit");
        json_builder_add_double_value(builder, limit);
    }
    if (has_remaining) {
        json_builder_set_member_name(builder, "remaining");
        json_builder_add_double_value(builder, remaining);
    }
    if (member_number(entry, "window_minutes", &value)) {
        json_builder_set_member_name(builder, "window_minutes");
        json_builder_add_int_value(builder, (gint64)value);
    }
    if (member_number(entry, "cost_usd", &value)) {
        json_builder_set_member_name(builder, "cost_usd");
        json_builder_add_double_value(builder, value);
    }
    if (model != NULL) {
        json_builder_set_member_name(builder, "model");
        json_builder_add_string_value(builder, model);
    }

    /* an exact instant, else the source's own words, else whatever date it gave */
    if (parse_instant(reset_at, &instant)) {
        json_builder_set_member_name(builder, "reset_at");
        json_builder_add_int_value(builder, instant);
    }
    if (reset_text != NULL) {
        json_builder_set_member_name(builder, "reset_text");
        json_builder_add_string_value(builder, reset_text);
    } else if (reset_at != NULL && instant == 0) {
        json_builder_set_member_name(builder, "reset_text");
        json_builder_add_string_value(builder, reset_at);
    }
    json_builder_end_object(builder);
}

JsonNode *
clawt_provider_usage_normalize(JsonNode *report)
{
    g_autoptr(JsonBuilder) builder = NULL;
    JsonObject *root;
    JsonArray *entries;
    const gchar *text;
    gdouble version = 0.0;
    gint64 collected = 0;
    guint i;

    if (report == NULL || !JSON_NODE_HOLDS_OBJECT(report))
        return NULL;
    root = json_node_get_object(report);
    if (root == NULL)
        return NULL;

    /* A later schema may mean something else by the same names. */
    if (!member_number(root, "schema_version", &version) || version != 1.0)
        return NULL;
    if (g_strcmp0(member_text(root, "kind"), "usage") != 0)
        return NULL;

    builder = json_builder_new();
    json_builder_begin_object(builder);

    if ((text = member_text(root, "plan")) != NULL) {
        json_builder_set_member_name(builder, "plan");
        json_builder_add_string_value(builder, text);
    }
    if ((text = member_text(root, "source")) != NULL) {
        json_builder_set_member_name(builder, "source");
        json_builder_add_string_value(builder, text);
    }
    if ((text = member_text(root, "detail")) != NULL) {
        json_builder_set_member_name(builder, "detail");
        json_builder_add_string_value(builder, text);
    }
    json_builder_set_member_name(builder, "partial");
    json_builder_add_boolean_value(builder,
        g_strcmp0(member_text(root, "availability"), "partial") == 0);
    if (parse_instant(member_text(root, "collected_at"), &collected)) {
        json_builder_set_member_name(builder, "collected_at");
        json_builder_add_int_value(builder, collected);
    }

    json_builder_set_member_name(builder, "windows");
    json_builder_begin_array(builder);
    entries = json_object_has_member(root, "entries") &&
              JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "entries"))
                  ? json_object_get_array_member(root, "entries")
                  : NULL;
    for (i = 0; entries != NULL && i < json_array_get_length(entries); i++) {
        JsonNode *element = json_array_get_element(entries, i);

        if (element != NULL && JSON_NODE_HOLDS_OBJECT(element) &&
            json_node_get_object(element) != NULL)
            add_window(builder, json_node_get_object(element));
    }
    json_builder_end_array(builder);

    json_builder_end_object(builder);
    return json_builder_get_root(builder);
}

gboolean
clawt_usage_window_remaining(JsonObject *window, gdouble *percent)
{
    gdouble value = 0.0;

    if (!member_number(window, "remaining_percent", &value))
        return FALSE;
    if (percent != NULL)
        *percent = clamp_percent(value);
    return TRUE;
}

ClawtAllowanceLevel
clawt_usage_window_level(JsonObject *window)
{
    gdouble left = 0.0;

    if (!clawt_usage_window_remaining(window, &left))
        return CLAWT_ALLOWANCE_UNKNOWN;
    if (left <= (gdouble)CLAWT_ALLOWANCE_CRITICAL_PERCENT)
        return CLAWT_ALLOWANCE_CRITICAL;
    if (left <= (gdouble)CLAWT_ALLOWANCE_LOW_PERCENT)
        return CLAWT_ALLOWANCE_LOW;
    return CLAWT_ALLOWANCE_PLENTY;
}

const gchar *
clawt_allowance_level_tone(ClawtAllowanceLevel level)
{
    switch (level) {
    case CLAWT_ALLOWANCE_UNKNOWN:
        return "neutral";
    case CLAWT_ALLOWANCE_PLENTY:
        return "good";
    case CLAWT_ALLOWANCE_LOW:
        return "warn";
    case CLAWT_ALLOWANCE_CRITICAL:
        return "bad";
    }
    return "neutral";
}

/* "1,234,567" -- token counts read badly as a wall of digits. */
static gchar *
grouped(gdouble value)
{
    g_autofree gchar *digits = g_strdup_printf("%.0f", fabs(value));
    GString *out = g_string_new(value < 0 ? "-" : "");
    gsize len = strlen(digits);
    gsize i;

    for (i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            g_string_append_c(out, ',');
        g_string_append_c(out, digits[i]);
    }
    return g_string_free(out, FALSE);
}

/* An amount in the window's own unit. */
static gchar *
amount(const gchar *unit, gdouble value)
{
    if (g_strcmp0(unit, "USD") == 0)
        return g_strdup_printf("$%.2f", value);
    if (g_strcmp0(unit, "tokens") == 0) {
        g_autofree gchar *n = grouped(value);

        return g_strdup_printf("%s tokens", n);
    }
    if (g_strcmp0(unit, "percent") == 0)
        return g_strdup_printf("%.0f%%", value);
    {
        g_autofree gchar *n = grouped(value);

        return unit != NULL ? g_strdup_printf("%s %s", n, unit) : g_steal_pointer(&n);
    }
}

gchar *
clawt_usage_window_value_label(JsonObject *window)
{
    const gchar *unit = member_text(window, "unit");
    gdouble left = 0.0;
    gdouble value = 0.0;
    gdouble limit = 0.0;
    gboolean has_limit = member_number(window, "limit", &limit);

    /* money and token windows read better as amounts than as a share */
    if (g_strcmp0(unit, "percent") != 0 && has_limit) {
        g_autofree gchar *whole = amount(unit, limit);

        if (member_number(window, "remaining", &value)) {
            g_autofree gchar *part = amount(unit, value);

            return g_strdup_printf("%s of %s left", part, whole);
        }
        if (member_number(window, "used", &value)) {
            g_autofree gchar *part = amount(unit, value);

            return g_strdup_printf("%s of %s used", part, whole);
        }
    }

    if (clawt_usage_window_remaining(window, &left))
        return g_strdup_printf("%.0f%% left", floor(left));

    if (member_number(window, "percent", &value))
        return g_strdup_printf("%.0f%% (direction not reported)", value);

    if (member_number(window, "used", &value)) {
        g_autofree gchar *part = amount(unit, value);

        return g_strdup_printf("%s used", part);
    }
    if (member_number(window, "remaining", &value)) {
        g_autofree gchar *part = amount(unit, value);

        return g_strdup_printf("%s left", part);
    }
    if (member_number(window, "cost_usd", &value))
        return g_strdup_printf("$%.2f spent", value);

    return g_strdup("Not reported");
}

gchar *
clawt_usage_window_reset_label(JsonObject *window, gint64 now)
{
    const gchar *text = member_text(window, "reset_text");
    gdouble at = 0.0;
    gint64 delta;

    if (!member_number(window, "reset_at", &at))
        return text != NULL ? g_strdup_printf("resets %s", text) : NULL;

    delta = (gint64)at - now;
    if (delta <= 0)
        return g_strdup("resetting now");

    if (delta < 24 * 3600) {
        gint64 hours = delta / 3600;
        gint64 minutes = (delta % 3600) / 60;

        if (hours == 0)
            return g_strdup_printf("resets in %" G_GINT64_FORMAT "m",
                                   MAX(minutes, (gint64)1));
        return g_strdup_printf("resets in %" G_GINT64_FORMAT "h %" G_GINT64_FORMAT "m",
                               hours, minutes);
    }

    {
        g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc((gint64)at);
        g_autoptr(GDateTime) local = utc != NULL ? g_date_time_to_local(utc) : NULL;
        g_autofree gchar *when = NULL;

        if (local == NULL)
            return text != NULL ? g_strdup_printf("resets %s", text) : NULL;
        /* %-d, never %e: GLib pads %e with a figure space */
        when = g_date_time_format(local, delta < 7 * 24 * 3600 ? "%a %H:%M" : "%-d %b");
        return g_strdup_printf("resets %s", when);
    }
}

gchar *
clawt_usage_meter_text(gdouble remaining_percent, guint width)
{
    GString *out;
    guint filled;
    guint i;

    if (width == 0)
        width = 20;
    remaining_percent = clamp_percent(remaining_percent);
    filled = (guint)floor(remaining_percent * (gdouble)width / 100.0 + 0.5);

    out = g_string_new("\xe2\x96\x95");  /* U+2595 RIGHT ONE EIGHTH BLOCK */
    for (i = 0; i < width; i++)
        g_string_append(out, i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91");
    g_string_append(out, "\xe2\x96\x8f");  /* U+258F LEFT ONE EIGHTH BLOCK */
    return g_string_free(out, FALSE);
}

gchar *
clawt_usage_login_title(JsonObject *login)
{
    const gchar *provider = member_text(login, "provider");
    const gchar *account = member_text(login, "account");
    const gchar *plan = member_text(login, "plan");
    const ClawtProviderInfo *info = clawt_model_catalog_find_provider(provider);
    GString *out = g_string_new(info != NULL ? info->label
                                : provider != NULL ? provider : "Unknown provider");

    g_string_append(out, " \xe2\x80\x94 ");  /* em dash */
    g_string_append(out, account != NULL ? account : "default login");
    if (plan != NULL)
        g_string_append_printf(out, " (%s)", plan);
    return g_string_free(out, FALSE);
}

gchar *
clawt_usage_login_status_label(JsonObject *login, gint64 now)
{
    const gchar *state = member_text(login, "state");
    const gchar *message = member_text(login, "message");
    gdouble read_at = 0.0;

    if (g_strcmp0(state, "unsupported") == 0)
        return g_strdup_printf("Reports no plan limits%s%s",
                               message != NULL ? ": " : "",
                               message != NULL ? message : "");
    if (g_strcmp0(state, "error") == 0)
        return g_strdup_printf("Could not be read: %s",
                               message != NULL ? message : "no reason given");
    if (g_strcmp0(state, "pending") == 0 &&
        !member_number(login, "read_at", &read_at))
        return g_strdup("Asking the provider\xe2\x80\xa6");
    if (!member_number(login, "read_at", &read_at))
        return g_strdup("Not read yet");

    {
        g_autofree gchar *ago = clawt_time_ago_label((gint64)read_at * G_USEC_PER_SEC,
                                                     now * G_USEC_PER_SEC);
        const gchar *suffix = g_strcmp0(state, "pending") == 0 ? ", refreshing" : "";

        if (member_bool(login, "partial"))
            return g_strdup_printf("Read %s from the CLI's own panel; partial%s",
                                   ago, suffix);
        return g_strdup_printf("Read %s%s", ago, suffix);
    }
}

ClawtAllowanceLevel
clawt_usage_login_level(JsonObject *login)
{
    JsonArray *windows;
    ClawtAllowanceLevel worst = CLAWT_ALLOWANCE_UNKNOWN;
    guint i;

    if (login == NULL || !json_object_has_member(login, "windows") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(login, "windows")))
        return CLAWT_ALLOWANCE_UNKNOWN;
    windows = json_object_get_array_member(login, "windows");

    /* ranked by worry, not by the enum's order: UNKNOWN is the least */
    for (i = 0; i < json_array_get_length(windows); i++) {
        JsonNode *node = json_array_get_element(windows, i);
        ClawtAllowanceLevel level;

        if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
            continue;
        level = clawt_usage_window_level(json_node_get_object(node));
        switch (level) {
        case CLAWT_ALLOWANCE_CRITICAL:
            return CLAWT_ALLOWANCE_CRITICAL;
        case CLAWT_ALLOWANCE_LOW:
            worst = CLAWT_ALLOWANCE_LOW;
            break;
        case CLAWT_ALLOWANCE_PLENTY:
            if (worst == CLAWT_ALLOWANCE_UNKNOWN)
                worst = CLAWT_ALLOWANCE_PLENTY;
            break;
        case CLAWT_ALLOWANCE_UNKNOWN:
            break;
        }
    }
    return worst;
}
