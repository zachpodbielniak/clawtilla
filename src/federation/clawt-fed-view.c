/*
 * clawt-fed-view.c - How a linked fleet is worded, in every client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-view.h"

static const gchar *
str(
    JsonObject  *o,
    const gchar *name
){
    JsonNode *n = o != NULL ? json_object_get_member(o, name) : NULL;

    if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(n);
}

static gint64
num(
    JsonObject  *o,
    const gchar *name
){
    JsonNode *n = o != NULL ? json_object_get_member(o, name) : NULL;

    if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64)
        return 0;
    return json_node_get_int(n);
}

gchar *
clawt_fed_peer_title(
    JsonObject *peer
){
    const gchar *alias = str(peer, "alias");
    const gchar *label = str(peer, "label");

    if (label == NULL || *label == '\0' || g_strcmp0(label, alias) == 0)
        return g_strdup(alias != NULL ? alias : "?");
    return g_strdup_printf("%s (%s)", label, alias);
}

gchar *
clawt_fed_peer_summary(
    JsonObject *peer
){
    GString *out = g_string_new(NULL);
    ClawtFedTier tier;
    JsonNode *tags_node;
    const gchar *owner = str(peer, "owner");
    const gchar *status = str(peer, "status");
    ClawtFedHealth health;

    if (g_strcmp0(status, "pending") == 0)
        return g_strdup("Waiting for your approval");
    if (g_strcmp0(status, "revoked") == 0)
        return g_strdup("Revoked: this link is over");

    if (clawt_fed_tier_from_nick(str(peer, "tier"), &tier))
        g_string_append(out, clawt_fed_tier_label(tier));
    if (owner != NULL && *owner != '\0')
        g_string_append_printf(out, " \302\267 %s", owner);
    tags_node = peer != NULL ? json_object_get_member(peer, "tags") : NULL;
    if (tags_node != NULL && JSON_NODE_HOLDS_ARRAY(tags_node)) {
        JsonArray *tags = json_node_get_array(tags_node);
        guint i;
        for (i = 0; tags != NULL && i < json_array_get_length(tags); i++)
            g_string_append_printf(out, "%s#%s", i == 0 ? " \302\267 " : " ",
                                   json_array_get_string_element(tags, i));
    }
    if (clawt_fed_health_from_nick(str(peer, "health"), &health))
        g_string_append_printf(out, " \302\267 %s", clawt_fed_health_label(health));
    if (num(peer, "latency_ms") > 0)
        g_string_append_printf(out, " \302\267 %" G_GINT64_FORMAT " ms", num(peer, "latency_ms"));
    return g_string_free(out, FALSE);
}

const gchar *
clawt_fed_health_glyph(
    const gchar *health
){
    ClawtFedHealth h;

    if (!clawt_fed_health_from_nick(health, &h))
        return "\342\227\214";  /* ◌ */
    switch (h) {
    case CLAWT_FED_HEALTH_LIVE:         return "\342\227\217";  /* ● */
    case CLAWT_FED_HEALTH_DEGRADED:
    case CLAWT_FED_HEALTH_CONNECTING:   return "\342\227\220";  /* ◐ */
    case CLAWT_FED_HEALTH_OFFLINE:
    case CLAWT_FED_HEALTH_INCOMPATIBLE: return "\342\227\213";  /* ○ */
    case CLAWT_FED_HEALTH_REVOKED:      return "\342\234\225";  /* ✕ */
    case CLAWT_FED_HEALTH_PENDING:      return "\342\227\214";  /* ◌ */
    }
    return "\342\227\214";
}

gchar *
clawt_fed_audit_line(
    JsonObject *entry,
    gint64      now
){
    g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(num(entry, "time"));
    g_autofree gchar *stamp = NULL;
    const gchar *detail = str(entry, "detail");

    if (when != NULL)
        stamp = g_date_time_format(when, now - num(entry, "time") > 86400 ? "%F %H:%M" : "%H:%M");
    return g_strdup_printf("%s  %s %s %s %s%s%s", stamp != NULL ? stamp : "?",
                           g_strcmp0(str(entry, "direction"), "in") == 0 ? "\342\206\220"
                           : g_strcmp0(str(entry, "direction"), "out") == 0 ? "\342\206\222"
                                                                            : "\302\267",
                           str(entry, "peer") != NULL && *str(entry, "peer") != '\0'
                               ? str(entry, "peer") : "this fleet",
                           str(entry, "kind") != NULL ? str(entry, "kind") : "",
                           str(entry, "outcome") != NULL ? str(entry, "outcome") : "",
                           detail != NULL && *detail != '\0' ? " \342\200\224 " : "",
                           detail != NULL ? detail : "");
}
