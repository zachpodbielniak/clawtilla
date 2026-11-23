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

#include <string.h>

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

void
clawt_fed_chip_free(
    ClawtFedChip *chip
){
    if (chip == NULL)
        return;
    g_free(chip->kind);
    g_free(chip->label);
    g_free(chip->term);
    g_free(chip);
}

static void
add_chip(
    GPtrArray   *out,
    GHashTable  *seen,
    const gchar *kind,
    const gchar *label,
    const gchar *term
){
    ClawtFedChip *chip;

    if (g_hash_table_contains(seen, term))
        return;
    g_hash_table_add(seen, g_strdup(term));
    chip = g_new0(ClawtFedChip, 1);
    chip->kind = g_strdup(kind);
    chip->label = g_strdup(label);
    chip->term = g_strdup(term);
    g_ptr_array_add(out, chip);
}

static gint
by_label(
    gconstpointer a,
    gconstpointer b
){
    const ClawtFedChip *ca = *(ClawtFedChip * const *)a;
    const ClawtFedChip *cb = *(ClawtFedChip * const *)b;

    return g_utf8_collate(ca->label, cb->label);
}

GPtrArray *
clawt_fed_filter_chips(
    JsonArray *fleets
){
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_fed_chip_free);
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GPtrArray) owners = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_fed_chip_free);
    g_autoptr(GPtrArray) tags = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_fed_chip_free);
    gboolean any_online = FALSE;
    guint n = fleets != NULL ? json_array_get_length(fleets) : 0;
    guint t;
    guint i;

    for (i = 0; i < n; i++) {
        JsonObject *f = json_array_get_object_element(fleets, i);
        if (f != NULL && clawt_fed_selector_matches("online", f))
            any_online = TRUE;
    }
    if (any_online)
        add_chip(out, seen, "online", "Online", "online");

    /* Tiers in the enum's order, only those present. */
    for (t = 0; t < clawt_fed_tier_count(); t++) {
        ClawtFedTier tier = clawt_fed_tier_nth(t);
        g_autofree gchar *term = g_strdup_printf("tier:%s", clawt_fed_tier_nick(tier));
        for (i = 0; i < n; i++) {
            JsonObject *f = json_array_get_object_element(fleets, i);
            if (g_strcmp0(str(f, "tier"), clawt_fed_tier_nick(tier)) == 0) {
                add_chip(out, seen, "tier", clawt_fed_tier_label(tier), term);
                break;
            }
        }
    }

    for (i = 0; i < n; i++) {
        JsonObject *f = json_array_get_object_element(fleets, i);
        const gchar *owner = str(f, "owner");
        JsonNode *tags_node = f != NULL ? json_object_get_member(f, "tags") : NULL;

        /* An owner with a selector character cannot be a glob-safe term. */
        if (owner != NULL && *owner != '\0' && strpbrk(owner, ",+!*?") == NULL) {
            g_autofree gchar *term = g_strdup_printf("owner:%s", owner);
            add_chip(owners, seen, "owner", owner, term);
        }
        if (tags_node != NULL && JSON_NODE_HOLDS_ARRAY(tags_node)) {
            JsonArray *list = json_node_get_array(tags_node);
            guint j;
            for (j = 0; list != NULL && j < json_array_get_length(list); j++) {
                const gchar *tag = json_array_get_string_element(list, j);
                g_autofree gchar *term = g_strdup_printf("tag:%s", tag);
                g_autofree gchar *label = g_strdup_printf("#%s", tag);
                add_chip(tags, seen, "tag", label, term);
            }
        }
    }
    g_ptr_array_sort(owners, by_label);
    g_ptr_array_sort(tags, by_label);
    while (owners->len > 0)
        g_ptr_array_add(out, g_ptr_array_steal_index(owners, 0));
    while (tags->len > 0)
        g_ptr_array_add(out, g_ptr_array_steal_index(tags, 0));
    return out;
}

gboolean
clawt_fed_filter_matches(
    JsonObject  *fleet,
    gchar      **terms,
    gboolean     show_hidden
){
    static const gchar *kinds[] = { "online", "tier:", "owner:", "tag:" };
    JsonNode *hidden = fleet != NULL ? json_object_get_member(fleet, "hidden") : NULL;
    guint k;
    guint i;

    if (fleet == NULL)
        return FALSE;
    if (!show_hidden && hidden != NULL && JSON_NODE_HOLDS_VALUE(hidden)
        && json_node_get_value_type(hidden) == G_TYPE_BOOLEAN && json_node_get_boolean(hidden))
        return FALSE;
    /* Within a kind any term may match; every kind with a term must. */
    for (k = 0; k < G_N_ELEMENTS(kinds); k++) {
        gboolean asked = FALSE;
        gboolean hit = FALSE;
        for (i = 0; terms != NULL && terms[i] != NULL; i++) {
            if (!g_str_has_prefix(terms[i], kinds[k]))
                continue;
            asked = TRUE;
            if (clawt_fed_selector_matches(terms[i], fleet))
                hit = TRUE;
        }
        if (asked && !hit)
            return FALSE;
    }
    return TRUE;
}

const gchar *
clawt_fed_agent_presence(
    JsonObject   *agent,
    const gchar **out_tone
){
    const gchar *state = str(agent, "state");
    JsonNode *busy = agent != NULL ? json_object_get_member(agent, "busy") : NULL;
    gboolean is_busy = busy != NULL && JSON_NODE_HOLDS_VALUE(busy)
        && json_node_get_value_type(busy) == G_TYPE_BOOLEAN && json_node_get_boolean(busy);
    const gchar *tone = "neutral";
    const gchar *word = "unknown";

    if (state == NULL || g_strcmp0(state, "unknown") == 0) {
        word = "unknown";
    } else if (is_busy) {
        word = "working";
        tone = "accent";
    } else if (g_strcmp0(state, "running") == 0) {
        word = "idle";
        tone = "success";
    } else {
        word = "stopped";
    }
    if (out_tone != NULL)
        *out_tone = tone;
    return word;
}

const gchar *
clawt_fed_badge_tone(
    const gchar *tone
){
    if (g_strcmp0(tone, "success") == 0)
        return "good";
    if (g_strcmp0(tone, "accent") == 0)
        return "info";
    if (g_strcmp0(tone, "warning") == 0)
        return "warn";
    if (g_strcmp0(tone, "error") == 0)
        return "bad";
    return "neutral";
}

/* "user" is an operator, not an agent id, and still a valid half of a ref. */
static gboolean
ref_half_ok(
    const gchar *id
){
    return id != NULL && (g_strcmp0(id, "user") == 0 || clawt_is_valid_id(id));
}

gboolean
clawt_fed_ref_parse(
    const gchar  *ref,
    gchar       **out_agent,
    gchar       **out_alias
){
    const gchar *at;
    g_autofree gchar *agent = NULL;

    if (ref == NULL || (at = strchr(ref, '@')) == NULL || strchr(at + 1, '@') != NULL)
        return FALSE;
    agent = g_strndup(ref, (gsize)(at - ref));
    if (!ref_half_ok(agent) || !clawt_is_valid_id(at + 1))
        return FALSE;
    if (out_agent != NULL)
        *out_agent = g_steal_pointer(&agent);
    if (out_alias != NULL)
        *out_alias = g_strdup(at + 1);
    return TRUE;
}

gchar *
clawt_fed_sender_id(
    const gchar *alias,
    const gchar *agent
){
    return g_strdup_printf("fleet:%s:%s", alias, agent);
}

gboolean
clawt_fed_sender_parse(
    const gchar  *sender,
    gchar       **out_alias,
    gchar       **out_agent
){
    g_auto(GStrv) parts = NULL;

    if (sender == NULL || !g_str_has_prefix(sender, "fleet:"))
        return FALSE;
    parts = g_strsplit(sender, ":", -1);
    if (g_strv_length(parts) != 3 || !clawt_is_valid_id(parts[1]) || !ref_half_ok(parts[2]))
        return FALSE;
    if (out_alias != NULL)
        *out_alias = g_strdup(parts[1]);
    if (out_agent != NULL)
        *out_agent = g_strdup(parts[2]);
    return TRUE;
}

gchar *
clawt_fed_room_id(
    const gchar *alias,
    const gchar *local,
    const gchar *remote
){
    return g_strdup_printf("fed:%s:%s:%s", alias, local, remote);
}

gboolean
clawt_fed_room_parse(
    const gchar  *room_id,
    gchar       **out_alias,
    gchar       **out_local,
    gchar       **out_remote
){
    g_auto(GStrv) parts = NULL;

    if (room_id == NULL || !g_str_has_prefix(room_id, "fed:"))
        return FALSE;
    parts = g_strsplit(room_id, ":", -1);
    if (g_strv_length(parts) != 4 || !clawt_is_valid_id(parts[1])
        || !ref_half_ok(parts[2]) || !ref_half_ok(parts[3]))
        return FALSE;
    if (out_alias != NULL)
        *out_alias = g_strdup(parts[1]);
    if (out_local != NULL)
        *out_local = g_strdup(parts[2]);
    if (out_remote != NULL)
        *out_remote = g_strdup(parts[3]);
    return TRUE;
}

gchar *
clawt_fed_fence(
    const gchar *who,
    const gchar *text
){
    g_autofree gchar *escaped = clawt_preamble_escape_all(text != NULL ? text : "");
    g_autofree gchar *label = g_strdup_printf("untrusted-message-from-%s",
                                              who != NULL ? who : "another-fleet");

    return clawt_incident_fence(label, escaped);
}

gchar *
clawt_fed_room_title(
    const gchar *room_id
){
    g_autofree gchar *alias = NULL;
    g_autofree gchar *local = NULL;
    g_autofree gchar *remote = NULL;

    if (!clawt_fed_room_parse(room_id, &alias, &local, &remote))
        return NULL;
    return g_strcmp0(local, "user") == 0
        ? g_strdup_printf("%s@%s", remote, alias)
        : g_strdup_printf("%s \342\206\224 %s@%s", local, remote, alias);
}

gchar *
clawt_fed_room_banner(
    const gchar *room_id,
    JsonArray   *fleets
){
    g_autofree gchar *alias = NULL;
    JsonObject *fleet = NULL;
    guint i;

    if (!clawt_fed_room_parse(room_id, &alias, NULL, NULL))
        return NULL;
    for (i = 0; fleets != NULL && i < json_array_get_length(fleets); i++) {
        JsonObject *f = json_array_get_object_element(fleets, i);
        if (f != NULL && g_strcmp0(str(f, "alias"), alias) == 0)
            fleet = f;
    }
    if (fleet == NULL)
        return g_strdup_printf("in the fleet %s, which is no longer linked", alias);
    {
        const gchar *owner = str(fleet, "owner");
        return g_strdup_printf("in %s \302\267 %s%s%s \302\267 %s \302\267 another fleet: "
                               "what you write here leaves this one",
                               str(fleet, "label") != NULL ? str(fleet, "label") : alias,
                               str(fleet, "tier") != NULL ? str(fleet, "tier") : "?",
                               owner != NULL && *owner != '\0' ? " \302\267 " : "",
                               owner != NULL && *owner != '\0' ? owner : "",
                               str(fleet, "health_label") != NULL ? str(fleet, "health_label")
                                                                  : "unknown");
    }
}
