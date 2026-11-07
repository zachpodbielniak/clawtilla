/*
 * clawt-lrg-settings-model.h - What the Settings dialog and the create
 *                              dialogs decide, without libregnum
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every sentence a settings row says, every payload a form sends and
 * every refusal a form gives before it sends anything lives here, in
 * lrg-settings-model.c, which includes no libregnum header.  The view
 * files only draw what these return and send what these build.
 *
 * Two reasons, the same two as clawt-lrg-model.h: the decisions are
 * checked by tests/test-lrg-models-settings.c on a machine with no
 * display, and a rule written once here is a rule the drawing code
 * cannot quietly decide a second way in another file.  The GTK client
 * keeps most of these inline beside its widgets, which is how its
 * integration saver and its integration builder came to disagree about
 * which fields existed.
 *
 * Every name here is prefixed clawt_lrg_settings_ or clawt_lrg_create_
 * because the whole client links into one binary and other pages keep
 * models of their own.
 */

#pragma once

#include <clawtilla.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ── Shared ───────────────────────────────────────────────────────── */

/**
 * clawt_lrg_settings_member_array:
 * @object: (nullable): a reply object
 * @member: the member to read
 *
 * Returns: (transfer none) (nullable): the array, or %NULL when the
 *   member is missing or is not an array -- never an assertion, which
 *   is what json_object_get_array_member() gives an older daemon
 */
JsonArray *clawt_lrg_settings_member_array(JsonObject  *object,
                                           const gchar *member);

/**
 * clawt_lrg_settings_join_strings:
 * @object: (nullable): an object
 * @member: an array-of-strings member
 * @separator: what goes between them
 *
 * Returns: (transfer full): the strings joined; "" when there are none
 */
gchar *clawt_lrg_settings_join_strings(JsonObject  *object,
                                       const gchar *member,
                                       const gchar *separator);

/**
 * clawt_lrg_settings_split_list:
 * @text: (nullable): comma-separated text a person typed
 *
 * Splits, trims and drops empty items.  A comma because these are ids
 * and room addresses, which never contain one.
 *
 * Returns: (transfer full): the items; empty for %NULL or ""
 */
GStrv clawt_lrg_settings_split_list(const gchar *text);

/* ── Cloud images ─────────────────────────────────────────────────── */

/**
 * clawt_lrg_settings_human_size:
 * @bytes: a size
 *
 * Returns: (transfer full): "1.2 GB", "557 MB", "16 kB" or "12 B"
 */
gchar *clawt_lrg_settings_human_size(gint64 bytes);

/**
 * clawt_lrg_settings_image_downloading:
 * @image: one entry of `image.vm_list`'s `images`
 *
 * Returns: whether it is still arriving
 */
gboolean clawt_lrg_settings_image_downloading(JsonObject *image);

/**
 * clawt_lrg_settings_image_subtitle:
 * @image: one entry of `image.vm_list`'s `images`
 *
 * "16 kB of 557 MB" while downloading, "557 MB — https://…" once it is
 * here.  The progress text and the bar move together: a bar at a fifth
 * beside the size the row was built with reads as a broken bar.
 *
 * Returns: (transfer full): the subtitle
 */
gchar *clawt_lrg_settings_image_subtitle(JsonObject *image);

/**
 * clawt_lrg_settings_progress_text:
 * @done: bytes so far
 * @total: bytes expected
 *
 * Returns: (transfer full): "16 kB of 557 MB"
 */
gchar *clawt_lrg_settings_progress_text(gint64 done, gint64 total);

/**
 * clawt_lrg_settings_progress_fraction:
 * @done: bytes so far
 * @total: bytes expected, or 0 when unknown
 *
 * Returns: 0..1, or -1 when the total is unknown (an indeterminate bar)
 */
gdouble clawt_lrg_settings_progress_fraction(gint64 done, gint64 total);

/**
 * clawt_lrg_settings_image_failed_text:
 * @name: the image
 * @failure: (nullable): what `image.finished` said went wrong
 *
 * Returns: (transfer full) (nullable): the sentence, or %NULL when it
 *   did not fail
 */
gchar *clawt_lrg_settings_image_failed_text(const gchar *name,
                                            const gchar *failure);

/* ── Spending ─────────────────────────────────────────────────────── */

/**
 * clawt_lrg_settings_period_count:
 *
 * The windows the Spending page offers, "All time" first: the first
 * question is what the fleet has cost at all, and a narrower window
 * that happened to be empty would look like a feature that does not
 * work.
 *
 * Returns: how many
 */
guint clawt_lrg_settings_period_count(void);

/**
 * clawt_lrg_settings_period_label:
 * @n: an index below clawt_lrg_settings_period_count()
 *
 * Returns: (transfer none): the label
 */
const gchar *clawt_lrg_settings_period_label(guint n);

/**
 * clawt_lrg_settings_period_since:
 * @n: an index below clawt_lrg_settings_period_count()
 * @now: the current local time
 *
 * Returns: the `since` to send, in unix *seconds* as `usage.summary`
 *   wants; 0 for everything
 */
gint64 clawt_lrg_settings_period_since(guint n, GDateTime *now);

/**
 * clawt_lrg_settings_usage_payload:
 * @since: unix seconds, or 0
 *
 * Returns: (transfer full): the `usage.summary` request, with `since`
 *   as a JSON integer
 */
JsonNode *clawt_lrg_settings_usage_payload(gint64 since);

/**
 * clawt_lrg_settings_spend_subtitle:
 * @agent: one entry of `usage.summary`'s `agents`
 *
 * Returns: (transfer full): "3 turns, 1200 tokens out", or that nothing
 *   was recorded
 */
gchar *clawt_lrg_settings_spend_subtitle(JsonObject *agent);

/**
 * clawt_lrg_settings_spend_total_subtitle:
 * @total: (nullable): `usage.summary`'s `total`
 *
 * Returns: (transfer full): "12 turns across the fleet"
 */
gchar *clawt_lrg_settings_spend_total_subtitle(JsonObject *total);

/**
 * clawt_lrg_settings_spend_cost:
 * @object: (nullable): an agent or the total
 *
 * Returns: (transfer full): its `cost_micros` as money
 */
gchar *clawt_lrg_settings_spend_cost(JsonObject *object);

/* ── Teams ────────────────────────────────────────────────────────── */

/**
 * clawt_lrg_settings_team_subtitle:
 * @team: one entry of `team.list`'s `teams`
 *
 * The description first, and a blank one said as what it costs:
 * the chief of staff routes work by reading it, so a team without one
 * is sent nothing.
 *
 * Returns: (transfer full): the subtitle
 */
gchar *clawt_lrg_settings_team_subtitle(JsonObject *team);

/**
 * clawt_lrg_settings_team_removed_text:
 * @orphaned: how many agents still name the team
 *
 * Returns: (transfer full): the toast after `team.remove`
 */
gchar *clawt_lrg_settings_team_removed_text(gint64 orphaned);

/* ── Shared folders ───────────────────────────────────────────────── */

/**
 * clawt_lrg_settings_folder_scope_text:
 * @mount: one entry of `defaults.mount.list`'s `mounts`
 *
 * Who a folder reaches, on every row -- a list where most rows say
 * nothing invites reading the silent ones as unknown.
 *
 * Returns: (transfer full): "every agent", "team backend, agent kudu",
 *   "nobody (no list)"
 */
gchar *clawt_lrg_settings_folder_scope_text(JsonObject *mount);

/**
 * clawt_lrg_settings_folder_subtitle:
 * @mount: one entry of `defaults.mount.list`'s `mounts`
 *
 * Both paths, always, and the relabel when it is not the default: an
 * agent's tools run on the host and its shell inside the computer, so a
 * folder has two names.
 *
 * Returns: (transfer full): the subtitle
 */
gchar *clawt_lrg_settings_folder_subtitle(JsonObject *mount);

/**
 * clawt_lrg_settings_relabel_label:
 * @nick: (nullable): a relabel nick, or %NULL / "" for the default
 *
 * Returns: (transfer none): the wording, walked from clawt_relabel_*()
 */
const gchar *clawt_lrg_settings_relabel_label(const gchar *nick);

/**
 * clawt_lrg_settings_folder_add_payload:
 * @source: the host path
 * @target: (nullable): the path inside; empty means the same one
 * @writable: rw or ro
 * @who: (nullable): teams or agents, comma separated; empty means all
 * @relabel: (nullable): a relabel nick, or %NULL / "" to send none
 * @error: (out) (optional): why the form is refused
 *
 * The names in @who go up as one `who` list for the daemon to sort into
 * teams and agents: it answers the same question clawt_mount_covers()
 * does, and a client sorting them itself filed every undeclared team
 * under agents, where it matched nothing.
 *
 * Returns: (transfer full) (nullable): the `defaults.mount.add` request
 */
JsonNode *clawt_lrg_settings_folder_add_payload(const gchar  *source,
                                                const gchar  *target,
                                                gboolean      writable,
                                                const gchar  *who,
                                                const gchar  *relabel,
                                                GError      **error);

/* ── Integrations ─────────────────────────────────────────────────── */

/**
 * clawt_lrg_settings_scope_count:
 *
 * Who an integration reaches, walked from #ClawtScope rather than kept
 * as a list of three here.
 *
 * Returns: how many scopes
 */
guint clawt_lrg_settings_scope_count(void);

/**
 * clawt_lrg_settings_scope_nth_nick:
 * @n: an index below clawt_lrg_settings_scope_count()
 *
 * Returns: (transfer none): the nick, widest first ("all")
 */
const gchar *clawt_lrg_settings_scope_nth_nick(guint n);

/**
 * clawt_lrg_settings_scope_label:
 * @nick: a scope nick
 *
 * Returns: (transfer none): "Every agent", "These agents", "Nobody", or
 *   @nick itself for a scope this build has no wording for
 */
const gchar *clawt_lrg_settings_scope_label(const gchar *nick);

/**
 * clawt_lrg_settings_integration_subtitle:
 * @integration: one entry of `integration.list`'s `integrations`
 *
 * Returns: (transfer full): "Matrix — every agent (3)", with " (off)"
 *   when it is disabled
 */
gchar *clawt_lrg_settings_integration_subtitle(JsonObject *integration);

/**
 * clawt_lrg_settings_integration_type_text:
 * @type_id: an integration type id
 * @summary: (nullable): what `integration.types` said it is for
 *
 * What the chosen type does and what it will ask for, for the moment
 * before somebody picks one.
 *
 * Returns: (transfer full): the text
 */
gchar *clawt_lrg_settings_integration_type_text(const gchar *type_id,
                                                const gchar *summary);

/**
 * clawt_lrg_settings_find_integration:
 * @reply: (nullable): an `integration.list` reply
 * @name: the instance
 *
 * Returns: (transfer none) (nullable): the instance
 */
JsonObject *clawt_lrg_settings_find_integration(JsonNode    *reply,
                                                const gchar *name);

/**
 * clawt_lrg_settings_field_slot:
 * @field: a field from clawt_integration_fields()
 *
 * Where the form keeps this field's value: its key, or its label when
 * it is conditional -- two fields can share a key (notify's `url` is a
 * topic under ntfy and a server under Gotify), and both need a control.
 *
 * Returns: (transfer none): the slot
 */
const gchar *clawt_lrg_settings_field_slot(const ClawtIntegrationField *field);

/**
 * clawt_lrg_settings_flag_slot:
 * @field: a %CLAWT_FIELD_FLAGS field
 * @choice: one of its choices
 *
 * Returns: (transfer full): "events.question" and so on
 */
gchar *clawt_lrg_settings_flag_slot(const ClawtIntegrationField *field,
                                    const gchar                 *choice);

/**
 * clawt_lrg_settings_field_initial:
 * @field: a field (not %CLAWT_FIELD_FLAGS)
 * @integration: (nullable): the instance as the daemon has it
 *
 * What the form opens with, as text: "true"/"false" for a boolean, the
 * chosen value for a choice, a list joined with ", ", a number
 * formatted (a port is a JSON integer, and the string reader answers
 * one with its fallback -- every port would have opened empty), and
 * always "" for a secret, whose value never comes back.
 *
 * Returns: (transfer full): the value
 */
gchar *clawt_lrg_settings_field_initial(const ClawtIntegrationField *field,
                                        JsonObject                  *integration);

/**
 * clawt_lrg_settings_flag_initial:
 * @field: a %CLAWT_FIELD_FLAGS field
 * @choice: one of its choices
 * @integration: (nullable): the instance
 *
 * Returns: whether @choice starts ticked; an instance with no list is
 *   on the default, question and error
 */
gboolean clawt_lrg_settings_flag_initial(const ClawtIntegrationField *field,
                                         const gchar                 *choice,
                                         JsonObject                  *integration);

/**
 * clawt_lrg_settings_when_value:
 * @type_id: the integration type
 * @values: slot -> text, the form's current state
 * @when_key: (nullable): the deciding field's key
 *
 * The deciding field's value, resolved to its default when the form has
 * none -- passing %NULL through would hide every field of the backend
 * the instance is actually on.
 *
 * Returns: (transfer none) (nullable): the value
 */
const gchar *clawt_lrg_settings_when_value(const gchar *type_id,
                                           GHashTable  *values,
                                           const gchar *when_key);

/**
 * ClawtLrgIntegrationForm:
 * @name: the instance
 * @type_id: its type
 * @enabled: the Enabled switch
 * @description: (nullable): what it is for
 * @scope: a scope nick
 * @agents: (array zero-terminated=1) (nullable): the ticked agents, sent
 *   whatever the scope so switching to everyone and back loses nothing
 * @values: slot -> text for every field control
 *
 * The integration editor's state, as text.
 */
typedef struct {
    const gchar        *name;
    const gchar        *type_id;
    gboolean            enabled;
    const gchar        *description;
    const gchar        *scope;
    const gchar *const *agents;
    GHashTable         *values;
} ClawtLrgIntegrationForm;

/**
 * clawt_lrg_settings_integration_payload:
 * @form: the editor's state
 * @error: (out) (optional): why it is refused
 *
 * The `integration.update` request, built by walking the same
 * clawt_integration_fields() table and the same applies-predicate the
 * form was drawn from -- so a field that was not shown is not saved.
 * A secret goes up as a reference (env:NAME, file:PATH), and an empty
 * one means keep what is set.
 *
 * Returns: (transfer full) (nullable): the request
 */
JsonNode *clawt_lrg_settings_integration_payload(
    const ClawtLrgIntegrationForm *form,
    GError                       **error);

/**
 * clawt_lrg_settings_health_text:
 * @check: one entry of `integration.health`'s `checks`
 *
 * Returns: (transfer none): "It answered." or what went wrong
 */
const gchar *clawt_lrg_settings_health_text(JsonObject *check);

/* ── Connectors ───────────────────────────────────────────────────── */

/**
 * clawt_lrg_settings_connector_state:
 * @entry: one entry of `connector.list`'s `connectors`
 * @now: unix seconds, or 0 to read the clock
 *
 * Returns: the state, through clawt_connector_state()
 */
ClawtConnectorState clawt_lrg_settings_connector_state(JsonObject *entry,
                                                       gint64      now);

/**
 * clawt_lrg_settings_connector_tone:
 * @state: a state
 *
 * Returns: (transfer none): the badge tone; a switch with no default,
 *   so a state added later is a -Wswitch warning here
 */
const gchar *clawt_lrg_settings_connector_tone(ClawtConnectorState state);

/**
 * clawt_lrg_settings_connector_subtitle:
 * @entry: one entry of `connector.list`'s `connectors`
 *
 * Returns: (transfer full): "github -- work"
 */
gchar *clawt_lrg_settings_connector_subtitle(JsonObject *entry);

/**
 * clawt_lrg_settings_catalog_label:
 * @entry: one entry of `connector.catalog`'s `connectors`
 *
 * With the auth kind, because it decides what happens next.
 *
 * Returns: (transfer full): "GitHub (device)"
 */
gchar *clawt_lrg_settings_catalog_label(JsonObject *entry);

/**
 * clawt_lrg_settings_connector_payload:
 * @name: what to call it
 * @provider: the catalogue id
 * @scope: (nullable): a scope nick; "selected" when %NULL
 * @account: (nullable): "work", "personal"
 * @client_id: (nullable): an OAuth application
 * @instance: (nullable): a self-hosted base
 * @agents: (nullable): comma separated
 * @error: (out) (optional): why it is refused
 *
 * Returns: (transfer full) (nullable): the `integration.add` request
 */
JsonNode *clawt_lrg_settings_connector_payload(const gchar  *name,
                                               const gchar  *provider,
                                               const gchar  *scope,
                                               const gchar  *account,
                                               const gchar  *client_id,
                                               const gchar  *instance,
                                               const gchar  *agents,
                                               GError      **error);

/**
 * clawt_lrg_settings_revoked_text:
 * @reply: (nullable): the `connector.revoke` reply
 *
 * Says which of the two things happened: told the provider, or only
 * forgotten here -- somebody who believes a token is dead and finds it
 * working months later was misled by this sentence.
 *
 * Returns: (transfer none): the toast
 */
const gchar *clawt_lrg_settings_revoked_text(JsonObject *reply);

/**
 * clawt_lrg_settings_imported_text:
 * @imported: how many `connector.registry_refresh` brought in
 *
 * Returns: (transfer full): the toast
 */
gchar *clawt_lrg_settings_imported_text(gint64 imported);

/* ── Connections ──────────────────────────────────────────────────── */

/**
 * clawt_lrg_settings_reach_tone:
 * @reach: a probe's verdict
 *
 * Returns: (transfer none): "good", "bad", or "neutral" for unknown --
 *   not asked is not a failure
 */
const gchar *clawt_lrg_settings_reach_tone(ClawtReachability reach);

/**
 * clawt_lrg_settings_reach_detail:
 * @status: (nullable): what a probe found
 *
 * The version and agent count when it answered, the reason when it did
 * not, and the version mismatch when there is one.
 *
 * Returns: (transfer full) (nullable): the line, or %NULL before a probe
 */
gchar *clawt_lrg_settings_reach_detail(const ClawtConnectionStatus *status);

/**
 * clawt_lrg_settings_connection_from_form:
 * @existing: (element-type ClawtConnection): the profiles already saved
 * @name: (nullable): what to call it; the host when empty
 * @host: the address
 * @port_text: (nullable): the port; the default when empty
 * @token: (nullable): the bearer token
 * @tls: TLS
 * @accept_unknown: accept a certificate nobody vouches for
 * @error: (out) (optional): why the form is refused
 *
 * A name is how the menu and the CLI's --profile refer to a connection,
 * so two cannot share one.
 *
 * Returns: (transfer full) (nullable): the new profile
 */
ClawtConnection *clawt_lrg_settings_connection_from_form(
    GPtrArray    *existing,
    const gchar  *name,
    const gchar  *host,
    const gchar  *port_text,
    const gchar  *token,
    gboolean      tls,
    gboolean      accept_unknown,
    GError      **error);

/**
 * clawt_lrg_settings_daemon_version:
 * @status: (nullable): a `control.status` reply
 * @out_update: (out) (transfer full) (optional): a newer clawtilla the
 *   daemon says is available, only when it says one is
 *
 * Returns: (transfer full) (nullable): the daemon's version
 */
gchar *clawt_lrg_settings_daemon_version(JsonNode  *status,
                                         gchar    **out_update);

/* ── Appearance ───────────────────────────────────────────────────── */

/**
 * clawt_lrg_settings_scale_step:
 * @current: the scale as set (0 follows the monitor)
 * @proposed: what the spin box moved to
 *
 * Between 0 and %CLAWT_LRG_SCALE_MIN there is nothing to choose: the
 * setter clamps everything there up to the minimum, so a person
 * stepping down from 0.5 would be stuck at 0.5 for ever.  Stepping down
 * from the minimum lands on 0 (follow the monitor); stepping up from 0
 * lands on the minimum.
 *
 * Returns: the scale to store
 */
gdouble clawt_lrg_settings_scale_step(gdouble current, gdouble proposed);

/**
 * clawt_lrg_settings_scale_text:
 * @scale: the stored scale
 *
 * Returns: (transfer full): "Follows the monitor" or "1.25×"
 */
gchar *clawt_lrg_settings_scale_text(gdouble scale);

/**
 * clawt_lrg_settings_palette_note:
 * @appearance: the appearance
 *
 * For a palette read from disk, which colours it leaves out -- each
 * keeps the stock value, one widget in the wrong grey with nothing to
 * say why.
 *
 * Returns: (transfer full) (nullable): the note, or %NULL when there is
 *   nothing to say
 */
gchar *clawt_lrg_settings_palette_note(ClawtAppearance *appearance);

/**
 * clawt_lrg_settings_font_text:
 * @family: (nullable): the chosen family
 * @monospace: which font
 *
 * Naming the desktop rather than "Default": the two look identical and
 * only one keeps following the desktop.
 *
 * Returns: (transfer none): the line
 */
const gchar *clawt_lrg_settings_font_text(const gchar *family,
                                          gboolean     monospace);

/* ── New agent ────────────────────────────────────────────────────── */

/**
 * clawt_lrg_create_filter_providers:
 * @catalog: (nullable): a `model.list` reply
 * @require: (nullable): "agent" or "tools", the flag a provider must
 *   carry; %NULL keeps them all
 * @want_provider: (nullable): kept whatever it is, so an agent already
 *   on a provider this view would not offer still shows its own
 *
 * "agent" is what libreclaw can drive (anything else is rewritten to
 * claude-code); "tools" is what the designer can use.  A provider with
 * no such member is an older daemon's, and kept: filtering on absence
 * emptied the whole list once.
 *
 * Returns: (transfer full) (nullable): a filtered copy of `providers`
 */
JsonArray *clawt_lrg_create_filter_providers(JsonNode    *catalog,
                                             const gchar *require,
                                             const gchar *want_provider);

/**
 * clawt_lrg_create_model_label:
 * @model: one entry of a provider's `models`
 *
 * Returns: (transfer full): "Sonnet - fast and capable"
 */
gchar *clawt_lrg_create_model_label(JsonObject *model);

/**
 * clawt_lrg_create_catalog_index:
 * @items: (nullable): the catalogue's entries
 * @member: which member identifies one ("reference", "path")
 * @want: (nullable): the value already chosen
 * @fallback: (nullable): the catalogue's own default
 *
 * Which entry to select.  A value not on the list is not an error: it
 * is "Other", one past the end, with the value in the entry -- an agent
 * configured by hand keeps what it was given.  With nothing asked for,
 * the catalogue's default, else the first entry, else "Other".
 *
 * Returns: the index; the array length means "Other"
 */
guint clawt_lrg_create_catalog_index(JsonArray   *items,
                                     const gchar *member,
                                     const gchar *want,
                                     const gchar *fallback);

/**
 * clawt_lrg_create_image_note:
 * @image: (nullable): the chosen `image.list` entry, or %NULL for Other
 *
 * The full reference with any note: a label like "Fedora 44" does not
 * say which registry it comes from.
 *
 * Returns: (transfer full): the line
 */
gchar *clawt_lrg_create_image_note(JsonObject *image);

/**
 * clawt_lrg_create_disk_note:
 * @image: (nullable): the chosen `image.vm_list` entry, or %NULL
 * @n_images: how many cloud images there are
 *
 * A VM without a disk defines, starts and never boots, so the empty
 * case says where to get one.
 *
 * Returns: (transfer none): the line, or the image's path
 */
const gchar *clawt_lrg_create_disk_note(JsonObject *image, guint n_images);

/**
 * clawt_lrg_create_team_choices:
 * @teams: (nullable): a `team.list` reply
 * @current: (nullable): the team already named
 * @ids: (element-type utf8): filled with ids, "" first for none
 * @labels: (element-type utf8): filled in parallel
 *
 * A team the agent names but nobody declared is kept, marked: a control
 * that cannot represent the current value replaces it on save.
 */
void clawt_lrg_create_team_choices(JsonNode    *teams,
                                   const gchar *current,
                                   GPtrArray   *ids,
                                   GPtrArray   *labels);

/**
 * clawt_lrg_create_preview_text:
 * @result: the `design.agent` reply
 *
 * The proposed configuration, and the org files listed rather than
 * shown -- hundreds of lines each make a dialog nobody can read.
 *
 * Returns: (transfer full): the body of "Proposed agent"
 */
gchar *clawt_lrg_create_preview_text(JsonObject *result);

/**
 * clawt_lrg_create_created_text:
 * @reply: (nullable): the `agent.create` or `design.commit` reply
 * @agent_id: (nullable): the id, for the sentence
 *
 * What actually happened: created and started, created but not
 * started (and why), or -- from an older daemon that says neither --
 * created.
 *
 * Returns: (transfer full): the toast
 */
gchar *clawt_lrg_create_created_text(JsonObject  *reply,
                                     const gchar *agent_id);

/* ── Import ───────────────────────────────────────────────────────── */

/**
 * clawt_lrg_create_found_subtitle:
 * @entry: one entry of `agent.discover`'s `found`
 *
 * Returns: (transfer none): what it holds, else its path
 */
const gchar *clawt_lrg_create_found_subtitle(JsonObject *entry);

/**
 * clawt_lrg_create_import_payload:
 * @agent_id: (nullable): the id to give it
 * @mode: how
 * @url: (nullable): the repository, for a mode that takes one
 * @path: (nullable): the directory, for the others
 * @keep_git: copy .git too
 * @error: (out) (optional): why it is refused
 *
 * Returns: (transfer full) (nullable): the `agent.import` request
 */
JsonNode *clawt_lrg_create_import_payload(const gchar     *agent_id,
                                          ClawtImportMode  mode,
                                          const gchar     *url,
                                          const gchar     *path,
                                          gboolean         keep_git,
                                          GError         **error);

/* ── New room ─────────────────────────────────────────────────────── */

/**
 * clawt_lrg_create_room_payload:
 * @room_id: (nullable): the id
 * @name: (nullable): the name; the id when empty
 * @members: (array zero-terminated=1): the agents ticked
 * @error: (out) (optional): why it is refused -- a room of one is the
 *   conversation you already have with it
 *
 * Returns: (transfer full) (nullable): the `room.create` request
 */
JsonNode *clawt_lrg_create_room_payload(const gchar        *room_id,
                                        const gchar        *name,
                                        const gchar *const *members,
                                        GError            **error);

/**
 * clawt_lrg_create_room_created_text:
 * @n_members: how many
 *
 * More than two is a group, and a group delivers by mention; said now,
 * because this is the moment somebody is about to type in it.
 *
 * Returns: (transfer none): the toast
 */
const gchar *clawt_lrg_create_room_created_text(guint n_members);

G_END_DECLS
