/*
 * clawt-fed-selector.c - One grammar for naming peers and agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-selector.h"

#include <string.h>

typedef enum {
    SUBJECT_PEER,
    SUBJECT_AGENT
} Subject;

typedef struct {
    const gchar  *agent_id;
    const gchar  *team;
    gchar       **tags;
} AgentFacts;

static gboolean
glob_matches(
    const gchar *pattern,
    const gchar *value
){
    g_autofree gchar *p = NULL;
    g_autofree gchar *v = NULL;

    if (value == NULL)
        return FALSE;
    p = g_utf8_casefold(pattern, -1);
    v = g_utf8_casefold(value, -1);
    return g_pattern_match_simple(p, v);
}

static const gchar *
peer_string(
    JsonObject  *peer,
    const gchar *name
){
    JsonNode *node;

    if (peer == NULL)
        return NULL;
    node = json_object_get_member(peer, name);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)
        || json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(node);
}

static gboolean
peer_has_tag(
    JsonObject  *peer,
    const gchar *tag
){
    JsonNode *node;
    JsonArray *tags;
    guint i;

    if (peer == NULL)
        return FALSE;
    node = json_object_get_member(peer, "tags");
    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
        return FALSE;
    tags = json_node_get_array(node);
    for (i = 0; tags != NULL && i < json_array_get_length(tags); i++) {
        JsonNode *item = json_array_get_element(tags, i);
        if (JSON_NODE_HOLDS_VALUE(item)
            && json_node_get_value_type(item) == G_TYPE_STRING
            && g_ascii_strcasecmp(json_node_get_string(item), tag) == 0)
            return TRUE;
    }
    return FALSE;
}

/*
 * One term, with its `!` already stripped.  Returns FALSE and sets
 * @error for a term that is not well formed; otherwise sets @out.
 */
static gboolean
term_eval(
    Subject           subject,
    const gchar      *term,
    JsonObject       *peer,
    const AgentFacts *agent,
    gboolean         *out,
    GError          **error
){
    const gchar *colon;
    g_autofree gchar *key = NULL;
    const gchar *value;

    *out = FALSE;
    if (*term == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "empty term");
        return FALSE;
    }
    if (strcmp(term, "*") == 0) {
        *out = TRUE;
        return TRUE;
    }

    colon = strchr(term, ':');
    if (colon == NULL) {
        if (subject == SUBJECT_PEER) {
            if (g_ascii_strcasecmp(term, "online") == 0) {
                const gchar *h = peer_string(peer, "health");
                *out = g_strcmp0(h, "live") == 0 || g_strcmp0(h, "degraded") == 0;
                return TRUE;
            }
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "\"%s\": a peer term is key:value (name, tier, owner, "
                        "tag, health), `online` or `*`", term);
            return FALSE;
        }
        /* A bare word names an agent id, glob allowed. */
        *out = agent != NULL && glob_matches(term, agent->agent_id);
        return TRUE;
    }

    key = g_ascii_strdown(term, colon - term);
    value = colon + 1;
    if (*value == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "\"%s\" has no value after the colon", term);
        return FALSE;
    }

    if (subject == SUBJECT_AGENT) {
        if (strcmp(key, "team") == 0) {
            *out = agent != NULL && agent->team != NULL
                && glob_matches(value, agent->team);
            return TRUE;
        }
        if (strcmp(key, "tag") == 0) {
            guint i;
            for (i = 0; agent != NULL && agent->tags != NULL && agent->tags[i] != NULL; i++)
                if (g_ascii_strcasecmp(agent->tags[i], value) == 0)
                    *out = TRUE;
            return TRUE;
        }
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "\"%s\": an agent term is an id, team:NAME, tag:TAG or `*`",
                    term);
        return FALSE;
    }

    if (strcmp(key, "name") == 0) {
        *out = glob_matches(value, peer_string(peer, "alias"))
            || glob_matches(value, peer_string(peer, "label"));
        return TRUE;
    }
    if (strcmp(key, "owner") == 0) {
        *out = glob_matches(value, peer_string(peer, "owner"));
        return TRUE;
    }
    if (strcmp(key, "tag") == 0) {
        *out = peer_has_tag(peer, value);
        return TRUE;
    }
    if (strcmp(key, "tier") == 0) {
        ClawtFedTier tier;
        if (!clawt_fed_tier_from_nick(value, &tier)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "\"%s\": the tiers are owned, trusted, limited and "
                        "untrusted", term);
            return FALSE;
        }
        *out = g_strcmp0(peer_string(peer, "tier"), clawt_fed_tier_nick(tier)) == 0;
        return TRUE;
    }
    if (strcmp(key, "health") == 0) {
        ClawtFedHealth health;
        if (!clawt_fed_health_from_nick(value, &health)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "\"%s\" names no link state", term);
            return FALSE;
        }
        *out = g_strcmp0(peer_string(peer, "health"), clawt_fed_health_nick(health)) == 0;
        return TRUE;
    }
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                "\"%s\": unknown key \"%s\" (name, tier, owner, tag, health)",
                term, key);
    return FALSE;
}

/* Alternatives by `,`, each a conjunction by `+` of possibly negated terms. */
static gboolean
selector_eval(
    Subject           subject,
    const gchar      *selector,
    JsonObject       *peer,
    const AgentFacts *agent,
    gboolean         *out,
    GError          **error
){
    g_auto(GStrv) alternatives = NULL;
    gboolean any = FALSE;
    guint i;

    *out = FALSE;
    if (selector == NULL || *selector == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "empty selector");
        return FALSE;
    }
    alternatives = g_strsplit(selector, ",", -1);
    for (i = 0; alternatives[i] != NULL; i++) {
        g_auto(GStrv) terms = NULL;
        gboolean all = TRUE;
        guint j;

        g_strstrip(alternatives[i]);
        terms = g_strsplit(alternatives[i], "+", -1);
        for (j = 0; terms[j] != NULL; j++) {
            const gchar *term = g_strstrip(terms[j]);
            gboolean negate = FALSE;
            gboolean hit;

            if (*term == '!') {
                negate = TRUE;
                term++;
            }
            if (!term_eval(subject, term, peer, agent, &hit, error))
                return FALSE;
            if (negate)
                hit = !hit;
            if (!hit)
                all = FALSE;
        }
        if (all)
            any = TRUE;
    }
    *out = any;
    return TRUE;
}

gboolean
clawt_fed_selector_check(
    const gchar  *selector,
    GError      **error
){
    gboolean ignored;

    return selector_eval(SUBJECT_PEER, selector, NULL, NULL, &ignored, error);
}

gboolean
clawt_fed_selector_matches(
    const gchar *selector,
    JsonObject  *peer
){
    gboolean hit = FALSE;

    if (!selector_eval(SUBJECT_PEER, selector, peer, NULL, &hit, NULL))
        return FALSE;
    return hit;
}

gboolean
clawt_fed_agent_selector_check(
    const gchar  *selector,
    GError      **error
){
    AgentFacts none = { "", NULL, NULL };
    gboolean ignored;

    return selector_eval(SUBJECT_AGENT, selector, NULL, &none, &ignored, error);
}

gboolean
clawt_fed_agent_selector_matches(
    const gchar *selector,
    const gchar *agent_id,
    const gchar *team,
    gchar      **tags
){
    AgentFacts facts;
    gboolean hit = FALSE;

    facts.agent_id = agent_id;
    facts.team = team;
    facts.tags = tags;
    if (!selector_eval(SUBJECT_AGENT, selector, NULL, &facts, &hit, NULL))
        return FALSE;
    return hit;
}
