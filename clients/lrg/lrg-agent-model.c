/*
 * lrg-agent-model.c - What the Agent, Mailbox and Computer pages decide
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * No libregnum here: the pages in lrg-inspector.c, lrg-mailbox.c,
 * lrg-computer.c and lrg-hold.c only draw what these functions return,
 * and tests/test-lrg-models-agent.c checks them without a display.
 *
 * Every member read off a reply was checked against the daemon's own
 * json_builder_set_member_name() (src/core/daemon-*.c, clawt-daemon.c),
 * because a client reading a name that sounds right gets its fallback
 * in silence -- AGENTS.md has a dozen of those.
 */

#include "clawt-lrg-agent-model.h"

#include <string.h>

/* ── Small readers ────────────────────────────────────────────────── */

/* An array member, or NULL when it is absent or not an array. */
static JsonArray *
member_array(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;

    node = json_object_get_member(object, name);

    return JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

/* An object member, or NULL. */
static JsonObject *
member_object(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;

    node = json_object_get_member(object, name);

    return JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

/* The i-th element as an object, or NULL for anything else. */
static JsonObject *
element_object(JsonArray *array, guint i)
{
    JsonNode *node = json_array_get_element(array, i);

    return JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static const gchar *
string_or(JsonObject *object, const gchar *name, const gchar *fallback)
{
    const gchar *value = clawt_ipc_payload_string(object, name);

    return (value != NULL) ? value : fallback;
}

/* ── Choices ──────────────────────────────────────────────────────── */

ClawtLrgAgentChoices *
clawt_lrg_agent_choices_new(void)
{
    ClawtLrgAgentChoices *choices = g_new0(ClawtLrgAgentChoices, 1);

    /*
     * NULL-terminated, so the arrays' pdata can go straight to a combo
     * that takes a NULL-terminated list.
     */
    choices->ids = g_ptr_array_new_null_terminated(8, g_free, TRUE);
    choices->labels = g_ptr_array_new_null_terminated(8, g_free, TRUE);

    return choices;
}

void
clawt_lrg_agent_choices_free(ClawtLrgAgentChoices *choices)
{
    if (choices == NULL)
        return;

    g_ptr_array_unref(choices->ids);
    g_ptr_array_unref(choices->labels);
    g_free(choices->selected);
    g_free(choices->other);
    g_free(choices);
}

void
clawt_lrg_agent_choices_add(ClawtLrgAgentChoices *choices, const gchar *id,
                      const gchar *label)
{
    g_return_if_fail(choices != NULL);
    g_return_if_fail(id != NULL);

    g_ptr_array_add(choices->ids, g_strdup(id));
    g_ptr_array_add(choices->labels, g_strdup(label != NULL ? label : id));
}

gboolean
clawt_lrg_agent_choices_has(ClawtLrgAgentChoices *choices, const gchar *id)
{
    guint i;

    for (i = 0; choices != NULL && i < choices->ids->len; i++) {
        if (g_strcmp0(g_ptr_array_index(choices->ids, i), id) == 0)
            return TRUE;
    }

    return FALSE;
}

const gchar *const *
clawt_lrg_agent_choices_ids(ClawtLrgAgentChoices *choices)
{
    return (const gchar *const *)choices->ids->pdata;
}

const gchar *const *
clawt_lrg_agent_choices_labels(ClawtLrgAgentChoices *choices)
{
    return (const gchar *const *)choices->labels->pdata;
}

/*
 * Selects @current, adding it under "(not declared)" when the list does
 * not have it.  A combo that cannot show the current value replaces it
 * the moment somebody saves the page without touching it (AGENTS.md, "A
 * control that cannot represent the current value will silently replace
 * it") -- so the value joins the list rather than the first entry
 * standing in for it.
 */
static void
choices_keep_current(ClawtLrgAgentChoices *choices, const gchar *current,
                     const gchar *suffix)
{
    const gchar *wanted = (current != NULL) ? current : "";

    if (!clawt_lrg_agent_choices_has(choices, wanted)) {
        g_autofree gchar *label = g_strconcat(wanted, suffix, NULL);

        clawt_lrg_agent_choices_add(choices, wanted, label);
    }

    g_free(choices->selected);
    choices->selected = g_strdup(wanted);
}

gchar *
clawt_lrg_agent_choice_value(const gchar *combo_id, const gchar *other_text)
{
    if (g_strcmp0(combo_id, CLAWT_LRG_AGENT_CHOICE_OTHER) == 0) {
        if (other_text == NULL || *other_text == '\0')
            return NULL;

        return g_strstrip(g_strdup(other_text));
    }

    return g_strdup(combo_id);
}

ClawtLrgAgentChoices *
clawt_lrg_agent_enum_choices(GType type, const gchar *current)
{
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();

    /*
     * Walked from the type, never listed.  Every hand-written copy of an
     * option's values in this project has drifted from the enum it
     * copied (AGENTS.md, "Never hand-maintain a list").
     */
    if (G_TYPE_IS_ENUM(type)) {
        g_autoptr(GEnumClass) klass = g_type_class_ref(type);
        guint i;

        for (i = 0; i < klass->n_values; i++)
            clawt_lrg_agent_choices_add(choices, klass->values[i].value_nick,
                                  klass->values[i].value_nick);
    } else if (G_TYPE_IS_FLAGS(type)) {
        g_autoptr(GFlagsClass) klass = g_type_class_ref(type);
        guint i;

        for (i = 0; i < klass->n_values; i++)
            clawt_lrg_agent_choices_add(choices, klass->values[i].value_nick,
                                  klass->values[i].value_nick);
    }

    /*
     * An unset enum reports "" and inherits from the fleet; it stays
     * unset rather than being pinned to whatever happens to be first.
     */
    if (current == NULL || *current == '\0') {
        clawt_lrg_agent_choices_add(choices, "", "(follows the fleet)");
        choices->selected = g_strdup("");
        return choices;
    }

    choices_keep_current(choices, current, " (not a known value)");
    return choices;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_computer_type_choices(const gchar *current)
{
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();
    guint n = clawt_computer_type_count();
    guint i;

    /*
     * The library's own enumeration with its labels, so a type added
     * there is offered here (and `make parity` sees this client walk it).
     */
    for (i = 0; i < n; i++)
        clawt_lrg_agent_choices_add(choices, clawt_computer_type_nth_nick(i),
                              clawt_computer_type_nth_label(i));

    if (current == NULL || *current == '\0') {
        clawt_lrg_agent_choices_add(choices, "", "(follows the fleet)");
        choices->selected = g_strdup("");
        return choices;
    }

    choices_keep_current(choices, current, " (not a known type)");
    return choices;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_effort_choices(const gchar *current)
{
    /*
     * The one list here with no enumeration behind it: model.effort is a
     * string in the schema and the levels are the CLIs', documented in
     * the schema's own sentence.  GTK carries the same five.  An unknown
     * level a person typed into the YAML is kept, not replaced.
     */
    static const gchar *const levels[] = {
        "low", "medium", "high", "xhigh", "max", NULL
    };
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();
    guint i;

    clawt_lrg_agent_choices_add(choices, "", "(the model's default)");

    for (i = 0; levels[i] != NULL; i++)
        clawt_lrg_agent_choices_add(choices, levels[i], levels[i]);

    choices_keep_current(choices, current, " (not a known level)");
    return choices;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_resolution_choices(const gchar *current)
{
    /*
     * The sizes worth offering, plus whatever this VM already has: one
     * set by hand would otherwise show as the first entry and a save
     * would quietly change it.
     */
    static const gchar *const common[] = {
        "1280x800", "1280x1024", "1440x900", "1600x900", "1680x1050",
        "1920x1080", "1920x1200", "2560x1440", "3840x2160", NULL
    };
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();
    guint i;

    for (i = 0; common[i] != NULL; i++)
        clawt_lrg_agent_choices_add(choices, common[i], common[i]);

    if (current == NULL || *current == '\0') {
        choices->selected = g_strdup("1280x800");
        return choices;
    }

    choices_keep_current(choices, current, "");
    return choices;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_mount_mode_choices(void)
{
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_enum_choices(CLAWT_TYPE_MOUNT_MODE,
                                                      "ro");

    return choices;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_relabel_choices(void)
{
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();
    guint i;

    /*
     * Walked from the library.  This is the setting that decides whether
     * podman will start the container at all, and its refusal names it --
     * a client that cannot spell the remedy its own error recommends is
     * the client that caused the error.
     */
    for (i = 0; i < clawt_relabel_count(); i++)
        clawt_lrg_agent_choices_add(choices, clawt_relabel_nth_nick(i),
                              clawt_relabel_nth_label(i));

    choices->selected = g_strdup(
        clawt_enum_to_nick(CLAWT_TYPE_RELABEL, CLAWT_RELABEL_SHARED));

    return choices;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_team_choices(JsonNode *reply, const gchar *current)
{
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();
    JsonArray *teams = member_array(clawt_ipc_reply_object(reply), "teams");
    guint i;

    /* "No team" is a real choice: it is how an agent comes off one. */
    clawt_lrg_agent_choices_add(choices, "", "No team");

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = element_object(teams, i);
        const gchar *id = clawt_ipc_payload_string(team, "id");

        if (id == NULL || *id == '\0')
            continue;

        clawt_lrg_agent_choices_add(choices, id, string_or(team, "name", id));
    }

    choices_keep_current(choices, current, " (not declared)");
    return choices;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_account_choices(JsonNode *reply, const gchar *current)
{
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();
    JsonArray *accounts = member_array(clawt_ipc_reply_object(reply),
                                       "accounts");
    guint i;

    clawt_lrg_agent_choices_add(choices, "", "The CLI's own login");

    /*
     * Only what agent.set will accept -- the daemon answers with the
     * same check -- except the current one, which stays even when it is
     * no longer usable so that saving cannot quietly drop it.
     */
    for (i = 0; accounts != NULL && i < json_array_get_length(accounts); i++) {
        JsonObject *account = element_object(accounts, i);
        const gchar *id = clawt_ipc_payload_string(account, "id");
        const gchar *description = clawt_ipc_payload_string(account,
                                                            "description");
        gboolean usable = clawt_ipc_payload_boolean(account, "usable", FALSE);
        g_autofree gchar *label = NULL;

        if (id == NULL || (!usable && g_strcmp0(id, current) != 0))
            continue;

        label = g_strdup_printf(
            "%s%s%s%s", id, description != NULL ? " -- " : "",
            description != NULL ? description : "",
            usable ? "" : " (cannot be used: see the agent's warning)");
        clawt_lrg_agent_choices_add(choices, id, label);
    }

    choices_keep_current(choices, current, " (not declared)");
    return choices;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_shared_choices(JsonNode *reply, const gchar *current)
{
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();
    JsonArray *computers = member_array(clawt_ipc_reply_object(reply),
                                        "computers");
    guint i;

    clawt_lrg_agent_choices_add(choices, "", "Its own machine");

    for (i = 0; computers != NULL && i < json_array_get_length(computers);
         i++) {
        JsonObject *computer = element_object(computers, i);
        const gchar *id = clawt_ipc_payload_string(computer, "id");
        const gchar *description = clawt_ipc_payload_string(computer,
                                                            "description");
        g_autofree gchar *label = NULL;

        /* A host machine cannot be shared, and agent.set refuses it. */
        if (id == NULL ||
            (!clawt_ipc_payload_boolean(computer, "shareable", TRUE) &&
             g_strcmp0(id, current) != 0))
            continue;

        label = g_strdup_printf("%s (%s)%s%s", id,
                                string_or(computer, "type", "none"),
                                description != NULL ? " -- " : "",
                                description != NULL ? description : "");
        clawt_lrg_agent_choices_add(choices, id, label);
    }

    choices_keep_current(choices, current, " (not declared)");
    return choices;
}

/*
 * The image and disk choosers share a shape: a catalogue, an "Other…"
 * entry past its end, and a value that is not in the catalogue kept in
 * the free entry rather than retargeted at whatever is first.
 */
static ClawtLrgAgentChoices *
catalogue_choices(JsonNode *reply, const gchar *id_member,
                  const gchar *label_member, const gchar *current,
                  const gchar *fallback)
{
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();
    JsonArray *images = member_array(clawt_ipc_reply_object(reply), "images");
    gboolean matched = FALSE;
    guint i;

    for (i = 0; images != NULL && i < json_array_get_length(images); i++) {
        JsonObject *image = element_object(images, i);
        const gchar *id = clawt_ipc_payload_string(image, id_member);

        if (id == NULL || *id == '\0')
            continue;

        clawt_lrg_agent_choices_add(choices, id, string_or(image, label_member, id));

        if (g_strcmp0(id, current) == 0)
            matched = TRUE;
    }

    clawt_lrg_agent_choices_add(choices, CLAWT_LRG_AGENT_CHOICE_OTHER, "Other\xe2\x80\xa6");

    if (matched) {
        choices->selected = g_strdup(current);
    } else if (current != NULL && *current != '\0') {
        choices->selected = g_strdup(CLAWT_LRG_AGENT_CHOICE_OTHER);
        choices->other = g_strdup(current);
    } else if (fallback != NULL && clawt_lrg_agent_choices_has(choices, fallback)) {
        choices->selected = g_strdup(fallback);
    } else {
        choices->selected = g_strdup(CLAWT_LRG_AGENT_CHOICE_OTHER);
    }

    return choices;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_image_choices(JsonNode *reply, const gchar *current)
{
    /*
     * With nothing chosen, the daemon's own `default` -- what the agent
     * would run in if the field stayed empty -- rather than the first
     * entry in the catalogue.
     */
    return catalogue_choices(
        reply, "reference", "label", current,
        clawt_ipc_payload_string(clawt_ipc_reply_object(reply), "default"));
}

ClawtLrgAgentChoices *
clawt_lrg_agent_disk_choices(JsonNode *reply, const gchar *current)
{
    /*
     * No fallback: a VM with no disk does not boot, so an agent with
     * none chosen must look like it, with "Other" and an empty path.
     */
    return catalogue_choices(reply, "path", "name", current, NULL);
}

gchar *
clawt_lrg_agent_image_note(JsonNode *reply, const gchar *id)
{
    JsonArray *images = member_array(clawt_ipc_reply_object(reply), "images");
    guint i;

    if (g_strcmp0(id, CLAWT_LRG_AGENT_CHOICE_OTHER) == 0)
        return g_strdup("any reference podman can pull");

    /*
     * The full reference under the label, because "Fedora 44" does not
     * say which registry it comes from -- which is exactly what differs
     * between two machines that disagree about an unqualified name.
     */
    for (i = 0; images != NULL && i < json_array_get_length(images); i++) {
        JsonObject *image = element_object(images, i);
        const gchar *note;

        if (g_strcmp0(clawt_ipc_payload_string(image, "reference"), id) != 0)
            continue;

        note = clawt_ipc_payload_string(image, "note");

        return (note != NULL) ? g_strdup_printf("%s -- %s", id, note)
                              : g_strdup(id);
    }

    return g_strdup(id != NULL ? id : "");
}

gchar *
clawt_lrg_agent_disk_note(JsonNode *reply, const gchar *id)
{
    JsonArray *images = member_array(clawt_ipc_reply_object(reply), "images");

    if (g_strcmp0(id, CLAWT_LRG_AGENT_CHOICE_OTHER) != 0)
        return g_strdup(id != NULL ? id : "");

    return g_strdup(
        (images == NULL || json_array_get_length(images) == 0)
            ? "No cloud images yet \xe2\x80\x94 fetch one from Settings"
            : "A path to a qcow2 on this machine");
}

/* The provider objects of a model.list reply. */
static JsonArray *
catalog_providers(JsonNode *catalog)
{
    return member_array(clawt_ipc_reply_object(catalog), "providers");
}

static JsonObject *
catalog_provider(JsonNode *catalog, const gchar *provider)
{
    JsonArray *providers = catalog_providers(catalog);
    guint i;

    for (i = 0; providers != NULL && i < json_array_get_length(providers);
         i++) {
        JsonObject *object = element_object(providers, i);

        if (g_strcmp0(clawt_ipc_payload_string(object, "id"), provider) == 0)
            return object;
    }

    return NULL;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_provider_choices(JsonNode *catalog, const gchar *current)
{
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();
    JsonArray *providers = catalog_providers(catalog);
    guint i;

    for (i = 0; providers != NULL && i < json_array_get_length(providers);
         i++) {
        JsonObject *provider = element_object(providers, i);
        const gchar *id = clawt_ipc_payload_string(provider, "id");

        if (id == NULL)
            continue;

        /*
         * Only what libreclaw can drive as an agent: anything else it
         * rewrites to claude-code, so an HTTP provider offered here gives
         * an agent Claude Code holding a model name it rejects.  A
         * missing flag is an older daemon -- kept, since an empty list is
         * a worse failure -- and the current provider is kept whatever it
         * is, or the combo would point at the first entry and a save
         * would change the provider to something nobody chose.
         */
        if (provider != NULL && json_object_has_member(provider, "agent") &&
            !clawt_ipc_payload_boolean(provider, "agent", TRUE) &&
            g_strcmp0(id, current) != 0)
            continue;

        clawt_lrg_agent_choices_add(choices, id, string_or(provider, "label", id));
    }

    if (current == NULL || *current == '\0') {
        clawt_lrg_agent_choices_add(choices, "", "(follows the fleet)");
        choices->selected = g_strdup("");
        return choices;
    }

    choices_keep_current(choices, current, " (not offered)");
    return choices;
}

ClawtLrgAgentChoices *
clawt_lrg_agent_model_choices(JsonNode *catalog, const gchar *provider,
                              const gchar *current)
{
    ClawtLrgAgentChoices *choices = clawt_lrg_agent_choices_new();
    JsonObject *object = catalog_provider(catalog, provider);
    JsonArray *models = member_array(object, "models");
    gboolean matched = FALSE;
    guint i;

    /*
     * The models a provider runs are not interchangeable -- "sonnet"
     * means nothing to Ollama -- so the list is per provider.
     */
    for (i = 0; models != NULL && i < json_array_get_length(models); i++) {
        JsonObject *model = element_object(models, i);
        const gchar *id = clawt_ipc_payload_string(model, "id");
        const gchar *note = clawt_ipc_payload_string(model, "note");
        g_autofree gchar *label = NULL;

        if (id == NULL)
            continue;

        label = (note != NULL)
            ? g_strdup_printf("%s - %s", string_or(model, "label", id), note)
            : g_strdup(string_or(model, "label", id));
        clawt_lrg_agent_choices_add(choices, id, label);

        if (g_strcmp0(id, current) == 0)
            matched = TRUE;
    }

    /*
     * The catalogue is curated and goes stale, so every provider also
     * takes a name typed by hand; a current model it does not list goes
     * there rather than being replaced.
     */
    clawt_lrg_agent_choices_add(choices, CLAWT_LRG_AGENT_CHOICE_OTHER,
                          "Something else\xe2\x80\xa6");

    if (matched) {
        choices->selected = g_strdup(current);
    } else if (current != NULL && *current != '\0') {
        choices->selected = g_strdup(CLAWT_LRG_AGENT_CHOICE_OTHER);
        choices->other = g_strdup(current);
    } else if (choices->ids->len > 1) {
        choices->selected = g_strdup(g_ptr_array_index(choices->ids, 0));
    } else {
        choices->selected = g_strdup(CLAWT_LRG_AGENT_CHOICE_OTHER);
    }

    return choices;
}

const gchar *
clawt_lrg_agent_provider_note(JsonNode *catalog, const gchar *provider)
{
    return clawt_ipc_payload_string(catalog_provider(catalog, provider),
                                    "note");
}

/* ── The settings form ────────────────────────────────────────────── */

/*
 * A friendly heading for a key's first component, and the order the
 * groups appear in -- the web inspector's table, so the two clients that
 * draw a generated form draw the same cards.  The *set* of groups comes
 * from the schema: a prefix with no row here gets a heading made from
 * its own name rather than being dropped.
 */
static const struct {
    const gchar *prefix;
    const gchar *title;
    const gchar *summary;
} form_groups[] = {
    { "",           "Identity",
      "What this agent is called and where it sits in the fleet." },
    { "model",      "Model",
      "Which CLI backend answers, and with what." },
    { "runtime",    "Runtime",
      "How the agent is started, and what happens when it stops." },
    { "persona",    "Persona",
      "The system prompt and the identity files folded into it." },
    { "memory",     "Memory budget",
      "libreclaw's MEMORY.md size limits. Not the searchable store -- "
      "that is memories.*, a different thing with a similar name." },
    { "computer",   "Computer",
      "The machine this agent can reach, and how far it is confined. "
      "The model and the computer take effect when it next starts." },
    { "tools",      "Tools",
      "Which orchestration tools the agent is offered." },
    { "workspace",  "Workspace", NULL },
    { "libreclaw",  "Passthrough",
      "Merged verbatim into the rendered libreclaw config." }
};

static gchar *
key_prefix(const gchar *key)
{
    const gchar *dot = strchr(key, '.');

    return (dot == NULL) ? g_strdup("") : g_strndup(key, (gsize)(dot - key));
}

gchar *
clawt_lrg_agent_key_label(const gchar *key)
{
    const gchar *last = strrchr(key, '.');
    g_autofree gchar *base = g_strdup(last != NULL ? last + 1 : key);
    gchar *p;

    for (p = base; *p != '\0'; p++) {
        if (*p == '_')
            *p = ' ';
    }

    /*
     * The middle components stay in front, so computer.vm.ssh_user reads
     * "vm · ssh user" rather than a bare "ssh user" that could belong to
     * any of three backends.
     */
    if (last != NULL) {
        g_autofree gchar *head = g_strndup(key, (gsize)(last - key));
        const gchar *dot = strchr(head, '.');

        if (dot != NULL)
            return g_strdup_printf("%s \xc2\xb7 %s", dot + 1, base);
    }

    return g_steal_pointer(&base);
}

gboolean
clawt_lrg_agent_key_skipped(const gchar *key, const ClawtSchemaEntry *entry)
{
    if (key == NULL || entry == NULL)
        return TRUE;

    if (entry->type == CLAWT_SCHEMA_SECTION ||
        entry->type == CLAWT_SCHEMA_MAPPING ||
        entry->type == CLAWT_SCHEMA_LIST_OF)
        return TRUE;

    /* The id names the agent; editing it here would rename nothing. */
    if (g_strcmp0(key, "id") == 0)
        return TRUE;

    /* Integrations and mounts have cards of their own on this page. */
    if (g_str_has_prefix(key, "integrations.") ||
        g_str_has_prefix(key, "computer.mounts"))
        return TRUE;

    return FALSE;
}

gboolean
clawt_lrg_agent_key_belongs_to_shared(const gchar *key, const gchar *use)
{
    return use != NULL && *use != '\0' && key != NULL &&
           g_str_has_prefix(key, "computer.") &&
           g_strcmp0(key, "computer.use") != 0;
}

ClawtLrgAgentFieldKind
clawt_lrg_agent_field_kind(const gchar *key, const ClawtSchemaEntry *entry)
{
    /*
     * The strings whose values the daemon knows, drawn as choosers.  The
     * schema calls each of these a string, so the generic path would be
     * a text box asking somebody to know a team id or an image reference
     * by heart -- while the daemon can say what the values are.
     */
    if (g_strcmp0(key, "team") == 0)
        return CLAWT_LRG_AGENT_FIELD_TEAM;
    if (g_strcmp0(key, "model.account") == 0)
        return CLAWT_LRG_AGENT_FIELD_ACCOUNT;
    if (g_strcmp0(key, "computer.use") == 0)
        return CLAWT_LRG_AGENT_FIELD_SHARED;
    if (g_strcmp0(key, "model.provider") == 0)
        return CLAWT_LRG_AGENT_FIELD_PROVIDER;
    if (g_strcmp0(key, "model.model") == 0)
        return CLAWT_LRG_AGENT_FIELD_MODEL;
    if (g_strcmp0(key, "model.effort") == 0)
        return CLAWT_LRG_AGENT_FIELD_EFFORT;
    if (g_strcmp0(key, "computer.type") == 0)
        return CLAWT_LRG_AGENT_FIELD_COMPUTER_TYPE;
    if (g_strcmp0(key, "computer.container.image") == 0)
        return CLAWT_LRG_AGENT_FIELD_IMAGE;
    if (g_strcmp0(key, "computer.vm.image") == 0)
        return CLAWT_LRG_AGENT_FIELD_DISK;
    if (g_strcmp0(key, "computer.vm.resolution") == 0)
        return CLAWT_LRG_AGENT_FIELD_RESOLUTION;

    /*
     * Everything else by what the schema says it is -- a value written
     * without dispatching on the type is how a list was once saved as a
     * scalar and read back as the default (AGENTS.md, "Config").
     */
    switch (entry->type) {
    case CLAWT_SCHEMA_BOOLEAN:
        return CLAWT_LRG_AGENT_FIELD_SWITCH;

    case CLAWT_SCHEMA_ENUM:
        return (entry->enum_type != NULL) ? CLAWT_LRG_AGENT_FIELD_ENUM
                                          : CLAWT_LRG_AGENT_FIELD_ENTRY;

    case CLAWT_SCHEMA_SECRET:
        return CLAWT_LRG_AGENT_FIELD_SECRET;

    case CLAWT_SCHEMA_STRING_LIST:
        return CLAWT_LRG_AGENT_FIELD_LIST;

    case CLAWT_SCHEMA_STRING:
    case CLAWT_SCHEMA_PATH:
    case CLAWT_SCHEMA_INT:
    case CLAWT_SCHEMA_DOUBLE:
    case CLAWT_SCHEMA_SECTION:
    case CLAWT_SCHEMA_MAPPING:
    case CLAWT_SCHEMA_LIST_OF:
        break;
    }

    /*
     * A prompt is a paragraph.  The schema does not tell prose from a
     * word -- both are strings -- so the two keys that are genuinely
     * prose are named.
     */
    if (g_str_has_suffix(key, "system_prompt") ||
        g_str_has_suffix(key, "prompt_suffix"))
        return CLAWT_LRG_AGENT_FIELD_PROSE;

    return CLAWT_LRG_AGENT_FIELD_ENTRY;
}

static gchar *
first_line(const gchar *doc)
{
    const gchar *newline;

    if (doc == NULL)
        return NULL;

    newline = strchr(doc, '\n');

    return (newline == NULL) ? g_strdup(doc)
                             : g_strndup(doc, (gsize)(newline - doc));
}

static void
field_free(gpointer data)
{
    ClawtLrgAgentField *field = data;

    g_free(field->key);
    g_free(field->label);
    g_free(field->hint);
    g_free(field->value);
    g_free(field);
}

static void
group_free(gpointer data)
{
    ClawtLrgAgentGroup *group = data;

    g_free(group->prefix);
    g_free(group->title);
    g_free(group->summary);
    g_ptr_array_unref(group->fields);
    g_free(group);
}

static ClawtLrgAgentGroup *
group_new(const gchar *prefix)
{
    ClawtLrgAgentGroup *group = g_new0(ClawtLrgAgentGroup, 1);
    guint i;

    group->prefix = g_strdup(prefix);
    group->title = g_strdup(prefix);
    group->fields = g_ptr_array_new_with_free_func(field_free);

    for (i = 0; i < G_N_ELEMENTS(form_groups); i++) {
        if (g_strcmp0(form_groups[i].prefix, prefix) != 0)
            continue;

        g_free(group->title);
        group->title = g_strdup(form_groups[i].title);
        group->summary = g_strdup(form_groups[i].summary);
    }

    return group;
}

GPtrArray *
clawt_lrg_agent_form(JsonObject *settings)
{
    g_autoptr(GHashTable) by_prefix =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *groups = g_ptr_array_new_with_free_func(group_free);
    g_autoptr(GPtrArray) tail = g_ptr_array_new();
    const ClawtSchemaEntry *schema;
    const gchar *use = clawt_ipc_payload_string(settings, "computer.use");
    gsize n_entries = 0;
    gsize i;

    /* The known groups first, in the table's order. */
    for (i = 0; i < G_N_ELEMENTS(form_groups); i++) {
        ClawtLrgAgentGroup *group = group_new(form_groups[i].prefix);

        g_ptr_array_add(groups, group);
        g_hash_table_insert(by_prefix, g_strdup(group->prefix), group);
    }

    schema = clawt_config_schema_get(&n_entries);

    /*
     * Collected by prefix rather than emitting a heading whenever the
     * prefix changes: the schema is in the order the generated YAML
     * wants, which interleaves persona.* with bare keys, and the web
     * client once drew two cards both called Identity from exactly that.
     */
    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *key = clawt_config_schema_agent_name(entry);
        g_autofree gchar *prefix = NULL;
        ClawtLrgAgentGroup *group;
        ClawtLrgAgentField *field;

        if (clawt_lrg_agent_key_skipped(key, entry) ||
            clawt_lrg_agent_key_belongs_to_shared(key, use))
            continue;

        prefix = key_prefix(key);
        group = g_hash_table_lookup(by_prefix, prefix);

        if (group == NULL) {
            group = group_new(prefix);
            g_ptr_array_add(tail, group);
            g_hash_table_insert(by_prefix, g_strdup(prefix), group);
        }

        field = g_new0(ClawtLrgAgentField, 1);
        field->key = g_strdup(key);
        field->label = clawt_lrg_agent_key_label(key);
        field->hint = first_line(entry->doc);
        field->value = g_strdup(string_or(settings, key, ""));
        field->kind = clawt_lrg_agent_field_kind(key, entry);
        field->enum_type = (entry->enum_type != NULL) ? entry->enum_type()
                                                      : G_TYPE_INVALID;
        field->dangerous = (entry->flags & CLAWT_SCHEMA_FLAG_DANGEROUS) != 0;
        g_ptr_array_add(group->fields, field);
    }

    /* Then any group the table does not name, in schema order. */
    for (i = 0; i < tail->len; i++)
        g_ptr_array_add(groups, g_ptr_array_index(tail, i));

    /* A known group the schema gave nothing to is not drawn at all. */
    for (i = groups->len; i > 0; i--) {
        ClawtLrgAgentGroup *group = g_ptr_array_index(groups, i - 1);

        if (group->fields->len == 0)
            g_ptr_array_remove_index(groups, i - 1);
    }

    return groups;
}

ClawtLrgAgentSetting *
clawt_lrg_agent_setting_new(const gchar *key, const gchar *value)
{
    ClawtLrgAgentSetting *setting = g_new0(ClawtLrgAgentSetting, 1);

    setting->key = g_strdup(key);
    setting->value = g_strdup(value);

    return setting;
}

void
clawt_lrg_agent_setting_free(gpointer data)
{
    ClawtLrgAgentSetting *setting = data;

    if (setting == NULL)
        return;

    g_free(setting->key);
    g_free(setting->value);
    g_free(setting);
}

static const gchar *
posted_value(GPtrArray *posted, const gchar *key, gboolean *found)
{
    guint i;

    for (i = 0; i < posted->len; i++) {
        ClawtLrgAgentSetting *setting = g_ptr_array_index(posted, i);

        if (g_strcmp0(setting->key, key) == 0) {
            *found = TRUE;
            return setting->value;
        }
    }

    *found = FALSE;
    return NULL;
}

GPtrArray *
clawt_lrg_agent_save_plan(GPtrArray *posted, JsonObject *settings)
{
    GPtrArray *plan = g_ptr_array_new_with_free_func(clawt_lrg_agent_setting_free);
    const gchar *use;
    gboolean use_posted = FALSE;
    gboolean account_posted = FALSE;
    const gchar *account;
    guint i;

    g_return_val_if_fail(posted != NULL, plan);

    /*
     * The shared machine as it will be after this save: moving onto one
     * in the same save as editing this agent's own computer settings
     * would have the rest refused, since they are the machine's now.
     */
    use = posted_value(posted, "computer.use", &use_posted);

    if (!use_posted || use == NULL)
        use = string_or(settings, "computer.use", "");

    /*
     * The account comes off first when it changes from one login to
     * another; the schema order then sets the provider before the new
     * account goes back on.
     */
    account = posted_value(posted, "model.account", &account_posted);

    if (account_posted && account != NULL &&
        g_strcmp0(account, string_or(settings, "model.account", "")) != 0 &&
        *string_or(settings, "model.account", "") != '\0' && *account != '\0')
        g_ptr_array_add(plan, clawt_lrg_agent_setting_new("model.account", ""));

    for (i = 0; i < posted->len; i++) {
        ClawtLrgAgentSetting *setting = g_ptr_array_index(posted, i);

        /* NULL: a secret left empty, or a control with no answer. */
        if (setting->value == NULL)
            continue;

        if (clawt_lrg_agent_key_belongs_to_shared(setting->key, use))
            continue;

        /*
         * Only what changed.  agent.set re-renders every agent's files on
         * each call, and an option left alone is left alone rather than
         * pinned to whatever value it reported -- a default written back
         * is a default that no longer follows the fleet.
         */
        if (g_strcmp0(setting->value,
                      string_or(settings, setting->key, "")) == 0)
            continue;

        g_ptr_array_add(plan, clawt_lrg_agent_setting_new(setting->key,
                                                    setting->value));
    }

    return plan;
}

gchar *
clawt_lrg_agent_saved_text(guint changed, gboolean restart_required)
{
    if (changed == 0)
        return g_strdup("Nothing to save: every setting is as it was.");

    /*
     * The tool list is named when it is the reason, because there
     * "restart it" is the whole answer: an AI CLI lists its tools once,
     * at session start, and an agent told about a permission it cannot
     * see says, accurately, that it does not have the tool.
     */
    if (restart_required)
        return g_strdup("Saved. Restart the agent -- it lists its tools "
                        "when it starts, so it cannot see the change until "
                        "then.");

    return g_strdup("Saved. Restart the agent for the model or computer to "
                    "take effect.");
}

/* ── At a glance ──────────────────────────────────────────────────── */

const gchar *
clawt_lrg_agent_state_tone(const gchar *state)
{
    gint value = -1;

    if (!clawt_enum_from_nick(CLAWT_TYPE_AGENT_STATE, state, &value))
        return "neutral";

    /*
     * Every state named and no default: -Wswitch then names a state the
     * library adds, rather than it being drawn in whatever tone a
     * default happened to give (AGENTS.md, "A colour decided by
     * comparing against a spelled-out nickname").
     */
    switch ((ClawtAgentState)value) {
    case CLAWT_AGENT_STATE_RUNNING:
        return "good";
    case CLAWT_AGENT_STATE_STARTING:
    case CLAWT_AGENT_STATE_STOPPING:
    case CLAWT_AGENT_STATE_DEGRADED:
        return "warn";
    case CLAWT_AGENT_STATE_ERROR:
    case CLAWT_AGENT_STATE_SHADOW:
        return "bad";
    case CLAWT_AGENT_STATE_STOPPED:
        return "neutral";
    }

    return "neutral";
}

void
clawt_lrg_agent_verbs(const gchar *state,
                      ClawtLrgAgentVerb out[CLAWT_LRG_AGENT_VERBS])
{
    gboolean shadow = g_strcmp0(state, "shadow") == 0;

    /*
     * Each button's sensitivity comes from what the agent can actually
     * do, so nothing offers what would fail -- and a disabled one says
     * why, beside it.
     */
    out[0].label = "Start";
    out[0].kind = "agent.start";
    out[0].enabled = !shadow && g_strcmp0(state, "running") != 0;
    out[0].why_not = shadow ? "its configuration could not be understood"
                            : "already running";

    out[1].label = "Stop";
    out[1].kind = "agent.stop";
    out[1].enabled = !shadow && g_strcmp0(state, "stopped") != 0;
    out[1].why_not = "not running";

    out[2].label = "Restart";
    out[2].kind = "agent.restart";
    out[2].enabled = !shadow;
    out[2].why_not = "this agent cannot start";
}

gchar *
clawt_lrg_agent_identity_text(JsonObject *identity)
{
    gint64 bytes = clawt_ipc_payload_int(identity, "bytes", 0);
    gint64 limit = clawt_ipc_payload_int(identity, "limit", 0);

    /*
     * Shown always while there is a count, because a size somebody can
     * watch is the point: a row that appears only in trouble teaches
     * nobody what normal looks like.
     */
    if (bytes <= 0)
        return NULL;

    return g_strdup_printf("%" G_GINT64_FORMAT " bytes of %" G_GINT64_FORMAT,
                           bytes, limit);
}

const gchar *
clawt_lrg_agent_identity_tone(JsonObject *identity)
{
    gint64 bytes = clawt_ipc_payload_int(identity, "bytes", 0);
    gint64 limit = clawt_ipc_payload_int(identity, "limit", 0);

    return (limit > 0 && bytes >= limit) ? "bad" : "warn";
}

gchar *
clawt_lrg_agent_usage_text(JsonNode *reply, const gchar *agent_id)
{
    JsonArray *agents = member_array(clawt_ipc_reply_object(reply), "agents");
    guint i;

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *row = element_object(agents, i);
        gint64 cost;
        g_autofree gchar *money = NULL;

        if (g_strcmp0(clawt_ipc_payload_string(row, "id"), agent_id) != 0)
            continue;

        /* -1 is unknown, never free: say so rather than print $0. */
        cost = clawt_ipc_payload_int(row, "cost_micros", -1);
        money = (cost >= 0) ? clawt_usage_format_cost(cost)
                            : g_strdup("cost unknown");

        return g_strdup_printf(
            "%" G_GINT64_FORMAT " turn(s) \xc2\xb7 %" G_GINT64_FORMAT
            " in / %" G_GINT64_FORMAT " out tokens \xc2\xb7 %s",
            clawt_ipc_payload_int(row, "turns", 0),
            clawt_ipc_payload_int(row, "input_tokens", 0),
            clawt_ipc_payload_int(row, "output_tokens", 0), money);
    }

    return NULL;
}

gchar *
clawt_lrg_agent_removed_text(const gchar *agent_id, JsonNode *reply)
{
    const gchar *computer = clawt_ipc_payload_string(
        clawt_ipc_reply_object(reply), "computer");

    /*
     * The computer's fate is reported rather than assumed: a teardown
     * that failed does not fail the removal, and "is gone" over a
     * container still running would be a lie.
     */
    if (g_strcmp0(computer, "removed") == 0)
        return g_strdup_printf("%s and its computer are gone.", agent_id);

    if (computer != NULL)
        return g_strdup_printf("%s is gone; its computer was not removed: %s",
                               agent_id, computer);

    return g_strdup_printf("%s is gone.", agent_id);
}

gchar *
clawt_lrg_agent_reset_text(JsonNode *reply)
{
    JsonObject *result = clawt_ipc_reply_object(reply);
    gint64 cleared = clawt_ipc_payload_int(result, "sessions_cleared", 0);

    /* `restarted` is a JSON boolean: read as one (AGENTS.md, "The wire"). */
    return g_strdup_printf(
        "Session reset: %" G_GINT64_FORMAT " stored session%s cleared%s. "
        "The next thing you say starts a new one.",
        cleared, cleared == 1 ? "" : "s",
        clawt_ipc_payload_boolean(result, "restarted", FALSE)
            ? " and the agent restarted" : "");
}

gchar *
clawt_lrg_agent_file_subtitle(JsonObject *file)
{
    const gchar *title = string_or(file, "title", "");

    /*
     * "prompt" marks the files read into every turn: those are where an
     * agent is actually configured, as opposed to merely wired up.
     */
    if (clawt_ipc_payload_boolean(file, "identity", FALSE))
        return g_strdup_printf("prompt \xc2\xb7 %s", title);

    return g_strdup(title);
}

/* ── Integrations ─────────────────────────────────────────────────── */

void
clawt_lrg_agent_integration_row_free(gpointer data)
{
    ClawtLrgAgentIntegrationRow *row = data;

    g_free(row->name);
    g_free(row->subtitle);
    g_free(row);
}

GPtrArray *
clawt_lrg_agent_integration_rows(JsonNode *reply)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(
        clawt_lrg_agent_integration_row_free);
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *integrations = member_array(root, "integrations");
    JsonArray *bindings = member_array(root, "bindings");
    guint i;

    for (i = 0; integrations != NULL &&
                i < json_array_get_length(integrations); i++) {
        JsonObject *integration = element_object(integrations, i);
        ClawtLrgAgentIntegrationRow *row = g_new0(ClawtLrgAgentIntegrationRow, 1);

        row->name = g_strdup(string_or(integration, "name", "?"));
        row->subtitle = g_strdup_printf(
            "%s%s", string_or(integration, "type", "?"),
            g_strcmp0(clawt_ipc_payload_string(integration, "scope"),
                      "all") == 0 ? " \xe2\x80\x94 every agent" : "");
        row->shared = TRUE;
        row->covers = clawt_ipc_payload_boolean(integration, "covers", FALSE);
        g_ptr_array_add(rows, row);
    }

    /*
     * The agent's own inline blocks have no switch -- they were written
     * inside this agent and belong to it -- but are shown, or an agent
     * with a Matrix block of its own would look like it had no Matrix.
     */
    for (i = 0; bindings != NULL && i < json_array_get_length(bindings);
         i++) {
        JsonObject *binding = element_object(bindings, i);
        gboolean valid = clawt_ipc_payload_boolean(binding, "valid", TRUE);
        ClawtLrgAgentIntegrationRow *row;

        if (clawt_ipc_payload_boolean(binding, "shared", FALSE))
            continue;

        row = g_new0(ClawtLrgAgentIntegrationRow, 1);
        row->name = g_strdup(string_or(binding, "name", "?"));
        row->subtitle = g_strdup_printf(
            "%s \xe2\x80\x94 configured inside this agent%s%s",
            string_or(binding, "type", "?"), valid ? "" : ": ",
            valid ? "" : string_or(binding, "problem", ""));
        row->covers = TRUE;
        g_ptr_array_add(rows, row);
    }

    return rows;
}

JsonNode *
clawt_lrg_agent_integration_update(JsonNode *list, const gchar *name,
                                   const gchar *agent_id, gboolean wanted,
                                   gchar **refusal)
{
    JsonArray *integrations = member_array(clawt_ipc_reply_object(list),
                                           "integrations");
    JsonObject *integration = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    JsonArray *agents;
    gboolean present = FALSE;
    guint i;

    if (refusal != NULL)
        *refusal = NULL;

    for (i = 0; integrations != NULL &&
                i < json_array_get_length(integrations); i++) {
        JsonObject *candidate = element_object(integrations, i);

        if (g_strcmp0(clawt_ipc_payload_string(candidate, "name"), name) == 0)
            integration = candidate;
    }

    if (integration == NULL || agent_id == NULL) {
        if (refusal != NULL)
            *refusal = g_strdup_printf("There is no integration called %s "
                                       "any more.", name != NULL ? name : "?");
        return NULL;
    }

    if (g_strcmp0(clawt_ipc_payload_string(integration, "scope"), "all") == 0) {
        if (refusal != NULL)
            *refusal = g_strdup("That one is set to every agent. Change its "
                                "scope in Settings to pick agents.");
        return NULL;
    }

    agents = member_array(integration, "agents");
    builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);
    json_builder_set_member_name(builder, "scope");
    json_builder_add_string_value(builder, "selected");
    json_builder_set_member_name(builder, "agents");
    json_builder_begin_array(builder);

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonNode *node = json_array_get_element(agents, i);
        const gchar *id;

        if (json_node_get_value_type(node) != G_TYPE_STRING)
            continue;

        id = json_node_get_string(node);

        if (g_strcmp0(id, agent_id) == 0) {
            present = TRUE;

            if (!wanted)
                continue;
        }

        json_builder_add_string_value(builder, id);
    }

    if (wanted && !present)
        json_builder_add_string_value(builder, agent_id);

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* ── Mounts ───────────────────────────────────────────────────────── */

gchar *
clawt_lrg_agent_mount_subtitle(JsonObject *mount)
{
    return g_strdup_printf("%s  (%s)", string_or(mount, "source", "-"),
                           string_or(mount, "mode", "ro"));
}

gchar *
clawt_lrg_computer_mount_title(JsonObject *mount)
{
    /*
     * Both spellings, host first.  An agent's own read and write run on
     * the host and only computer_exec enters the guest, so a path given
     * without saying which side it is on gets looked for on the wrong one.
     */
    return g_strdup_printf("%s  =  %s", string_or(mount, "source", "?"),
                           string_or(mount, "target", "?"));
}

const gchar *
clawt_lrg_agent_mounts_summary(const gchar *computer_type)
{
    gint value = CLAWT_COMPUTER_NONE;

    clawt_enum_from_nick(CLAWT_TYPE_COMPUTER_TYPE, computer_type, &value);

    /*
     * A type that takes no mounts has no card at all -- a host agent's
     * computer is this machine, and what limits it is confinement.  The
     * caller asks clawt_computer_type_takes_mounts() for that; this is
     * only the sentence.
     */
    if (value == CLAWT_COMPUTER_VM)
        return "Passed into the VM over virtiofs. Changes apply when it "
               "next starts.";

    return "Bind-mounted into the container. Changes apply when it next "
           "starts.";
}

const gchar *
clawt_lrg_agent_mount_refusal(const gchar *source, const gchar *target)
{
    if (source == NULL || *source == '\0' || target == NULL ||
        *target == '\0')
        return "A share needs a folder and a path inside.";

    return NULL;
}

/* ── Mailbox ──────────────────────────────────────────────────────── */

void
clawt_lrg_mailbox_row_free(gpointer data)
{
    ClawtLrgMailboxRow *row = data;

    g_free(row->id);
    g_free(row->title);
    g_free(row->body);
    g_free(row->badge);
    g_free(row->badge_tone);
    g_free(row);
}

static ClawtLrgMailboxRow *
mailbox_row(JsonObject *item, gboolean dead)
{
    ClawtLrgMailboxRow *row = g_new0(ClawtLrgMailboxRow, 1);
    const gchar *last_error = clawt_ipc_payload_string(item, "last_error");

    row->id = g_strdup(string_or(item, "id", ""));
    row->body = g_strdup(string_or(item, "body", ""));
    row->dead = dead;

    /*
     * A follow-up held while the agent is mid-turn is not a mailbox item:
     * it has no id to ack and nothing to requeue, so it gets neither.
     */
    if (g_strcmp0(clawt_ipc_payload_string(item, "state"),
                  "queued-follow-up") == 0) {
        row->title = g_strdup_printf("Queued follow-up \xc2\xb7 %s",
                                     string_or(item, "room", ""));
        row->badge = g_strdup("Queued follow-up");
        row->badge_tone = g_strdup("info");
        row->follow_up = TRUE;
        return row;
    }

    row->title = g_strdup_printf("from %s", string_or(item, "from", "?"));
    row->can_ack = *row->id != '\0';

    /* Only a dead letter has run out of attempts to be put back into. */
    row->can_requeue = dead && *row->id != '\0';

    if (last_error != NULL) {
        row->badge = g_strdup_printf("failed: %s", last_error);
        row->badge_tone = g_strdup("bad");
    } else if (dead) {
        row->badge = g_strdup("dead letter");
        row->badge_tone = g_strdup("bad");
    }

    return row;
}

GPtrArray *
clawt_lrg_mailbox_rows(JsonNode *list, JsonNode *dead)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(
        clawt_lrg_mailbox_row_free);
    JsonObject *root = clawt_ipc_reply_object(list);
    JsonArray *items = member_array(root, "items");
    JsonArray *follow_ups = member_array(root, "follow_ups");
    JsonArray *letters = member_array(clawt_ipc_reply_object(dead), "items");
    guint i;

    for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
        JsonObject *item = element_object(items, i);

        if (item != NULL)
            g_ptr_array_add(rows, mailbox_row(item, FALSE));
    }

    for (i = 0; follow_ups != NULL && i < json_array_get_length(follow_ups);
         i++) {
        JsonObject *item = element_object(follow_ups, i);

        if (item != NULL)
            g_ptr_array_add(rows, mailbox_row(item, FALSE));
    }

    /*
     * Dead letters after the waiting ones.  Nothing is dropped silently,
     * so an item that ran out of attempts has to be somewhere a person
     * can see it and put it back.
     */
    for (i = 0; letters != NULL && i < json_array_get_length(letters); i++) {
        JsonObject *item = element_object(letters, i);

        if (item != NULL)
            g_ptr_array_add(rows, mailbox_row(item, TRUE));
    }

    return rows;
}

gchar *
clawt_lrg_mailbox_summary(JsonNode *list)
{
    JsonObject *root = clawt_ipc_reply_object(list);
    gint64 depth = clawt_ipc_payload_int(root, "depth", 0);
    JsonArray *held = member_array(root, "follow_ups");

    if (held != NULL)
        return g_strdup_printf(
            "%" G_GINT64_FORMAT " waiting for delivery \xc2\xb7 %u queued "
            "follow-up%s", depth, json_array_get_length(held),
            json_array_get_length(held) == 1 ? "" : "s");

    return g_strdup_printf("%" G_GINT64_FORMAT " message(s) waiting", depth);
}

const gchar *
clawt_lrg_mailbox_empty_text(const gchar *agent_state)
{
    /*
     * Empty is an answer here only once it says why.  A running agent's
     * mailbox is empty because delivery acknowledges at the socket: a
     * message is in the agent's own queue the moment it arrives, so an
     * empty list means "nothing stuck", not "nothing sent" (AGENTS.md,
     * "An agent's mailbox is empty while it is running").
     */
    if (g_strcmp0(agent_state, "running") == 0 ||
        g_strcmp0(agent_state, "degraded") == 0)
        return "Nothing waiting. A running agent takes each message the "
               "moment it arrives -- delivery is acknowledged at its socket -- "
               "so its mailbox only fills while it is stopped, held or "
               "unreachable.";

    return "Nothing waiting. Messages sent while this agent is stopped wait "
           "here, in order, and go out when it starts.";
}

gchar *
clawt_lrg_mailbox_purged_text(JsonNode *reply)
{
    return g_strdup_printf("Purged %" G_GINT64_FORMAT " expired item(s).",
                           clawt_ipc_payload_int(clawt_ipc_reply_object(reply),
                                                 "purged", 0));
}

/* ── Computer ─────────────────────────────────────────────────────── */

static ClawtComputerType
computer_type_of(JsonObject *agent)
{
    gint value = CLAWT_COMPUTER_NONE;

    clawt_enum_from_nick(CLAWT_TYPE_COMPUTER_TYPE,
                         clawt_ipc_payload_string(agent, "computer"), &value);

    return (ClawtComputerType)value;
}

gboolean
clawt_lrg_computer_present(JsonObject *agent)
{
    const gchar *caps = clawt_ipc_payload_string(agent, "caps");

    return caps != NULL && strstr(caps, "computer") != NULL;
}

gboolean
clawt_lrg_computer_offers_lifecycle(JsonObject *agent)
{
    /*
     * The library's predicate, not a list of types here -- a client that
     * answered from its own list would offer Stop on a backend added
     * later, or fail to.  The daemon answers the same question as
     * `computer_machine`; both must agree before a verb is offered.
     */
    return clawt_computer_type_has_machine(computer_type_of(agent)) &&
           clawt_ipc_payload_boolean(agent, "computer_machine", TRUE);
}

gboolean
clawt_lrg_computer_offers_screen(JsonObject *agent)
{
    return clawt_ipc_payload_boolean(
        agent, "computer_screen",
        clawt_computer_type_has_screen(computer_type_of(agent)));
}

gchar *
clawt_lrg_computer_state_text(JsonObject *agent, JsonNode *status)
{
    const gchar *type = string_or(agent, "computer", "none");

    if (agent == NULL)
        return g_strdup("No agent selected.");

    if (!clawt_lrg_computer_present(agent))
        return g_strdup("This agent has no computer.");

    /*
     * The daemon's state is what says *why* -- "stopped" reads very
     * differently from "running" -- and a stopped agent has no computer
     * object to report on, which is said rather than left blank.
     */
    if (status == NULL)
        return g_strdup_printf("%s \xe2\x80\x94 built when the agent starts",
                               type);

    return g_strdup_printf("%s \xe2\x80\x94 %s", type,
                           string_or(clawt_ipc_reply_object(status), "state",
                                     "unknown"));
}

gchar *
clawt_lrg_computer_screen_text(JsonObject *status, gint64 now_us)
{
    g_autoptr(GString) said = g_string_new(NULL);
    gint64 stamp = clawt_ipc_payload_int(status, "stamp", 0);
    const gchar *error = clawt_ipc_payload_string(status, "error");
    const gchar *request = clawt_ipc_payload_string(status, "request");

    if (status == NULL)
        return g_strdup("The screen cannot be read.");

    if (!clawt_ipc_payload_boolean(status, "observable", FALSE))
        return g_strdup(error != NULL
                        ? error
                        : "No screen to watch yet: the agent is not running, "
                          "or it has no desktop.");

    /*
     * A stale frame is labelled with its age rather than shown as
     * current; the daemon decides "stale" with the library's threshold
     * so a browser and this window agree about when a picture stopped
     * being news.
     */
    if (stamp <= 0) {
        g_string_append(said, "No frame yet.");
    } else if (clawt_ipc_payload_boolean(status, "stale", FALSE)) {
        g_autofree gchar *ago = clawt_time_ago_label(stamp, now_us);

        g_string_append_printf(said, "Stale \xe2\x80\x94 %s.", ago);
    } else {
        g_string_append(said, "Live.");
    }

    if (clawt_ipc_payload_boolean(status, "held", FALSE))
        g_string_append_printf(said, " Held by %s.",
                               string_or(status, "holder", "somebody"));

    if (request != NULL)
        g_string_append_printf(said, " It has asked for hands: %s", request);

    if (error != NULL)
        g_string_append_printf(said, " %s", error);

    return g_string_free(g_steal_pointer(&said), FALSE);
}

gchar *
clawt_lrg_computer_exec_output(JsonNode *reply)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    GString *out = g_string_new(NULL);
    gint64 exit_status = clawt_ipc_payload_int(root, "exit", 0);

    g_string_append(out, string_or(root, "stdout", ""));

    /*
     * stderr too.  A console that swallowed it would leave a failing
     * command looking like one that produced nothing.
     */
    g_string_append(out, string_or(root, "stderr", ""));

    if (out->len > 0 && out->str[out->len - 1] != '\n')
        g_string_append_c(out, '\n');

    if (exit_status != 0)
        g_string_append_printf(out, "[exit %" G_GINT64_FORMAT "]\n",
                               exit_status);

    return g_string_free(out, FALSE);
}

GPtrArray *
clawt_lrg_computer_exchange_names(JsonNode *reply)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *entries = member_array(root, "entries");
    guint i;

    if (entries == NULL)
        entries = member_array(root, "files");

    /* Strings today; objects with a path are read too, as GTK does. */
    for (i = 0; entries != NULL && i < json_array_get_length(entries); i++) {
        JsonNode *node = json_array_get_element(entries, i);

        if (JSON_NODE_HOLDS_OBJECT(node))
            g_ptr_array_add(names, g_strdup(string_or(
                json_node_get_object(node), "path", "?")));
        else if (json_node_get_value_type(node) == G_TYPE_STRING)
            g_ptr_array_add(names, g_strdup(json_node_get_string(node)));
    }

    return names;
}

gchar *
clawt_lrg_computer_lifecycle_text(const gchar *agent_id, JsonNode *reply)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    const gchar *state = string_or(root, "state", "?");

    if (clawt_ipc_payload_boolean(root, "removes", FALSE))
        return g_strdup_printf("%s: the machine is %s. It does not survive a "
                               "stop, so starting it builds a fresh one.",
                               agent_id, state);

    return g_strdup_printf("%s: the machine is %s.", agent_id, state);
}

gchar *
clawt_lrg_computer_stop_warning(const gchar *agent_id)
{
    return g_strdup_printf(
        "%s does not keep its machine across a stop, so everything inside it "
        "goes -- anything the agent installed, and anything it wrote outside "
        "a shared folder.\n\nSet computer.container.keep to keep it instead.",
        agent_id != NULL ? agent_id : "This agent");
}

JsonNode *
clawt_lrg_computer_input_payload(const gchar *agent_id, const gchar *text,
                                 const gchar *click, gchar **refusal)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    if (refusal != NULL)
        *refusal = NULL;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, agent_id != NULL ? agent_id : "");

    if (text != NULL && *text != '\0') {
        /*
         * A leading "@" is a key or a combo rather than text, because a
         * second entry for keys is a second control somebody has to
         * notice -- and "ctrl+l" typed into an address bar is an ordinary
         * thing to want to type.
         */
        json_builder_set_member_name(builder, "kind");
        json_builder_add_string_value(builder,
                                      (text[0] == '@' && text[1] != '\0')
                                      ? "key" : "text");
        json_builder_set_member_name(builder, "text");
        json_builder_add_string_value(builder,
                                      (text[0] == '@' && text[1] != '\0')
                                      ? text + 1 : text);
    } else if (click != NULL && strchr(click, ',') != NULL) {
        g_auto(GStrv) parts = g_strsplit(click, ",", 2);
        gchar *end_x = NULL;
        gchar *end_y = NULL;
        gint64 x = g_ascii_strtoll(g_strstrip(parts[0]), &end_x, 10);
        gint64 y = g_ascii_strtoll(g_strstrip(parts[1]), &end_y, 10);

        /*
         * Refused rather than sent as 0,0: a typo would otherwise click
         * the top-left corner of somebody's screen, which on most
         * desktops opens something.
         */
        if (end_x == parts[0] || *end_x != '\0' || end_y == parts[1] ||
            *end_y != '\0' || x < 0 || y < 0) {
            if (refusal != NULL)
                *refusal = g_strdup("A click is two whole numbers, x,y.");
            return NULL;
        }

        json_builder_set_member_name(builder, "kind");
        json_builder_add_string_value(builder, "click");
        json_builder_set_member_name(builder, "x");
        json_builder_add_int_value(builder, x);
        json_builder_set_member_name(builder, "y");
        json_builder_add_int_value(builder, y);
    } else {
        if (refusal != NULL)
            *refusal = g_strdup("Nothing to send: type something, or give a "
                                "coordinate as x,y.");
        return NULL;
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* ── Holding the fleet ────────────────────────────────────────────── */

gboolean
clawt_lrg_hold_is_held(JsonNode *reply)
{
    JsonObject *hold = member_object(clawt_ipc_reply_object(reply), "hold");

    return clawt_ipc_payload_boolean(hold, "held", FALSE);
}

gchar *
clawt_lrg_hold_paused_text(JsonNode *reply)
{
    JsonObject *hold = member_object(clawt_ipc_reply_object(reply), "hold");
    gint64 draining = clawt_ipc_payload_int(hold, "draining", 0);

    /*
     * The number in flight is the answer somebody is waiting for: zero
     * is the moment a restart is safe, which is what a hold exists to
     * give.
     */
    if (draining > 0)
        return g_strdup_printf("Held. %" G_GINT64_FORMAT " turn(s) still "
                               "finishing.", draining);

    return g_strdup("Held, with nothing in flight.");
}

gchar *
clawt_lrg_hold_available_update(JsonNode *reply)
{
    JsonObject *update = member_object(clawt_ipc_reply_object(reply),
                                       "update");

    /*
     * Only when the daemon says one is available.  `latest` on its own
     * would announce the version already running; the comparison is the
     * daemon's, written once, rather than each client's.
     */
    if (!clawt_ipc_payload_boolean(update, "available", FALSE))
        return NULL;

    return g_strdup(clawt_ipc_payload_string(update, "latest"));
}
