/*
 * gtk-inspector.c - The inspector page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything an agent is configured with, and the two things that page
 * does beyond editing: deleting the agent, and opening one of its
 * workspace files in $EDITOR.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

static gboolean     apply_schema_rows(ClawtWindow *self);

/* ── Inspector ───────────────────────────────────────────────────── */

static GtkWidget *
info_row(const gchar *title, const gchar *value)
{
    GtkWidget *row = adw_action_row_new();

    clawt_gtk_set_row_text(row, title, value != NULL ? value : "-");

    return row;
}

/* Shared by the inspector's buttons and the sidebar's context menu. */
void
clawt_gtk_agent_action(ClawtWindow *self, const gchar *kind)
{
    g_autoptr(JsonNode) reply = NULL;

    if (self->selected_agent == NULL || kind == NULL)
        return;

    reply = clawt_window_request(
        self, kind, clawt_build_payload("agent", self->selected_agent, NULL));

    (void)reply;

    /*
     * Refreshed whether or not it worked.  A start that fails still moves
     * the agent to ERROR, and skipping the refresh on failure left the
     * sidebar showing "stopped" next to a toast explaining why it could
     * not start -- two contradictory answers on screen at once.
     */
    clawt_gtk_refresh_agents(self);
    clawt_gtk_refresh_selected(self);
}

static void
on_agent_action(GtkButton *button, gpointer user_data)
{
    clawt_gtk_agent_action(user_data, g_object_get_data(G_OBJECT(button), "kind"));
}

static GtkWidget *
action_button(ClawtWindow *self, const gchar *label, const gchar *kind,
              gboolean sensitive, const gchar *why_not)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    g_object_set_data_full(G_OBJECT(button), "kind", g_strdup(kind), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_agent_action), self);

    gtk_widget_set_sensitive(button, sensitive);

    /*
     * A disabled control says why.  A greyed-out button with no
     * explanation is the interface equivalent of a shrug.
     */
    if (!sensitive && why_not != NULL)
        gtk_widget_set_tooltip_text(button, why_not);

    return button;
}

/*
 * Sends one setting, and reports it if the daemon refuses.
 *
 * Returns: %TRUE if it was accepted
 */
gboolean
clawt_gtk_apply_setting(ClawtWindow *self, const gchar *key, const gchar *value)
{
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(
        self, "agent.set",
        clawt_build_payload("agent", self->selected_agent, "key", key,
                            "value", value, NULL));

    if (reply == NULL)
        return FALSE;

    /*
     * An AI CLI lists its tools once, when its session starts, so a
     * permission changed under a running agent reaches its files and not
     * its session. Remembered rather than toasted here, because a save
     * applies a dozen settings and a dozen toasts is not an answer.
     */
    if (clawt_json_boolean(clawt_payload_of(reply), "restart_required",
                           FALSE))
        self->settings_need_restart = TRUE;

    return TRUE;
}

/*
 * The computer types, walked from the library rather than named here.
 *
 * There were four copies of this list in this file and a fifth in the
 * web client, so adding a type meant editing five places and the one
 * missed was a type the fleet had and a client could not offer. Same
 * shape as the colour schemes, and invisible to `make parity` for the
 * same reason: a computer type sends no frame of its own.
 *
 * Built once and kept, because a GtkStringList made from it outlives
 * the call and the nicks are static storage in the library.
 */
const gchar *const *
clawt_gtk_computer_type_nicks(void)
{
    static const gchar **nicks = NULL;

    if (nicks == NULL) {
        guint n = clawt_computer_type_count();
        guint i;

        nicks = g_new0(const gchar *, n + 1);

        for (i = 0; i < n; i++)
            nicks[i] = clawt_computer_type_nth_nick(i);
    }

    return (const gchar *const *)nicks;
}

/*
 * A combo's selection as a type nick.
 *
 * Clamped against the library's own count. The bound used to be a
 * literal 3, which is the kind of number that is silently one short the
 * day a type is added -- and the symptom would be the last entry in the
 * list choosing the one before it.
 */
const gchar *
clawt_gtk_computer_type_nick_at(guint selected)
{
    return clawt_computer_type_nth_nick(
        MIN(selected, clawt_computer_type_count() - 1));
}

ClawtComputerType
clawt_gtk_computer_type_from_nick(const gchar *nick)
{
    gint value = CLAWT_COMPUTER_NONE;

    clawt_enum_from_nick(CLAWT_TYPE_COMPUTER_TYPE, nick, &value);

    return (ClawtComputerType)value;
}

static void
on_save_agent(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    /* Per save, not per session. */
    self->settings_need_restart = FALSE;

    g_autofree gchar *model = NULL;
    static const gchar *const efforts[] = { "low", "medium", "high",
                                            "xhigh", "max" };
    static const gchar *const restarts[] = { "never", "on-failure",
                                             "always" };
    gboolean ok = TRUE;

    (void)button;

    if (self->selected_agent == NULL)
        return;

    ok &= apply_schema_rows(self);
    ok &= clawt_gtk_apply_setting(self, "name",
                                  gtk_editable_get_text(GTK_EDITABLE(self->name_row)));
    ok &= clawt_gtk_apply_setting(self, "description",
                                  gtk_editable_get_text(
                            GTK_EDITABLE(self->description_row)));
    ok &= clawt_gtk_apply_setting(self, "skills",
                                  gtk_editable_get_text(
                            GTK_EDITABLE(self->skills_row)));

    if (clawt_gtk_chooser_provider_id(&self->inspector_models) != NULL)
        ok &= clawt_gtk_apply_setting(self, "model.provider",
                                      clawt_gtk_chooser_provider_id(&self->inspector_models));

    model = clawt_gtk_chooser_model(&self->inspector_models);

    if (model != NULL)
        ok &= clawt_gtk_apply_setting(self, "model.model", model);

    ok &= clawt_gtk_apply_setting(self, "model.effort",
                                  efforts[MIN(adw_combo_row_get_selected(
                                        ADW_COMBO_ROW(self->effort_row)), 4)]);

    ok &= clawt_gtk_apply_setting(self, "computer.type",
                                  clawt_gtk_computer_type_nick_at(adw_combo_row_get_selected(
                            ADW_COMBO_ROW(self->computer_row))));

    if (self->vm_cpus_row != NULL) {
        g_autofree gchar *image = clawt_gtk_disk_chooser_value(&self->inspector_disk);
        const gchar *cpus = gtk_editable_get_text(
            GTK_EDITABLE(self->vm_cpus_row));
        const gchar *memory = gtk_editable_get_text(
            GTK_EDITABLE(self->vm_memory_row));
        const gchar *disk = gtk_editable_get_text(
            GTK_EDITABLE(self->vm_disk_row));
        const gchar *ssh_host = gtk_editable_get_text(
            GTK_EDITABLE(self->vm_ssh_host_row));

        if (image != NULL && *image != '\0')
            ok &= clawt_gtk_apply_setting(self, "computer.vm.image", image);

        if (cpus != NULL && *cpus != '\0')
            ok &= clawt_gtk_apply_setting(self, "computer.vm.cpus", cpus);

        if (memory != NULL && *memory != '\0')
            ok &= clawt_gtk_apply_setting(self, "computer.vm.memory_mb", memory);

        if (disk != NULL && *disk != '\0')
            ok &= clawt_gtk_apply_setting(self, "computer.vm.disk_gb", disk);

        /*
         * Read out of the model rather than from a fixed table, because
         * the list is built per agent: one whose size is not among the
         * common ones has it added, so saving cannot silently replace
         * it with the first entry.
         */
        if (self->vm_resolution_row != NULL) {
            GtkStringList *sizes = GTK_STRING_LIST(
                adw_combo_row_get_model(
                    ADW_COMBO_ROW(self->vm_resolution_row)));
            const gchar *resolution = gtk_string_list_get_string(
                sizes,
                adw_combo_row_get_selected(
                    ADW_COMBO_ROW(self->vm_resolution_row)));

            if (resolution != NULL && *resolution != '\0')
                ok &= clawt_gtk_apply_setting(self, "computer.vm.resolution",
                                              resolution);
        }

        if (ssh_host != NULL && *ssh_host != '\0')
            ok &= clawt_gtk_apply_setting(self, "computer.vm.ssh_host", ssh_host);

        /*
         * Written unconditionally, unlike the entries above.  A switch
         * turned *off* is a value of its own, and skipping an empty one
         * the way an empty text field is skipped would make the desktop
         * impossible to turn back off from here.
         */
        ok &= clawt_gtk_apply_setting(
            self, "computer.desktop.enabled",
            adw_switch_row_get_active(
                ADW_SWITCH_ROW(self->vm_desktop_row)) ? "true" : "false");
        ok &= clawt_gtk_apply_setting(
            self, "computer.desktop.allow_input",
            adw_switch_row_get_active(
                ADW_SWITCH_ROW(self->vm_desktop_input_row)) ? "true"
                                                            : "false");
    }

    if (self->inspector_image.row != NULL) {
        g_autofree gchar *image = clawt_gtk_image_chooser_value(&self->inspector_image);

        if (image != NULL)
            ok &= clawt_gtk_apply_setting(self, "computer.container.image", image);
    }

    ok &= clawt_gtk_apply_setting(self, "runtime.restart",
                                  restarts[MIN(adw_combo_row_get_selected(
                                         ADW_COMBO_ROW(self->restart_row)),
                                               2)]);

    ok &= clawt_gtk_apply_setting(self, "runtime.autostart",
                                  adw_switch_row_get_active(
                            ADW_SWITCH_ROW(self->autostart_row))
                            ? "true" : "false");

    if (self->team_row != NULL && self->team_ids != NULL) {
        guint chosen = adw_combo_row_get_selected(
            ADW_COMBO_ROW(self->team_row));
        guint count = g_strv_length(self->team_ids);

        /*
         * Written even when empty -- that is how an agent is taken off a
         * team, and skipping a blank would make "No team" the one choice
         * in this dialog that does nothing.
         */
        if (chosen < count)
            ok &= clawt_gtk_apply_setting(self, "team", self->team_ids[chosen]);
    }

    if (self->team_role_row != NULL) {
        static const gchar *const roles[] = { "member", "lead" };

        ok &= clawt_gtk_apply_setting(self, "team_role",
                                      roles[MIN(adw_combo_row_get_selected(
                                          ADW_COMBO_ROW(self->team_role_row)),
                                                1)]);
    }

    ok &= clawt_gtk_apply_setting(self, "chief_of_staff",
                                  adw_switch_row_get_active(
                            ADW_SWITCH_ROW(self->chief_row))
                            ? "true" : "false");

    ok &= clawt_gtk_apply_setting(self, "tools.manage_fleet",
                                  adw_switch_row_get_active(
                            ADW_SWITCH_ROW(self->manage_fleet_row))
                            ? "true" : "false");

    if (!ok)
        return;

    /*
     * Said plainly, because most of these only take effect on the next
     * start -- the model and the computer especially.  A reload does not
     * restart running agents on purpose, and an interface that implied
     * otherwise would have people wondering why nothing changed.
     */
    /*
     * The tool list is named separately when it changed, because it is
     * the one where "restart it" is not general advice but the whole
     * answer: an AI CLI lists its tools once, at session start, so a
     * permission granted under a running agent reaches its files and not
     * its session -- and the agent then says, accurately, that it does
     * not have the tool.
     */
    if (self->settings_need_restart) {
        clawt_window_toast(self,
                           "Saved. Restart the agent -- it lists its tools "
                           "when it starts, so it cannot see the change "
                           "until then.");
    } else {
        clawt_window_toast(self,
                           "Saved. Restart the agent for the model or "
                           "computer to take effect.");
    }

    clawt_gtk_refresh_agents(self);
}

/* ── Deleting ────────────────────────────────────────────────────── */

static void
on_delete_confirmed_twice(AdwAlertDialog *dialog, gchar *response,
                          gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *check = g_object_get_data(G_OBJECT(dialog), "remove-computer");
    GtkWidget *purge_check = g_object_get_data(G_OBJECT(dialog),
                                               "remove-files");
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *agent_id = NULL;
    gboolean with_computer;
    gboolean purge;

    if (g_strcmp0(response, "delete") != 0)
        return;

    agent_id = g_strdup(self->selected_agent);
    with_computer = check != NULL &&
                    gtk_check_button_get_active(GTK_CHECK_BUTTON(check));
    purge = purge_check != NULL &&
            gtk_check_button_get_active(GTK_CHECK_BUTTON(purge_check));

    reply = clawt_window_request(
        self, "agent.remove",
        clawt_build_payload("agent", agent_id,
                            "remove_computer", with_computer ? "true" : NULL,
                            "remove_files", purge ? "true" : NULL,
                            NULL));

    if (reply == NULL)
        return;

    g_clear_pointer(&self->selected_agent, g_free);
    g_clear_pointer(&self->selected_room, g_free);
    clawt_gtk_clear_box(self->inspector);
    clawt_gtk_reset_transcript(self);

    {
        const gchar *computer = clawt_json_string(clawt_payload_of(reply),
                                                   "computer", NULL);
        g_autofree gchar *message = NULL;

        /*
         * The computer's fate is reported rather than assumed: a
         * teardown that failed is not fatal to the removal, and a toast
         * saying "is gone" over a container still running would be a
         * lie.
         */
        if (computer != NULL && g_strcmp0(computer, "removed") == 0)
            message = g_strdup_printf("%s and its computer are gone.",
                                      agent_id);
        else if (computer != NULL)
            message = g_strdup_printf("%s is gone; its computer was not "
                                      "removed: %s", agent_id, computer);
        else
            message = g_strdup_printf("%s is gone.", agent_id);

        clawt_window_toast(self, message);
    }

    clawt_gtk_refresh_agents(self);
}

static void
on_delete_confirmed_once(AdwAlertDialog *dialog, gchar *response,
                         gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwAlertDialog *second;

    (void)dialog;

    if (g_strcmp0(response, "delete") != 0)
        return;

    /*
     * A second, deliberately louder confirmation.  Removing an agent
     * takes its running process and its place in every room with it, and
     * a single click between "browsing" and "gone" is too few.
     */
    second = ADW_ALERT_DIALOG(
        adw_alert_dialog_new("ARE YOU REALLY SURE!?", NULL));

    adw_alert_dialog_set_body(
        second,
        "This removes the agent from the fleet and stops it if it is "
        "running.\n\n"
        "Its mailbox and transcripts stay on disk -- removing an agent is "
        "reversible, deleting its history is not.");

    /*
     * The container or VM is a separate decision.
     *
     * Removing the agent used to leave it running under a name derived
     * from an agent that no longer existed, so the only way to find it
     * again was to remember what it had been called. Off by default,
     * because a container may hold work that was never anywhere else.
     */
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *purge;

        gtk_widget_set_margin_top(box, 12);

        if (g_strcmp0(self->inspector_computer, "container") == 0 ||
            g_strcmp0(self->inspector_computer, "vm") == 0) {
            GtkWidget *check;
            g_autofree gchar *label = g_strdup_printf(
                "Also delete its %s, clawt-%s",
                g_strcmp0(self->inspector_computer, "vm") == 0
                    ? "virtual machine" : "container",
                self->selected_agent);

            check = gtk_check_button_new_with_label(label);
            gtk_box_append(GTK_BOX(box), check);

            /*
             * Kept on the dialog rather than in the window: the dialog
             * is what the response handler is given, and a second delete
             * started before the first finished would otherwise read the
             * wrong checkbox.
             */
            g_object_set_data(G_OBJECT(second), "remove-computer", check);
        }

        /*
         * And the files, which is the half with no undo.
         *
         * Off by default and worded without euphemism: removing an agent
         * from the fleet is reversible and deleting what it wrote is
         * not. It is here because a throwaway agent made to test
         * something should be throwable away, and leaving seven
         * abandoned workspaces behind is its own kind of mess.
         */
        purge = gtk_check_button_new_with_label(
            "Also delete everything it owns: its persona, notes, "
            "mailbox, memories, transcripts and credentials");
        gtk_label_set_wrap(
            GTK_LABEL(gtk_widget_get_last_child(purge)), TRUE);
        gtk_box_append(GTK_BOX(box), purge);
        g_object_set_data(G_OBJECT(second), "remove-files", purge);

        adw_alert_dialog_set_extra_child(second, box);
    }

    adw_alert_dialog_add_response(second, "cancel", "Keep it");
    adw_alert_dialog_add_response(second, "delete", "Delete it for good");
    adw_alert_dialog_set_response_appearance(second, "delete",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(second, "cancel");
    adw_alert_dialog_set_close_response(second, "cancel");

    g_signal_connect(second, "response",
                     G_CALLBACK(on_delete_confirmed_twice), self);

    adw_dialog_present(ADW_DIALOG(second), GTK_WIDGET(self));
}

void
clawt_gtk_delete_agent(ClawtWindow *self)
{
    AdwAlertDialog *first;
    g_autofree gchar *heading = NULL;

    if (self->selected_agent == NULL)
        return;

    heading = g_strdup_printf("Delete %s?", self->selected_agent);

    first = ADW_ALERT_DIALOG(adw_alert_dialog_new("Are you sure?", NULL));

    adw_alert_dialog_set_body(first, heading);
    adw_alert_dialog_add_response(first, "cancel", "Cancel");
    adw_alert_dialog_add_response(first, "delete", "Delete");
    adw_alert_dialog_set_response_appearance(first, "delete",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(first, "cancel");
    adw_alert_dialog_set_close_response(first, "cancel");

    g_signal_connect(first, "response",
                     G_CALLBACK(on_delete_confirmed_once), self);

    adw_dialog_present(ADW_DIALOG(first), GTK_WIDGET(self));
}

static void
on_delete_agent(GtkButton *button, gpointer user_data)
{
    (void)button;

    clawt_gtk_delete_agent(user_data);
}

/* ── The inspector ───────────────────────────────────────────────── */

static guint
index_of(const gchar *const *values, const gchar *wanted)
{
    guint i;

    for (i = 0; values[i] != NULL; i++) {
        if (g_strcmp0(values[i], wanted) == 0)
            return i;
    }

    return 0;
}

static GtkWidget *
combo_row(const gchar *title, const gchar *const *values,
          const gchar *selected)
{
    GtkWidget *row = adw_combo_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_combo_row_set_model(ADW_COMBO_ROW(row),
                            G_LIST_MODEL(gtk_string_list_new(values)));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row),
                               index_of(values, selected));

    return row;
}

/*
 * The screen sizes worth offering, plus whatever this agent already has.
 *
 * The second half matters: a resolution set by hand or by another client
 * that is not in this list would otherwise be shown as the first entry,
 * and saving the page -- without touching this row -- would quietly
 * change it. A combo box has no way to say "something else", so the
 * something else joins the list.
 */
static GtkWidget *
resolution_row(const gchar *current)
{
    static const gchar *const common[] = {
        "1280x800", "1280x1024", "1440x900", "1600x900", "1680x1050",
        "1920x1080", "1920x1200", "2560x1440", "3840x2160", NULL
    };
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    gboolean known = FALSE;
    gsize i;

    for (i = 0; common[i] != NULL; i++) {
        if (g_strcmp0(common[i], current) == 0)
            known = TRUE;

        g_ptr_array_add(values, (gpointer)common[i]);
    }

    if (current != NULL && *current != '\0' && !known)
        g_ptr_array_insert(values, 0, (gpointer)current);

    g_ptr_array_add(values, NULL);

    return combo_row("Screen size", (const gchar *const *)values->pdata,
                     current);
}

/*
 * One row built from a schema entry, and the key it writes.
 */
typedef struct {
    GtkWidget       *row;
    gchar           *key;
    ClawtSchemaType  type;
    GStrv            choices;               /* for an enum */
} SchemaRow;

void
clawt_gtk_schema_row_free(gpointer data)
{
    SchemaRow *row = data;

    g_free(row->key);
    g_strfreev(row->choices);
    g_free(row);
}

static GtkWidget *
entry_row(const gchar *title, const gchar *value)
{
    GtkWidget *row = adw_entry_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    gtk_editable_set_text(GTK_EDITABLE(row), value != NULL ? value : "");

    return row;
}

static GtkWidget *
switch_row(const gchar *title, const gchar *subtitle, gboolean active)
{
    GtkWidget *row = adw_switch_row_new();

    clawt_gtk_set_row_text(row, title, subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);

    return row;
}

/*
 * The nicknames an enum option accepts, from its own GType.
 */
static GStrv
schema_enum_choices(const ClawtSchemaEntry *entry)
{
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    g_autoptr(GEnumClass) klass = NULL;
    guint i;

    if (entry->enum_type == NULL || !G_TYPE_IS_ENUM(entry->enum_type()))
        return NULL;

    klass = g_type_class_ref(entry->enum_type());

    for (i = 0; i < klass->n_values; i++)
        g_ptr_array_add(values, g_strdup(klass->values[i].value_nick));

    g_ptr_array_add(values, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&values), FALSE);
}

/*
 * Every option an agent can set that has no hand-built row of its own.
 *
 * Walked from the schema rather than listed, which is what makes these
 * appear at all: the six mailbox overrides and three memories ones are
 * not `agents.*` keys, so nothing here knew their names until
 * clawt_config_schema_agent_name() could say. They were settable in a
 * config file and absent from both clients for the whole life of the
 * feature.
 *
 * The rows are generic on purpose. The hand-built ones above earn their
 * copy -- "May create agents" explains which of two similarly-named
 * settings actually grants the tool -- and a generated row cannot do
 * that. These get the schema's own first line instead, which is better
 * than not existing.
 */
static void
build_schema_rows(ClawtWindow *self, JsonObject *settings)
{
    GtkWidget *group = adw_preferences_group_new();
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;
    guint added = 0;

    g_ptr_array_set_size(self->schema_rows, 0);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Mailbox and memories");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Fleet settings this agent may override. Left as they are, each "
        "follows the fleet.");

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *key = clawt_config_schema_agent_name(entry);
        const gchar *value;
        SchemaRow *record;

        /*
         * Only the ones with no row of their own. An `agents.*` option
         * is already on screen above, written by hand.
         */
        if (key == NULL || g_str_has_prefix(entry->key, "agents."))
            continue;

        value = (settings != NULL && json_object_has_member(settings, key))
                ? json_object_get_string_member(settings, key) : NULL;

        record = g_new0(SchemaRow, 1);
        record->key = g_strdup(key);
        record->type = entry->type;

        switch (entry->type) {
        case CLAWT_SCHEMA_BOOLEAN:
            record->row = switch_row(key, entry->doc,
                                     g_strcmp0(value, "true") == 0);
            break;

        case CLAWT_SCHEMA_ENUM:
            record->choices = schema_enum_choices(entry);

            if (record->choices != NULL) {
                record->row = combo_row(
                    key, (const gchar *const *)record->choices, value);
                break;
            }

            record->row = entry_row(key, value);
            record->type = CLAWT_SCHEMA_STRING;
            break;

        default:
            record->row = entry_row(key, value);
            break;
        }

        g_ptr_array_add(self->schema_rows, record);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), record->row);
        added++;
    }

    if (added == 0) {
        gtk_widget_unparent(group);
        return;
    }

    gtk_box_append(self->inspector, group);
}

/*
 * Writes back whatever the generic rows hold.
 */
static gboolean
apply_schema_rows(ClawtWindow *self)
{
    gboolean ok = TRUE;
    guint i;

    for (i = 0; i < self->schema_rows->len; i++) {
        SchemaRow *record = g_ptr_array_index(self->schema_rows, i);
        g_autofree gchar *value = NULL;

        switch (record->type) {
        case CLAWT_SCHEMA_BOOLEAN:
            value = g_strdup(
                adw_switch_row_get_active(ADW_SWITCH_ROW(record->row))
                ? "true" : "false");
            break;

        case CLAWT_SCHEMA_ENUM: {
            guint selected =
                adw_combo_row_get_selected(ADW_COMBO_ROW(record->row));

            if (record->choices == NULL ||
                selected >= g_strv_length(record->choices))
                continue;

            value = g_strdup(record->choices[selected]);
            break;
        }

        default:
            value = g_strdup(
                gtk_editable_get_text(GTK_EDITABLE(record->row)));
            break;
        }

        ok &= clawt_gtk_apply_setting(self, record->key, value);
    }

    return ok;
}

/*
 * Shares a host folder with the agent's computer.
 *
 * The mount list has always been read and applied -- bind mounts for a
 * container, virtiofs devices for a VM -- and no client could write one,
 * so the only way to share a folder was to edit the YAML by hand.
 */
static void
on_add_mount(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    static const gchar *const modes[] = { "ro", "rw" };
    const gchar *source;
    const gchar *target;
    guint mode;

    (void)button;

    if (self->selected_agent == NULL || self->mount_source_row == NULL)
        return;

    source = gtk_editable_get_text(GTK_EDITABLE(self->mount_source_row));
    target = gtk_editable_get_text(GTK_EDITABLE(self->mount_target_row));

    if (source == NULL || *source == '\0' || target == NULL ||
        *target == '\0') {
        clawt_window_toast(self, "A share needs a folder and a path inside.");
        return;
    }

    mode = adw_combo_row_get_selected(ADW_COMBO_ROW(self->mount_mode_row));

    reply = clawt_window_request(
        self, "agent.mount.add",
        clawt_build_payload("agent", self->selected_agent,
                            "source", source, "target", target,
                            "mode", modes[MIN(mode, 1)], NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Shared. It appears when the agent restarts.");
    clawt_gtk_refresh_selected(self);
}

static void
on_remove_mount(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *target = g_object_get_data(G_OBJECT(button), "target");
    g_autoptr(JsonNode) reply = NULL;

    if (self->selected_agent == NULL || target == NULL)
        return;

    reply = clawt_window_request(
        self, "agent.mount.remove",
        clawt_build_payload("agent", self->selected_agent, "target", target,
                            NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Removed. Takes effect on the next restart.");
    clawt_gtk_refresh_selected(self);
}

/*
 * The shared folders group, for a computer that can have them.
 *
 * A host agent is left out: its computer is the machine itself, so there
 * is nothing to mount into -- what limits it there is confinement, not
 * a mount list.
 */
static void
build_mounts(ClawtWindow *self, const gchar *computer_type)
{
    static const gchar *const modes[] = { "ro", "rw", NULL };
    g_autoptr(JsonNode) reply = NULL;
    GtkWidget *group;
    GtkWidget *add;
    JsonArray *mounts;
    guint i;

    self->mount_source_row = NULL;
    self->mount_target_row = NULL;
    self->mount_mode_row = NULL;

    if (!clawt_computer_type_takes_mounts(
            clawt_gtk_computer_type_from_nick(computer_type)))
        return;

    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Shared folders");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        g_strcmp0(computer_type, "vm") == 0
            ? "Passed into the VM over virtiofs. Changes apply when it "
              "next starts."
            : "Bind-mounted into the container, SELinux-relabelled so "
              "they are actually readable. Changes apply when it next "
              "starts.");

    reply = clawt_window_request(
        self, "agent.mount.list",
        clawt_build_payload("agent", self->selected_agent, NULL));

    mounts = (reply != NULL)
             ? json_object_get_array_member(clawt_payload_of(reply), "mounts")
             : NULL;

    for (i = 0; mounts != NULL && i < json_array_get_length(mounts); i++) {
        JsonObject *mount = json_array_get_object_element(mounts, i);
        const gchar *target = clawt_json_string(mount, "target", "?");
        GtkWidget *row = adw_action_row_new();
        GtkWidget *remove;
        g_autofree gchar *subtitle = g_strdup_printf(
            "%s  (%s)", clawt_json_string(mount, "source", "-"),
            clawt_json_string(mount, "mode", "ro"));

        clawt_gtk_set_row_text(row, target, subtitle);

        remove = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(remove, "flat");
        gtk_widget_set_tooltip_text(remove, "Stop sharing this folder");
        g_object_set_data_full(G_OBJECT(remove), "target", g_strdup(target),
                               g_free);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_mount),
                         self);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    self->mount_source_row = entry_row("Folder on this machine", NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->mount_source_row);

    self->mount_target_row = entry_row("Where the agent sees it", NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->mount_target_row);

    self->mount_mode_row = combo_row("Access", modes, "ro");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->mount_mode_row);

    add = gtk_button_new_with_label("Share this folder");
    gtk_widget_set_margin_top(add, 6);
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_mount), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add);

    gtk_box_append(self->inspector, group);
}

/* ── Opening a workspace file ────────────────────────────────────── */

/*
 * One attempt at running an editor, so a failed one can be retried
 * differently.
 *
 * Holds a reference to the window: this outlives the click that started
 * it, and an editor the user leaves open for an hour would otherwise be
 * writing its result into freed memory when they finally quit it.
 */
typedef struct {
    ClawtWindow *window;
    gchar       *path;
    gchar       *name;
    gboolean     in_terminal;
    gint64       started;
} EditorLaunch;

static void editor_launch(EditorLaunch *launch);

static void
editor_launch_free(EditorLaunch *launch)
{
    g_clear_object(&launch->window);
    g_free(launch->path);
    g_free(launch->name);
    g_free(launch);
}

/*
 * $CLAWT_EDITOR first, so this can be pointed somewhere else without
 * changing what every other program on the machine does.
 */
const gchar *
clawt_gtk_editor_command(void)
{
    const gchar *editor = g_getenv("CLAWT_EDITOR");

    if (editor == NULL || editor[0] == '\0')
        editor = g_getenv("VISUAL");

    if (editor == NULL || editor[0] == '\0')
        editor = g_getenv("EDITOR");

    return (editor != NULL && editor[0] != '\0') ? editor : NULL;
}

/*
 * A terminal to run a terminal-only editor inside.
 *
 * There is no way to ask an editor whether it needs one, so this is
 * only reached after running it bare has already failed -- which is
 * exactly what a curses editor does when it is started without a
 * terminal, and quickly. The flag differs per terminal: some take the
 * command straight after the binary, some want a separator first.
 */
static gboolean
terminal_prefix(GPtrArray *argv)
{
    static const struct {
        const gchar *binary;
        const gchar *flag;
    } terminals[] = {
        { "xdg-terminal-exec", NULL },
        { "gst",               "-e" },
        { "foot",              NULL },
        { "kitty",             NULL },
        { "wezterm",           "start" },
        { "alacritty",         "-e" },
        { "gnome-terminal",    "--" },
        { "xterm",             "-e" }
    };
    const gchar *configured = g_getenv("TERMINAL");
    gsize i;

    if (configured != NULL && configured[0] != '\0') {
        g_ptr_array_add(argv, g_strdup(configured));
        g_ptr_array_add(argv, g_strdup("-e"));
        return TRUE;
    }

    for (i = 0; i < G_N_ELEMENTS(terminals); i++) {
        g_autofree gchar *found = g_find_program_in_path(terminals[i].binary);

        if (found == NULL)
            continue;

        g_ptr_array_add(argv, g_steal_pointer(&found));

        if (terminals[i].flag != NULL)
            g_ptr_array_add(argv, g_strdup(terminals[i].flag));

        return TRUE;
    }

    return FALSE;
}

static void
on_editor_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    EditorLaunch *launch = user_data;
    g_autoptr(GError) error = NULL;

    if (g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error)) {
        editor_launch_free(launch);
        return;
    }

    /*
     * A terminal editor started without a terminal fails immediately,
     * and there is no way to know in advance which kind $EDITOR is --
     * emacsclient opens a window here and vi does not. So the bare run
     * is the test, and this is the second attempt.
     *
     * Only when it failed straight away, though: an editor the user
     * spent ten minutes in and then quit with a non-zero status has
     * already done its job, and popping a terminal open at that point
     * would be baffling.
     */
    if (!launch->in_terminal &&
        g_get_monotonic_time() - launch->started < 2 * G_USEC_PER_SEC) {
        launch->in_terminal = TRUE;
        editor_launch(launch);
        return;
    }

    {
        g_autofree gchar *message =
            g_strdup_printf("could not open %s: %s", launch->name,
                            error->message);

        clawt_window_toast(launch->window, message);
    }

    editor_launch_free(launch);
}

static void
editor_launch(EditorLaunch *launch)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) parts = NULL;
    const gchar *editor = clawt_gtk_editor_command();
    gint i;

    if (launch->in_terminal && !terminal_prefix(argv)) {
        clawt_window_toast(launch->window,
                           "no terminal found to run $EDITOR in");
        editor_launch_free(launch);
        return;
    }

    /*
     * $EDITOR is a command line, not a program name: "emacsclient -nw"
     * and "code --wait" are both ordinary settings, so it is parsed
     * rather than exec'd whole.
     */
    if (editor == NULL || !g_shell_parse_argv(editor, NULL, &parts, &error)) {
        g_autofree gchar *message =
            g_strdup_printf("$EDITOR (%s) could not be parsed: %s",
                            editor != NULL ? editor : "unset",
                            error != NULL ? error->message : "it is not set");

        clawt_window_toast(launch->window, message);
        editor_launch_free(launch);
        return;
    }

    for (i = 0; parts[i] != NULL; i++)
        g_ptr_array_add(argv, g_strdup(parts[i]));

    g_ptr_array_add(argv, g_strdup(launch->path));
    g_ptr_array_add(argv, NULL);

    process = g_subprocess_newv((const gchar *const *)argv->pdata,
                                G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &error);

    if (process == NULL) {
        g_autofree gchar *message =
            g_strdup_printf("could not run %s: %s", editor, error->message);

        clawt_window_toast(launch->window, message);
        editor_launch_free(launch);
        return;
    }

    launch->started = g_get_monotonic_time();
    g_subprocess_wait_check_async(process, NULL, on_editor_finished, launch);
}

void
clawt_gtk_open_path_in_editor(ClawtWindow *self, const gchar *path, const gchar *name)
{
    EditorLaunch *launch;

    if (path == NULL || path[0] == '\0')
        return;

    if (clawt_gtk_editor_command() == NULL) {
        g_autofree gchar *message =
            g_strdup_printf("no editor set; try `clawtilla agent edit %s %s`",
                            self->selected_agent != NULL
                                ? self->selected_agent : "<agent>",
                            name != NULL ? name : "<file>");

        clawt_window_toast(self, message);
        return;
    }

    launch = g_new0(EditorLaunch, 1);
    launch->window = g_object_ref(self);
    launch->path = g_strdup(path);
    launch->name = g_strdup(name);

    editor_launch(launch);
}

static void
on_open_file(GtkButton *button, gpointer user_data)
{
    clawt_gtk_open_path_in_editor(user_data,
                                  g_object_get_data(G_OBJECT(button), "path"),
                                  g_object_get_data(G_OBJECT(button), "name"));
}

/*
 * Which integrations this agent has, and a switch for each shared one.
 *
 * The switch edits the instance's `agents` list rather than writing
 * anything into the agent, because that is where the scope lives -- and
 * the alternative, an inline copy per agent, is the duplication the
 * shared instances exist to end.
 */
static void
on_agent_integration_toggled(GObject *row, GParamSpec *spec,
                             gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name = g_object_get_data(row, "integration");
    g_autoptr(JsonNode) list = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    gboolean wanted = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
    JsonObject *integration;
    JsonArray *agents;
    gboolean present = FALSE;
    guint i;

    (void)spec;

    if (self->selected_agent == NULL || name == NULL)
        return;

    list = clawt_window_request(self, "integration.list", NULL);
    integration = clawt_gtk_find_integration(list, name);

    if (integration == NULL)
        return;

    /*
     * `all` is left alone.  Switching one agent off an integration
     * everybody has is a decision about the whole fleet, and quietly
     * turning it into a list of everyone-but-this-one is not what the
     * switch appears to say.
     */
    if (g_strcmp0(clawt_json_string(integration, "scope", ""), "all") == 0) {
        clawt_window_toast(self,
                           "That one is set to every agent. Change its "
                           "scope in Settings to pick agents.");
        return;
    }

    agents = json_object_get_array_member(integration, "agents");

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);
    json_builder_set_member_name(builder, "scope");
    json_builder_add_string_value(builder, "selected");
    json_builder_set_member_name(builder, "agents");
    json_builder_begin_array(builder);

    for (i = 0; i < json_array_get_length(agents); i++) {
        const gchar *id = json_array_get_string_element(agents, i);

        if (g_strcmp0(id, self->selected_agent) == 0) {
            present = TRUE;

            if (!wanted)
                continue;
        }

        json_builder_add_string_value(builder, id);
    }

    if (wanted && !present)
        json_builder_add_string_value(builder, self->selected_agent);

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    reply = clawt_window_request(self, "integration.update",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_selected(self);
}

static void
build_agent_integrations(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    GtkWidget *group;
    JsonObject *root;
    JsonArray *integrations;
    JsonArray *bindings;
    guint i;

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "integration.list",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    root = clawt_payload_of(reply);
    integrations = json_object_get_array_member(root, "integrations");
    bindings = json_object_has_member(root, "bindings")
        ? json_object_get_array_member(root, "bindings") : NULL;

    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Integrations");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "What reaches this agent from outside the fleet. Add and configure "
        "them in Settings; here you choose which ones this agent gets.");

    for (i = 0; i < json_array_get_length(integrations); i++) {
        JsonObject *integration =
            json_array_get_object_element(integrations, i);
        const gchar *name = clawt_json_string(integration, "name", "?");
        gboolean covers = json_object_has_member(integration, "covers") &&
                          json_object_get_boolean_member(integration,
                                                         "covers");
        GtkWidget *row = adw_switch_row_new();
        g_autofree gchar *subtitle = NULL;

        subtitle = g_strdup_printf(
            "%s%s", clawt_json_string(integration, "type", "?"),
            g_strcmp0(clawt_json_string(integration, "scope", ""),
                      "all") == 0 ? " \342\200\224 every agent" : "");

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
        adw_switch_row_set_active(ADW_SWITCH_ROW(row), covers);

        g_object_set_data_full(G_OBJECT(row), "integration", g_strdup(name),
                               g_free);

        /*
         * Connected after the initial state is set, so building the
         * inspector does not look like somebody flipping every switch.
         */
        g_signal_connect(row, "notify::active",
                         G_CALLBACK(on_agent_integration_toggled), self);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    /*
     * The agent's own inline blocks are not instances and have no switch:
     * they were written inside this agent and belong to it. Shown anyway,
     * because an agent with a Matrix block of its own would otherwise
     * look like it had no Matrix at all.
     */
    for (i = 0; bindings != NULL && i < json_array_get_length(bindings); i++) {
        JsonObject *binding = json_array_get_object_element(bindings, i);
        g_autofree gchar *subtitle = NULL;

        if (json_object_get_boolean_member(binding, "shared"))
            continue;

        subtitle = g_strdup_printf(
            "%s \342\200\224 configured inside this agent%s%s",
            clawt_json_string(binding, "type", "?"),
            json_object_get_boolean_member(binding, "valid") ? "" : ": ",
            json_object_get_boolean_member(binding, "valid")
                ? "" : clawt_json_string(binding, "problem", ""));

        adw_preferences_group_add(
            ADW_PREFERENCES_GROUP(group),
            info_row(clawt_json_string(binding, "name", "?"), subtitle));
    }

    if (json_array_get_length(integrations) == 0 &&
        (bindings == NULL || json_array_get_length(bindings) == 0))
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(group),
            "None yet. Settings \342\206\222 Integrations adds one -- a "
            "Matrix account to talk to this agent from your phone, or an "
            "MCP server to give it tools.");

    gtk_box_append(self->inspector, group);
}

/*
 * The agent's workspace files, each openable in $EDITOR.
 *
 * These are the files the agent reads as its system prompt, plus the
 * .mcp.json that decides which MCP servers it can call -- so this is
 * where an agent is actually configured, as opposed to merely wired up.
 */
static void
build_files(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    GtkWidget *group;
    JsonArray *files;
    guint i;

    reply = clawt_window_request(
        self, "agent.files",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    files = json_object_get_array_member(clawt_payload_of(reply), "files");

    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Files");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Opened in $EDITOR. The ones marked \"prompt\" are read into every "
        "turn; .mcp.json is the list of MCP servers this agent may call, "
        "and clawtilla only ever rewrites its own entry in it.");

    for (i = 0; files != NULL && i < json_array_get_length(files); i++) {
        JsonObject *file = json_array_get_object_element(files, i);
        const gchar *name = clawt_json_string(file, "name", "?");
        const gchar *path = clawt_json_string(file, "path", "");
        GtkWidget *row = adw_action_row_new();
        GtkWidget *open;
        g_autofree gchar *subtitle = NULL;

        subtitle = json_object_get_boolean_member(file, "identity")
                   ? g_strdup_printf("prompt \xc2\xb7 %s",
                                     clawt_json_string(file, "title", ""))
                   : g_strdup(clawt_json_string(file, "title", ""));

        clawt_gtk_set_row_text(row, name, subtitle);
        gtk_widget_set_tooltip_text(row, path);

        /*
         * An explicit button rather than an activatable row: libadwaita
         * clears GtkListBoxRow:activatable on an AdwActionRow unless it
         * has an activatable-widget, so ::row-activated would never
         * fire and clicking would appear to do nothing.
         */
        open = gtk_button_new_from_icon_name("document-edit-symbolic");
        gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(open, "flat");
        gtk_widget_set_tooltip_text(open, "Open in $EDITOR");
        g_object_set_data_full(G_OBJECT(open), "path", g_strdup(path),
                               g_free);
        g_object_set_data_full(G_OBJECT(open), "name", g_strdup(name),
                               g_free);
        g_signal_connect(open, "clicked", G_CALLBACK(on_open_file), self);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    gtk_box_append(self->inspector, group);
}

/*
 * Rebuilding is the only way to apply anything cloud-init reads at first
 * boot -- the login, the desktop, the package list.  Those are fixed for
 * the life of the overlay, so changing them in the config and restarting
 * looks like it did nothing.
 */
static void
on_rebuild_confirmed(AdwAlertDialog *dialog, const gchar *response,
                     gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "rebuild") != 0)
        return;

    reply = clawt_window_request(
        self, "computer.rebuild",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Rebuilt. Its contents are gone, and "
                             "cloud-init runs again on the next start.");
    clawt_gtk_refresh_selected(self);
}

static void
on_rebuild_computer(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwAlertDialog *ask;

    (void)button;

    ask = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "Rebuild this computer?",
        "Everything in it is destroyed and it is built again from the "
        "image.\n\n"
        "This is what applies settings cloud-init only reads at first "
        "boot -- the login, the desktop, the packages -- which are "
        "otherwise fixed for the life of the machine. It is also how to "
        "get one back that was deleted outside clawtilla.\n\n"
        "The agent has to be stopped."));

    adw_alert_dialog_add_response(ask, "cancel", "Cancel");
    adw_alert_dialog_add_response(ask, "rebuild", "Rebuild");
    adw_alert_dialog_set_response_appearance(ask, "rebuild",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ask, "cancel");

    g_signal_connect(ask, "response", G_CALLBACK(on_rebuild_confirmed), self);
    adw_dialog_present(ADW_DIALOG(ask), GTK_WIDGET(self));
}

/* ── The profile picture ─────────────────────────────────────────── */

/*
 * Sends bytes, and only bytes, to agent.avatar_set.
 *
 * Not a path: the daemon writes them into the agent's own directory and
 * the extension it chooses comes from sniffing them, never from
 * anything this client claims. On success the cache clawt_gtk_avatar_texture()
 * keeps has to be told, or the sidebar and the transcript would go on
 * showing whatever they had -- an event with the same effect is on its
 * way from the daemon, but this window asked for the change and should
 * not wait for its own echo to draw it.
 */
static void
avatar_upload_bytes(ClawtWindow *self, const gchar *agent_id, GBytes *bytes)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *encoded = NULL;
    gconstpointer data;
    gsize length = 0;

    data = g_bytes_get_data(bytes, &length);
    encoded = g_base64_encode(data, length);

    reply = clawt_window_request(
        self, "agent.avatar_set",
        clawt_build_payload("agent", agent_id, "data", encoded, NULL));

    if (reply == NULL)
        return;

    clawt_gtk_avatar_invalidate(agent_id);
    clawt_window_toast(self, "Profile picture updated.");
    clawt_gtk_refresh_selected(self);
    clawt_gtk_refresh_agents(self);
}

static void
on_avatar_file_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GFile) file = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *agent_id = g_object_get_data(source, "agent-id");

    file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result,
                                       &error);

    if (file == NULL) {
        /* Dismissing the dialog is not a failure worth a toast. */
        if (error != NULL &&
            !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
            clawt_window_toast(self, error->message);

        g_object_unref(self);
        return;
    }

    if (agent_id != NULL) {
        g_autofree gchar *contents = NULL;
        g_autoptr(GError) read_error = NULL;
        gsize length = 0;

        if (g_file_load_contents(file, NULL, &contents, &length, NULL,
                                 &read_error)) {
            g_autoptr(GBytes) bytes = g_bytes_new(contents, length);

            avatar_upload_bytes(self, agent_id, bytes);
        } else {
            clawt_window_toast(self, read_error->message);
        }
    }

    g_object_unref(self);
}

static void
on_avatar_choose_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();

    (void)button;

    if (self->selected_agent == NULL)
        return;

    /*
     * No filter set: the daemon sniffs the bytes and refuses anything
     * that is not png/jpeg/webp with a message this window turns into a
     * toast, which is a more honest answer than a filter guessing from
     * a name a file may not even have.
     */
    gtk_file_dialog_set_title(dialog, "Choose a profile picture");
    g_object_set_data_full(G_OBJECT(dialog), "agent-id",
                           g_strdup(self->selected_agent), g_free);

    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL,
                         on_avatar_file_chosen, g_object_ref(self));
}

/* One heap struct rather than object data on the window: two pastes in
 * flight at once (unlikely, but a double-click manages it) must not
 * clobber each other's agent id. */
typedef struct {
    ClawtWindow *window;
    gchar       *agent_id;
} AvatarPaste;

static void
avatar_paste_free(AvatarPaste *paste)
{
    g_object_unref(paste->window);
    g_free(paste->agent_id);
    g_free(paste);
}

static void
on_avatar_texture_pasted(GObject *source, GAsyncResult *result, gpointer user_data)
{
    AvatarPaste *paste = user_data;
    g_autoptr(GdkTexture) texture = NULL;
    g_autoptr(GBytes) png = NULL;
    g_autoptr(GError) error = NULL;

    texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), result,
                                                &error);

    if (texture != NULL) {
        /*
         * PNG, because a pasted screenshot is a texture with no file
         * behind it, and clawt_avatar_sniff_mime_type() reads png, jpeg
         * and webp -- the composer's own paste path made the same
         * choice for the same reason.
         */
        png = gdk_texture_save_to_png_bytes(texture);
        avatar_upload_bytes(paste->window, paste->agent_id, png);
    } else if (error != NULL &&
              !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        clawt_window_toast(paste->window, error->message);
    }

    avatar_paste_free(paste);
}

static void
on_avatar_paste_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
    AvatarPaste *paste;

    (void)button;

    if (self->selected_agent == NULL)
        return;

    if (!gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE)) {
        clawt_window_toast(self, "the clipboard does not hold an image");
        return;
    }

    paste = g_new0(AvatarPaste, 1);
    paste->window = g_object_ref(self);
    paste->agent_id = g_strdup(self->selected_agent);

    gdk_clipboard_read_texture_async(clipboard, NULL, on_avatar_texture_pasted,
                                     paste);
}

static gboolean
on_avatar_drop(GtkDropTarget *target, const GValue *value, gdouble x,
              gdouble y, gpointer user_data)
{
    ClawtWindow *self = user_data;
    GdkFileList *list;
    GSList *files;
    GFile *file;
    g_autofree gchar *contents = NULL;
    g_autoptr(GError) error = NULL;
    gsize length = 0;

    (void)target;
    (void)x;
    (void)y;

    if (self->selected_agent == NULL || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;

    list = g_value_get_boxed(value);
    files = gdk_file_list_get_files(list);

    if (files == NULL)
        return FALSE;

    file = files->data;

    if (g_file_load_contents(file, NULL, &contents, &length, NULL, &error)) {
        g_autoptr(GBytes) bytes = g_bytes_new(contents, length);

        avatar_upload_bytes(self, self->selected_agent, bytes);
    } else {
        clawt_window_toast(self, error->message);
    }

    g_slist_free(files);
    return TRUE;
}

static void
on_avatar_clear_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "agent.avatar_clear",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    clawt_gtk_avatar_invalidate(self->selected_agent);
    clawt_gtk_refresh_selected(self);
    clawt_gtk_refresh_agents(self);
}

/*
 * The face, and the three ways to change it.
 *
 * A dedicated group rather than a row among "Settings": a picture is not
 * a config value typed into an entry, it is dropped, pasted or chosen,
 * and the face itself is the preview -- there is nothing else worth
 * showing beside it.
 */
static void
build_avatar_group(ClawtWindow *self, JsonObject *agent)
{
    GtkWidget *group;
    GtkWidget *row;
    GtkWidget *avatar;
    GtkWidget *box;
    GtkWidget *choose;
    GtkWidget *paste;
    GtkWidget *clear;
    GtkDropTarget *drop;
    const gchar *agent_id = clawt_json_string(agent, "id", "");
    const gchar *name = clawt_json_string(agent, "name", agent_id);
    const gchar *configured = clawt_json_string(agent, "avatar", "");
    gboolean has_avatar = clawt_json_boolean(agent, "has_avatar", FALSE);

    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Profile picture");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        has_avatar
            ? ((configured != NULL && *configured != '\0')
                   ? "Set from agents.avatar."
                   : "Auto-detected: profile-picture.png (or .jpg, .jpeg, "
                     "or .webp) in the agent's own directory.")
            : "No picture yet, so initials are shown instead. Drop an "
              "image onto the face, choose one, or paste from the "
              "clipboard.");

    row = adw_action_row_new();
    clawt_gtk_set_row_text(row, "Picture",
                          "PNG, JPEG or WebP -- the file's own bytes decide, "
                          "not its name.");

    avatar = clawt_gtk_build_avatar(
        self->client, name, agent_id, has_avatar,
        clawt_json_string(agent, "color", NULL), 64);
    gtk_widget_set_valign(avatar, GTK_ALIGN_CENTER);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), avatar);

    drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_avatar_drop), self);
    gtk_widget_add_controller(avatar, GTK_EVENT_CONTROLLER(drop));

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    choose = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_tooltip_text(choose, "Choose an image file");
    gtk_widget_add_css_class(choose, "flat");
    g_signal_connect(choose, "clicked", G_CALLBACK(on_avatar_choose_clicked),
                     self);
    gtk_box_append(GTK_BOX(box), choose);

    paste = gtk_button_new_from_icon_name("edit-paste-symbolic");
    gtk_widget_set_tooltip_text(paste, "Paste an image from the clipboard");
    gtk_widget_add_css_class(paste, "flat");
    g_signal_connect(paste, "clicked", G_CALLBACK(on_avatar_paste_clicked),
                     self);
    gtk_box_append(GTK_BOX(box), paste);

    clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(clear, "Remove the picture");
    gtk_widget_add_css_class(clear, "flat");
    gtk_widget_set_sensitive(clear, has_avatar);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_avatar_clear_clicked),
                     self);
    gtk_box_append(GTK_BOX(box), clear);

    adw_action_row_add_suffix(ADW_ACTION_ROW(row), box);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);

    gtk_box_append(self->inspector, group);
}

void
clawt_gtk_build_inspector(ClawtWindow *self, JsonObject *agent, JsonObject *payload)
{
    static const gchar *const efforts[] = { "low", "medium", "high",
                                            "xhigh", "max", NULL };
    static const gchar *const restarts[] = { "never", "on-failure",
                                             "always", NULL };
    GtkWidget *group;
    GtkWidget *actions;
    GtkWidget *save;
    GtkWidget *danger;
    GtkWidget *delete_button;
    const gchar *state = clawt_json_string(agent, "state", "stopped");
    const gchar *caps = clawt_json_string(agent, "caps", "");
    gboolean is_shadow = g_strcmp0(state, "shadow") == 0;

    clawt_gtk_clear_box(self->inspector);
    g_clear_pointer(&self->inspector_models.catalog, json_node_unref);

    /*
     * The image chooser is rebuilt with the inspector, so its catalogue
     * and its widget pointers are cleared together -- a stale row
     * pointer here is one the save handler would write through after
     * the widget had been destroyed.
     */
    g_clear_pointer(&self->inspector_image.catalog, json_node_unref);
    self->inspector_image.row = NULL;
    self->inspector_image.entry = NULL;

    /* ── What it is, and what it is doing ── */
    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Agent");

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              info_row("Id", clawt_json_string(agent, "id",
                                                               "?")));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              info_row("State", state));

    if (clawt_json_string(agent, "detail", NULL) != NULL)
        adw_preferences_group_add(
            ADW_PREFERENCES_GROUP(group),
            info_row("Why", clawt_json_string(agent, "detail", NULL)));

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              info_row("Can do", caps));

    /*
     * What the persona costs, and -- only when it is worth saying --
     * which files account for it.
     *
     * An agent whose identity files outgrow a single command-line
     * argument cannot start a fresh session on a backend that passes the
     * system prompt as one, and the kernel's refusal names neither the
     * files nor the limit.  It is silent right up to the cliff, so the
     * number belongs somewhere a person looks *before* anything fails.
     *
     * The warning row is drawn only past the threshold.  A byte count is
     * shown always, because a size somebody can watch is the whole point
     * and a row that appears only in trouble teaches nobody what normal
     * looks like.
     */
    if (json_object_has_member(payload, "identity")) {
        JsonObject *identity =
            json_object_get_object_member(payload, "identity");
        gint64 bytes = clawt_json_int(identity, "bytes", 0);
        gint64 limit = clawt_json_int(identity, "limit", 0);
        const gchar *verdict = clawt_json_string(identity, "verdict", NULL);

        if (bytes > 0) {
            g_autofree gchar *text = g_strdup_printf(
                "%" G_GINT64_FORMAT " bytes of %" G_GINT64_FORMAT,
                bytes, limit);

            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                      info_row("Identity", text));
        }

        if (verdict != NULL) {
            GtkWidget *row = info_row("Too large", verdict);

            /*
             * Wrapped rather than ellipsised: the sentence names the
             * files to shorten, and a subtitle cut at the width of the
             * pane would hide exactly the half somebody needs.
             */
            adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
            gtk_widget_add_css_class(row, (bytes >= limit) ? "error"
                                                           : "warning");
            gtk_widget_add_css_class(row, "clawt-identity-size");
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
        }
    }

    gtk_box_append(self->inspector, group);

    build_avatar_group(self, agent);

    /* ── Editable ── */
    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Settings");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The model and the computer take effect when the agent next "
        "starts.");

    self->name_row = entry_row("Name", clawt_json_string(agent, "name", ""));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->name_row);

    self->description_row = entry_row(
        "Description", clawt_json_string(agent, "description", ""));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->description_row);

    /*
     * Which skills this agent is assigned, on top of its team's and the
     * fleet's.
     *
     * Names, comma-separated, exactly as the web client's generic field
     * takes them -- the daemon dispatches on the schema type, so both
     * clients write a YAML sequence rather than a scalar. A name that
     * matches nothing is a warning on the Skills page rather than a
     * refusal here: a fleet is edited by hand and half-built states are
     * ordinary.
     */
    self->skills_row = entry_row(
        "Skills",
        (payload != NULL && json_object_has_member(payload, "settings"))
            ? clawt_json_string(
                  json_object_get_object_member(payload, "settings"),
                  "skills", "")
            : "");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->skills_row);

    clawt_gtk_model_chooser_build(&self->inspector_models, self, group,
                                  clawt_json_string(agent, "provider", NULL),
                                  clawt_json_string(agent, "model", NULL));

    self->effort_row = combo_row("Effort", efforts,
                                 clawt_json_string(agent, "effort",
                                                   "medium"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->effort_row);

    self->computer_row = combo_row("Computer", clawt_gtk_computer_type_nicks(),
                                   clawt_json_string(agent, "computer",
                                                     "none"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->computer_row);

    /*
     * The image, for a container agent.  Built only when there is one:
     * an agent that is not a container has nothing to show, and the
     * inspector is rebuilt whenever the selection changes, so switching
     * the computer type and saving brings the row in on the next
     * refresh.
     */
    g_free(self->inspector_computer);
    self->inspector_computer =
        g_strdup(clawt_json_string(agent, "computer", "none"));

    if (clawt_computer_type_takes_image(
            clawt_gtk_computer_type_from_nick(self->inspector_computer)))
        clawt_gtk_image_chooser_build(&self->inspector_image, self, group,
                                      clawt_json_string(agent, "image", NULL));
    else
        self->inspector_image.row = NULL;

    /*
     * The VM's disk, size and address.  Only shown for a VM: no other
     * backend reads these, and a row that quietly does nothing is worse
     * than no row.
     */
    self->inspector_disk.row = NULL;
    self->vm_cpus_row = NULL;
    self->vm_memory_row = NULL;
    self->vm_disk_row = NULL;
    self->vm_resolution_row = NULL;
    self->vm_ssh_host_row = NULL;
    self->vm_desktop_row = NULL;
    self->vm_desktop_input_row = NULL;

    if (g_strcmp0(self->inspector_computer, "vm") == 0) {
        clawt_gtk_disk_chooser_build(&self->inspector_disk, self, group,
                                     clawt_json_string(agent, "vm_image", NULL));

        self->vm_cpus_row = entry_row(
            "Cores", clawt_json_string(agent, "vm_cpus", ""));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_cpus_row);

        self->vm_memory_row = entry_row(
            "Memory (MB)", clawt_json_string(agent, "vm_memory_mb", ""));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_memory_row);

        self->vm_disk_row = entry_row(
            "Disk (GB)", clawt_json_string(agent, "vm_disk_gb", ""));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_disk_row);

        /*
         * Beside the other things the machine is made of, because that is
         * what it is -- and unlike almost everything else about a VM it
         * is not baked into the cloud-init seed, so it applies at the
         * guest's next boot rather than needing the machine rebuilt.
         */
        self->vm_resolution_row = resolution_row(
            clawt_json_string(agent, "vm_resolution", "1280x800"));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_resolution_row);

        /*
         * A cloud image has no desktop at all, so this installs one --
         * GNOME, GDM and an autologin -- on the guest's first boot. It
         * takes a while and it wants the memory and disk above.
         */
        self->vm_desktop_row = switch_row(
            "Desktop in the VM",
            "Installs GNOME and logs it in, then lets this agent see it. "
            "Built on first boot, so give it time.",
            json_object_has_member(agent, "desktop_enabled")
                ? json_object_get_boolean_member(agent, "desktop_enabled")
                : FALSE);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_desktop_row);

        /*
         * Separate from seeing it, and off by default.  An agent that can
         * screenshot but not click is a genuinely useful amount of access
         * and a much smaller grant.
         */
        self->vm_desktop_input_row = switch_row(
            "Let it click and type",
            "Without this the agent can take screenshots and list windows "
            "but cannot act.",
            json_object_has_member(agent, "desktop_input")
                ? json_object_get_boolean_member(agent, "desktop_input")
                : FALSE);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_desktop_input_row);

        /*
         * Left empty, clawtilla forwards a port to the guest itself.  It
         * is here for the VM that lives somewhere clawtilla did not put
         * it.
         */
        self->vm_ssh_host_row = entry_row(
            "SSH address (optional)",
            clawt_json_string(agent, "vm_ssh_host", ""));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_ssh_host_row);
    }

    self->restart_row = combo_row("Restart", restarts,
                                  clawt_json_string(agent, "restart",
                                                    "on-failure"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->restart_row);

    self->autostart_row = switch_row(
        "Start with the daemon", NULL,
        json_object_has_member(agent, "autostart")
            ? json_object_get_boolean_member(agent, "autostart") : TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->autostart_row);

    /*
     * Which team, and what standing on it. Beside the chief switch
     * because the three answer one question between them: who this
     * agent may hand work to.
     */
    {
        GtkStringList *choices;

        g_clear_pointer(&self->team_ids, g_strfreev);
        choices = clawt_gtk_team_choices(self, clawt_json_string(agent, "team", ""),
                                         &self->team_ids);

        self->team_row = adw_combo_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->team_row),
                                      "Team");
        adw_combo_row_set_model(ADW_COMBO_ROW(self->team_row),
                                G_LIST_MODEL(choices));
        adw_combo_row_set_selected(
            ADW_COMBO_ROW(self->team_row),
            clawt_gtk_team_index_of(self->team_ids,
                                    clawt_json_string(agent, "team", "")));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->team_row);
    }

    {
        static const gchar *const roles[] = { "member", "lead", NULL };

        self->team_role_row = combo_row(
            "Role on that team", roles,
            clawt_json_string(agent, "team_role", "member"));
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(self->team_role_row),
            "A lead assigns work inside its own team. A member talks to "
            "anyone and assigns to nobody.");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->team_role_row);
    }

    self->chief_row = switch_row(
        "Chief of staff", "Hands work to the other agents",
        json_object_has_member(agent, "chief_of_staff") &&
        json_object_get_boolean_member(agent, "chief_of_staff"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->chief_row);

    /*
     * Beside the chief-of-staff switch, because that is where somebody
     * looking for it goes.
     *
     * Being the chief of staff and being allowed to create agents are
     * two settings, and the first is the one with the obvious name -- a
     * person enabled it, asked their chief to make an agent, and was
     * told it had no such tool. Which was true: the tool is gated on
     * this one, and nothing on screen said so.
     */
    self->manage_fleet_row = switch_row(
        "May create agents",
        "Adds clawtilla_create_agent. It can give a new agent a container "
        "or a VM, which is a machine that runs code.",
        json_object_has_member(agent, "manage_fleet") &&
        json_object_get_boolean_member(agent, "manage_fleet"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->manage_fleet_row);

    save = gtk_button_new_with_label("Save changes");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_margin_top(save, 12);
    g_signal_connect(save, "clicked", G_CALLBACK(on_save_agent), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), save);

    gtk_box_append(self->inspector, group);

    /* Credentials: named, never valued. */
    if (json_object_has_member(agent, "credentials")) {
        JsonObject *credentials = json_object_get_object_member(agent,
                                                                "credentials");
        g_autoptr(GList) names = json_object_get_members(credentials);
        GtkWidget *creds = adw_preferences_group_new();
        GList *l;

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(creds),
                                        "Credentials");
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(creds),
            "Shown by reference. clawtilla never sends the values to a "
            "client, so nothing secret can end up in a screenshot.");

        for (l = names; l != NULL; l = l->next)
            adw_preferences_group_add(
                ADW_PREFERENCES_GROUP(creds),
                info_row(l->data,
                         json_object_get_string_member(credentials,
                                                       l->data)));

        if (names != NULL)
            gtk_box_append(self->inspector, creds);
    }

    build_agent_integrations(self);

    if (clawt_json_string(payload, "computer_detail", NULL) != NULL) {
        GtkWidget *detail = adw_preferences_group_new();
        GtkWidget *label = gtk_label_new(
            clawt_json_string(payload, "computer_detail", ""));

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(detail),
                                        "Computer");
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE);
        gtk_widget_set_margin_top(label, 6);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(detail), label);

        gtk_box_append(self->inspector, detail);
    }

    /*
     * Offered whenever there is a computer configured, not only when one
     * currently exists -- the case it is most wanted for is a guest that
     * has gone, where there is nothing left to report on above.
     */
    if (g_strcmp0(clawt_json_string(agent, "computer", "none"),
                  "none") != 0) {
        GtkWidget *rebuild_group = adw_preferences_group_new();
        GtkWidget *rebuild = gtk_button_new_with_label("Rebuild computer");

        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(rebuild_group),
            "Destroys it and builds it again from the image. The only way "
            "to apply the login, the desktop or the package list, which "
            "are read once at first boot.");

        gtk_widget_add_css_class(rebuild, "destructive-action");
        gtk_widget_set_halign(rebuild, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(rebuild, 6);

        /* Destroying the machine underneath a running agent is not a
         * thing to do carefully; it is a thing not to offer. */
        gtk_widget_set_sensitive(
            rebuild, g_strcmp0(clawt_json_string(agent, "state", ""),
                               "stopped") == 0);

        g_signal_connect(rebuild, "clicked",
                         G_CALLBACK(on_rebuild_computer), self);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(rebuild_group),
                                  rebuild);

        gtk_box_append(self->inspector, rebuild_group);
    }

    /* ── Running it ── */
    actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(GTK_WIDGET(actions), 12);
    gtk_widget_set_halign(actions, GTK_ALIGN_CENTER);

    /*
     * Every control's sensitivity comes from what the agent can actually
     * do, so nothing here offers something that would fail.
     */
    gtk_box_append(GTK_BOX(actions),
                   action_button(self, "Start", "agent.start",
                                 !is_shadow &&
                                 g_strcmp0(state, "running") != 0,
                                 is_shadow
                                     ? "this agent's configuration could "
                                       "not be understood"
                                     : "already running"));
    gtk_box_append(GTK_BOX(actions),
                   action_button(self, "Stop", "agent.stop",
                                 g_strcmp0(state, "stopped") != 0 &&
                                 !is_shadow, "not running"));
    gtk_box_append(GTK_BOX(actions),
                   action_button(self, "Restart", "agent.restart",
                                 !is_shadow,
                                 "this agent cannot start"));

    gtk_box_append(self->inspector, actions);

    build_mounts(self, self->inspector_computer);
    build_files(self);

    /* ── Removing it ── */
    /*
     * Built here rather than in the "Settings" group above, so the
     * generated rows sit apart from the hand-written ones and nobody
     * has to wonder which is which.
     */
    build_schema_rows(self,
                      (payload != NULL &&
                       json_object_has_member(payload, "settings"))
                      ? json_object_get_object_member(payload, "settings")
                      : NULL);

    danger = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(danger),
                                    "Danger zone");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(danger),
        "Removing an agent stops it and takes it out of every room. Its "
        "mailbox and transcripts stay on disk.");

    delete_button = gtk_button_new_with_label("Delete this agent");
    gtk_widget_add_css_class(delete_button, "destructive-action");
    gtk_widget_set_margin_top(delete_button, 6);
    g_signal_connect(delete_button, "clicked", G_CALLBACK(on_delete_agent),
                     self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(danger), delete_button);

    gtk_box_append(self->inspector, danger);
}

GtkWidget *
clawt_gtk_build_inspector_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->inspector = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    gtk_widget_set_margin_top(GTK_WIDGET(self->inspector), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->inspector), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(self->inspector), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->inspector), 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->inspector));

    return scroll;
}
