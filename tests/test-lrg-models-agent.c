/*
 * test-lrg-models-agent.c - The Agent, Mailbox and Computer pages' decisions
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: no daemon, no display, no libregnum.  Replies are written
 * here in the shapes the daemon's json_builder_set_member_name() calls
 * produce, so a member read under the wrong name fails a test rather
 * than falling back in silence.
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

#include "../clients/lrg/lrg-agent-model.c"

static JsonNode *
parse(const gchar *text)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;

    g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
    g_assert_no_error(error);

    return json_node_copy(json_parser_get_root(parser));
}

static guint
index_of(ClawtLrgAgentChoices *choices, const gchar *id)
{
    guint i;

    for (i = 0; i < choices->ids->len; i++) {
        if (g_strcmp0(g_ptr_array_index(choices->ids, i), id) == 0)
            return i;
    }

    return G_MAXUINT;
}

/* ── Choices ──────────────────────────────────────────────────────── */

static void
test_choices_are_null_terminated(void)
{
    g_autoptr(ClawtLrgAgentChoices) choices = clawt_lrg_agent_choices_new();

    clawt_lrg_agent_choices_add(choices, "a", "A");
    clawt_lrg_agent_choices_add(choices, "b", NULL);

    g_assert_cmpstr(clawt_lrg_agent_choices_ids(choices)[1], ==, "b");
    g_assert_null(clawt_lrg_agent_choices_ids(choices)[2]);
    g_assert_cmpstr(clawt_lrg_agent_choices_labels(choices)[1], ==, "b");
    g_assert_null(clawt_lrg_agent_choices_labels(choices)[2]);
}

static void
test_choice_value_other(void)
{
    g_autofree gchar *picked = clawt_lrg_agent_choice_value("x", "ignored");
    g_autofree gchar *typed = clawt_lrg_agent_choice_value(CLAWT_LRG_AGENT_CHOICE_OTHER,
                                                     "  quay.io/x  ");
    g_autofree gchar *empty = clawt_lrg_agent_choice_value(CLAWT_LRG_AGENT_CHOICE_OTHER,
                                                     "");

    g_assert_cmpstr(picked, ==, "x");
    g_assert_cmpstr(typed, ==, "quay.io/x");
    g_assert_null(empty);
}

static void
test_enum_choices_walk_the_type(void)
{
    g_autoptr(ClawtLrgAgentChoices) choices =
        clawt_lrg_agent_enum_choices(CLAWT_TYPE_MOUNT_MODE, "rw");
    g_autoptr(GEnumClass) klass = g_type_class_ref(CLAWT_TYPE_MOUNT_MODE);

    /* every value the enum has, and no extra one for a known current */
    g_assert_cmpuint(choices->ids->len, ==, klass->n_values);
    g_assert_cmpstr(choices->selected, ==, "rw");
}

static void
test_enum_choices_keep_unknown_and_unset(void)
{
    g_autoptr(ClawtLrgAgentChoices) typo =
        clawt_lrg_agent_enum_choices(CLAWT_TYPE_MOUNT_MODE, "rx");
    g_autoptr(ClawtLrgAgentChoices) unset =
        clawt_lrg_agent_enum_choices(CLAWT_TYPE_MOUNT_MODE, NULL);

    /* a typo is shown as itself, not replaced by member 0 */
    g_assert_cmpstr(typo->selected, ==, "rx");
    g_assert_cmpuint(index_of(typo, "rx"), !=, G_MAXUINT);

    /* unset stays unset rather than being pinned to the first value */
    g_assert_cmpstr(unset->selected, ==, "");
}

static void
test_computer_types_walk_the_library(void)
{
    g_autoptr(ClawtLrgAgentChoices) choices =
        clawt_lrg_agent_computer_type_choices("vm");

    g_assert_cmpuint(choices->ids->len, ==, clawt_computer_type_count());
    g_assert_cmpstr(choices->selected, ==, "vm");
}

static void
test_team_choices(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"teams\":[{\"id\":\"ops\",\"name\":\"Operations\"},"
        "{\"id\":\"dev\"}]}");
    g_autoptr(ClawtLrgAgentChoices) known =
        clawt_lrg_agent_team_choices(reply, "dev");
    g_autoptr(ClawtLrgAgentChoices) ghost =
        clawt_lrg_agent_team_choices(reply, "sales");
    g_autoptr(ClawtLrgAgentChoices) none =
        clawt_lrg_agent_team_choices(NULL, "");

    g_assert_cmpstr(g_ptr_array_index(known->ids, 0), ==, "");
    g_assert_cmpstr(g_ptr_array_index(known->labels, 1), ==, "Operations");
    g_assert_cmpstr(known->selected, ==, "dev");
    g_assert_cmpuint(known->ids->len, ==, 3);

    /* undeclared, but still the value: added, labelled, selected */
    g_assert_cmpuint(ghost->ids->len, ==, 4);
    g_assert_cmpstr(ghost->selected, ==, "sales");
    g_assert_cmpstr(g_ptr_array_index(ghost->labels, 3), ==,
                    "sales (not declared)");

    /* no reply: "No team" alone, selected */
    g_assert_cmpuint(none->ids->len, ==, 1);
    g_assert_cmpstr(none->selected, ==, "");
}

static void
test_account_choices_drop_unusable_but_keep_current(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"accounts\":["
        "{\"id\":\"work\",\"usable\":true,\"description\":\"Max plan\"},"
        "{\"id\":\"old\",\"usable\":false},"
        "{\"id\":\"spare\",\"usable\":false}]}");
    g_autoptr(ClawtLrgAgentChoices) choices =
        clawt_lrg_agent_account_choices(reply, "old");

    g_assert_cmpuint(index_of(choices, "work"), ==, 1);
    g_assert_cmpstr(g_ptr_array_index(choices->labels, 1), ==,
                    "work -- Max plan");
    g_assert_cmpuint(index_of(choices, "old"), !=, G_MAXUINT);
    g_assert_cmpuint(index_of(choices, "spare"), ==, G_MAXUINT);
    g_assert_cmpstr(choices->selected, ==, "old");
}

static void
test_shared_choices_leave_out_hosts(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"computers\":["
        "{\"id\":\"lab\",\"type\":\"vm\",\"shareable\":true},"
        "{\"id\":\"this\",\"type\":\"host\",\"shareable\":false}]}");
    g_autoptr(ClawtLrgAgentChoices) choices =
        clawt_lrg_agent_shared_choices(reply, NULL);

    g_assert_cmpuint(choices->ids->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(choices->labels, 1), ==, "lab (vm)");
    g_assert_cmpstr(choices->selected, ==, "");
}

static void
test_image_choices(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"default\":\"fedora:44\",\"images\":["
        "{\"reference\":\"debian:13\",\"label\":\"Debian 13\"},"
        "{\"reference\":\"fedora:44\",\"label\":\"Fedora 44\","
        "\"note\":\"the default\"}]}");
    g_autoptr(ClawtLrgAgentChoices) fresh =
        clawt_lrg_agent_image_choices(reply, NULL);
    g_autoptr(ClawtLrgAgentChoices) known =
        clawt_lrg_agent_image_choices(reply, "debian:13");
    g_autoptr(ClawtLrgAgentChoices) typed =
        clawt_lrg_agent_image_choices(reply, "quay.io/me/box");
    g_autofree gchar *note = clawt_lrg_agent_image_note(reply, "fedora:44");

    /* nothing chosen: the daemon's default, not the first entry */
    g_assert_cmpstr(fresh->selected, ==, "fedora:44");
    g_assert_cmpstr(known->selected, ==, "debian:13");

    /* a reference not in the catalogue goes into the entry */
    g_assert_cmpstr(typed->selected, ==, CLAWT_LRG_AGENT_CHOICE_OTHER);
    g_assert_cmpstr(typed->other, ==, "quay.io/me/box");

    g_assert_cmpstr(note, ==, "fedora:44 -- the default");
}

static void
test_disk_choices_without_a_disk_look_wrong(void)
{
    g_autoptr(JsonNode) empty = parse("{\"images\":[]}");
    g_autoptr(JsonNode) one = parse(
        "{\"images\":[{\"name\":\"Fedora Cloud\",\"path\":\"/i/f.qcow2\"}]}");
    g_autoptr(ClawtLrgAgentChoices) none =
        clawt_lrg_agent_disk_choices(empty, "");
    g_autoptr(ClawtLrgAgentChoices) picked =
        clawt_lrg_agent_disk_choices(one, "/i/f.qcow2");
    g_autoptr(ClawtLrgAgentChoices) unpicked =
        clawt_lrg_agent_disk_choices(one, NULL);
    g_autofree gchar *hint = clawt_lrg_agent_disk_note(empty,
                                                       CLAWT_LRG_AGENT_CHOICE_OTHER);

    g_assert_cmpstr(none->selected, ==, CLAWT_LRG_AGENT_CHOICE_OTHER);
    g_assert_cmpstr(picked->selected, ==, "/i/f.qcow2");

    /* no fallback: a VM with no disk must not appear to have one */
    g_assert_cmpstr(unpicked->selected, ==, CLAWT_LRG_AGENT_CHOICE_OTHER);
    g_assert_nonnull(strstr(hint, "Settings"));
}

static void
test_provider_and_model_choices(void)
{
    g_autoptr(JsonNode) catalog = parse(
        "{\"providers\":["
        "{\"id\":\"claude-code\",\"label\":\"Claude Code\",\"agent\":true,"
        " \"models\":[{\"id\":\"sonnet\",\"label\":\"Sonnet\"},"
        "             {\"id\":\"opus\",\"label\":\"Opus\",\"note\":\"slow\"}]},"
        "{\"id\":\"openai\",\"label\":\"OpenAI\",\"agent\":false,"
        " \"models\":[]},"
        "{\"id\":\"legacy\",\"label\":\"Legacy\",\"models\":[]}]}");
    g_autoptr(ClawtLrgAgentChoices) providers =
        clawt_lrg_agent_provider_choices(catalog, "claude-code");
    g_autoptr(ClawtLrgAgentChoices) kept =
        clawt_lrg_agent_provider_choices(catalog, "openai");
    g_autoptr(ClawtLrgAgentChoices) models =
        clawt_lrg_agent_model_choices(catalog, "claude-code", "opus");
    g_autoptr(ClawtLrgAgentChoices) typed =
        clawt_lrg_agent_model_choices(catalog, "claude-code", "haiku-9");

    /* an HTTP provider is not an agent backend... */
    g_assert_cmpuint(index_of(providers, "openai"), ==, G_MAXUINT);
    /* ...an older daemon's provider with no flag is kept... */
    g_assert_cmpuint(index_of(providers, "legacy"), !=, G_MAXUINT);
    /* ...and the agent's own provider survives the filter */
    g_assert_cmpstr(kept->selected, ==, "openai");
    g_assert_cmpstr(g_ptr_array_index(kept->labels, index_of(kept, "openai")),
                    ==, "OpenAI");

    g_assert_cmpstr(models->selected, ==, "opus");
    g_assert_cmpstr(g_ptr_array_index(models->labels, 1), ==, "Opus - slow");
    g_assert_cmpstr(typed->selected, ==, CLAWT_LRG_AGENT_CHOICE_OTHER);
    g_assert_cmpstr(typed->other, ==, "haiku-9");
}

static void
test_resolution_keeps_an_unusual_size(void)
{
    g_autoptr(ClawtLrgAgentChoices) odd =
        clawt_lrg_agent_resolution_choices("1366x768");
    g_autoptr(ClawtLrgAgentChoices) common =
        clawt_lrg_agent_resolution_choices("1920x1080");

    g_assert_cmpstr(odd->selected, ==, "1366x768");
    g_assert_cmpuint(index_of(odd, "1366x768"), !=, G_MAXUINT);
    g_assert_cmpuint(odd->ids->len, ==, common->ids->len + 1);
}

static void
test_relabel_walks_the_library(void)
{
    g_autoptr(ClawtLrgAgentChoices) choices = clawt_lrg_agent_relabel_choices();

    g_assert_cmpuint(choices->ids->len, ==, clawt_relabel_count());
    g_assert_cmpstr(choices->selected, ==,
                    clawt_enum_to_nick(CLAWT_TYPE_RELABEL,
                                       CLAWT_RELABEL_SHARED));
}

/* ── The settings form ────────────────────────────────────────────── */

static ClawtLrgAgentField *
find_field(GPtrArray *groups, const gchar *key, ClawtLrgAgentGroup **out)
{
    guint g;
    guint f;

    for (g = 0; g < groups->len; g++) {
        ClawtLrgAgentGroup *group = g_ptr_array_index(groups, g);

        for (f = 0; f < group->fields->len; f++) {
            ClawtLrgAgentField *field = g_ptr_array_index(group->fields, f);

            if (g_strcmp0(field->key, key) == 0) {
                if (out != NULL)
                    *out = group;
                return field;
            }
        }
    }

    return NULL;
}

static void
test_form_walks_the_schema(void)
{
    g_autoptr(JsonNode) settings = parse(
        "{\"name\":\"Kudu\",\"team\":\"ops\",\"model.provider\":\"claude-code\","
        "\"memories.enabled\":\"true\"}");
    g_autoptr(GPtrArray) groups =
        clawt_lrg_agent_form(json_node_get_object(settings));
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;
    ClawtLrgAgentGroup *group = NULL;
    ClawtLrgAgentField *field;

    /*
     * Every agent-settable key the schema has is on the form unless the
     * page edits it elsewhere -- checked against the schema itself, so a
     * key added there and dropped here is a failure.
     */
    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const gchar *key = clawt_config_schema_agent_name(&schema[i]);

        if (clawt_lrg_agent_key_skipped(key, &schema[i]))
            continue;

        g_assert_nonnull(find_field(groups, key, NULL));
    }

    field = find_field(groups, "name", &group);
    g_assert_cmpstr(field->value, ==, "Kudu");
    g_assert_cmpstr(group->title, ==, "Identity");

    g_assert_cmpint(find_field(groups, "team", NULL)->kind, ==,
                    CLAWT_LRG_AGENT_FIELD_TEAM);
    g_assert_cmpint(find_field(groups, "model.provider", NULL)->kind, ==,
                    CLAWT_LRG_AGENT_FIELD_PROVIDER);
    g_assert_cmpint(find_field(groups, "memories.enabled", NULL)->kind, ==,
                    CLAWT_LRG_AGENT_FIELD_SWITCH);
    g_assert_cmpint(find_field(groups, "runtime.restart", NULL)->kind, ==,
                    CLAWT_LRG_AGENT_FIELD_ENUM);
    g_assert_cmpint(find_field(groups, "computer.vm.image", NULL)->kind, ==,
                    CLAWT_LRG_AGENT_FIELD_DISK);

    /*
     * The agent's own sandbox wears the library's label and sentence, the
     * same words the GTK and web clients use, and is still the enum
     * walked from its type.
     */
    field = find_field(groups, "runtime.confine", &group);
    g_assert_nonnull(field);
    g_assert_cmpstr(group->title, ==, "Runtime");
    g_assert_cmpstr(field->label, ==, clawt_agent_confine_label());
    g_assert_cmpstr(field->hint, ==, clawt_agent_confine_explanation());
    g_assert_cmpint(field->kind, ==, CLAWT_LRG_AGENT_FIELD_ENUM);
    g_assert_true(field->enum_type == CLAWT_TYPE_RUNTIME_CONFINE);

    /* unset is "" rather than absent */
    g_assert_cmpstr(find_field(groups, "description", NULL)->value, ==, "");

    /* edited elsewhere on the page */
    g_assert_null(find_field(groups, "id", NULL));

    /* no empty card */
    for (i = 0; i < groups->len; i++)
        g_assert_cmpuint(((ClawtLrgAgentGroup *)
                          g_ptr_array_index(groups, i))->fields->len, >, 0);
}

static void
test_form_hides_the_shared_machines_keys(void)
{
    g_autoptr(JsonNode) settings = parse(
        "{\"computer.use\":\"lab\",\"computer.type\":\"vm\"}");
    g_autoptr(GPtrArray) groups =
        clawt_lrg_agent_form(json_node_get_object(settings));

    g_assert_nonnull(find_field(groups, "computer.use", NULL));
    g_assert_null(find_field(groups, "computer.type", NULL));
    g_assert_null(find_field(groups, "computer.vm.image", NULL));
}

static void
test_key_label(void)
{
    g_autofree gchar *bare = clawt_lrg_agent_key_label("chief_of_staff");
    g_autofree gchar *deep = clawt_lrg_agent_key_label("computer.vm.ssh_user");
    g_autofree gchar *shallow = clawt_lrg_agent_key_label("model.effort");

    g_assert_cmpstr(bare, ==, "chief of staff");
    g_assert_cmpstr(deep, ==, "vm \xc2\xb7 ssh user");
    g_assert_cmpstr(shallow, ==, "effort");
}

static GPtrArray *
posted(const gchar *first_key, ...)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(clawt_lrg_agent_setting_free);
    const gchar *key = first_key;
    va_list args;

    va_start(args, first_key);

    while (key != NULL) {
        const gchar *value = va_arg(args, const gchar *);

        g_ptr_array_add(out, clawt_lrg_agent_setting_new(key, value));
        key = va_arg(args, const gchar *);
    }

    va_end(args);

    return out;
}

static void
test_save_sends_only_what_changed(void)
{
    g_autoptr(JsonNode) settings = parse(
        "{\"name\":\"Kudu\",\"description\":\"\",\"mailbox.max_depth\":\"100\"}");
    g_autoptr(GPtrArray) form = posted("name", "Kudu", "description", "hi",
                                       "mailbox.max_depth", "100",
                                       "persona.secret", NULL, NULL);
    g_autoptr(GPtrArray) plan =
        clawt_lrg_agent_save_plan(form, json_node_get_object(settings));
    ClawtLrgAgentSetting *only;

    g_assert_cmpuint(plan->len, ==, 1);
    only = g_ptr_array_index(plan, 0);
    g_assert_cmpstr(only->key, ==, "description");
    g_assert_cmpstr(only->value, ==, "hi");
}

static void
test_save_clears_the_account_before_the_provider(void)
{
    g_autoptr(JsonNode) settings = parse(
        "{\"model.provider\":\"claude-code\",\"model.account\":\"work\"}");
    g_autoptr(GPtrArray) form = posted("model.provider", "codex",
                                       "model.account", "codex-login", NULL);
    g_autoptr(GPtrArray) plan =
        clawt_lrg_agent_save_plan(form, json_node_get_object(settings));

    g_assert_cmpuint(plan->len, ==, 3);
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 0))->key, ==,
                    "model.account");
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 0))->value, ==,
                    "");
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 1))->key, ==,
                    "model.provider");
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 2))->value, ==,
                    "codex-login");
}

static void
test_save_does_not_clear_an_unset_account(void)
{
    g_autoptr(JsonNode) settings = parse("{\"model.account\":\"\"}");
    g_autoptr(GPtrArray) form = posted("model.account", "work", NULL);
    g_autoptr(GPtrArray) plan =
        clawt_lrg_agent_save_plan(form, json_node_get_object(settings));

    g_assert_cmpuint(plan->len, ==, 1);
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 0))->value, ==,
                    "work");
}

static void
test_save_moving_onto_a_shared_machine(void)
{
    g_autoptr(JsonNode) settings = parse(
        "{\"computer.use\":\"\",\"computer.type\":\"container\"}");
    g_autoptr(GPtrArray) form = posted("computer.use", "lab",
                                       "computer.type", "vm", NULL);
    g_autoptr(GPtrArray) plan =
        clawt_lrg_agent_save_plan(form, json_node_get_object(settings));

    /* the machine's keys are the machine's now: refused if sent */
    g_assert_cmpuint(plan->len, ==, 1);
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 0))->key, ==,
                    "computer.use");
}

/* The fallback list goes last, after the model it falls back from. */
static void
test_save_sends_the_fallbacks_last(void)
{
    g_autoptr(JsonNode) settings = parse(
        "{\"model.model\":\"opus\",\"model.fallbacks\":\"\"}");
    g_autoptr(GPtrArray) form = posted("model.fallbacks", "codex-cli:gpt-5.5",
                                       "model.model", "sonnet", NULL);
    g_autoptr(GPtrArray) plan =
        clawt_lrg_agent_save_plan(form, json_node_get_object(settings));

    g_assert_cmpuint(plan->len, ==, 2);
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 0))->key, ==,
                    "model.model");
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 1))->key, ==,
                    "model.fallbacks");
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 1))->value, ==,
                    "codex-cli:gpt-5.5");
}

/*
 * With the provider changing too, the old list comes off first: the
 * daemon checks a provider against the current list, and refuses one
 * that would make it a rotation.
 */
static void
test_save_clears_the_fallbacks_before_the_provider(void)
{
    g_autoptr(JsonNode) settings = parse(
        "{\"model.provider\":\"claude-code\","
        "\"model.fallbacks\":\"codex-cli:gpt-5.5\"}");
    g_autoptr(GPtrArray) form = posted("model.provider", "codex-cli",
                                       "model.fallbacks", "claude-code:sonnet",
                                       NULL);
    g_autoptr(GPtrArray) plan =
        clawt_lrg_agent_save_plan(form, json_node_get_object(settings));

    g_assert_cmpuint(plan->len, ==, 3);
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 0))->key, ==,
                    "model.fallbacks");
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 0))->value, ==,
                    "");
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 1))->key, ==,
                    "model.provider");
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 2))->value, ==,
                    "claude-code:sonnet");
}

/* An unchanged list is not sent, and not cleared either. */
static void
test_save_leaves_unchanged_fallbacks(void)
{
    g_autoptr(JsonNode) settings = parse(
        "{\"model.provider\":\"claude-code\","
        "\"model.fallbacks\":\"codex-cli:gpt-5.5\"}");
    g_autoptr(GPtrArray) form = posted("model.provider", "claude-tmux",
                                       "model.fallbacks", "codex-cli:gpt-5.5",
                                       NULL);
    g_autoptr(GPtrArray) plan =
        clawt_lrg_agent_save_plan(form, json_node_get_object(settings));

    g_assert_cmpuint(plan->len, ==, 1);
    g_assert_cmpstr(((ClawtLrgAgentSetting *)g_ptr_array_index(plan, 0))->key, ==,
                    "model.provider");
}

/*
 * An entry's account picker offers logins for its own CLI, the CLI's
 * own login, and the account it already names whatever that is.
 */
static void
test_fallback_account_choices(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"accounts\":["
        "{\"id\":\"backup\",\"provider\":\"codex-cli\"},"
        "{\"id\":\"work\",\"provider\":\"claude-code\"}]}");
    g_autoptr(ClawtLrgAgentChoices) codex =
        clawt_lrg_agent_fallback_account_choices(reply, "codex-cli", NULL);
    g_autoptr(ClawtLrgAgentChoices) kept =
        clawt_lrg_agent_fallback_account_choices(reply, "codex-cli", "work");

    g_assert_cmpuint(index_of(codex, ""), ==, 0);
    g_assert_cmpuint(index_of(codex, "backup"), !=, G_MAXUINT);
    g_assert_cmpuint(index_of(codex, "work"), ==, G_MAXUINT);
    g_assert_cmpstr(codex->selected, ==, "");

    g_assert_cmpuint(index_of(kept, "work"), !=, G_MAXUINT);
    g_assert_cmpstr(kept->selected, ==, "work");
}

/* Add, remove and the two moves; a new entry is refused until it has a model. */
static void
test_fallback_edit_and_spell(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) list = clawt_model_fallback_parse_list(
        "codex-cli:gpt-5.5@backup, claude-code:sonnet", &error);
    g_autofree gchar *moved = NULL;
    g_autofree gchar *refused = NULL;
    g_autofree gchar *removed = NULL;

    g_assert_no_error(error);

    g_assert_true(clawt_lrg_agent_fallback_edit(list, "down", 0, NULL));
    moved = clawt_lrg_agent_fallback_spell(list, NULL);
    g_assert_cmpstr(moved, ==, "claude-code:sonnet, codex-cli:gpt-5.5@backup");

    g_assert_false(clawt_lrg_agent_fallback_edit(list, "up", 0, NULL));
    g_assert_false(clawt_lrg_agent_fallback_edit(list, "down", 1, NULL));
    g_assert_false(clawt_lrg_agent_fallback_edit(list, "remove", 7, NULL));

    g_assert_true(clawt_lrg_agent_fallback_edit(list, "add", 0, "claude-code"));
    g_assert_cmpuint(list->len, ==, 3);
    refused = clawt_lrg_agent_fallback_spell(list, &error);
    g_assert_null(refused);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_assert_nonnull(strstr(error->message, "Fallback 3"));

    g_assert_true(clawt_lrg_agent_fallback_edit(list, "remove", 2, NULL));
    g_assert_true(clawt_lrg_agent_fallback_edit(list, "up", 1, NULL));
    removed = clawt_lrg_agent_fallback_spell(list, NULL);
    g_assert_cmpstr(removed, ==, "codex-cli:gpt-5.5@backup, claude-code:sonnet");
}

/* The form leaves the list's child keys to the editor. */
static void
test_fallback_keys_are_not_form_fields(void)
{
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;
    guint seen = 0;

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const gchar *key = clawt_config_schema_agent_name(&schema[i]);

        if (key == NULL || !g_str_has_prefix(key, "model.fallbacks"))
            continue;

        seen++;
        g_assert_true(clawt_lrg_agent_key_skipped(key, &schema[i]));
    }

    /* the walk does reach them, or this test checks nothing */
    g_assert_cmpuint(seen, >, 0);
}

static void
test_saved_text(void)
{
    g_autofree gchar *none = clawt_lrg_agent_saved_text(0, FALSE);
    g_autofree gchar *tools = clawt_lrg_agent_saved_text(2, TRUE);
    g_autofree gchar *plain = clawt_lrg_agent_saved_text(2, FALSE);

    g_assert_nonnull(strstr(none, "Nothing"));
    g_assert_nonnull(strstr(tools, "lists its tools"));
    g_assert_nonnull(strstr(plain, "model or computer"));
}

/* ── At a glance ──────────────────────────────────────────────────── */

static void
test_state_tone_covers_every_state(void)
{
    g_autoptr(GEnumClass) klass = g_type_class_ref(CLAWT_TYPE_AGENT_STATE);
    guint i;

    g_assert_cmpstr(clawt_lrg_agent_state_tone("running"), ==, "good");
    g_assert_cmpstr(clawt_lrg_agent_state_tone("error"), ==, "bad");
    g_assert_cmpstr(clawt_lrg_agent_state_tone("stopped"), ==, "neutral");
    g_assert_cmpstr(clawt_lrg_agent_state_tone("nonsense"), ==, "neutral");

    /* every nick the enum produces resolves to a tone the theme paints */
    for (i = 0; i < klass->n_values; i++) {
        const gchar *tone =
            clawt_lrg_agent_state_tone(klass->values[i].value_nick);

        g_assert_true(g_strcmp0(tone, "good") == 0 ||
                      g_strcmp0(tone, "warn") == 0 ||
                      g_strcmp0(tone, "bad") == 0 ||
                      g_strcmp0(tone, "neutral") == 0);
    }
}

static void
test_verbs(void)
{
    ClawtLrgAgentVerb running[CLAWT_LRG_AGENT_VERBS];
    ClawtLrgAgentVerb stopped[CLAWT_LRG_AGENT_VERBS];
    ClawtLrgAgentVerb shadow[CLAWT_LRG_AGENT_VERBS];

    clawt_lrg_agent_verbs("running", running);
    clawt_lrg_agent_verbs("stopped", stopped);
    clawt_lrg_agent_verbs("shadow", shadow);

    g_assert_false(running[0].enabled);
    g_assert_true(running[1].enabled);
    g_assert_true(stopped[0].enabled);
    g_assert_false(stopped[1].enabled);
    g_assert_cmpstr(stopped[1].why_not, ==, "not running");

    /* nothing an agent that could not be understood could honour */
    g_assert_false(shadow[0].enabled);
    g_assert_false(shadow[1].enabled);
    g_assert_false(shadow[2].enabled);
    g_assert_cmpstr(shadow[2].kind, ==, "agent.restart");
}

static void
test_identity(void)
{
    g_autoptr(JsonNode) small = parse("{\"bytes\":1200,\"limit\":131072}");
    g_autoptr(JsonNode) huge = parse(
        "{\"bytes\":140000,\"limit\":131072,\"verdict\":\"shorten SOUL.org\"}");
    g_autoptr(JsonNode) none = parse("{\"bytes\":0,\"limit\":131072}");
    g_autofree gchar *text =
        clawt_lrg_agent_identity_text(json_node_get_object(small));

    g_assert_cmpstr(text, ==, "1200 bytes of 131072");
    g_assert_null(clawt_lrg_agent_identity_text(json_node_get_object(none)));
    g_assert_cmpstr(clawt_lrg_agent_identity_tone(json_node_get_object(huge)),
                    ==, "bad");
    g_assert_cmpstr(clawt_lrg_agent_identity_tone(json_node_get_object(small)),
                    ==, "warn");
}

static void
test_usage_two_agents(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"agents\":["
        "{\"id\":\"a\",\"turns\":3,\"input_tokens\":10,\"output_tokens\":20,"
        " \"cost_micros\":1500000},"
        "{\"id\":\"b\",\"turns\":1,\"input_tokens\":1,\"output_tokens\":2,"
        " \"cost_micros\":-1}],"
        "\"total\":{\"turns\":4}}");
    g_autofree gchar *a = clawt_lrg_agent_usage_text(reply, "a");
    g_autofree gchar *b = clawt_lrg_agent_usage_text(reply, "b");

    g_assert_nonnull(strstr(a, "3 turn(s)"));
    g_assert_nonnull(strstr(a, "10 in / 20 out"));
    g_assert_nonnull(strstr(b, "cost unknown"));
    g_assert_null(clawt_lrg_agent_usage_text(reply, "c"));
}

static void
test_removed_and_reset_text(void)
{
    g_autoptr(JsonNode) gone = parse("{\"computer\":\"removed\"}");
    g_autoptr(JsonNode) kept = parse("{\"computer\":\"kept: shared\"}");
    g_autoptr(JsonNode) reset = parse(
        "{\"sessions_cleared\":1,\"moved\":\"\",\"restarted\":true}");
    g_autofree gchar *a = clawt_lrg_agent_removed_text("kudu", gone);
    g_autofree gchar *b = clawt_lrg_agent_removed_text("kudu", kept);
    g_autofree gchar *c = clawt_lrg_agent_removed_text("kudu", NULL);
    g_autofree gchar *d = clawt_lrg_agent_reset_text(reset);

    g_assert_cmpstr(a, ==, "kudu and its computer are gone.");
    g_assert_nonnull(strstr(b, "was not removed: kept: shared"));
    g_assert_cmpstr(c, ==, "kudu is gone.");

    /* the boolean read as a boolean: a string reader would miss it */
    g_assert_nonnull(strstr(d, "1 stored session cleared and the agent "
                               "restarted"));
}

static void
test_file_subtitle(void)
{
    g_autoptr(JsonNode) prompt = parse(
        "{\"name\":\"SOUL.org\",\"title\":\"Who it is\",\"identity\":true}");
    g_autoptr(JsonNode) plain = parse(
        "{\"name\":\".mcp.json\",\"title\":\"MCP servers\",\"identity\":false}");
    g_autofree gchar *a =
        clawt_lrg_agent_file_subtitle(json_node_get_object(prompt));
    g_autofree gchar *b =
        clawt_lrg_agent_file_subtitle(json_node_get_object(plain));

    g_assert_cmpstr(a, ==, "prompt \xc2\xb7 Who it is");
    g_assert_cmpstr(b, ==, "MCP servers");
}

/* ── Integrations ─────────────────────────────────────────────────── */

static void
test_integration_rows(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"integrations\":["
        "{\"name\":\"phone\",\"type\":\"matrix\",\"scope\":\"selected\","
        " \"agents\":[\"a\"],\"covers\":true},"
        "{\"name\":\"tools\",\"type\":\"mcp\",\"scope\":\"all\","
        " \"agents\":[\"a\",\"b\"],\"covers\":true}],"
        "\"bindings\":["
        "{\"name\":\"phone\",\"type\":\"matrix\",\"shared\":true,\"valid\":true},"
        "{\"name\":\"own\",\"type\":\"email\",\"shared\":false,\"valid\":false,"
        " \"problem\":\"no password\"}]}");
    g_autoptr(GPtrArray) rows = clawt_lrg_agent_integration_rows(reply);
    ClawtLrgAgentIntegrationRow *own;

    g_assert_cmpuint(rows->len, ==, 3);
    g_assert_true(((ClawtLrgAgentIntegrationRow *)
                   g_ptr_array_index(rows, 0))->shared);
    g_assert_nonnull(strstr(((ClawtLrgAgentIntegrationRow *)
                             g_ptr_array_index(rows, 1))->subtitle,
                            "every agent"));

    own = g_ptr_array_index(rows, 2);
    g_assert_false(own->shared);
    g_assert_nonnull(strstr(own->subtitle, "no password"));
}

static void
test_integration_update_two_agents(void)
{
    g_autoptr(JsonNode) list = parse(
        "{\"integrations\":["
        "{\"name\":\"phone\",\"scope\":\"selected\",\"agents\":[\"a\",\"b\"]},"
        "{\"name\":\"tools\",\"scope\":\"all\",\"agents\":[\"a\"]}]}");
    g_autoptr(JsonNode) off = clawt_lrg_agent_integration_update(
        list, "phone", "a", FALSE, NULL);
    g_autoptr(JsonNode) on = clawt_lrg_agent_integration_update(
        list, "phone", "c", TRUE, NULL);
    g_autoptr(JsonNode) again = clawt_lrg_agent_integration_update(
        list, "phone", "b", TRUE, NULL);
    g_autofree gchar *refusal = NULL;
    g_autoptr(JsonNode) everyone = clawt_lrg_agent_integration_update(
        list, "tools", "a", FALSE, &refusal);
    JsonArray *agents;

    agents = json_object_get_array_member(json_node_get_object(off), "agents");
    g_assert_cmpuint(json_array_get_length(agents), ==, 1);
    g_assert_cmpstr(json_array_get_string_element(agents, 0), ==, "b");
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(off),
                                                  "scope"), ==, "selected");

    agents = json_object_get_array_member(json_node_get_object(on), "agents");
    g_assert_cmpuint(json_array_get_length(agents), ==, 3);

    /* turning on one already there does not add it twice */
    agents = json_object_get_array_member(json_node_get_object(again),
                                          "agents");
    g_assert_cmpuint(json_array_get_length(agents), ==, 2);

    /* an every-agent instance is not quietly made everyone-but-one */
    g_assert_null(everyone);
    g_assert_nonnull(strstr(refusal, "every agent"));
}

/* ── Mounts ───────────────────────────────────────────────────────── */

static void
test_mounts(void)
{
    g_autoptr(JsonNode) mount = parse(
        "{\"source\":\"/home/z/src\",\"target\":\"/src\",\"mode\":\"rw\"}");
    g_autofree gchar *subtitle =
        clawt_lrg_agent_mount_subtitle(json_node_get_object(mount));
    g_autofree gchar *title =
        clawt_lrg_computer_mount_title(json_node_get_object(mount));

    g_assert_cmpstr(subtitle, ==, "/home/z/src  (rw)");
    g_assert_cmpstr(title, ==, "/home/z/src  =  /src");
    g_assert_nonnull(strstr(clawt_lrg_agent_mounts_summary("vm"),
                            "virtiofs"));
    g_assert_nonnull(strstr(clawt_lrg_agent_mounts_summary("container"),
                            "Bind"));
    g_assert_nonnull(clawt_lrg_agent_mount_refusal("/x", ""));
    g_assert_null(clawt_lrg_agent_mount_refusal("/x", "/y"));
}

/* ── Mailbox ──────────────────────────────────────────────────────── */

static void
test_mailbox_rows(void)
{
    g_autoptr(JsonNode) list = parse(
        "{\"items\":[{\"id\":\"1\",\"from\":\"user\",\"body\":\"hi\","
        "  \"state\":\"pending\"},"
        " {\"id\":\"2\",\"from\":\"kudu\",\"body\":\"x\",\"state\":\"pending\","
        "  \"last_error\":\"socket closed\"}],"
        "\"follow_ups\":[{\"from\":\"user\",\"room\":\"dm:a\",\"body\":\"and\","
        "  \"state\":\"queued-follow-up\"}],"
        "\"depth\":2}");
    g_autoptr(JsonNode) dead = parse(
        "{\"items\":[{\"id\":\"9\",\"from\":\"user\",\"body\":\"lost\","
        "  \"state\":\"dead\"}],\"depth\":0}");
    g_autoptr(GPtrArray) rows = clawt_lrg_mailbox_rows(list, dead);
    g_autofree gchar *summary = clawt_lrg_mailbox_summary(list);
    ClawtLrgMailboxRow *row;

    g_assert_cmpuint(rows->len, ==, 4);

    row = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(row->title, ==, "from user");
    g_assert_true(row->can_ack);
    g_assert_false(row->can_requeue);
    g_assert_null(row->badge);

    row = g_ptr_array_index(rows, 1);
    g_assert_cmpstr(row->badge_tone, ==, "bad");
    g_assert_nonnull(strstr(row->badge, "socket closed"));

    /* a held follow-up has nothing to ack or requeue */
    row = g_ptr_array_index(rows, 2);
    g_assert_true(row->follow_up);
    g_assert_false(row->can_ack);
    g_assert_false(row->can_requeue);

    /* the dead letter, last, and the only one that can be put back */
    row = g_ptr_array_index(rows, 3);
    g_assert_true(row->dead);
    g_assert_true(row->can_requeue);
    g_assert_cmpstr(row->id, ==, "9");

    g_assert_cmpstr(summary, ==,
                    "2 waiting for delivery \xc2\xb7 1 queued follow-up");
}

static void
test_mailbox_empty_says_why(void)
{
    g_assert_nonnull(strstr(clawt_lrg_mailbox_empty_text("running"),
                            "socket"));
    g_assert_nonnull(strstr(clawt_lrg_mailbox_empty_text("stopped"),
                            "when it starts"));
}

static void
test_mailbox_failed_reply(void)
{
    g_autoptr(GPtrArray) rows = clawt_lrg_mailbox_rows(NULL, NULL);
    g_autoptr(JsonNode) purged = parse("{\"purged\":3}");
    g_autofree gchar *said = clawt_lrg_mailbox_purged_text(purged);

    g_assert_cmpuint(rows->len, ==, 0);
    g_assert_cmpstr(said, ==, "Purged 3 expired item(s).");
}

/* ── Computer ─────────────────────────────────────────────────────── */

static void
test_lifecycle_offered_from_the_library(void)
{
    g_autoptr(JsonNode) vm = parse(
        "{\"computer\":\"vm\",\"computer_machine\":true,\"caps\":\"computer\"}");
    g_autoptr(JsonNode) host = parse(
        "{\"computer\":\"host\",\"caps\":\"computer\"}");
    g_autoptr(JsonNode) none = parse("{\"computer\":\"none\",\"caps\":\"\"}");

    g_assert_true(clawt_lrg_computer_offers_lifecycle(json_node_get_object(vm)));
    g_assert_false(clawt_lrg_computer_offers_lifecycle(
        json_node_get_object(host)));
    g_assert_false(clawt_lrg_computer_offers_lifecycle(
        json_node_get_object(none)));
    g_assert_true(clawt_lrg_computer_present(json_node_get_object(host)));
    g_assert_false(clawt_lrg_computer_present(json_node_get_object(none)));
}

static void
test_screen_text(void)
{
    gint64 now = 1700000000000000;
    g_autoptr(JsonNode) off = parse("{\"observable\":false}");
    g_autoptr(JsonNode) fresh = parse(
        "{\"observable\":true,\"stamp\":0,\"stale\":false,\"held\":false}");
    g_autoptr(JsonNode) live = parse(
        "{\"observable\":true,\"stamp\":1700000000000000,\"stale\":false,"
        "\"held\":true,\"holder\":\"the lrg client\","
        "\"request\":\"click OK\"}");
    g_autoptr(JsonNode) old = parse(
        "{\"observable\":true,\"stamp\":1699999000000000,\"stale\":true,"
        "\"held\":false}");
    g_autofree gchar *a =
        clawt_lrg_computer_screen_text(json_node_get_object(off), now);
    g_autofree gchar *b =
        clawt_lrg_computer_screen_text(json_node_get_object(fresh), now);
    g_autofree gchar *c =
        clawt_lrg_computer_screen_text(json_node_get_object(live), now);
    g_autofree gchar *d =
        clawt_lrg_computer_screen_text(json_node_get_object(old), now);

    g_assert_nonnull(strstr(a, "not running"));
    g_assert_cmpstr(b, ==, "No frame yet.");
    g_assert_cmpstr(c, ==, "Live. Held by the lrg client. It has asked for "
                           "hands: click OK");
    g_assert_true(g_str_has_prefix(d, "Stale"));
}

static void
test_input_payload(void)
{
    g_autoptr(JsonNode) text = clawt_lrg_computer_input_payload(
        "a", "hello", "1,2", NULL);
    g_autoptr(JsonNode) key = clawt_lrg_computer_input_payload(
        "a", "@ctrl+l", NULL, NULL);
    g_autoptr(JsonNode) click = clawt_lrg_computer_input_payload(
        "a", "", " 10 , 20 ", NULL);
    g_autofree gchar *why_bad = NULL;
    g_autoptr(JsonNode) bad = clawt_lrg_computer_input_payload(
        "a", NULL, "ten,20", &why_bad);
    g_autofree gchar *why_empty = NULL;
    g_autoptr(JsonNode) empty = clawt_lrg_computer_input_payload(
        "a", NULL, NULL, &why_empty);
    JsonObject *object;

    /* typing first, when both are filled */
    object = json_node_get_object(text);
    g_assert_cmpstr(json_object_get_string_member(object, "kind"), ==, "text");
    g_assert_cmpstr(json_object_get_string_member(object, "text"), ==, "hello");

    object = json_node_get_object(key);
    g_assert_cmpstr(json_object_get_string_member(object, "kind"), ==, "key");
    g_assert_cmpstr(json_object_get_string_member(object, "text"), ==,
                    "ctrl+l");

    /* coordinates as integers, which is what the daemon reads */
    object = json_node_get_object(click);
    g_assert_cmpstr(json_object_get_string_member(object, "kind"), ==,
                    "click");
    g_assert_cmpint(clawt_ipc_payload_int(object, "x", -1), ==, 10);
    g_assert_cmpint(clawt_ipc_payload_int(object, "y", -1), ==, 20);

    g_assert_null(bad);
    g_assert_nonnull(why_bad);
    g_assert_null(empty);
    g_assert_nonnull(strstr(why_empty, "Nothing to send"));
}

static void
test_exec_output_and_exchange(void)
{
    g_autoptr(JsonNode) ok = parse(
        "{\"exit\":0,\"stdout\":\"hi\\n\",\"stderr\":\"\"}");
    g_autoptr(JsonNode) failed = parse(
        "{\"exit\":2,\"stdout\":\"\",\"stderr\":\"no such file\"}");
    g_autoptr(JsonNode) exchange = parse(
        "{\"entries\":[\"in/\",\"out/report.txt\"]}");
    g_autofree gchar *a = clawt_lrg_computer_exec_output(ok);
    g_autofree gchar *b = clawt_lrg_computer_exec_output(failed);
    g_autoptr(GPtrArray) names = clawt_lrg_computer_exchange_names(exchange);

    g_assert_cmpstr(a, ==, "hi\n");
    g_assert_cmpstr(b, ==, "no such file\n[exit 2]\n");
    g_assert_cmpuint(names->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(names, 1), ==, "out/report.txt");
}

static void
test_state_and_lifecycle_text(void)
{
    g_autoptr(JsonNode) agent = parse(
        "{\"computer\":\"container\",\"caps\":\"computer\"}");
    g_autoptr(JsonNode) status = parse("{\"state\":\"running\"}");
    g_autoptr(JsonNode) stopped = parse(
        "{\"state\":\"absent\",\"removes\":true}");
    g_autofree gchar *a =
        clawt_lrg_computer_state_text(json_node_get_object(agent), status);
    g_autofree gchar *b =
        clawt_lrg_computer_state_text(json_node_get_object(agent), NULL);
    g_autofree gchar *c = clawt_lrg_computer_lifecycle_text("kudu", stopped);

    g_assert_cmpstr(a, ==, "container \xe2\x80\x94 running");
    g_assert_nonnull(strstr(b, "built when the agent starts"));
    g_assert_nonnull(strstr(c, "builds a fresh one"));
}

/* ── Hold ─────────────────────────────────────────────────────────── */

static void
test_hold(void)
{
    g_autoptr(JsonNode) held = parse(
        "{\"version\":\"0.6.2\",\"hold\":{\"held\":true,\"fleet\":true,"
        "\"draining\":2},\"update\":{\"available\":true,\"latest\":\"0.7.0\"}}");
    g_autoptr(JsonNode) free_fleet = parse(
        "{\"hold\":{\"held\":false,\"draining\":0},"
        "\"update\":{\"available\":false,\"latest\":\"0.6.2\"}}");
    g_autofree gchar *draining = clawt_lrg_hold_paused_text(held);
    g_autofree gchar *idle = clawt_lrg_hold_paused_text(free_fleet);
    g_autofree gchar *update = clawt_lrg_hold_available_update(held);

    g_assert_true(clawt_lrg_hold_is_held(held));
    g_assert_false(clawt_lrg_hold_is_held(free_fleet));
    g_assert_false(clawt_lrg_hold_is_held(NULL));
    g_assert_cmpstr(draining, ==, "Held. 2 turn(s) still finishing.");
    g_assert_cmpstr(idle, ==, "Held, with nothing in flight.");
    g_assert_cmpstr(update, ==, "0.7.0");

    /* `latest` alone is the version running, not an update */
    g_assert_null(clawt_lrg_hold_available_update(free_fleet));
}

gint
main(gint argc, gchar **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/lrg-agent/choices/null-terminated",
                    test_choices_are_null_terminated);
    g_test_add_func("/lrg-agent/choices/other", test_choice_value_other);
    g_test_add_func("/lrg-agent/choices/enum", test_enum_choices_walk_the_type);
    g_test_add_func("/lrg-agent/choices/enum-unknown",
                    test_enum_choices_keep_unknown_and_unset);
    g_test_add_func("/lrg-agent/choices/computer-types",
                    test_computer_types_walk_the_library);
    g_test_add_func("/lrg-agent/choices/teams", test_team_choices);
    g_test_add_func("/lrg-agent/choices/accounts",
                    test_account_choices_drop_unusable_but_keep_current);
    g_test_add_func("/lrg-agent/choices/shared",
                    test_shared_choices_leave_out_hosts);
    g_test_add_func("/lrg-agent/choices/images", test_image_choices);
    g_test_add_func("/lrg-agent/choices/disks",
                    test_disk_choices_without_a_disk_look_wrong);
    g_test_add_func("/lrg-agent/choices/models",
                    test_provider_and_model_choices);
    g_test_add_func("/lrg-agent/choices/resolution",
                    test_resolution_keeps_an_unusual_size);
    g_test_add_func("/lrg-agent/choices/relabel",
                    test_relabel_walks_the_library);
    g_test_add_func("/lrg-agent/form/schema", test_form_walks_the_schema);
    g_test_add_func("/lrg-agent/form/shared",
                    test_form_hides_the_shared_machines_keys);
    g_test_add_func("/lrg-agent/form/label", test_key_label);
    g_test_add_func("/lrg-agent/save/changed",
                    test_save_sends_only_what_changed);
    g_test_add_func("/lrg-agent/save/account",
                    test_save_clears_the_account_before_the_provider);
    g_test_add_func("/lrg-agent/save/account-unset",
                    test_save_does_not_clear_an_unset_account);
    g_test_add_func("/lrg-agent/save/shared",
                    test_save_moving_onto_a_shared_machine);
    g_test_add_func("/lrg-agent/save/text", test_saved_text);
    g_test_add_func("/lrg-agent/save/fallbacks-last",
                    test_save_sends_the_fallbacks_last);
    g_test_add_func("/lrg-agent/save/fallbacks-before-provider",
                    test_save_clears_the_fallbacks_before_the_provider);
    g_test_add_func("/lrg-agent/save/fallbacks-unchanged",
                    test_save_leaves_unchanged_fallbacks);
    g_test_add_func("/lrg-agent/fallbacks/accounts",
                    test_fallback_account_choices);
    g_test_add_func("/lrg-agent/fallbacks/edit", test_fallback_edit_and_spell);
    g_test_add_func("/lrg-agent/fallbacks/not-form-fields",
                    test_fallback_keys_are_not_form_fields);
    g_test_add_func("/lrg-agent/glance/tone",
                    test_state_tone_covers_every_state);
    g_test_add_func("/lrg-agent/glance/verbs", test_verbs);
    g_test_add_func("/lrg-agent/glance/identity", test_identity);
    g_test_add_func("/lrg-agent/glance/usage", test_usage_two_agents);
    g_test_add_func("/lrg-agent/glance/removed",
                    test_removed_and_reset_text);
    g_test_add_func("/lrg-agent/files/subtitle", test_file_subtitle);
    g_test_add_func("/lrg-agent/integrations/rows", test_integration_rows);
    g_test_add_func("/lrg-agent/integrations/update",
                    test_integration_update_two_agents);
    g_test_add_func("/lrg-agent/mounts", test_mounts);
    g_test_add_func("/lrg-agent/mailbox/rows", test_mailbox_rows);
    g_test_add_func("/lrg-agent/mailbox/empty", test_mailbox_empty_says_why);
    g_test_add_func("/lrg-agent/mailbox/failed", test_mailbox_failed_reply);
    g_test_add_func("/lrg-agent/computer/lifecycle",
                    test_lifecycle_offered_from_the_library);
    g_test_add_func("/lrg-agent/computer/screen", test_screen_text);
    g_test_add_func("/lrg-agent/computer/input", test_input_payload);
    g_test_add_func("/lrg-agent/computer/exec",
                    test_exec_output_and_exchange);
    g_test_add_func("/lrg-agent/computer/state",
                    test_state_and_lifecycle_text);
    g_test_add_func("/lrg-agent/hold", test_hold);

    return g_test_run();
}
