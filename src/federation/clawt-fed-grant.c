/*
 * clawt-fed-grant.c - What one fleet lets another do
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-grant.h"
#include "federation/clawt-fed-selector.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* ---- enums ----------------------------------------------------------- */

static const GEnumValue tier_values[] = {
    { CLAWT_FED_TIER_OWNED, "CLAWT_FED_TIER_OWNED", "owned" },
    { CLAWT_FED_TIER_TRUSTED, "CLAWT_FED_TIER_TRUSTED", "trusted" },
    { CLAWT_FED_TIER_LIMITED, "CLAWT_FED_TIER_LIMITED", "limited" },
    { CLAWT_FED_TIER_UNTRUSTED, "CLAWT_FED_TIER_UNTRUSTED", "untrusted" },
    { 0, NULL, NULL }
};

static const GEnumValue health_values[] = {
    { CLAWT_FED_HEALTH_PENDING, "CLAWT_FED_HEALTH_PENDING", "pending" },
    { CLAWT_FED_HEALTH_CONNECTING, "CLAWT_FED_HEALTH_CONNECTING", "connecting" },
    { CLAWT_FED_HEALTH_LIVE, "CLAWT_FED_HEALTH_LIVE", "live" },
    { CLAWT_FED_HEALTH_DEGRADED, "CLAWT_FED_HEALTH_DEGRADED", "degraded" },
    { CLAWT_FED_HEALTH_OFFLINE, "CLAWT_FED_HEALTH_OFFLINE", "offline" },
    { CLAWT_FED_HEALTH_REVOKED, "CLAWT_FED_HEALTH_REVOKED", "revoked" },
    { CLAWT_FED_HEALTH_INCOMPATIBLE, "CLAWT_FED_HEALTH_INCOMPATIBLE", "incompatible" },
    { 0, NULL, NULL }
};

static const GEnumValue reach_values[] = {
    { CLAWT_FED_REACH_ALL, "CLAWT_FED_REACH_ALL", "all" },
    { CLAWT_FED_REACH_LISTED, "CLAWT_FED_REACH_LISTED", "listed" },
    { CLAWT_FED_REACH_INBOX, "CLAWT_FED_REACH_INBOX", "inbox" },
    { CLAWT_FED_REACH_NONE, "CLAWT_FED_REACH_NONE", "none" },
    { 0, NULL, NULL }
};

static const GEnumValue transcripts_values[] = {
    { CLAWT_FED_TRANSCRIPTS_NONE, "CLAWT_FED_TRANSCRIPTS_NONE", "none" },
    { CLAWT_FED_TRANSCRIPTS_OWN_TASKS, "CLAWT_FED_TRANSCRIPTS_OWN_TASKS", "own_tasks" },
    { CLAWT_FED_TRANSCRIPTS_REACHABLE, "CLAWT_FED_TRANSCRIPTS_REACHABLE", "reachable" },
    { 0, NULL, NULL }
};

#define DEFINE_ENUM(lower, Camel, values)                                   \
    GType                                                                   \
    lower##_get_type(void)                                                  \
    {                                                                       \
        static gsize type_id = 0;                                           \
        if (g_once_init_enter(&type_id)) {                                  \
            GType t = g_enum_register_static(Camel, values);                \
            g_once_init_leave(&type_id, t);                                 \
        }                                                                   \
        return type_id;                                                     \
    }

DEFINE_ENUM(clawt_fed_tier, "ClawtFedTier", tier_values)
DEFINE_ENUM(clawt_fed_health, "ClawtFedHealth", health_values)
DEFINE_ENUM(clawt_fed_reach, "ClawtFedReach", reach_values)
DEFINE_ENUM(clawt_fed_transcripts, "ClawtFedTranscripts", transcripts_values)

guint
clawt_fed_tier_count(void)
{
    return G_N_ELEMENTS(tier_values) - 1;
}

ClawtFedTier
clawt_fed_tier_nth(
    guint n
){
    if (n >= clawt_fed_tier_count())
        return CLAWT_FED_TIER_UNTRUSTED;
    return (ClawtFedTier)tier_values[n].value;
}

const gchar *
clawt_fed_tier_nick(
    ClawtFedTier tier
){
    switch (tier) {
    case CLAWT_FED_TIER_OWNED:     return "owned";
    case CLAWT_FED_TIER_TRUSTED:   return "trusted";
    case CLAWT_FED_TIER_LIMITED:   return "limited";
    case CLAWT_FED_TIER_UNTRUSTED: return "untrusted";
    }
    return "untrusted";
}

const gchar *
clawt_fed_tier_label(
    ClawtFedTier tier
){
    switch (tier) {
    case CLAWT_FED_TIER_OWNED:     return "Owned";
    case CLAWT_FED_TIER_TRUSTED:   return "Trusted";
    case CLAWT_FED_TIER_LIMITED:   return "Limited";
    case CLAWT_FED_TIER_UNTRUSTED: return "Untrusted";
    }
    return "Untrusted";
}

const gchar *
clawt_fed_tier_summary(
    ClawtFedTier tier
){
    switch (tier) {
    case CLAWT_FED_TIER_OWNED:
        return "A fleet you run yourself: reaches every agent, delegates, "
               "reads transcripts, no limits.";
    case CLAWT_FED_TIER_TRUSTED:
        return "A colleague you trust: messages and delegates to any agent; "
               "writes and commands ask you first.";
    case CLAWT_FED_TIER_LIMITED:
        return "A colleague you trust less: only the agents you list, "
               "delegation asks you, no commands.";
    case CLAWT_FED_TIER_UNTRUSTED:
        return "Anybody else: one inbox agent, and every message waits for "
               "your approval.";
    }
    return "";
}

/* No default: a new tier is a -Wswitch warning until it has a colour. */
const gchar *
clawt_fed_tier_tone(
    ClawtFedTier tier
){
    switch (tier) {
    case CLAWT_FED_TIER_OWNED:     return "success";
    case CLAWT_FED_TIER_TRUSTED:   return "accent";
    case CLAWT_FED_TIER_LIMITED:   return "warning";
    case CLAWT_FED_TIER_UNTRUSTED: return "error";
    }
    return "neutral";
}

gboolean
clawt_fed_tier_from_nick(
    const gchar  *nick,
    ClawtFedTier *out_tier
){
    gint value;

    if (nick == NULL || !clawt_enum_from_nick(CLAWT_TYPE_FED_TIER, nick, &value))
        return FALSE;
    *out_tier = (ClawtFedTier)value;
    return TRUE;
}

const gchar *
clawt_fed_health_nick(
    ClawtFedHealth health
){
    switch (health) {
    case CLAWT_FED_HEALTH_PENDING:      return "pending";
    case CLAWT_FED_HEALTH_CONNECTING:   return "connecting";
    case CLAWT_FED_HEALTH_LIVE:         return "live";
    case CLAWT_FED_HEALTH_DEGRADED:     return "degraded";
    case CLAWT_FED_HEALTH_OFFLINE:      return "offline";
    case CLAWT_FED_HEALTH_REVOKED:      return "revoked";
    case CLAWT_FED_HEALTH_INCOMPATIBLE: return "incompatible";
    }
    return "offline";
}

const gchar *
clawt_fed_health_label(
    ClawtFedHealth health
){
    switch (health) {
    case CLAWT_FED_HEALTH_PENDING:      return "Waiting";
    case CLAWT_FED_HEALTH_CONNECTING:   return "Connecting";
    case CLAWT_FED_HEALTH_LIVE:         return "Live";
    case CLAWT_FED_HEALTH_DEGRADED:     return "Reachable, no live updates";
    case CLAWT_FED_HEALTH_OFFLINE:      return "Offline";
    case CLAWT_FED_HEALTH_REVOKED:      return "Revoked";
    case CLAWT_FED_HEALTH_INCOMPATIBLE: return "Incompatible version";
    }
    return "Offline";
}

const gchar *
clawt_fed_health_tone(
    ClawtFedHealth health
){
    switch (health) {
    case CLAWT_FED_HEALTH_PENDING:      return "neutral";
    case CLAWT_FED_HEALTH_CONNECTING:   return "accent";
    case CLAWT_FED_HEALTH_LIVE:         return "success";
    case CLAWT_FED_HEALTH_DEGRADED:     return "warning";
    case CLAWT_FED_HEALTH_OFFLINE:      return "error";
    case CLAWT_FED_HEALTH_REVOKED:      return "neutral";
    case CLAWT_FED_HEALTH_INCOMPATIBLE: return "error";
    }
    return "neutral";
}

gboolean
clawt_fed_health_from_nick(
    const gchar    *nick,
    ClawtFedHealth *out_health
){
    gint value;

    if (nick == NULL || !clawt_enum_from_nick(CLAWT_TYPE_FED_HEALTH, nick, &value))
        return FALSE;
    *out_health = (ClawtFedHealth)value;
    return TRUE;
}

/* ---- the grant ------------------------------------------------------- */

G_DEFINE_BOXED_TYPE(ClawtFedGrant, clawt_fed_grant,
                    clawt_fed_grant_copy, clawt_fed_grant_free)

ClawtFedGrant *
clawt_fed_grant_copy(
    const ClawtFedGrant *grant
){
    ClawtFedGrant *copy;

    copy = g_new(ClawtFedGrant, 1);
    *copy = *grant;
    copy->agents = g_strdupv(grant->agents);
    return copy;
}

void
clawt_fed_grant_free(
    ClawtFedGrant *grant
){
    if (grant == NULL)
        return;
    g_strfreev(grant->agents);
    g_free(grant);
}

/*
 * The presets.  Rows are the table in docs/federation.org; the test
 * walks every tier and every field, so a tier added without a row fails.
 */
ClawtFedGrant *
clawt_fed_grant_preset(
    ClawtFedTier tier
){
    ClawtFedGrant *g;
    guint i;

    g = g_new0(ClawtFedGrant, 1);
    g->agents = g_new0(gchar *, 1);

    switch (tier) {
    case CLAWT_FED_TIER_OWNED:
        g->reach = CLAWT_FED_REACH_ALL;
        g->message = CLAWT_POLICY_ALLOW;
        g->delegate = CLAWT_POLICY_ALLOW;
        g->see_roster = g->see_presence = g->see_typing = g->see_tasks = TRUE;
        g->transcripts = CLAWT_FED_TRANSCRIPTS_REACHABLE;
        for (i = 0; i < CLAWT_FED_ACTION_CLASSES; i++)
            g->caps[i] = CLAWT_POLICY_ALLOW;
        g->caps[CLAWT_ACTION_DESKTOP_INPUT] = CLAWT_POLICY_ASK;
        g->caps[CLAWT_ACTION_FLEET_ADMIN] = CLAWT_POLICY_ASK;
        g->subdelegate = TRUE;
        g->join_rooms = TRUE;
        break;
    case CLAWT_FED_TIER_TRUSTED:
        g->reach = CLAWT_FED_REACH_ALL;
        g->message = CLAWT_POLICY_ALLOW;
        g->delegate = CLAWT_POLICY_ALLOW;
        g->see_roster = g->see_presence = g->see_typing = g->see_tasks = TRUE;
        g->transcripts = CLAWT_FED_TRANSCRIPTS_OWN_TASKS;
        g->caps[CLAWT_ACTION_READ] = CLAWT_POLICY_ALLOW;
        g->caps[CLAWT_ACTION_MESSAGE] = CLAWT_POLICY_ALLOW;
        g->caps[CLAWT_ACTION_WRITE] = CLAWT_POLICY_ASK;
        g->caps[CLAWT_ACTION_DELETE] = CLAWT_POLICY_ASK;
        g->caps[CLAWT_ACTION_EXEC] = CLAWT_POLICY_ASK;
        g->caps[CLAWT_ACTION_EXTERNAL] = CLAWT_POLICY_ASK;
        g->caps[CLAWT_ACTION_DESKTOP_INPUT] = CLAWT_POLICY_DENY;
        g->caps[CLAWT_ACTION_FLEET_ADMIN] = CLAWT_POLICY_DENY;
        g->budget_cents_per_day = 2000;
        g->messages_per_hour = 120;
        g->max_concurrent_tasks = 4;
        g->max_hops_in = 4;
        g->subdelegate = TRUE;
        g->join_rooms = TRUE;
        break;
    case CLAWT_FED_TIER_LIMITED:
        g->reach = CLAWT_FED_REACH_LISTED;
        g->message = CLAWT_POLICY_ALLOW;
        g->delegate = CLAWT_POLICY_ASK;
        g->see_roster = g->see_presence = g->see_typing = g->see_tasks = TRUE;
        g->transcripts = CLAWT_FED_TRANSCRIPTS_NONE;
        g->caps[CLAWT_ACTION_READ] = CLAWT_POLICY_ALLOW;
        g->caps[CLAWT_ACTION_MESSAGE] = CLAWT_POLICY_ASK;
        g->caps[CLAWT_ACTION_WRITE] = CLAWT_POLICY_ASK;
        g->caps[CLAWT_ACTION_DELETE] = CLAWT_POLICY_ASK;
        g->caps[CLAWT_ACTION_EXEC] = CLAWT_POLICY_DENY;
        g->caps[CLAWT_ACTION_EXTERNAL] = CLAWT_POLICY_DENY;
        g->caps[CLAWT_ACTION_DESKTOP_INPUT] = CLAWT_POLICY_DENY;
        g->caps[CLAWT_ACTION_FLEET_ADMIN] = CLAWT_POLICY_DENY;
        g->budget_cents_per_day = 500;
        g->messages_per_hour = 30;
        g->max_concurrent_tasks = 1;
        g->max_hops_in = 2;
        break;
    case CLAWT_FED_TIER_UNTRUSTED:
        g->reach = CLAWT_FED_REACH_INBOX;
        g->message = CLAWT_POLICY_REVIEW;
        g->delegate = CLAWT_POLICY_DENY;
        g->see_roster = TRUE;
        g->see_presence = TRUE;
        g->transcripts = CLAWT_FED_TRANSCRIPTS_NONE;
        for (i = 0; i < CLAWT_FED_ACTION_CLASSES; i++)
            g->caps[i] = CLAWT_POLICY_DENY;
        g->caps[CLAWT_ACTION_READ] = CLAWT_POLICY_ASK;
        g->budget_cents_per_day = 100;
        g->messages_per_hour = 10;
        g->max_concurrent_tasks = 1;
        g->max_hops_in = 1;
        break;
    }
    return g;
}

/* ---- the field table ------------------------------------------------- */

typedef struct {
    const gchar       *name;
    ClawtFedFieldKind  kind;
    GType            (*type)(void);
    gsize              offset;
    const gchar       *doc;
} Field;

static const Field fields[] = {
    { "reach", CLAWT_FED_FIELD_ENUM, clawt_fed_reach_get_type,
      G_STRUCT_OFFSET(ClawtFedGrant, reach),
      "Which of our agents the peer may address: all, listed, inbox or none." },
    { "agents", CLAWT_FED_FIELD_LIST, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, agents),
      "Agent selectors for reach=listed: ids, team:NAME, tag:TAG." },
    { "message", CLAWT_FED_FIELD_ENUM, clawt_policy_mode_get_type,
      G_STRUCT_OFFSET(ClawtFedGrant, message),
      "Inbound messages: allow, review (held for your approval) or deny." },
    { "delegate", CLAWT_FED_FIELD_ENUM, clawt_policy_mode_get_type,
      G_STRUCT_OFFSET(ClawtFedGrant, delegate),
      "Tasks the peer hands us: allow, ask or deny." },
    { "see_roster", CLAWT_FED_FIELD_BOOL, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, see_roster),
      "Whether the peer sees the agents it may reach, and their teams." },
    { "see_presence", CLAWT_FED_FIELD_BOOL, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, see_presence),
      "Whether the peer sees who is working." },
    { "see_typing", CLAWT_FED_FIELD_BOOL, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, see_typing),
      "Whether the peer sees who is typing, live." },
    { "see_tasks", CLAWT_FED_FIELD_BOOL, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, see_tasks),
      "Whether the peer sees the state of tasks it is party to." },
    { "transcripts", CLAWT_FED_FIELD_ENUM, clawt_fed_transcripts_get_type,
      G_STRUCT_OFFSET(ClawtFedGrant, transcripts),
      "What the peer may read back: none, own_tasks or reachable." },
    { "budget_cents_per_day", CLAWT_FED_FIELD_INT, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, budget_cents_per_day),
      "Spend on the peer's behalf per day, in cents; 0 is unlimited." },
    { "messages_per_hour", CLAWT_FED_FIELD_INT, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, messages_per_hour),
      "Inbound messages per hour; 0 is unlimited." },
    { "max_concurrent_tasks", CLAWT_FED_FIELD_INT, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, max_concurrent_tasks),
      "Tasks for the peer running at once; 0 is unlimited." },
    { "max_hops_in", CLAWT_FED_FIELD_INT, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, max_hops_in),
      "How deep a chain from the peer may go here; 0 is the local max_hops." },
    { "subdelegate", CLAWT_FED_FIELD_BOOL, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, subdelegate),
      "Whether work done for the peer may delegate to other agents here." },
    { "join_rooms", CLAWT_FED_FIELD_BOOL, NULL,
      G_STRUCT_OFFSET(ClawtFedGrant, join_rooms),
      "Whether the peer may be a member of shared rooms." },
};

guint
clawt_fed_grant_field_count(void)
{
    return G_N_ELEMENTS(fields) + CLAWT_FED_ACTION_CLASSES;
}

/* `cap.<action class nick>`, built once from the enum itself. */
static const gchar *
cap_name(
    guint klass
){
    static gchar *names[CLAWT_FED_ACTION_CLASSES];
    static gsize once = 0;

    if (g_once_init_enter(&once)) {
        GEnumClass *ec = g_type_class_ref(CLAWT_TYPE_ACTION_CLASS);
        guint i;
        for (i = 0; i < CLAWT_FED_ACTION_CLASSES; i++) {
            GEnumValue *v = g_enum_get_value(ec, (gint)i);
            names[i] = g_strdup_printf("cap.%s", v != NULL ? v->value_nick : "unknown");
        }
        g_type_class_unref(ec);
        g_once_init_leave(&once, 1);
    }
    return names[klass];
}

const gchar *
clawt_fed_grant_field_name(
    guint n
){
    if (n < G_N_ELEMENTS(fields))
        return fields[n].name;
    n -= G_N_ELEMENTS(fields);
    if (n < CLAWT_FED_ACTION_CLASSES)
        return cap_name(n);
    return NULL;
}

ClawtFedFieldKind
clawt_fed_grant_field_kind(
    guint n
){
    if (n < G_N_ELEMENTS(fields))
        return fields[n].kind;
    return CLAWT_FED_FIELD_ENUM;
}

GType
clawt_fed_grant_field_enum_type(
    guint n
){
    if (n < G_N_ELEMENTS(fields))
        return fields[n].type != NULL ? fields[n].type() : G_TYPE_INVALID;
    if (n - G_N_ELEMENTS(fields) < CLAWT_FED_ACTION_CLASSES)
        return CLAWT_TYPE_POLICY_MODE;
    return G_TYPE_INVALID;
}

const gchar *
clawt_fed_grant_field_doc(
    guint n
){
    if (n < G_N_ELEMENTS(fields))
        return fields[n].doc;
    return "The most work done for the peer may do in this action class: "
           "allow, review, ask, hand_back or deny.  Combined with the agent's "
           "own rules; the stricter wins.";
}

gint
clawt_fed_grant_field_find(
    const gchar *name
){
    guint i;

    if (name == NULL)
        return -1;
    for (i = 0; i < clawt_fed_grant_field_count(); i++)
        if (g_ascii_strcasecmp(clawt_fed_grant_field_name(i), name) == 0)
            return (gint)i;
    return -1;
}

static gpointer
field_slot(
    const ClawtFedGrant *grant,
    guint                n
){
    if (n < G_N_ELEMENTS(fields))
        return G_STRUCT_MEMBER_P((gpointer)grant, fields[n].offset);
    return (gpointer)&grant->caps[n - G_N_ELEMENTS(fields)];
}

static const gchar *
enum_nick(
    GType type,
    gint  value
){
    GEnumClass *ec;
    GEnumValue *v;
    const gchar *nick;

    ec = g_type_class_ref(type);
    v = g_enum_get_value(ec, value);
    nick = v != NULL ? v->value_nick : "";
    g_type_class_unref(ec);
    return nick;
}

gchar *
clawt_fed_grant_get(
    const ClawtFedGrant *grant,
    guint                n
){
    gpointer slot;

    if (n >= clawt_fed_grant_field_count())
        return NULL;
    slot = field_slot(grant, n);
    switch (clawt_fed_grant_field_kind(n)) {
    case CLAWT_FED_FIELD_BOOL:
        return g_strdup(*(gboolean *)slot ? "true" : "false");
    case CLAWT_FED_FIELD_INT:
        return g_strdup_printf("%d", *(gint *)slot);
    case CLAWT_FED_FIELD_ENUM:
        return g_strdup(enum_nick(clawt_fed_grant_field_enum_type(n), *(gint *)slot));
    case CLAWT_FED_FIELD_LIST:
        return g_strjoinv(",", *(gchar ***)slot);
    }
    return NULL;
}

static gboolean
parse_bool(
    const gchar *value,
    gboolean    *out
){
    if (g_ascii_strcasecmp(value, "true") == 0 || g_ascii_strcasecmp(value, "yes") == 0
        || g_ascii_strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0) {
        *out = TRUE;
        return TRUE;
    }
    if (g_ascii_strcasecmp(value, "false") == 0 || g_ascii_strcasecmp(value, "no") == 0
        || g_ascii_strcasecmp(value, "off") == 0 || strcmp(value, "0") == 0) {
        *out = FALSE;
        return TRUE;
    }
    return FALSE;
}

static gchar *
enum_choices(
    GType type
){
    GEnumClass *ec;
    GString *out;
    guint i;

    ec = g_type_class_ref(type);
    out = g_string_new(NULL);
    for (i = 0; i < ec->n_values; i++) {
        if (i > 0)
            g_string_append(out, ", ");
        g_string_append(out, ec->values[i].value_nick);
    }
    g_type_class_unref(ec);
    return g_string_free(out, FALSE);
}

gboolean
clawt_fed_grant_set(
    ClawtFedGrant *grant,
    const gchar   *name,
    const gchar   *value,
    GError       **error
){
    gint n;
    gpointer slot;

    n = clawt_fed_grant_field_find(name);
    if (n < 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "no grant field \"%s\"; `clawtilla federation explain` "
                    "lists them", name != NULL ? name : "");
        return FALSE;
    }
    if (value == NULL)
        value = "";
    slot = field_slot(grant, (guint)n);

    /* Dispatch on what the field is; never store a value as another kind. */
    switch (clawt_fed_grant_field_kind((guint)n)) {
    case CLAWT_FED_FIELD_BOOL: {
        gboolean b;
        if (!parse_bool(value, &b)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "%s is true or false, not \"%s\"", name, value);
            return FALSE;
        }
        *(gboolean *)slot = b;
        return TRUE;
    }
    case CLAWT_FED_FIELD_INT: {
        gchar *end = NULL;
        gint64 v;
        errno = 0;
        v = g_ascii_strtoll(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0' || v < 0 || v > G_MAXINT) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "%s is a whole number, 0 or more, not \"%s\"", name, value);
            return FALSE;
        }
        *(gint *)slot = (gint)v;
        return TRUE;
    }
    case CLAWT_FED_FIELD_ENUM: {
        GType type = clawt_fed_grant_field_enum_type((guint)n);
        gint v;
        if (!clawt_enum_from_nick(type, value, &v)) {
            g_autofree gchar *choices = enum_choices(type);
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "%s is one of %s, not \"%s\"", name, choices, value);
            return FALSE;
        }
        *(gint *)slot = v;
        return TRUE;
    }
    case CLAWT_FED_FIELD_LIST: {
        g_auto(GStrv) parts = g_strsplit(value, ",", -1);
        GPtrArray *clean = g_ptr_array_new();
        guint i;
        for (i = 0; parts[i] != NULL; i++) {
            g_autoptr(GError) local = NULL;
            gchar *item = g_strstrip(parts[i]);
            if (*item == '\0')
                continue;
            if (!clawt_fed_agent_selector_check(item, &local)) {
                g_ptr_array_free(clean, TRUE);
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "%s: %s", name, local->message);
                return FALSE;
            }
            g_ptr_array_add(clean, g_strdup(item));
        }
        g_ptr_array_add(clean, NULL);
        g_strfreev(*(gchar ***)slot);
        *(gchar ***)slot = (gchar **)g_ptr_array_free(clean, FALSE);
        return TRUE;
    }
    }
    return FALSE;
}

JsonObject *
clawt_fed_grant_to_json(
    const ClawtFedGrant *grant
){
    JsonObject *object;
    guint i;

    object = json_object_new();
    for (i = 0; i < clawt_fed_grant_field_count(); i++) {
        g_autofree gchar *value = clawt_fed_grant_get(grant, i);
        json_object_set_string_member(object, clawt_fed_grant_field_name(i), value);
    }
    return object;
}

/* Applies `field=value; field=value` to @grant, all or nothing. */
static gboolean
apply_assignments(
    ClawtFedGrant  *grant,
    const gchar    *assignments,
    const gchar    *source,
    GHashTable     *explain,
    GError        **error
){
    g_autoptr(ClawtFedGrant) trial = NULL;
    g_auto(GStrv) parts = NULL;
    g_autoptr(GPtrArray) touched = NULL;
    guint i;

    trial = clawt_fed_grant_copy(grant);
    touched = g_ptr_array_new_with_free_func(g_free);
    parts = g_strsplit(assignments, ";", -1);
    for (i = 0; parts[i] != NULL; i++) {
        gchar *item = g_strstrip(parts[i]);
        gchar *eq;

        if (*item == '\0')
            continue;
        eq = strchr(item, '=');
        if (eq == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "\"%s\" is not field=value", item);
            return FALSE;
        }
        *eq = '\0';
        if (!clawt_fed_grant_set(trial, g_strstrip(item), g_strstrip(eq + 1), error))
            return FALSE;
        g_ptr_array_add(touched, g_ascii_strdown(item, -1));
    }

    /* Commit. */
    g_strfreev(grant->agents);
    *grant = *trial;
    trial->agents = NULL;
    if (explain != NULL)
        for (i = 0; i < touched->len; i++)
            g_hash_table_replace(explain, g_strdup(g_ptr_array_index(touched, i)),
                                 g_strdup(source));
    return TRUE;
}

ClawtFedGrant *
clawt_fed_grant_resolve(
    ClawtFedTier   tier,
    gchar        **rules,
    JsonObject    *peer,
    JsonObject    *overrides,
    GHashTable   **out_explain,
    GPtrArray    **out_problems
){
    ClawtFedGrant *grant;
    g_autoptr(GHashTable) explain = NULL;
    g_autoptr(GPtrArray) problems = NULL;
    g_autofree gchar *tier_source = NULL;
    guint i;

    grant = clawt_fed_grant_preset(tier);
    explain = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    problems = g_ptr_array_new_with_free_func(g_free);
    tier_source = g_strdup_printf("tier %s", clawt_fed_tier_nick(tier));
    for (i = 0; i < clawt_fed_grant_field_count(); i++)
        g_hash_table_replace(explain, g_strdup(clawt_fed_grant_field_name(i)),
                             g_strdup(tier_source));

    /* Rules, in file order: `SELECTOR: field=value; field=value`. */
    for (i = 0; rules != NULL && rules[i] != NULL; i++) {
        g_autoptr(GError) error = NULL;
        g_autofree gchar *selector = NULL;
        g_autofree gchar *source = NULL;
        const gchar *split;

        split = strstr(rules[i], ": ");
        if (split == NULL) {
            g_ptr_array_add(problems, g_strdup_printf(
                "rule %u (\"%s\"): expected `SELECTOR: field=value`", i + 1, rules[i]));
            continue;
        }
        selector = g_strstrip(g_strndup(rules[i], split - rules[i]));
        if (!clawt_fed_selector_check(selector, &error)) {
            g_ptr_array_add(problems, g_strdup_printf("rule %u: %s", i + 1, error->message));
            continue;
        }
        if (!clawt_fed_selector_matches(selector, peer))
            continue;
        source = g_strdup_printf("rule %u: %s", i + 1, selector);
        if (!apply_assignments(grant, split + 2, source, explain, &error))
            g_ptr_array_add(problems, g_strdup_printf("rule %u: %s", i + 1, error->message));
    }

    /* The peer's own overrides, last. */
    if (overrides != NULL) {
        g_autoptr(GList) members = json_object_get_members(overrides);
        GList *l;

        for (l = members; l != NULL; l = l->next) {
            g_autoptr(GError) error = NULL;
            JsonNode *node = json_object_get_member(overrides, l->data);
            const gchar *value;

            if (!JSON_NODE_HOLDS_VALUE(node)
                || json_node_get_value_type(node) != G_TYPE_STRING) {
                g_ptr_array_add(problems, g_strdup_printf(
                    "override %s: not a string", (const gchar *)l->data));
                continue;
            }
            value = json_node_get_string(node);
            if (!clawt_fed_grant_set(grant, l->data, value, &error)) {
                g_ptr_array_add(problems, g_strdup_printf("override: %s", error->message));
                continue;
            }
            g_hash_table_replace(explain, g_ascii_strdown(l->data, -1),
                                 g_strdup("override"));
        }
    }

    /* Fixed, whatever anything says: a peer never administers this fleet. */
    grant->caps[CLAWT_ACTION_FLEET_ADMIN] = CLAWT_POLICY_DENY;
    g_hash_table_replace(explain, g_strdup(cap_name(CLAWT_ACTION_FLEET_ADMIN)),
                         g_strdup("fixed: a peer never administers this fleet"));

    if (out_explain != NULL)
        *out_explain = g_steal_pointer(&explain);
    if (out_problems != NULL)
        *out_problems = g_steal_pointer(&problems);
    return grant;
}

gboolean
clawt_fed_grant_reaches(
    const ClawtFedGrant *grant,
    const gchar         *agent_id,
    const gchar         *team,
    gchar              **tags,
    const gchar         *inbox_agent,
    gchar              **agent_reach,
    JsonObject          *peer
){
    gboolean granted = FALSE;
    guint i;

    switch (grant->reach) {
    case CLAWT_FED_REACH_ALL:
        granted = TRUE;
        break;
    case CLAWT_FED_REACH_LISTED:
        for (i = 0; grant->agents != NULL && grant->agents[i] != NULL; i++)
            if (clawt_fed_agent_selector_matches(grant->agents[i], agent_id, team, tags))
                granted = TRUE;
        /* The inbox agent is reachable at every tier that reaches anyone. */
        if (g_strcmp0(agent_id, inbox_agent) == 0)
            granted = TRUE;
        break;
    case CLAWT_FED_REACH_INBOX:
        granted = inbox_agent != NULL && g_strcmp0(agent_id, inbox_agent) == 0;
        break;
    case CLAWT_FED_REACH_NONE:
        granted = FALSE;
        break;
    }
    if (!granted)
        return FALSE;

    /* The agent's own list can only narrow. */
    if (agent_reach != NULL && agent_reach[0] != NULL) {
        for (i = 0; agent_reach[i] != NULL; i++)
            if (clawt_fed_selector_matches(agent_reach[i], peer))
                return TRUE;
        return FALSE;
    }
    return TRUE;
}

static gint
strictness(
    ClawtPolicyMode mode
){
    switch (mode) {
    case CLAWT_POLICY_ALLOW:     return 0;
    case CLAWT_POLICY_REVIEW:    return 1;
    case CLAWT_POLICY_ASK:       return 2;
    case CLAWT_POLICY_HAND_BACK: return 3;
    case CLAWT_POLICY_DENY:      return 4;
    }
    return 4;
}

ClawtPolicyMode
clawt_fed_policy_stricter(
    ClawtPolicyMode a,
    ClawtPolicyMode b
){
    return strictness(a) >= strictness(b) ? a : b;
}
