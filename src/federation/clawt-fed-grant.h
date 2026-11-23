/*
 * clawt-fed-grant.h - What one fleet lets another do
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A peer has a tier (owned, trusted, limited, untrusted).  A tier is a
 * preset *grant*: which of our agents the peer may reach, whether it may
 * message or delegate, what it may see, a cap on what work done for it
 * may do (per action class), and integer limits.  Any field can be
 * overridden, for every peer a rule matches or for one peer alone, and
 * an agent can narrow who reaches it -- never widen.
 *
 * The fields are a table (clawt_fed_grant_field_count()/_nth()), walked
 * by the CLI, the three permission editors and `explain`, so adding one
 * is one row here and nothing else to keep in step.
 *
 * The grant is computed by the fleet *giving* it.  What a peer reports it
 * allows us is shown, never enforced here: each side enforces its own.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include "clawt-enums.h"

G_BEGIN_DECLS

/**
 * ClawtFedTier:
 * @CLAWT_FED_TIER_OWNED: a fleet you run yourself
 * @CLAWT_FED_TIER_TRUSTED: a colleague you trust; writes and commands ask
 * @CLAWT_FED_TIER_LIMITED: a colleague you trust less; named agents only,
 *   delegation asks
 * @CLAWT_FED_TIER_UNTRUSTED: anybody else; one inbox agent, every message
 *   held for your approval
 *
 * How far a peer is trusted, as a starting point for its grant.
 */
typedef enum {
    CLAWT_FED_TIER_OWNED = 0,
    CLAWT_FED_TIER_TRUSTED,
    CLAWT_FED_TIER_LIMITED,
    CLAWT_FED_TIER_UNTRUSTED
} ClawtFedTier;

GType clawt_fed_tier_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_FED_TIER (clawt_fed_tier_get_type())

/**
 * clawt_fed_tier_count:
 *
 * Returns: how many tiers there are
 */
guint clawt_fed_tier_count(void);

/**
 * clawt_fed_tier_nth:
 * @n: an index below clawt_fed_tier_count()
 *
 * Returns: the tier at @n, most trusted first
 */
ClawtFedTier clawt_fed_tier_nth(guint n);

/**
 * clawt_fed_tier_nick:
 * @tier: a #ClawtFedTier
 *
 * Returns: (transfer none): `owned`, `trusted`, `limited` or `untrusted`
 */
const gchar *clawt_fed_tier_nick(ClawtFedTier tier);

/**
 * clawt_fed_tier_label:
 * @tier: a #ClawtFedTier
 *
 * Returns: (transfer none): what a client puts on the badge
 */
const gchar *clawt_fed_tier_label(ClawtFedTier tier);

/**
 * clawt_fed_tier_summary:
 * @tier: a #ClawtFedTier
 *
 * Returns: (transfer none): one sentence for the invite wizard's card
 */
const gchar *clawt_fed_tier_summary(ClawtFedTier tier);

/**
 * clawt_fed_tier_tone:
 * @tier: a #ClawtFedTier
 *
 * Returns: (transfer none): `success`, `accent`, `warning` or `error` --
 *   the same tone words the task badges use, so every client colours a
 *   tier from its existing palette
 */
const gchar *clawt_fed_tier_tone(ClawtFedTier tier);

/**
 * clawt_fed_tier_from_nick:
 * @nick: (nullable): a tier's nick
 * @out_tier: (out): the tier
 *
 * Returns: %TRUE if @nick names a tier
 */
gboolean clawt_fed_tier_from_nick(const gchar  *nick,
                                  ClawtFedTier *out_tier);

/**
 * ClawtFedHealth:
 * @CLAWT_FED_HEALTH_PENDING: accepted, waiting for approval or a first dial
 * @CLAWT_FED_HEALTH_CONNECTING: dialling
 * @CLAWT_FED_HEALTH_LIVE: answering, with a live event stream
 * @CLAWT_FED_HEALTH_DEGRADED: answering requests, but no event stream
 * @CLAWT_FED_HEALTH_OFFLINE: not answering
 * @CLAWT_FED_HEALTH_REVOKED: the link was revoked (by either side)
 * @CLAWT_FED_HEALTH_INCOMPATIBLE: answers with a protocol we do not speak
 *
 * The state of the link to one peer.
 */
typedef enum {
    CLAWT_FED_HEALTH_PENDING = 0,
    CLAWT_FED_HEALTH_CONNECTING,
    CLAWT_FED_HEALTH_LIVE,
    CLAWT_FED_HEALTH_DEGRADED,
    CLAWT_FED_HEALTH_OFFLINE,
    CLAWT_FED_HEALTH_REVOKED,
    CLAWT_FED_HEALTH_INCOMPATIBLE
} ClawtFedHealth;

GType clawt_fed_health_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_FED_HEALTH (clawt_fed_health_get_type())

/**
 * clawt_fed_health_nick:
 * @health: a #ClawtFedHealth
 *
 * Returns: (transfer none): its nick
 */
const gchar *clawt_fed_health_nick(ClawtFedHealth health);

/**
 * clawt_fed_health_label:
 * @health: a #ClawtFedHealth
 *
 * Returns: (transfer none): what a client shows beside the dot
 */
const gchar *clawt_fed_health_label(ClawtFedHealth health);

/**
 * clawt_fed_health_tone:
 * @health: a #ClawtFedHealth
 *
 * Returns: (transfer none): `success`, `accent`, `warning`, `error` or
 *   `neutral`
 */
const gchar *clawt_fed_health_tone(ClawtFedHealth health);

/**
 * clawt_fed_health_from_nick:
 * @nick: (nullable): a nick
 * @out_health: (out): the health
 *
 * Returns: %TRUE if @nick names a health
 */
gboolean clawt_fed_health_from_nick(const gchar    *nick,
                                    ClawtFedHealth *out_health);

/**
 * ClawtFedReach:
 * @CLAWT_FED_REACH_ALL: every agent (an agent can still narrow itself)
 * @CLAWT_FED_REACH_LISTED: the agents the grant's `agents` selectors match
 * @CLAWT_FED_REACH_INBOX: only the inbox agent (`federation.inbox_agent`)
 * @CLAWT_FED_REACH_NONE: nobody
 *
 * Which of our agents a peer may address.
 */
typedef enum {
    CLAWT_FED_REACH_ALL = 0,
    CLAWT_FED_REACH_LISTED,
    CLAWT_FED_REACH_INBOX,
    CLAWT_FED_REACH_NONE
} ClawtFedReach;

GType clawt_fed_reach_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_FED_REACH (clawt_fed_reach_get_type())

/**
 * ClawtFedTranscripts:
 * @CLAWT_FED_TRANSCRIPTS_NONE: no transcript is readable
 * @CLAWT_FED_TRANSCRIPTS_OWN_TASKS: the rooms of tasks the peer gave us
 * @CLAWT_FED_TRANSCRIPTS_REACHABLE: the peer's conversations with every
 *   agent it may reach
 *
 * What a peer may read back.
 */
typedef enum {
    CLAWT_FED_TRANSCRIPTS_NONE = 0,
    CLAWT_FED_TRANSCRIPTS_OWN_TASKS,
    CLAWT_FED_TRANSCRIPTS_REACHABLE
} ClawtFedTranscripts;

GType clawt_fed_transcripts_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_FED_TRANSCRIPTS (clawt_fed_transcripts_get_type())

/**
 * CLAWT_FED_ACTION_CLASSES:
 *
 * How many #ClawtActionClass values a grant caps.
 */
#define CLAWT_FED_ACTION_CLASSES (CLAWT_ACTION_FLEET_ADMIN + 1)

/**
 * ClawtFedGrant:
 * @reach: which agents
 * @agents: (array zero-terminated=1): selectors for %CLAWT_FED_REACH_LISTED
 * @message: messages: allow, review (held for approval) or deny
 * @delegate: delegation: allow, ask or deny
 * @see_roster: the agent list and teams
 * @see_presence: who is working
 * @see_typing: who is typing where
 * @see_tasks: the state of tasks the peer is party to
 * @transcripts: what may be read back
 * @caps: the cap on work done for the peer, per action class
 * @budget_cents_per_day: spend on the peer's behalf; 0 is unlimited
 * @messages_per_hour: inbound messages; 0 is unlimited
 * @max_concurrent_tasks: tasks for the peer running at once; 0 is none
 *   at all when @delegate is not deny would be useless, so 0 is unlimited
 * @max_hops_in: how deep a chain arriving from the peer may go here
 * @subdelegate: whether work done for the peer may delegate onward
 * @join_rooms: whether the peer may be a member of shared rooms
 *
 * A resolved grant.  All limits are integers -- a limit has to have
 * exactly one value.
 */
typedef struct {
    ClawtFedReach        reach;
    gchar              **agents;
    ClawtPolicyMode      message;
    ClawtPolicyMode      delegate;
    gboolean             see_roster;
    gboolean             see_presence;
    gboolean             see_typing;
    gboolean             see_tasks;
    ClawtFedTranscripts  transcripts;
    ClawtPolicyMode      caps[CLAWT_FED_ACTION_CLASSES];
    gint                 budget_cents_per_day;
    gint                 messages_per_hour;
    gint                 max_concurrent_tasks;
    gint                 max_hops_in;
    gboolean             subdelegate;
    gboolean             join_rooms;
} ClawtFedGrant;

GType clawt_fed_grant_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_FED_GRANT (clawt_fed_grant_get_type())

/**
 * clawt_fed_grant_preset:
 * @tier: a tier
 *
 * Returns: (transfer full): the tier's grant, before any override
 */
ClawtFedGrant *clawt_fed_grant_preset(ClawtFedTier tier);

/**
 * clawt_fed_grant_copy:
 * @grant: a #ClawtFedGrant
 *
 * Returns: (transfer full): a deep copy
 */
ClawtFedGrant *clawt_fed_grant_copy(const ClawtFedGrant *grant);

/**
 * clawt_fed_grant_free:
 * @grant: (transfer full) (nullable): a #ClawtFedGrant
 */
void clawt_fed_grant_free(ClawtFedGrant *grant);

/**
 * ClawtFedFieldKind:
 * @CLAWT_FED_FIELD_BOOL: true or false
 * @CLAWT_FED_FIELD_INT: a non-negative integer
 * @CLAWT_FED_FIELD_ENUM: a nick of the field's enum type
 * @CLAWT_FED_FIELD_LIST: comma-separated selectors
 *
 * The value a grant field holds.
 */
typedef enum {
    CLAWT_FED_FIELD_BOOL = 0,
    CLAWT_FED_FIELD_INT,
    CLAWT_FED_FIELD_ENUM,
    CLAWT_FED_FIELD_LIST
} ClawtFedFieldKind;

/**
 * clawt_fed_grant_field_count:
 *
 * Returns: how many fields a grant has (one per action class for the caps)
 */
guint clawt_fed_grant_field_count(void);

/**
 * clawt_fed_grant_field_name:
 * @n: an index below clawt_fed_grant_field_count()
 *
 * Returns: (transfer none): the field's name: `reach`, `message`,
 *   `cap.exec`, `budget_cents_per_day`, ...
 */
const gchar *clawt_fed_grant_field_name(guint n);

/**
 * clawt_fed_grant_field_kind:
 * @n: an index below clawt_fed_grant_field_count()
 *
 * Returns: what the field holds
 */
ClawtFedFieldKind clawt_fed_grant_field_kind(guint n);

/**
 * clawt_fed_grant_field_enum_type:
 * @n: an index below clawt_fed_grant_field_count()
 *
 * Returns: the enum type of an enum field, %G_TYPE_INVALID otherwise --
 *   an editor walks its values for the dropdown
 */
GType clawt_fed_grant_field_enum_type(guint n);

/**
 * clawt_fed_grant_field_doc:
 * @n: an index below clawt_fed_grant_field_count()
 *
 * Returns: (transfer none): one sentence on what the field does
 */
const gchar *clawt_fed_grant_field_doc(guint n);

/**
 * clawt_fed_grant_field_find:
 * @name: a field name
 *
 * Returns: the field's index, or -1 for no such field
 */
gint clawt_fed_grant_field_find(const gchar *name);

/**
 * clawt_fed_grant_get:
 * @grant: a #ClawtFedGrant
 * @n: an index below clawt_fed_grant_field_count()
 *
 * Returns: (transfer full): the field's value, in the spelling
 *   clawt_fed_grant_set() reads
 */
gchar *clawt_fed_grant_get(const ClawtFedGrant *grant,
                           guint                n);

/**
 * clawt_fed_grant_set:
 * @grant: a #ClawtFedGrant
 * @name: a field name
 * @value: its new value
 * @error: (out) (optional): return location for a #GError
 *
 * Dispatches on the field's kind.  An unknown field or a value its kind
 * cannot hold is refused, naming the accepted spellings -- never stored
 * as something else.
 *
 * Returns: %TRUE if the field was set
 */
gboolean clawt_fed_grant_set(ClawtFedGrant *grant,
                             const gchar   *name,
                             const gchar   *value,
                             GError       **error);

/**
 * clawt_fed_grant_to_json:
 * @grant: a #ClawtFedGrant
 *
 * Returns: (transfer full): one member per field, as strings
 */
JsonObject *clawt_fed_grant_to_json(const ClawtFedGrant *grant);

/**
 * clawt_fed_grant_resolve:
 * @tier: the peer's tier
 * @rules: (array zero-terminated=1) (nullable): `federation.rules`, each
 *   `SELECTOR: field=value[, field=value...]`
 * @peer: (nullable): the peer as a selector reads it (alias, label, tier,
 *   owner, tags, health)
 * @overrides: (nullable): the peer's own overrides, field -> value
 * @out_explain: (out) (optional) (transfer full) (element-type utf8 utf8): field -> where its value
 *   came from (`tier trusted`, `rule 2: tag:acme`, `override`)
 * @out_problems: (out) (optional) (transfer full) (element-type utf8): one line per rule or
 *   override that could not be applied
 *
 * Tier preset, then each matching rule in order, then the overrides.
 * Pure.  A rule or override that fails to apply is skipped and reported,
 * never half-applied.
 *
 * Returns: (transfer full): the grant
 */
ClawtFedGrant *clawt_fed_grant_resolve(ClawtFedTier   tier,
                                       gchar        **rules,
                                       JsonObject    *peer,
                                       JsonObject    *overrides,
                                       GHashTable   **out_explain,
                                       GPtrArray    **out_problems);

/**
 * clawt_fed_grant_reaches:
 * @grant: a resolved grant
 * @agent_id: one of our agents
 * @team: (nullable): its team
 * @tags: (array zero-terminated=1) (nullable): its team's tags
 * @inbox_agent: (nullable): `federation.inbox_agent`, resolved
 * @agent_reach: (array zero-terminated=1) (nullable): the agent's own
 *   `federation_reach`; when set, the peer must match it as well
 * @peer: (nullable): the peer, for @agent_reach
 *
 * Whether the peer may address @agent_id.  The agent's own list can only
 * take reach away.
 *
 * Returns: %TRUE if it may
 */
gboolean clawt_fed_grant_reaches(const ClawtFedGrant *grant,
                                 const gchar         *agent_id,
                                 const gchar         *team,
                                 gchar              **tags,
                                 const gchar         *inbox_agent,
                                 gchar              **agent_reach,
                                 JsonObject          *peer);

/**
 * clawt_fed_policy_stricter:
 * @a: a mode
 * @b: another
 *
 * The stricter of two modes, for combining an agent's own rule with the
 * cap a peer's grant puts on work done for it.  Deny over hand-back over
 * ask over review over allow.
 *
 * Returns: the stricter
 */
ClawtPolicyMode clawt_fed_policy_stricter(ClawtPolicyMode a,
                                          ClawtPolicyMode b);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtFedGrant, clawt_fed_grant_free)

G_END_DECLS
