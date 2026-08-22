/*
 * clawt-config.c - clawtilla configuration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "config/clawt-config.h"
#include "config/clawt-config-schema.h"
#include "computer/clawt-mount.h"

#include <yaml-glib.h>
#include <string.h>

struct _ClawtConfig {
    GObject      parent_instance;

    gchar       *path;
    YamlNode    *root;        /* always a mapping */
    GPtrArray   *agents;      /* ClawtAgentConfig* */
    GPtrArray   *warnings;    /* gchar* */
};

G_DEFINE_FINAL_TYPE(ClawtConfig, clawt_config, G_TYPE_OBJECT)

struct _ClawtAgentConfig {
    gint         ref_count;

    ClawtConfig *config;      /* unowned; the config outlives its agents */
    gchar       *id;
    YamlNode    *node;        /* the agent's mapping, unowned */
    gchar       *shadow_reason;
};

/* ── Path walking ────────────────────────────────────────────────── */

/*
 * Finds the node at a dotted path, optionally creating the mappings on the
 * way.  This is the whole of the "access by path" idea: one traversal, used
 * by every getter and setter, instead of a property per option.
 *
 * With @create set, every missing level is made a mapping -- including the
 * last.  That is right because the only caller that creates is looking up a
 * PARENT in order to put a leaf inside it; creating a null there instead
 * would make setting any value under a section the file does not yet have
 * fail, which is most of them on a fresh config.
 */
static YamlNode *
node_at_path(YamlNode    *root,
             const gchar *key,
             gboolean     create)
{
    g_auto(GStrv) parts = NULL;
    YamlNode *current = root;
    guint i;

    if (root == NULL || key == NULL)
        return NULL;

    parts = g_strsplit(key, ".", -1);

    for (i = 0; parts[i] != NULL; i++) {
        YamlMapping *mapping;
        YamlNode *next;

        if (yaml_node_get_node_type(current) != YAML_NODE_MAPPING)
            return NULL;

        mapping = yaml_node_get_mapping(current);
        next = yaml_mapping_get_member(mapping, parts[i]);

        if (next == NULL) {
            if (!create)
                return NULL;

            /*
             * NULL rather than a fresh mapping: yaml_node_new_mapping()
             * takes its argument (transfer none) and refs it, so passing
             * one leaks the caller's ref on every call.  Passing NULL
             * asks it to make its own.
             */
            next = yaml_node_new_mapping(NULL);
            yaml_mapping_set_member(mapping, parts[i], next);
            yaml_node_unref(next);

            next = yaml_mapping_get_member(mapping, parts[i]);
        }

        current = next;
    }

    return current;
}

/*
 * Re-attaches the schema's documentation to a node.
 *
 * Called when a value is written, so a config the daemon has rewritten
 * still explains itself.  The author's own comments are separate: those
 * were captured on load and live on the same nodes, so both survive.
 */
static void
apply_schema_comment(YamlNode *node, const gchar *schema_key)
{
    g_autoptr(GPtrArray) comment = NULL;

    if (node == NULL)
        return;

    if (yaml_node_get_leading_comments(node) != NULL)
        return;

    comment = clawt_config_schema_comment_for(schema_key);
    if (comment == NULL)
        return;

    yaml_node_set_leading_comments(node, comment);
    yaml_node_set_blank_before(node, TRUE);
}

/* ── Reading, with schema defaults ───────────────────────────────── */

static const gchar *
schema_default_for(const gchar *key)
{
    const ClawtSchemaEntry *entry = clawt_config_schema_lookup(key);

    return (entry != NULL) ? entry->default_value : NULL;
}

/*
 * Reads a mapping member as a string, tolerating absence.
 *
 * yaml_node_get_string() asserts on NULL, and yaml_mapping_get_member()
 * returns NULL for any key the author did not write -- which for optional
 * settings is most of them.  Wrapping the pair once means every optional
 * read is not its own chance to crash on a config that merely left
 * something out.
 */
static const gchar *
member_string(YamlMapping *mapping, const gchar *key)
{
    YamlNode *node;

    if (mapping == NULL || key == NULL)
        return NULL;

    node = yaml_mapping_get_member(mapping, key);
    if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
        return NULL;

    return yaml_node_get_string(node);
}

static const gchar *
lookup_string(YamlNode *root, const gchar *key)
{
    YamlNode *node = node_at_path(root, key, FALSE);

    if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
        return NULL;

    return yaml_node_get_string(node);
}

static gboolean
string_to_boolean(const gchar *value, gboolean fallback)
{
    if (value == NULL)
        return fallback;

    if (g_ascii_strcasecmp(value, "true") == 0 ||
        g_ascii_strcasecmp(value, "yes") == 0 ||
        g_ascii_strcasecmp(value, "on") == 0 ||
        g_strcmp0(value, "1") == 0)
        return TRUE;

    return FALSE;
}

/* ── Agent configuration ─────────────────────────────────────────── */

static ClawtAgentConfig *
clawt_agent_config_new(ClawtConfig *config,
                       const gchar *id,
                       YamlNode    *node)
{
    ClawtAgentConfig *self = g_new0(ClawtAgentConfig, 1);

    self->ref_count = 1;
    self->config = config;
    self->id = g_strdup(id);
    self->node = node;

    return self;
}

ClawtAgentConfig *
clawt_agent_config_ref(ClawtAgentConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

void
clawt_agent_config_unref(ClawtAgentConfig *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->id);
    g_free(self->shadow_reason);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtAgentConfig, clawt_agent_config,
                    clawt_agent_config_ref, clawt_agent_config_unref)

static void
agent_mark_shadow(ClawtAgentConfig *self, const gchar *format, ...)
{
    va_list args;

    /* The first reason is the useful one; later ones are usually knock-ons. */
    if (self->shadow_reason != NULL)
        return;

    va_start(args, format);
    self->shadow_reason = g_strdup_vprintf(format, args);
    va_end(args);
}

/*
 * Validates one agent, turning anything unusable into a shadow rather than
 * an error.
 *
 * This is the forward-compatibility rule: a config written by a newer
 * clawtilla naming a computer type this build has never heard of should
 * disable that one agent, not stop the daemon starting the other nine.
 */
static void
agent_validate(ClawtAgentConfig *self)
{
    const gchar *computer_type;
    const gchar *confine;
    gint value = 0;

    if (!clawt_is_valid_id(self->id)) {
        agent_mark_shadow(self,
                          "agent id '%s' is not usable: ids may hold only "
                          "lowercase letters, digits, '-' and '_', and must "
                          "not start with punctuation",
                          self->id != NULL ? self->id : "");
        return;
    }

    /*
     * The two persona forms are documented as alternatives and were never
     * checked, so both were rendered: an agent got its identity files AND
     * an inline prompt, with nothing saying only one was meant to apply.
     */
    if (clawt_agent_config_get_string(self, "persona.system_prompt") != NULL) {
        g_auto(GStrv) identity =
            clawt_agent_config_get_string_list(self, "persona.identity_files");

        if (identity != NULL && identity[0] != NULL) {
            agent_mark_shadow(self,
                              "persona.system_prompt and "
                              "persona.identity_files are alternatives; "
                              "set one or the other");
            return;
        }
    }

    computer_type = clawt_agent_config_get_string(self, "computer.type");
    if (computer_type != NULL &&
        !clawt_enum_from_nick(CLAWT_TYPE_COMPUTER_TYPE, computer_type,
                              &value)) {
        agent_mark_shadow(self,
                          "unknown computer type '%s'; this build knows "
                          "none, host, container and vm",
                          computer_type);
        return;
    }

    confine = clawt_agent_config_get_string(self, "computer.host.confine");
    if (confine != NULL &&
        !clawt_enum_from_nick(CLAWT_TYPE_CONFINE_MODE, confine, &value)) {
        agent_mark_shadow(self,
                          "unknown confinement mode '%s'; this build knows "
                          "none, workspace, allowlist and bwrap",
                          confine);
        return;
    }

    /*
     * A host computer without the confirmation is a shadow rather than a
     * silently-confined agent.  Quietly falling back to a safer mode would
     * be the wrong kind of helpful: the user asked for something the daemon
     * will not do, and should be told so.
     */
    if (g_strcmp0(computer_type, "host") == 0 &&
        !clawt_agent_config_get_boolean(self,
                                        "computer.host.confirm_host_control")) {
        agent_mark_shadow(self,
                          "a host computer needs "
                          "computer.host.confirm_host_control: true -- it "
                          "gives the agent your real machine");
        return;
    }

    {
        g_autofree gchar *mount_error = NULL;
        g_autoptr(GPtrArray) mounts = clawt_agent_config_get_mounts(self);
        guint i;

        for (i = 0; mounts != NULL && i < mounts->len; i++) {
            g_autoptr(GError) error = NULL;

            if (!clawt_mount_validate(g_ptr_array_index(mounts, i), &error)) {
                agent_mark_shadow(self, "mount %u: %s", i, error->message);
                return;
            }
        }

        /*
         * Overlapping targets are checked across the set rather than per
         * mount: two mounts at /work and /work/src would each validate alone
         * and then have one silently hide the other.
         */
        for (i = 0; mounts != NULL && i < mounts->len; i++) {
            const gchar *a = clawt_mount_get_target(
                g_ptr_array_index(mounts, i));
            guint j;

            for (j = i + 1; j < mounts->len; j++) {
                const gchar *b = clawt_mount_get_target(
                    g_ptr_array_index(mounts, j));

                if (g_strcmp0(a, b) == 0) {
                    agent_mark_shadow(self,
                                      "two mounts share the target '%s'", a);
                    return;
                }
            }
        }
    }
}

const gchar *
clawt_agent_config_get_id(ClawtAgentConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->id;
}

gboolean
clawt_agent_config_is_shadow(ClawtAgentConfig *self)
{
    g_return_val_if_fail(self != NULL, TRUE);
    return self->shadow_reason != NULL;
}

const gchar *
clawt_agent_config_get_shadow_reason(ClawtAgentConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->shadow_reason;
}

/*
 * Reads an agent value with a two-step fallback: the agent's own block, then
 * the matching defaults.* key, then the schema default.
 *
 * The defaults step is what makes the `defaults:` section mean anything --
 * an agent that says nothing about its model should follow the fleet's
 * choice rather than the schema's.
 */
const gchar *
clawt_agent_config_get_string(ClawtAgentConfig *self, const gchar *key)
{
    static const struct {
        const gchar *agent_key;
        const gchar *default_key;
    } inherited[] = {
        { "model.provider",  "defaults.provider"  },
        { "model.model",     "defaults.model"     },
        { "computer.type",   "defaults.computer"  },
        { "runtime.restart", "defaults.restart"   },
        { "computer.container.image", "defaults.container_image" },

        /*
         * These two fall back to the top-level key rather than a
         * defaults.* one, because that section *is* the fleet-wide
         * setting -- there is no defaults.memories, and inventing one
         * would be a second place to say the same thing.
         */
        { "memories.enabled",     "memories.enabled"     },
        { "memories.max_results", "memories.max_results" },
        { NULL, NULL }
    };
    g_autofree gchar *schema_key = NULL;
    const gchar *value;
    gsize i;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    value = lookup_string(self->node, key);
    if (value != NULL)
        return value;

    for (i = 0; inherited[i].agent_key != NULL; i++) {
        if (g_strcmp0(key, inherited[i].agent_key) != 0)
            continue;

        value = clawt_config_get_string(self->config, inherited[i].default_key);
        if (value != NULL)
            return value;
    }

    schema_key = g_strdup_printf("agents.%s", key);
    return schema_default_for(schema_key);
}

gchar *
clawt_agent_config_get_path_value(ClawtAgentConfig *self, const gchar *key)
{
    return clawt_expand_path(clawt_agent_config_get_string(self, key));
}

gboolean
clawt_agent_config_get_boolean(ClawtAgentConfig *self, const gchar *key)
{
    return string_to_boolean(clawt_agent_config_get_string(self, key), FALSE);
}

gint64
clawt_agent_config_get_int(ClawtAgentConfig *self, const gchar *key)
{
    const gchar *value = clawt_agent_config_get_string(self, key);

    return (value != NULL) ? g_ascii_strtoll(value, NULL, 10) : 0;
}

gint
clawt_agent_config_get_enum(ClawtAgentConfig *self, const gchar *key)
{
    g_autofree gchar *schema_key = NULL;
    const ClawtSchemaEntry *entry;
    const gchar *nick;
    gint value = 0;

    g_return_val_if_fail(self != NULL, 0);

    schema_key = g_strdup_printf("agents.%s", key);
    entry = clawt_config_schema_lookup(schema_key);

    if (entry == NULL || entry->enum_type == NULL)
        return 0;

    nick = clawt_agent_config_get_string(self, key);
    if (nick != NULL && clawt_enum_from_nick(entry->enum_type(), nick, &value))
        return value;

    if (entry->default_value != NULL &&
        clawt_enum_from_nick(entry->enum_type(), entry->default_value, &value))
        return value;

    return 0;
}

static GStrv
node_to_strv(YamlNode *node)
{
    YamlSequence *sequence;
    GPtrArray *out;
    guint i;
    guint length;

    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE)
        return NULL;

    sequence = yaml_node_get_sequence(node);
    length = yaml_sequence_get_length(sequence);
    out = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        const gchar *value;

        if (element == NULL ||
            yaml_node_get_node_type(element) != YAML_NODE_SCALAR)
            continue;

        value = yaml_node_get_string(element);
        if (value != NULL)
            g_ptr_array_add(out, g_strdup(value));
    }

    g_ptr_array_add(out, NULL);
    return (GStrv)g_ptr_array_free(out, FALSE);
}

GStrv
clawt_agent_config_get_string_list(ClawtAgentConfig *self, const gchar *key)
{
    g_return_val_if_fail(self != NULL, NULL);

    return node_to_strv(node_at_path(self->node, key, FALSE));
}

gboolean
clawt_agent_config_has_key(ClawtAgentConfig *self, const gchar *key)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return node_at_path(self->node, key, FALSE) != NULL;
}

static gboolean
set_scalar(YamlNode *root, const gchar *key, YamlNode *value,
           const gchar *schema_key)
{
    g_auto(GStrv) parts = NULL;
    YamlNode *parent;
    g_autofree gchar *parent_path = NULL;
    const gchar *leaf;
    gchar *last_dot;

    if (root == NULL || key == NULL)
        return FALSE;

    parent_path = g_strdup(key);
    last_dot = strrchr(parent_path, '.');

    if (last_dot != NULL) {
        *last_dot = '\0';
        leaf = last_dot + 1;
        parent = node_at_path(root, parent_path, TRUE);
    } else {
        leaf = key;
        parent = root;
    }

    if (parent == NULL ||
        yaml_node_get_node_type(parent) != YAML_NODE_MAPPING)
        return FALSE;

    yaml_mapping_set_member(yaml_node_get_mapping(parent), leaf, value);

    if (schema_key != NULL)
        apply_schema_comment(
            yaml_mapping_get_member(yaml_node_get_mapping(parent), leaf),
            schema_key);

    return TRUE;
}

/*
 * Adds one mount to computer.mounts, creating the sequence if needed.
 *
 * Mounts are the only list an agent's configuration holds, and
 * clawt_agent_config_set_string() cannot express one -- it writes a
 * scalar at a dotted path. Without this the mount list could be read
 * but never written, so declaring a shared folder meant editing the
 * YAML by hand and no client offered it at all.
 */
gboolean
clawt_agent_config_add_mount(ClawtAgentConfig *self,
                             ClawtMount       *mount)
{
    g_autoptr(YamlMapping) mapping = NULL;
    g_autoptr(YamlNode) element = NULL;
    YamlNode *computer;
    YamlNode *list;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(mount != NULL, FALSE);
    g_return_val_if_fail(clawt_mount_get_target(mount) != NULL, FALSE);

    computer = node_at_path(self->node, "computer", TRUE);

    if (computer == NULL ||
        yaml_node_get_node_type(computer) != YAML_NODE_MAPPING)
        return FALSE;

    list = yaml_mapping_get_member(yaml_node_get_mapping(computer),
                                    "mounts");

    if (list == NULL ||
        yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE) {
        /*
         * NULL, not a fresh mapping: yaml_node_new_sequence() takes its
         * argument (transfer none) and refs it, so passing one made here
         * leaks this function's reference every time.
         */
        g_autoptr(YamlNode) created = yaml_node_new_sequence(NULL);

        yaml_mapping_set_member(yaml_node_get_mapping(computer), "mounts",
                                 created);
        list = yaml_mapping_get_member(yaml_node_get_mapping(computer),
                                        "mounts");
    }

    if (list == NULL)
        return FALSE;

    mapping = yaml_mapping_new();

    if (clawt_mount_get_source(mount) != NULL) {
        g_autoptr(YamlNode) value =
            yaml_node_new_string(clawt_mount_get_source(mount));

        yaml_mapping_set_member(mapping, "source", value);
    }

    {
        g_autoptr(YamlNode) value =
            yaml_node_new_string(clawt_mount_get_target(mount));

        yaml_mapping_set_member(mapping, "target", value);
    }

    {
        g_autoptr(YamlNode) value = yaml_node_new_string(
            clawt_enum_to_nick(CLAWT_TYPE_MOUNT_MODE,
                               clawt_mount_get_mode(mount)));

        yaml_mapping_set_member(mapping, "mode", value);
    }

    if (clawt_mount_get_mount_type(mount) != CLAWT_MOUNT_BIND) {
        g_autoptr(YamlNode) value = yaml_node_new_string(
            clawt_enum_to_nick(CLAWT_TYPE_MOUNT_TYPE,
                               clawt_mount_get_mount_type(mount)));

        yaml_mapping_set_member(mapping, "type", value);
    }

    {
        /*
         * Always written, including "none".  A reader should be able to
         * see what will happen to the labels without knowing which way
         * an absent key falls.
         */
        g_autoptr(YamlNode) value = yaml_node_new_string(
            clawt_enum_to_nick(CLAWT_TYPE_RELABEL,
                               clawt_mount_get_relabel(mount)));

        yaml_mapping_set_member(mapping, "relabel", value);
    }

    if (clawt_mount_get_size(mount) != NULL) {
        g_autoptr(YamlNode) value =
            yaml_node_new_string(clawt_mount_get_size(mount));

        yaml_mapping_set_member(mapping, "size", value);
    }

    element = yaml_node_new_mapping(mapping);
    yaml_sequence_add_element(yaml_node_get_sequence(list), element);

    return TRUE;
}

/*
 * Removes the mount with this target.
 *
 * Keyed on the target rather than the source, because the target is
 * what has to be unique -- two sources cannot occupy one path inside
 * the computer, and validation already refuses that.
 */
gboolean
clawt_agent_config_remove_mount(ClawtAgentConfig *self,
                                const gchar      *target)
{
    YamlNode *list;
    YamlSequence *sequence;
    guint i;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(target != NULL, FALSE);

    list = node_at_path(self->node, "computer.mounts", FALSE);

    if (list == NULL || yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE)
        return FALSE;

    sequence = yaml_node_get_sequence(list);

    for (i = 0; i < yaml_sequence_get_length(sequence); i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);

        if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
            continue;

        if (g_strcmp0(member_string(yaml_node_get_mapping(element), "target"),
                      target) == 0) {
            /* void upstream: it either removes or warns. */
            yaml_sequence_remove_element(sequence, i);
            return TRUE;
        }
    }

    return FALSE;
}

gboolean
clawt_agent_config_set_string(ClawtAgentConfig *self,
                              const gchar      *key,
                              const gchar      *value)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);

    node = (value != NULL) ? yaml_node_new_string(value)
                           : yaml_node_new_null();
    schema_key = g_strdup_printf("agents.%s", key);

    return set_scalar(self->node, key, node, schema_key);
}

gboolean
clawt_agent_config_set_boolean(ClawtAgentConfig *self,
                               const gchar      *key,
                               gboolean          value)
{
    g_autoptr(YamlNode) node = yaml_node_new_boolean(value);
    g_autofree gchar *schema_key = g_strdup_printf("agents.%s", key);

    g_return_val_if_fail(self != NULL, FALSE);

    /*
     * Booleans and numbers are written plain so they come back as booleans
     * and numbers.  yaml-glib preserves scalar style, so a quoted "true"
     * would round-trip as the string "true" -- which reads identically and
     * behaves differently.
     */
    yaml_node_set_scalar_style(node, YAML_SCALAR_STYLE_PLAIN);

    return set_scalar(self->node, key, node, schema_key);
}

gboolean
clawt_agent_config_set_int(ClawtAgentConfig *self,
                           const gchar      *key,
                           gint64            value)
{
    g_autoptr(YamlNode) node = yaml_node_new_int(value);
    g_autofree gchar *schema_key = g_strdup_printf("agents.%s", key);

    g_return_val_if_fail(self != NULL, FALSE);

    yaml_node_set_scalar_style(node, YAML_SCALAR_STYLE_PLAIN);

    return set_scalar(self->node, key, node, schema_key);
}

gchar *
clawt_agent_config_get_workspace(ClawtAgentConfig *self)
{
    const gchar *configured;
    g_autofree gchar *root = NULL;

    g_return_val_if_fail(self != NULL, NULL);

    configured = lookup_string(self->node, "workspace");
    if (configured != NULL)
        return clawt_expand_path(configured);

    root = clawt_config_get_path_value(self->config, "defaults.workspace_root");
    if (root == NULL)
        return NULL;

    return g_build_filename(root, self->id, NULL);
}

GPtrArray *
clawt_agent_config_get_mounts(ClawtAgentConfig *self)
{
    YamlNode *node;
    YamlSequence *sequence;
    GPtrArray *out;
    guint i;
    guint length;

    g_return_val_if_fail(self != NULL, NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_mount_free);

    node = node_at_path(self->node, "computer.mounts", FALSE);
    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE)
        return out;

    sequence = yaml_node_get_sequence(node);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        YamlMapping *mapping;
        ClawtMount *mount;
        const gchar *source;
        const gchar *target;
        const gchar *nick;
        gint value = 0;

        if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
            continue;

        mapping = yaml_node_get_mapping(element);
        source = member_string(mapping, "source");
        target = member_string(mapping, "target");

        if (target == NULL)
            continue;

        mount = clawt_mount_new(source, target);

        nick = member_string(mapping, "mode");
        if (nick != NULL &&
            clawt_enum_from_nick(CLAWT_TYPE_MOUNT_MODE, nick, &value))
            clawt_mount_set_mode(mount, (ClawtMountMode)value);

        nick = member_string(mapping, "type");
        if (nick != NULL &&
            clawt_enum_from_nick(CLAWT_TYPE_MOUNT_TYPE, nick, &value))
            clawt_mount_set_mount_type(mount, (ClawtMountType)value);

        /*
         * Absent means shared, not none.
         *
         * A schema default only applies to a scalar at a dotted path;
         * nothing applies one to a member of a list, so an entry
         * written without `relabel` came back as none -- and on an
         * SELinux system an unlabelled bind mount is visible inside the
         * container with every access denied. Every shared folder
         * anyone declared failed with permission denied, which reads
         * like a permissions bug rather than a labelling one.
         */
        nick = member_string(mapping, "relabel");

        if (nick == NULL)
            clawt_mount_set_relabel(mount, CLAWT_RELABEL_SHARED);
        else if (clawt_enum_from_nick(CLAWT_TYPE_RELABEL, nick, &value))
            clawt_mount_set_relabel(mount, (ClawtRelabel)value);

        clawt_mount_set_size(mount, member_string(mapping, "size"));

        clawt_mount_set_create(
            mount, string_to_boolean(member_string(mapping, "create"), FALSE));
        clawt_mount_set_required(
            mount, string_to_boolean(member_string(mapping, "required"), TRUE));

        g_ptr_array_add(out, mount);
    }

    return out;
}

static GHashTable *
mapping_to_hash(YamlNode *node)
{
    GHashTable *out = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, g_free);
    YamlMapping *mapping;
    GList *members;
    GList *l;

    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
        return out;

    mapping = yaml_node_get_mapping(node);
    members = yaml_mapping_get_members(mapping);

    for (l = members; l != NULL; l = l->next) {
        const gchar *key = l->data;
        const gchar *value = member_string(mapping, key);

        if (value != NULL)
            g_hash_table_insert(out, g_strdup(key), g_strdup(value));
    }

    g_list_free(members);
    return out;
}

GHashTable *
clawt_agent_config_get_env(ClawtAgentConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return mapping_to_hash(node_at_path(self->node, "env", FALSE));
}

void
clawt_room_spec_free(ClawtRoomSpec *self)
{
    if (self == NULL)
        return;

    g_free(self->id);
    g_free(self->name);
    g_strfreev(self->members);
    g_free(self);
}

GPtrArray *
clawt_config_get_rooms(ClawtConfig *self)
{
    GPtrArray *out;
    YamlNode *rooms;
    YamlSequence *sequence;
    guint i;
    guint length;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_room_spec_free);

    rooms = node_at_path(self->root, "rooms", FALSE);
    if (rooms == NULL || yaml_node_get_node_type(rooms) != YAML_NODE_SEQUENCE)
        return out;

    sequence = yaml_node_get_sequence(rooms);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *entry = yaml_sequence_get_element(sequence, i);
        ClawtRoomSpec *spec;
        const gchar *id;

        if (entry == NULL ||
            yaml_node_get_node_type(entry) != YAML_NODE_MAPPING)
            continue;

        id = member_string(yaml_node_get_mapping(entry), "id");
        if (id == NULL) {
            /*
             * A room with no id cannot be referred to or joined, so it is
             * skipped with a warning rather than given a generated one --
             * a room nobody can name is not what the author meant.
             */
            g_warning("rooms[%u]: no id, so this room is ignored", i);
            continue;
        }

        spec = g_new0(ClawtRoomSpec, 1);
        spec->id = g_strdup(id);
        spec->name = g_strdup(member_string(yaml_node_get_mapping(entry),
                                            "name"));
        spec->members = node_to_strv(node_at_path(entry, "members", FALSE));

        {
            YamlNode *node = node_at_path(entry, "require_mention", FALSE);

            /*
             * Defaults to the schema default rather than to FALSE, so a
             * room that says nothing behaves the same as the documented
             * default instead of quietly being the opposite.
             */
            spec->require_mention = (node != NULL)
                ? yaml_node_get_boolean(node)
                : g_strcmp0(schema_default_for("rooms.require_mention"),
                            "true") == 0;
        }

        {
            YamlNode *node = node_at_path(entry, "max_hops", FALSE);

            spec->max_hops = (node != NULL)
                ? (guint)yaml_node_get_int(node) : 0;
        }

        g_ptr_array_add(out, spec);
    }

    return out;
}

GHashTable *
clawt_agent_config_resolve_credentials(ClawtAgentConfig  *self,
                                       const gchar       *secrets_dir,
                                       guint              timeout_seconds,
                                       GError           **error)
{
    g_autoptr(GHashTable) credentials = NULL;
    GHashTable *out;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_return_val_if_fail(self != NULL, NULL);

    credentials = clawt_agent_config_get_credentials(self);
    out = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    g_hash_table_iter_init(&iter, credentials);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_autofree gchar *resolved = NULL;
        g_autoptr(GError) local = NULL;

        resolved = clawt_secret_ref_resolve(value, secrets_dir,
                                            timeout_seconds, &local);

        if (resolved == NULL) {
            g_autofree gchar *described = clawt_secret_ref_describe(value);

            /*
             * Named by reference, never by value -- this message reaches
             * logs and IPC replies.
             */
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                        "credentials.%s: could not resolve %s: %s",
                        (const gchar *)key, described,
                        local != NULL ? local->message : "unknown reason");
            g_hash_table_unref(out);
            return NULL;
        }

        g_hash_table_insert(out, g_ascii_strup((const gchar *)key, -1),
                            g_steal_pointer(&resolved));
    }

    return out;
}

ClawtSecretRef *
clawt_agent_config_get_secret(ClawtAgentConfig *self, const gchar *key)
{
    YamlNode *node;
    ClawtSecretBackend default_backend;
    g_autoptr(GError) error = NULL;
    ClawtSecretRef *ref;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    node = node_at_path(self->node, key, FALSE);
    if (node == NULL)
        return NULL;

    default_backend = (ClawtSecretBackend)
        clawt_config_get_enum(self->config, "secrets.default_backend");

    ref = clawt_secret_ref_parse(node, default_backend, &error);
    if (ref == NULL && error != NULL) {
        /*
         * A malformed reference is a warning, not a hard failure: it
         * disables one credential rather than the agent, and the message
         * says which key so it can be fixed without bisecting the file.
         */
        g_warning("agent %s: %s: %s", self->id, key, error->message);
        return NULL;
    }

    return ref;
}

gchar *
clawt_agent_config_get_raw_yaml(ClawtAgentConfig *self, const gchar *key)
{
    g_autoptr(YamlGenerator) generator = NULL;
    YamlNode *node;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    node = node_at_path(self->node, key, FALSE);
    if (node == NULL)
        return NULL;

    generator = yaml_generator_new();
    yaml_generator_set_root(generator, node);

    return yaml_generator_to_data(generator, NULL, NULL);
}

GHashTable *
clawt_agent_config_get_credentials(ClawtAgentConfig *self)
{
    GHashTable *out;
    YamlNode *node;
    YamlMapping *mapping;
    GList *members;
    GList *l;
    ClawtSecretBackend default_backend;

    g_return_val_if_fail(self != NULL, NULL);

    out = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                (GDestroyNotify)clawt_secret_ref_free);

    node = node_at_path(self->node, "credentials", FALSE);
    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
        return out;

    default_backend = (ClawtSecretBackend)
        clawt_config_get_enum(self->config, "secrets.default_backend");

    mapping = yaml_node_get_mapping(node);
    members = yaml_mapping_get_members(mapping);

    for (l = members; l != NULL; l = l->next) {
        const gchar *key = l->data;
        g_autoptr(GError) error = NULL;
        ClawtSecretRef *ref;

        ref = clawt_secret_ref_parse(yaml_mapping_get_member(mapping, key),
                                     default_backend, &error);
        if (ref == NULL) {
            /*
             * A bad reference disables the agent rather than being skipped:
             * an agent silently missing the credential it needs fails later,
             * in a way that looks like the service being down.
             */
            agent_mark_shadow(self, "credential '%s': %s", key,
                              error->message);
            continue;
        }

        g_hash_table_insert(out, g_strdup(key), ref);
    }

    g_list_free(members);
    return out;
}

/* ── ClawtConfig ─────────────────────────────────────────────────── */

static void
clawt_config_finalize(GObject *object)
{
    ClawtConfig *self = CLAWT_CONFIG(object);

    g_clear_pointer(&self->path, g_free);
    g_clear_pointer(&self->root, yaml_node_unref);
    g_clear_pointer(&self->agents, g_ptr_array_unref);
    g_clear_pointer(&self->warnings, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_config_parent_class)->finalize(object);
}

static void
clawt_config_class_init(ClawtConfigClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = clawt_config_finalize;
}

static void
clawt_config_init(ClawtConfig *self)
{
    self->root = yaml_node_new_mapping(NULL);
    self->agents = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_agent_config_unref);
    self->warnings = g_ptr_array_new_with_free_func(g_free);
}

ClawtConfig *
clawt_config_new(void)
{
    return g_object_new(CLAWT_TYPE_CONFIG, NULL);
}

/*
 * Rebuilds the agent list from the YAML tree.
 *
 * Called after every load and after any structural change, so the list and
 * the tree cannot disagree -- an agent removed from one but not the other
 * would be a bug that only shows up on the next save.
 */
static void
reload_agents(ClawtConfig *self)
{
    YamlNode *node;
    YamlSequence *sequence;
    guint i;
    guint length;
    g_autoptr(GHashTable) seen = NULL;
    gboolean have_chief = FALSE;

    g_ptr_array_set_size(self->agents, 0);

    node = node_at_path(self->root, "agents", FALSE);
    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE)
        return;

    seen = g_hash_table_new(g_str_hash, g_str_equal);
    sequence = yaml_node_get_sequence(node);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        ClawtAgentConfig *agent;
        const gchar *id;

        if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING) {
            g_ptr_array_add(self->warnings,
                g_strdup_printf("agents[%u] is not a mapping; ignored", i));
            continue;
        }

        id = member_string(yaml_node_get_mapping(element), "id");

        if (id == NULL) {
            g_ptr_array_add(self->warnings,
                g_strdup_printf("agents[%u] has no id; ignored", i));
            continue;
        }

        agent = clawt_agent_config_new(self, id, element);

        if (g_hash_table_contains(seen, id)) {
            agent_mark_shadow(agent,
                              "another agent already uses the id '%s'", id);
        } else {
            g_hash_table_add(seen, (gpointer)id);
            agent_validate(agent);
        }

        /*
         * At most one chief-of-staff.  Two would mean work addressed to the
         * fleet going to whichever happened to be found first, which is a
         * coin toss dressed up as a routing rule.
         */
        if (clawt_agent_config_get_boolean(agent, "chief_of_staff")) {
            if (have_chief)
                agent_mark_shadow(agent,
                                  "another agent is already the "
                                  "chief-of-staff; only one may be");
            else
                have_chief = TRUE;
        }

        if (clawt_agent_config_is_shadow(agent))
            g_ptr_array_add(self->warnings,
                g_strdup_printf("agent '%s' disabled: %s", id,
                                clawt_agent_config_get_shadow_reason(agent)));

        g_ptr_array_add(self->agents, agent);
    }
}

/*
 * Walks the loaded tree and warns about keys the schema does not know.
 *
 * A warning rather than an error: a typo should be visible without being
 * fatal, and a key from a newer version should not stop an older daemon.
 * Agent blocks are skipped here -- they have their own shadow mechanism,
 * and the free-form env, credentials and libreclaw sections legitimately
 * contain keys no schema can enumerate.
 */
static void
warn_unknown_keys(ClawtConfig *self,
                  YamlNode    *node,
                  const gchar *prefix)
{
    YamlMapping *mapping;
    GList *members;
    GList *l;

    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
        return;

    mapping = yaml_node_get_mapping(node);
    members = yaml_mapping_get_members(mapping);

    for (l = members; l != NULL; l = l->next) {
        const gchar *key = l->data;
        g_autofree gchar *full = (prefix != NULL)
                                 ? g_strdup_printf("%s.%s", prefix, key)
                                 : g_strdup(key);
        const ClawtSchemaEntry *entry;

        if (g_strcmp0(full, "agents") == 0 || g_strcmp0(full, "rooms") == 0)
            continue;

        entry = clawt_config_schema_lookup(full);

        if (entry == NULL) {
            g_ptr_array_add(self->warnings,
                g_strdup_printf("unknown configuration key '%s'", full));
            continue;
        }

        if (entry->type == CLAWT_SCHEMA_SECTION)
            warn_unknown_keys(self, yaml_mapping_get_member(mapping, key),
                              full);
    }

    g_list_free(members);
}

static ClawtConfig *
config_from_parser(YamlParser *parser, const gchar *path, GError **error)
{
    ClawtConfig *self;
    YamlNode *root;

    root = yaml_parser_get_root(parser);

    if (root != NULL &&
        yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "the configuration must be a mapping at the top "
                            "level");
        return NULL;
    }

    self = clawt_config_new();
    self->path = g_strdup(path);

    /*
     * An empty or comment-only file is legitimate: a daemon on schema
     * defaults is a working daemon, and demanding content before the first
     * run would make it a chicken-and-egg problem.
     */
    if (root != NULL) {
        g_clear_pointer(&self->root, yaml_node_unref);
        self->root = yaml_node_ref(root);
    }

    warn_unknown_keys(self, self->root, NULL);
    reload_agents(self);

    return self;
}

ClawtConfig *
clawt_config_load(const gchar *path, GError **error)
{
    g_autoptr(YamlParser) parser = NULL;
    g_autofree gchar *resolved = NULL;

    if (path != NULL)
        resolved = clawt_expand_path(path);
    else
        resolved = g_build_filename(g_get_home_dir(), ".clawtilla",
                                    "config.yaml", NULL);

    if (!g_file_test(resolved, G_FILE_TEST_EXISTS)) {
        ClawtConfig *self = clawt_config_new();

        self->path = g_steal_pointer(&resolved);
        return self;
    }

    parser = yaml_parser_new();
    yaml_parser_set_capture_comments(parser, TRUE);

    if (!yaml_parser_load_from_file(parser, resolved, error)) {
        g_prefix_error(error, "%s: ", resolved);
        return NULL;
    }

    return config_from_parser(parser, resolved, error);
}

ClawtConfig *
clawt_config_load_from_string(const gchar *yaml, GError **error)
{
    g_autoptr(YamlParser) parser = NULL;

    g_return_val_if_fail(yaml != NULL, NULL);

    parser = yaml_parser_new();
    yaml_parser_set_capture_comments(parser, TRUE);

    if (!yaml_parser_load_from_data(parser, yaml, -1, error))
        return NULL;

    return config_from_parser(parser, NULL, error);
}

gchar *
clawt_config_to_string(ClawtConfig *self)
{
    g_autoptr(YamlGenerator) generator = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    generator = yaml_generator_new();
    yaml_generator_set_emit_comments(generator, TRUE);
    yaml_generator_set_root(generator, self->root);

    return yaml_generator_to_data(generator, NULL, NULL);
}

gboolean
clawt_config_save(ClawtConfig *self, GError **error)
{
    g_autofree gchar *text = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);

    if (self->path == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_WRITE,
                            "this configuration was not loaded from a file");
        return FALSE;
    }

    text = clawt_config_to_string(self);
    if (text == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_WRITE,
                            "the configuration could not be rendered");
        return FALSE;
    }

    /*
     * 0600, because credentials-by-reference still name files and
     * environment variables that say a good deal about the setup, and a
     * config nobody else can read costs nothing.
     */
    return clawt_write_file_atomic(self->path, text, -1, 0600, TRUE, error);
}

const gchar *
clawt_config_get_path(ClawtConfig *self)
{
    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);
    return self->path;
}

GPtrArray *
clawt_config_get_warnings(ClawtConfig *self)
{
    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);
    return self->warnings;
}

gboolean
clawt_config_validate(ClawtConfig *self, GError **error)
{
    const gchar *socket_path;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);

    socket_path = clawt_config_get_string(self, "daemon.socket");
    if (socket_path == NULL || *socket_path == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "daemon.socket must not be empty");
        return FALSE;
    }

    if (clawt_config_get_int(self, "orchestration.max_hops") < 1) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "orchestration.max_hops must be at least 1; "
                            "zero would stop agents talking to each other "
                            "at all");
        return FALSE;
    }

    return TRUE;
}

const gchar *
clawt_config_get_string(ClawtConfig *self, const gchar *key)
{
    const gchar *value;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);
    g_return_val_if_fail(key != NULL, NULL);

    value = lookup_string(self->root, key);

    return (value != NULL) ? value : schema_default_for(key);
}

gchar *
clawt_config_get_path_value(ClawtConfig *self, const gchar *key)
{
    return clawt_expand_path(clawt_config_get_string(self, key));
}

gboolean
clawt_config_get_boolean(ClawtConfig *self, const gchar *key)
{
    return string_to_boolean(clawt_config_get_string(self, key), FALSE);
}

gint64
clawt_config_get_int(ClawtConfig *self, const gchar *key)
{
    const gchar *value = clawt_config_get_string(self, key);

    return (value != NULL) ? g_ascii_strtoll(value, NULL, 10) : 0;
}

gdouble
clawt_config_get_double(ClawtConfig *self, const gchar *key)
{
    const gchar *value = clawt_config_get_string(self, key);

    return (value != NULL) ? g_ascii_strtod(value, NULL) : 0.0;
}

gint
clawt_config_get_enum(ClawtConfig *self, const gchar *key)
{
    const ClawtSchemaEntry *entry;
    const gchar *nick;
    gint value = 0;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), 0);

    entry = clawt_config_schema_lookup(key);
    if (entry == NULL || entry->enum_type == NULL)
        return 0;

    nick = clawt_config_get_string(self, key);
    if (nick != NULL && clawt_enum_from_nick(entry->enum_type(), nick, &value))
        return value;

    return 0;
}

GStrv
clawt_config_get_string_list(ClawtConfig *self, const gchar *key)
{
    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    return node_to_strv(node_at_path(self->root, key, FALSE));
}

gboolean
clawt_config_has_key(ClawtConfig *self, const gchar *key)
{
    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);

    return node_at_path(self->root, key, FALSE) != NULL;
}

gboolean
clawt_config_set_string(ClawtConfig *self, const gchar *key,
                        const gchar *value)
{
    g_autoptr(YamlNode) node = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);

    node = (value != NULL) ? yaml_node_new_string(value)
                           : yaml_node_new_null();

    return set_scalar(self->root, key, node, key);
}

gboolean
clawt_config_set_boolean(ClawtConfig *self, const gchar *key, gboolean value)
{
    g_autoptr(YamlNode) node = yaml_node_new_boolean(value);

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);

    yaml_node_set_scalar_style(node, YAML_SCALAR_STYLE_PLAIN);

    return set_scalar(self->root, key, node, key);
}

gboolean
clawt_config_set_int(ClawtConfig *self, const gchar *key, gint64 value)
{
    g_autoptr(YamlNode) node = yaml_node_new_int(value);

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);

    yaml_node_set_scalar_style(node, YAML_SCALAR_STYLE_PLAIN);

    return set_scalar(self->root, key, node, key);
}

gboolean
clawt_config_unset(ClawtConfig *self, const gchar *key)
{
    g_autofree gchar *parent_path = NULL;
    YamlNode *parent;
    const gchar *leaf;
    gchar *last_dot;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    parent_path = g_strdup(key);
    last_dot = strrchr(parent_path, '.');

    if (last_dot != NULL) {
        *last_dot = '\0';
        leaf = last_dot + 1;
        parent = node_at_path(self->root, parent_path, FALSE);
    } else {
        leaf = key;
        parent = self->root;
    }

    if (parent == NULL ||
        yaml_node_get_node_type(parent) != YAML_NODE_MAPPING)
        return FALSE;

    return yaml_mapping_remove_member(yaml_node_get_mapping(parent), leaf);
}

GPtrArray *
clawt_config_get_agents(ClawtConfig *self)
{
    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);
    return self->agents;
}

ClawtAgentConfig *
clawt_config_get_agent(ClawtConfig *self, const gchar *id)
{
    guint i;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);
    g_return_val_if_fail(id != NULL, NULL);

    for (i = 0; i < self->agents->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(self->agents, i);

        if (g_strcmp0(agent->id, id) == 0)
            return agent;
    }

    return NULL;
}

ClawtAgentConfig *
clawt_config_add_agent(ClawtConfig *self, const gchar *id, GError **error)
{
    YamlNode *agents_node;
    g_autoptr(YamlNode) entry = NULL;
    g_autoptr(YamlNode) id_node = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    if (!clawt_is_valid_id(id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable agent id: ids may hold only "
                    "lowercase letters, digits, '-' and '_', and must not "
                    "start with punctuation",
                    id != NULL ? id : "");
        return NULL;
    }

    if (clawt_config_get_agent(self, id) != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "an agent called '%s' already exists", id);
        return NULL;
    }

    agents_node = node_at_path(self->root, "agents", FALSE);

    if (agents_node == NULL ||
        yaml_node_get_node_type(agents_node) != YAML_NODE_SEQUENCE) {
        g_autoptr(YamlNode) fresh = yaml_node_new_sequence(NULL);

        yaml_mapping_set_member(yaml_node_get_mapping(self->root),
                                "agents", fresh);
        agents_node = node_at_path(self->root, "agents", FALSE);
        apply_schema_comment(agents_node, "agents");
    }

    entry = yaml_node_new_mapping(NULL);
    id_node = yaml_node_new_string(id);
    yaml_mapping_set_member(yaml_node_get_mapping(entry), "id", id_node);

    yaml_sequence_add_element(yaml_node_get_sequence(agents_node), entry);

    reload_agents(self);

    return clawt_config_get_agent(self, id);
}

gboolean
clawt_config_remove_agent(ClawtConfig *self, const gchar *id)
{
    YamlNode *agents_node;
    YamlSequence *sequence;
    guint i;
    guint length;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    agents_node = node_at_path(self->root, "agents", FALSE);
    if (agents_node == NULL ||
        yaml_node_get_node_type(agents_node) != YAML_NODE_SEQUENCE)
        return FALSE;

    sequence = yaml_node_get_sequence(agents_node);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        const gchar *element_id;

        if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
            continue;

        element_id = member_string(yaml_node_get_mapping(element), "id");

        if (g_strcmp0(element_id, id) != 0)
            continue;

        yaml_sequence_remove_element(sequence, i);
        reload_agents(self);
        return TRUE;
    }

    return FALSE;
}
