/*
 * clawt-hold.c - Putting the fleet down without losing what it was doing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Restarting the daemon killed every agent mid-turn, and a turn in
 * progress is lost work.  The practical result was that operators
 * stopped restarting: changes got batched and deferred because the cost
 * of applying one was "whatever every agent happened to be doing is
 * gone", and that cost is unbounded and unknowable at the moment you
 * press the button.
 *
 * The primitive this rests on already existed and was already proven.
 * clawt_agent_runtime_is_paused() is honoured by
 * clawt_mailbox_router_drain(), which returns without delivering
 * anything and leaves every item queued -- written for account session
 * allowance, with a comment describing exactly these semantics.  Three
 * things stopped it being reusable: it was a deadline rather than a
 * hold, it was in memory only, and nothing could set it.
 *
 * This is the third and the second.  The first is
 * clawt_agent_runtime_set_held(), beside the deadline it sits next to.
 */

#include "clawt-hold.h"

#include "clawt-error.h"
#include "clawt-util.h"

#include <glib/gstdio.h>
#include <yaml-glib.h>

struct _ClawtHold {
    GObject parent_instance;

    gchar      *state_path;

    gboolean    fleet;
    GHashTable *agents;    /* agent id -> NULL, a set */
    gint64      since;     /* microseconds */
    GPtrArray  *running;   /* gchar*, what to put back */
};

G_DEFINE_FINAL_TYPE(ClawtHold, clawt_hold, G_TYPE_OBJECT)

static void
clawt_hold_finalize(GObject *object)
{
    ClawtHold *self = CLAWT_HOLD(object);

    g_clear_pointer(&self->state_path, g_free);
    g_clear_pointer(&self->agents, g_hash_table_unref);
    g_clear_pointer(&self->running, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_hold_parent_class)->finalize(object);
}

static void
clawt_hold_class_init(ClawtHoldClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_hold_finalize;
}

static void
clawt_hold_init(ClawtHold *self)
{
    self->agents = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         NULL);
    self->running = g_ptr_array_new_with_free_func(g_free);
}

ClawtHold *
clawt_hold_new(const gchar *state_path)
{
    ClawtHold *self = g_object_new(CLAWT_TYPE_HOLD, NULL);

    self->state_path = g_strdup(state_path);

    return self;
}

/* ── Remembering ─────────────────────────────────────────────────── */

static void
read_strings(YamlMapping *mapping, const gchar *key, GHashTable *into_set,
             GPtrArray *into_list)
{
    YamlNode *node = yaml_mapping_get_member(mapping, key);
    YamlSequence *sequence;
    guint i;

    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE)
        return;

    sequence = yaml_node_get_sequence(node);

    for (i = 0; i < yaml_sequence_get_length(sequence); i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        const gchar *text;

        if (element == NULL ||
            yaml_node_get_node_type(element) != YAML_NODE_SCALAR)
            continue;

        text = yaml_node_get_string(element);

        if (text == NULL || *text == '\0')
            continue;

        if (into_set != NULL)
            g_hash_table_add(into_set, g_strdup(text));

        if (into_list != NULL)
            g_ptr_array_add(into_list, g_strdup(text));
    }
}

void
clawt_hold_load(ClawtHold *self)
{
    g_autoptr(YamlParser) parser = NULL;
    g_autoptr(GError) error = NULL;
    YamlNode *root;
    YamlMapping *mapping;
    YamlNode *node;

    g_return_if_fail(CLAWT_IS_HOLD(self));

    g_hash_table_remove_all(self->agents);
    g_ptr_array_set_size(self->running, 0);
    self->fleet = FALSE;
    self->since = 0;

    if (self->state_path == NULL ||
        !g_file_test(self->state_path, G_FILE_TEST_EXISTS))
        return;

    parser = yaml_parser_new();

    if (!yaml_parser_load_from_file(parser, self->state_path, &error)) {
        /*
         * A warning and no hold.  This file is bookkeeping about a
         * pause; refusing to bring the fleet up because a note about
         * one is corrupt trades a small loss for a total one.
         */
        g_warning("hold: %s could not be read (%s); starting with no hold",
                  self->state_path, error->message);
        return;
    }

    root = yaml_parser_get_root(parser);

    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
        return;

    mapping = yaml_node_get_mapping(root);

    node = yaml_mapping_get_member(mapping, "fleet");
    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        self->fleet = yaml_node_get_boolean(node);

    node = yaml_mapping_get_member(mapping, "since");
    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        self->since = yaml_node_get_int(node);

    read_strings(mapping, "agents", self->agents, NULL);
    read_strings(mapping, "running", NULL, self->running);
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

static void
add_sequence(YamlMapping *into, const gchar *key, GPtrArray *values)
{
    g_autoptr(YamlNode) node = NULL;
    YamlSequence *sequence;
    guint i;

    if (values->len == 0)
        return;

    node = yaml_node_new_sequence(NULL);
    sequence = yaml_node_get_sequence(node);

    for (i = 0; i < values->len; i++) {
        g_autoptr(YamlNode) element =
            yaml_node_new_string(g_ptr_array_index(values, i));

        yaml_sequence_add_element(sequence, element);
    }

    yaml_mapping_set_member(into, key, node);
}

gboolean
clawt_hold_save(ClawtHold *self, GError **error)
{
    g_autoptr(YamlNode) root = NULL;
    g_autoptr(YamlGenerator) generator = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(GPtrArray) named = NULL;
    YamlMapping *mapping;

    g_return_val_if_fail(CLAWT_IS_HOLD(self), FALSE);

    if (self->state_path == NULL)
        return TRUE;

    /*
     * No hold means no file.  Writing one that says nothing would give
     * "there is no hold" two spellings, and the next start would have to
     * know that an empty record and an absent one mean the same thing --
     * which is exactly the kind of pair this codebase keeps getting
     * wrong.
     */
    if (!clawt_hold_is_any(self) && self->running->len == 0) {
        if (g_file_test(self->state_path, G_FILE_TEST_EXISTS) &&
            g_unlink(self->state_path) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "could not remove %s", self->state_path);
            return FALSE;
        }

        return TRUE;
    }

    root = yaml_node_new_mapping(NULL);
    mapping = yaml_node_get_mapping(root);

    {
        g_autoptr(YamlNode) fleet = yaml_node_new_boolean(self->fleet);
        g_autoptr(YamlNode) since = yaml_node_new_int(self->since);

        yaml_mapping_set_member(mapping, "fleet", fleet);
        yaml_mapping_set_member(mapping, "since", since);
    }

    named = clawt_hold_held_agents(self);
    add_sequence(mapping, "agents", named);
    add_sequence(mapping, "running", self->running);

    generator = yaml_generator_new();
    yaml_generator_set_root(generator, root);
    text = yaml_generator_to_data(generator, NULL, error);

    if (text == NULL)
        return FALSE;

    /*
     * 0600: it names every agent that was running, which is a map of
     * what somebody's fleet does.
     */
    return clawt_write_file_atomic(self->state_path, text, -1, 0600, FALSE,
                                   error);
}

/* ── Applying ────────────────────────────────────────────────────── */

void
clawt_hold_apply(ClawtHold *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_HOLD(self));

    if (self->since == 0)
        self->since = g_get_real_time();

    if (agent_id == NULL) {
        self->fleet = TRUE;

        /*
         * The named set is left alone rather than cleared.  A fleet hold
         * released while an agent was individually held must leave that
         * agent held -- somebody paused it for a reason of its own, and
         * a fleet-wide release is not a statement about that reason.
         */
        return;
    }

    g_hash_table_add(self->agents, g_strdup(agent_id));
}

gboolean
clawt_hold_release(ClawtHold *self, const gchar *agent_id)
{
    gboolean changed = FALSE;

    g_return_val_if_fail(CLAWT_IS_HOLD(self), FALSE);

    if (agent_id == NULL) {
        changed = self->fleet || g_hash_table_size(self->agents) > 0;
        self->fleet = FALSE;
        g_hash_table_remove_all(self->agents);
    } else {
        changed = g_hash_table_remove(self->agents, agent_id);
    }

    if (!clawt_hold_is_any(self))
        self->since = 0;

    return changed;
}

gboolean
clawt_hold_covers(ClawtHold *self, const gchar *agent_id)
{
    g_return_val_if_fail(CLAWT_IS_HOLD(self), FALSE);

    if (self->fleet)
        return TRUE;

    return agent_id != NULL &&
           g_hash_table_contains(self->agents, agent_id);
}

gboolean
clawt_hold_is_fleet(ClawtHold *self)
{
    g_return_val_if_fail(CLAWT_IS_HOLD(self), FALSE);

    return self->fleet;
}

gboolean
clawt_hold_is_any(ClawtHold *self)
{
    g_return_val_if_fail(CLAWT_IS_HOLD(self), FALSE);

    return self->fleet || g_hash_table_size(self->agents) > 0;
}

gint64
clawt_hold_get_since(ClawtHold *self)
{
    g_return_val_if_fail(CLAWT_IS_HOLD(self), 0);

    return self->since;
}

void
clawt_hold_set_running(ClawtHold *self, GPtrArray *agent_ids)
{
    guint i;

    g_return_if_fail(CLAWT_IS_HOLD(self));

    g_ptr_array_set_size(self->running, 0);

    for (i = 0; agent_ids != NULL && i < agent_ids->len; i++)
        g_ptr_array_add(self->running,
                        g_strdup(g_ptr_array_index(agent_ids, i)));
}

GPtrArray *
clawt_hold_get_running(ClawtHold *self)
{
    g_return_val_if_fail(CLAWT_IS_HOLD(self), NULL);

    return self->running;
}

GPtrArray *
clawt_hold_held_agents(ClawtHold *self)
{
    GPtrArray *out;
    GHashTableIter iter;
    gpointer key;

    g_return_val_if_fail(CLAWT_IS_HOLD(self), NULL);

    out = g_ptr_array_new();

    g_hash_table_iter_init(&iter, self->agents);

    while (g_hash_table_iter_next(&iter, &key, NULL))
        g_ptr_array_add(out, key);

    /*
     * Sorted, because a hash table's order is not one -- and this list
     * is written to a file that is read back, shown in a listing and
     * compared in a test.
     */
    g_ptr_array_sort(out, compare_strings);

    return out;
}

gchar *
clawt_hold_label(gboolean held, gboolean draining)
{
    if (!held)
        return NULL;

    /*
     * Two words rather than one, because they answer different
     * questions.  "draining" means a turn is still running and a
     * restart would still cost it; "held" means it would not.
     */
    return g_strdup(draining ? "draining" : "held");
}
