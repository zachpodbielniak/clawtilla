/*
 * clawt-lrg-agent-model.h - What the Agent, Mailbox and Computer pages decide
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything lrg-inspector.c, lrg-mailbox.c, lrg-computer.c and
 * lrg-hold.c decide -- which rows a reply becomes, which choices a combo
 * offers and which one it starts on, which settings a save sends and in
 * what order, what a toast says -- lives behind this header, in
 * lrg-agent-model.c, which includes no libregnum header.
 * tests/test-lrg-models-agent.c checks it with no display and no daemon.
 */

#pragma once

#include <clawtilla.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/*
 * The id a combo uses for "a value typed by hand".  Not a value anything
 * can be called: it starts with a control character, so no image
 * reference, model name or path collides with it.
 */
#define CLAWT_LRG_AGENT_CHOICE_OTHER "\x1f" "other"

/* ── Choices ──────────────────────────────────────────────────────── */

/**
 * ClawtLrgAgentChoices:
 * @ids: (element-type utf8): the values, %NULL-terminated
 * @labels: (element-type utf8): what each is shown as, %NULL-terminated
 * @selected: (nullable): the id to start on
 * @other: (nullable): the text for the free entry when @selected is
 *   %CLAWT_LRG_AGENT_CHOICE_OTHER
 *
 * One combo's worth of choices.  Parallel arrays rather than a label
 * list, because a combo read back by its label breaks the day two
 * choices share one.
 */
typedef struct {
    GPtrArray *ids;
    GPtrArray *labels;
    gchar     *selected;
    gchar     *other;
} ClawtLrgAgentChoices;

ClawtLrgAgentChoices *clawt_lrg_agent_choices_new(void);
void                  clawt_lrg_agent_choices_free(
                          ClawtLrgAgentChoices *choices);
void                  clawt_lrg_agent_choices_add(
                          ClawtLrgAgentChoices *choices,
                          const gchar          *id,
                          const gchar          *label);
gboolean              clawt_lrg_agent_choices_has(
                          ClawtLrgAgentChoices *choices,
                          const gchar          *id);
const gchar *const   *clawt_lrg_agent_choices_ids(
                          ClawtLrgAgentChoices *choices);
const gchar *const   *clawt_lrg_agent_choices_labels(
                          ClawtLrgAgentChoices *choices);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtLrgAgentChoices, clawt_lrg_agent_choices_free)

/**
 * clawt_lrg_agent_choice_value:
 * @combo_id: (nullable): the id the combo has selected
 * @other_text: (nullable): what the free entry holds
 *
 * Returns: (transfer full) (nullable): the value a combo stands for --
 *   the typed text for %CLAWT_LRG_AGENT_CHOICE_OTHER (%NULL when it is
 *   empty), else the id
 */
gchar *clawt_lrg_agent_choice_value(const gchar *combo_id,
                                    const gchar *other_text);

/* Walked from @type; @current is kept even when the type lacks it. */
ClawtLrgAgentChoices *clawt_lrg_agent_enum_choices(GType        type,
                                                   const gchar *current);

ClawtLrgAgentChoices *clawt_lrg_agent_computer_type_choices(const gchar *current);
ClawtLrgAgentChoices *clawt_lrg_agent_effort_choices(const gchar *current);
ClawtLrgAgentChoices *clawt_lrg_agent_resolution_choices(const gchar *current);
ClawtLrgAgentChoices *clawt_lrg_agent_mount_mode_choices(void);
ClawtLrgAgentChoices *clawt_lrg_agent_relabel_choices(void);

/* From a team.list, account.list or computer.shared reply. */
ClawtLrgAgentChoices *clawt_lrg_agent_team_choices(JsonNode    *reply,
                                              const gchar *current);
ClawtLrgAgentChoices *clawt_lrg_agent_account_choices(JsonNode    *reply,
                                                 const gchar *current);
ClawtLrgAgentChoices *clawt_lrg_agent_shared_choices(JsonNode    *reply,
                                                const gchar *current);

/* From image.list (references) and image.vm_list (paths). */
ClawtLrgAgentChoices *clawt_lrg_agent_image_choices(JsonNode    *reply,
                                               const gchar *current);
ClawtLrgAgentChoices *clawt_lrg_agent_disk_choices(JsonNode    *reply,
                                              const gchar *current);
gchar           *clawt_lrg_agent_image_note(JsonNode    *reply,
                                            const gchar *id);
gchar           *clawt_lrg_agent_disk_note(JsonNode    *reply,
                                           const gchar *id);

/* From a model.list reply. */
ClawtLrgAgentChoices *clawt_lrg_agent_provider_choices(JsonNode    *catalog,
                                                  const gchar *current);
ClawtLrgAgentChoices *clawt_lrg_agent_model_choices(JsonNode    *catalog,
                                               const gchar *provider,
                                               const gchar *current);
const gchar     *clawt_lrg_agent_provider_note(JsonNode    *catalog,
                                               const gchar *provider);

/* ── The fallback list ────────────────────────────────────────────── */

/**
 * clawt_lrg_agent_fallback_account_choices:
 * @reply: (nullable): an account.list reply
 * @provider: (nullable): the CLI the entry runs
 * @current: (nullable): the account the entry names
 *
 * The logins one `model.fallbacks` entry can name: the CLI's own, the
 * accounts clawt_model_fallback_account_fits() says are for @provider,
 * and @current whatever it is.
 *
 * Returns: (transfer full): the choices
 */
ClawtLrgAgentChoices *clawt_lrg_agent_fallback_account_choices(
                          JsonNode    *reply,
                          const gchar *provider,
                          const gchar *current);

/**
 * clawt_lrg_agent_fallback_edit:
 * @list: (element-type ClawtModelFallback): the entries, edited in place
 * @op: "add", "remove", "up" or "down"
 * @index: the entry @op is about (ignored by "add")
 * @provider: (nullable): where a new entry starts
 *
 * One of the editor's buttons.  A new entry has no model yet, which
 * clawt_lrg_agent_fallback_spell() refuses until somebody picks one.
 *
 * Returns: %TRUE when @list changed
 */
gboolean clawt_lrg_agent_fallback_edit(GPtrArray   *list,
                                       const gchar *op,
                                       guint        index,
                                       const gchar *provider);

/**
 * clawt_lrg_agent_fallback_spell:
 * @list: (nullable) (element-type ClawtModelFallback): the entries
 * @error: where to say which entry is incomplete
 *
 * The list in the library's one spelling, or %NULL when an entry lacks
 * its provider or its model -- refused rather than dropped, since a
 * shorter list than the one on screen would be saved without a word.
 *
 * Returns: (transfer full) (nullable): the text for agent.set
 */
gchar *clawt_lrg_agent_fallback_spell(GPtrArray *list, GError **error);

/* ── The settings form ────────────────────────────────────────────── */

/**
 * ClawtLrgAgentFieldKind:
 * @CLAWT_LRG_AGENT_FIELD_ENTRY: a one-line text entry
 * @CLAWT_LRG_AGENT_FIELD_PROSE: a multi-line editor (a prompt)
 * @CLAWT_LRG_AGENT_FIELD_SWITCH: a boolean
 * @CLAWT_LRG_AGENT_FIELD_ENUM: a combo walked from the entry's #GType
 * @CLAWT_LRG_AGENT_FIELD_SECRET: an entry never prefilled; empty means unchanged
 * @CLAWT_LRG_AGENT_FIELD_LIST: comma-separated strings
 * @CLAWT_LRG_AGENT_FIELD_TEAM: a combo from team.list
 * @CLAWT_LRG_AGENT_FIELD_ACCOUNT: a combo from account.list
 * @CLAWT_LRG_AGENT_FIELD_SHARED: a combo from computer.shared
 * @CLAWT_LRG_AGENT_FIELD_PROVIDER: a combo from model.list
 * @CLAWT_LRG_AGENT_FIELD_MODEL: a combo from model.list, for the chosen provider
 * @CLAWT_LRG_AGENT_FIELD_EFFORT: a combo of effort levels
 * @CLAWT_LRG_AGENT_FIELD_COMPUTER_TYPE: a combo walked from the computer types
 * @CLAWT_LRG_AGENT_FIELD_IMAGE: a combo from image.list, plus a typed reference
 * @CLAWT_LRG_AGENT_FIELD_DISK: a combo from image.vm_list, plus a typed path
 * @CLAWT_LRG_AGENT_FIELD_RESOLUTION: a combo of screen sizes
 *
 * Which control a setting is drawn with.
 */
typedef enum {
    CLAWT_LRG_AGENT_FIELD_ENTRY = 0,
    CLAWT_LRG_AGENT_FIELD_PROSE,
    CLAWT_LRG_AGENT_FIELD_SWITCH,
    CLAWT_LRG_AGENT_FIELD_ENUM,
    CLAWT_LRG_AGENT_FIELD_SECRET,
    CLAWT_LRG_AGENT_FIELD_LIST,
    CLAWT_LRG_AGENT_FIELD_TEAM,
    CLAWT_LRG_AGENT_FIELD_ACCOUNT,
    CLAWT_LRG_AGENT_FIELD_SHARED,
    CLAWT_LRG_AGENT_FIELD_PROVIDER,
    CLAWT_LRG_AGENT_FIELD_MODEL,
    CLAWT_LRG_AGENT_FIELD_EFFORT,
    CLAWT_LRG_AGENT_FIELD_COMPUTER_TYPE,
    CLAWT_LRG_AGENT_FIELD_IMAGE,
    CLAWT_LRG_AGENT_FIELD_DISK,
    CLAWT_LRG_AGENT_FIELD_RESOLUTION
} ClawtLrgAgentFieldKind;

/**
 * ClawtLrgAgentField:
 * @key: the agent-relative key agent.set takes
 * @label: what the control is called
 * @hint: (nullable): the first line of the schema's documentation
 * @value: what the daemon reported ("" when unset)
 * @kind: which control
 * @enum_type: the #GType for %CLAWT_LRG_AGENT_FIELD_ENUM, else %G_TYPE_INVALID
 * @dangerous: the schema marks it as handing over real authority
 *
 * One setting on the Overview page.
 */
typedef struct {
    gchar             *key;
    gchar             *label;
    gchar             *hint;
    gchar             *value;
    ClawtLrgAgentFieldKind  kind;
    GType              enum_type;
    gboolean           dangerous;
} ClawtLrgAgentField;

/**
 * ClawtLrgAgentGroup:
 * @prefix: the keys' first component, "" for bare keys
 * @title: the heading
 * @summary: (nullable): one sentence under it
 * @fields: (element-type ClawtLrgAgentField): its settings, schema order
 *
 * One card of settings.
 */
typedef struct {
    gchar     *prefix;
    gchar     *title;
    gchar     *summary;
    GPtrArray *fields;
} ClawtLrgAgentGroup;

/**
 * clawt_lrg_agent_form:
 * @settings: (nullable): agent.show's `settings`
 *
 * Every setting an agent can carry, walked from the schema
 * (clawt_config_schema_get() and clawt_config_schema_agent_name()),
 * grouped by first component: the known groups first in a fixed order,
 * then any other in the order the schema first names it.  Keys the page
 * edits elsewhere (integrations, mounts) and structure (sections,
 * mappings) are left out; on a shared machine every computer.* key but
 * computer.use is too, because those are the machine's.
 *
 * Returns: (transfer full) (element-type ClawtLrgAgentGroup): the groups
 */
GPtrArray *clawt_lrg_agent_form(JsonObject *settings);

gboolean          clawt_lrg_agent_key_skipped(const gchar            *key,
                                              const ClawtSchemaEntry *entry);
gboolean          clawt_lrg_agent_key_belongs_to_shared(const gchar *key,
                                                        const gchar *use);
ClawtLrgAgentFieldKind clawt_lrg_agent_field_kind(const gchar            *key,
                                             const ClawtSchemaEntry *entry);
gchar            *clawt_lrg_agent_key_label(const gchar *key);

/**
 * ClawtLrgAgentSetting:
 * @key: an agent-relative key
 * @value: (nullable): the value to write; %NULL means "leave it"
 *
 * One `agent.set`.
 */
typedef struct {
    gchar *key;
    gchar *value;
} ClawtLrgAgentSetting;

ClawtLrgAgentSetting *clawt_lrg_agent_setting_new(const gchar *key, const gchar *value);
void             clawt_lrg_agent_setting_free(gpointer setting);

/**
 * clawt_lrg_agent_save_plan:
 * @posted: (element-type ClawtLrgAgentSetting): what each control holds, in
 *   schema order
 * @settings: (nullable): what agent.show reported
 *
 * The `agent.set` calls a save makes, in order: only what changed; never
 * a computer.* key the (new) shared machine owns; a %NULL value never;
 * and the account cleared first when it changes from one login to
 * another, because the daemon checks the provider/account pair and a
 * new provider set while the old login is still named is refused.
 * `model.fallbacks` goes last, and comes off first when the provider or
 * the account changes in the same save, for the same reason one level
 * up: the daemon checks the list against the backend it falls back from.
 *
 * Returns: (transfer full) (element-type ClawtLrgAgentSetting): the plan
 */
GPtrArray *clawt_lrg_agent_save_plan(GPtrArray  *posted,
                                     JsonObject *settings);

/* "Saved." with the restart advice that applies. */
gchar *clawt_lrg_agent_saved_text(guint changed, gboolean restart_required);

/* ── At a glance ──────────────────────────────────────────────────── */

/* The tone a state nick is drawn in. */
const gchar *clawt_lrg_agent_state_tone(const gchar *state);

/**
 * ClawtLrgAgentVerb:
 * @label: the button's text
 * @kind: the frame kind it sends
 * @enabled: whether it can work now
 * @why_not: (nullable): why not, said beside a disabled button
 *
 * One of Start, Stop and Restart.
 */
typedef struct {
    const gchar *label;
    const gchar *kind;
    gboolean     enabled;
    const gchar *why_not;
} ClawtLrgAgentVerb;

#define CLAWT_LRG_AGENT_VERBS (3)

void clawt_lrg_agent_verbs(const gchar       *state,
                           ClawtLrgAgentVerb  out[CLAWT_LRG_AGENT_VERBS]);

/* "N bytes of LIMIT", or %NULL when there is no count. */
gchar       *clawt_lrg_agent_identity_text(JsonObject *identity);
const gchar *clawt_lrg_agent_identity_tone(JsonObject *identity);

/* This agent's row of a usage.summary, as one line; %NULL if absent. */
gchar *clawt_lrg_agent_usage_text(JsonNode *reply, const gchar *agent_id);

gchar *clawt_lrg_agent_removed_text(const gchar *agent_id, JsonNode *reply);
gchar *clawt_lrg_agent_reset_text(JsonNode *reply);

/* "prompt · title" for a file agent.files reports. */
gchar *clawt_lrg_agent_file_subtitle(JsonObject *file);

/* ── Integrations ─────────────────────────────────────────────────── */

/**
 * ClawtLrgAgentIntegrationRow:
 * @name: the instance or block name
 * @subtitle: type, scope and any problem
 * @shared: a shared instance (has a switch) rather than an inline block
 * @covers: the instance reaches this agent
 *
 * One line of the Integrations card.
 */
typedef struct {
    gchar    *name;
    gchar    *subtitle;
    gboolean  shared;
    gboolean  covers;
} ClawtLrgAgentIntegrationRow;

void       clawt_lrg_agent_integration_row_free(gpointer row);
GPtrArray *clawt_lrg_agent_integration_rows(JsonNode *reply);

/**
 * clawt_lrg_agent_integration_update:
 * @list: an integration.list reply
 * @name: the instance
 * @agent_id: the agent
 * @wanted: whether it should reach @agent_id
 * @refusal: (out) (optional): why nothing should be sent
 *
 * The `integration.update` payload that adds @agent_id to, or takes it
 * off, the instance's `agents` list.  An instance set to every agent is
 * refused rather than rewritten as everyone-but-one: that is a decision
 * about the whole fleet, which the switch does not appear to make.
 *
 * Returns: (transfer full) (nullable): the payload
 */
JsonNode *clawt_lrg_agent_integration_update(JsonNode     *list,
                                             const gchar  *name,
                                             const gchar  *agent_id,
                                             gboolean      wanted,
                                             gchar       **refusal);

/* ── Mounts ───────────────────────────────────────────────────────── */

gchar       *clawt_lrg_agent_mount_subtitle(JsonObject *mount);
gchar       *clawt_lrg_computer_mount_title(JsonObject *mount);
const gchar *clawt_lrg_agent_mounts_summary(const gchar *computer_type);
const gchar *clawt_lrg_agent_mount_refusal(const gchar *source,
                                           const gchar *target);

/* ── Mailbox ──────────────────────────────────────────────────────── */

/**
 * ClawtLrgMailboxRow:
 * @id: the item id, for ack and requeue
 * @title: who it is from
 * @body: what it says
 * @badge: (nullable): a pill before it
 * @badge_tone: the pill's tone
 * @follow_up: a follow-up held while the agent is mid-turn
 * @dead: it ran out of attempts
 * @can_ack: acking it means something
 * @can_requeue: it can be put back
 *
 * One mailbox line.
 */
typedef struct {
    gchar    *id;
    gchar    *title;
    gchar    *body;
    gchar    *badge;
    gchar    *badge_tone;
    gboolean  follow_up;
    gboolean  dead;
    gboolean  can_ack;
    gboolean  can_requeue;
} ClawtLrgMailboxRow;

void       clawt_lrg_mailbox_row_free(gpointer row);
GPtrArray *clawt_lrg_mailbox_rows(JsonNode *list, JsonNode *dead);
gchar     *clawt_lrg_mailbox_summary(JsonNode *list);
const gchar *clawt_lrg_mailbox_empty_text(const gchar *agent_state);
gchar     *clawt_lrg_mailbox_purged_text(JsonNode *reply);

/* ── Computer ─────────────────────────────────────────────────────── */

gboolean  clawt_lrg_computer_present(JsonObject *agent);
gboolean  clawt_lrg_computer_offers_lifecycle(JsonObject *agent);
gboolean  clawt_lrg_computer_offers_screen(JsonObject *agent);
gchar    *clawt_lrg_computer_state_text(JsonObject *agent, JsonNode *status);
gchar    *clawt_lrg_computer_screen_text(JsonObject *status, gint64 now_us);
gchar    *clawt_lrg_computer_exec_output(JsonNode *reply);
GPtrArray *clawt_lrg_computer_exchange_names(JsonNode *reply);
gchar    *clawt_lrg_computer_lifecycle_text(const gchar *agent_id,
                                            JsonNode    *reply);
gchar    *clawt_lrg_computer_stop_warning(const gchar *agent_id);

/**
 * clawt_lrg_computer_input_payload:
 * @agent_id: the agent
 * @text: (nullable): what to type; "@name" is a key or combo
 * @click: (nullable): "x,y"
 * @refusal: (out) (optional): why there is nothing to send
 *
 * One `computer.input`, from whichever field was filled -- typing first,
 * then a click, as the GTK form does.
 *
 * Returns: (transfer full) (nullable): the payload
 */
JsonNode *clawt_lrg_computer_input_payload(const gchar  *agent_id,
                                           const gchar  *text,
                                           const gchar  *click,
                                           gchar       **refusal);

/* ── Holding the fleet ────────────────────────────────────────────── */

/* Whether a control.status / control.pause reply says the fleet is held. */
gboolean clawt_lrg_hold_is_held(JsonNode *reply);
gchar   *clawt_lrg_hold_paused_text(JsonNode *reply);

/* The newer version control.status reports, when one is available. */
gchar   *clawt_lrg_hold_available_update(JsonNode *reply);

G_END_DECLS
