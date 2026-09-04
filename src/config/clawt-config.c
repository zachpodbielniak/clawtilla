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
    GPtrArray   *integrations; /* ClawtIntegrationConfig* */
    GPtrArray   *routines;    /* ClawtRoutine* */
    GPtrArray   *triggers;    /* ClawtTrigger* */
    GPtrArray   *warnings;    /* gchar* */
};

G_DEFINE_FINAL_TYPE(ClawtConfig, clawt_config, G_TYPE_OBJECT)

/*
 * Defined with the rest of the integration code at the end of the file;
 * declared here because loading has to rebuild both lists, and putting
 * the whole of that section above the loader would bury it.
 */
static void reload_integrations(ClawtConfig *self);
static void reload_routines(ClawtConfig *self);
static void reload_triggers(ClawtConfig *self);

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
     * A well-formed id can still be a name the routing keys on.  An
     * agent called "clawtilla" would sign its messages as the system --
     * every one passing the loop guard unmeasured and closing the
     * exchange it lands in -- and one called "user" would read as the
     * operator.  Shadowed, not fatal: the other nine agents still start.
     */
    if (clawt_agent_id_is_reserved(self->id)) {
        agent_mark_shadow(self,
                          "agent id '%s' is reserved: it is a sender name "
                          "clawtilla's own routing keys on. Pick another.",
                          self->id);
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
        /*
         * The list comes from the enum rather than from a sentence here.
         * It used to be written out, and had read "none, host, container
         * and vm" ever since distrobox was added -- so the one message
         * whose whole job is to say what may be written left out a type
         * this build supports, and did it with complete confidence.
         */
        g_autofree gchar *known =
            clawt_enum_nick_list(CLAWT_TYPE_COMPUTER_TYPE);

        agent_mark_shadow(self,
                          "unknown computer type '%s'; this build knows %s",
                          computer_type, known);
        return;
    }

    confine = clawt_agent_config_get_string(self, "computer.host.confine");
    if (confine != NULL &&
        !clawt_enum_from_nick(CLAWT_TYPE_CONFINE_MODE, confine, &value)) {
        g_autofree gchar *known =
            clawt_enum_nick_list(CLAWT_TYPE_CONFINE_MODE);

        agent_mark_shadow(self,
                          "unknown confinement mode '%s'; this build knows "
                          "%s", confine, known);
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

    /*
     * And the fleet's half of the same decision.  The schema has said
     * since daemon.allow_unconfined_host was written that `confine: none`
     * "requires this AND confirm_host_control on the agent itself", and
     * only the agent's half was ever checked -- so the key existed, was
     * documented, defaulted to false, and gated nothing.  One line in one
     * agent block handed over the whole machine, which is precisely the
     * single typo the two-act rule exists to stop.
     */
    if (g_strcmp0(computer_type, "host") == 0 &&
        clawt_agent_config_get_enum(self, "computer.host.confine") ==
            CLAWT_CONFINE_NONE &&
        (self->config == NULL ||
         !clawt_config_get_boolean(self->config,
                                   "daemon.allow_unconfined_host"))) {
        agent_mark_shadow(self,
                          "computer.host.confine: none needs "
                          "daemon.allow_unconfined_host: true as well -- "
                          "running unconfined on your real machine takes "
                          "two deliberate acts, not one. Set it, or pick a "
                          "confinement mode");
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
clawt_agent_config_revalidate(ClawtAgentConfig *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    /*
     * Cleared first, because agent_mark_shadow() keeps the *first* reason
     * and ignores later ones -- so revalidating without this would leave
     * the old refusal in place however the config had changed.
     */
    g_clear_pointer(&self->shadow_reason, g_free);
    agent_validate(self);

    return self->shadow_reason == NULL;
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

/*
 * The schema entry describing an agent-relative key.
 *
 * Most are `agents.<key>`. The ones that can also be set fleet-wide are
 * not: `mailbox.overflow` inside an agent is the schema's
 * `orchestration.mailbox.overflow`, and there is no `agents.mailbox.*`
 * row at all.
 *
 * Looking only under `agents.` therefore found nothing for those, and
 * each getter did something different and wrong with that -- get_enum
 * returned 0, which for the overflow policy is `reject`, whatever the
 * fleet had chosen. One resolver now, used by all of them.
 */
static const ClawtSchemaEntry *
agent_schema_entry(const gchar *key)
{
    g_autofree gchar *schema_key = NULL;
    const ClawtSchemaEntry *entry;
    const gchar *fleet_key;

    if (key == NULL)
        return NULL;

    schema_key = g_strdup_printf("agents.%s", key);
    entry = clawt_config_schema_lookup(schema_key);

    if (entry != NULL)
        return entry;

    fleet_key = clawt_config_schema_fleet_key_for(key);

    return (fleet_key != NULL) ? clawt_config_schema_lookup(fleet_key) : NULL;
}

/*
 * The schema default for an agent-relative key, through the same
 * resolver.
 */
static const gchar *
agent_schema_default(const gchar *key)
{
    const ClawtSchemaEntry *entry = agent_schema_entry(key);

    return (entry != NULL) ? entry->default_value : NULL;
}

/*
 * The one key whose default depends on who the agent is.
 *
 * An orchestrator's job is carrying one operator's intent across
 * conversations, and the partitioned default is the wrong shape for
 * that job: the chief of staff this surfaced on was three context
 * windows that had never met, and a team lead is the same role one
 * level down.  So marking an agent chief_of_staff -- on the agent or
 * through orchestration.chief_of_staff -- or giving it team_role:
 * lead, flips this key's *default* to agent mode.  A default rather
 * than a write, deliberately: an explicit session.routing_mode always
 * wins, dropping the role restores the ordinary default, and nothing
 * mutates a second key behind the operator's back when a role is
 * toggled.
 *
 * The nick comes off libreclaw's own enum rather than a literal here,
 * because a literal that drifted from the enum would render a value
 * libreclaw warns about and silently reverts -- to precisely the
 * partitioning the role was supposed to leave.  The relationship
 * between the keys is stated in the schema's doc for
 * agents.session.routing_mode; this is the code for it, in the one
 * resolver every reader of the key goes through.
 */
/*
 * Whether @agent_id is in a room with more than two members.
 *
 * Which decides how many sessions the agent may be partitioned into,
 * and that is a correctness question rather than a preference.  A group
 * room is the first room where more than one *other* party speaks, so
 * under `sender-room` an agent has a session per counterparty in the
 * same room -- and every piece of clawtilla's per-room turn state is
 * keyed on the room alone, because until now a room implied one
 * session.  The typing indicator carries the room and not the session,
 * so the daemon cannot even tell those turns apart.
 */
gboolean
clawt_config_agent_is_in_a_group_room(ClawtConfig *config,
                                      const gchar *agent_id)
{
    g_autoptr(GPtrArray) rooms = NULL;
    guint i;

    if (config == NULL || agent_id == NULL)
        return FALSE;

    rooms = clawt_config_get_rooms(config);

    for (i = 0; rooms != NULL && i < rooms->len; i++) {
        ClawtRoomSpec *spec = g_ptr_array_index(rooms, i);
        gsize members = 0;
        gboolean here = FALSE;
        gsize j;

        for (j = 0; spec->members != NULL && spec->members[j] != NULL; j++) {
            members++;

            if (g_strcmp0(spec->members[j], agent_id) == 0)
                here = TRUE;
        }

        if (here && members > 2)
            return TRUE;
    }

    return FALSE;
}

static const gchar *
routing_mode_role_default(ClawtAgentConfig *self)
{
    GEnumClass *cls;
    const gchar *nick;
    gboolean orchestrator;

    orchestrator =
        clawt_agent_config_get_boolean(self, "chief_of_staff") ||
        (ClawtTeamRole)clawt_agent_config_get_enum(self, "team_role") ==
            CLAWT_TEAM_LEAD;

    /* The fleet-level spelling of the same role. */
    if (!orchestrator && self->config != NULL) {
        const gchar *named = clawt_config_get_string(
            self->config, "orchestration.chief_of_staff");

        orchestrator = named != NULL &&
            g_strcmp0(named, lookup_string(self->node, "id")) == 0;
    }

    /*
     * A group room takes the middle answer.
     *
     * `room` mode is one session per room, which is what every piece of
     * per-room turn state here already assumes -- and it leaves direct
     * conversations partitioned, since clawtilla always supplies a room
     * and `dm:<a>:<b>` is unique per pair.  An orchestrator still wants
     * `agent`, which subsumes it, so the stronger answer wins.
     */
    if (!orchestrator &&
        !clawt_config_agent_is_in_a_group_room(self->config,
                                               lookup_string(self->node,
                                                             "id")))
        return NULL;

    cls = g_type_class_ref(LC_TYPE_ROUTING_MODE);
    nick = g_enum_get_value(cls,
                            orchestrator ? LC_ROUTING_MODE_AGENT
                                         : LC_ROUTING_MODE_ROOM)->value_nick;
    g_type_class_unref(cls);

    /* The GEnumValue table is static storage; the nick outlives the ref. */
    return nick;
}

const gchar *
clawt_agent_config_get_string(ClawtAgentConfig *self, const gchar *key)
{
    const gchar *fleet_key;
    const gchar *value;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    value = lookup_string(self->node, key);
    if (value != NULL)
        return value;

    /*
     * Which fleet key this falls back to comes from the schema, not from
     * a list here. There were two such lists -- this one and the mailbox
     * one in clawt-agent-manager.c -- and between them they were the only
     * thing that knew an agent's spelling for a fleet option, which is
     * why the daemon could not report those options to a client at all.
     */
    fleet_key = clawt_config_schema_fleet_key_for(key);

    if (fleet_key != NULL) {
        value = clawt_config_get_string(self->config, fleet_key);

        if (value != NULL)
            return value;
    }

    /* Role-dependent default -- see routing_mode_role_default(). */
    if (g_strcmp0(key, "session.routing_mode") == 0) {
        value = routing_mode_role_default(self);

        if (value != NULL)
            return value;
    }

    return agent_schema_default(key);
}

ClawtConfig *
clawt_agent_config_get_config(ClawtAgentConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->config;
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
    const ClawtSchemaEntry *entry;
    const gchar *nick;
    gint value = 0;

    g_return_val_if_fail(self != NULL, 0);

    entry = agent_schema_entry(key);

    if (entry == NULL || entry->enum_type == NULL)
        return 0;

    nick = clawt_agent_config_get_string(self, key);
    if (nick != NULL && clawt_enum_from_nick(entry->enum_type(), nick, &value))
        return value;

    /*
     * The agents.* row's own default first, then the fleet key's.
     *
     * An inherited option -- computer.type, runtime.restart -- has no
     * default on the agents.* row, because the answer lives in
     * `defaults:`.  So a misspelt value on an agent fell past a NULL
     * and returned 0, which is a real value of each of these types:
     * `computer.type: cotainer` read as `none`, and the agent came up
     * with no computer rather than with the fleet's.
     */
    if (entry->default_value != NULL &&
        clawt_enum_from_nick(entry->enum_type(), entry->default_value, &value))
        return value;

    {
        const gchar *fleet_key =
            clawt_config_schema_fleet_key_for(key);
        const ClawtSchemaEntry *fleet = (fleet_key != NULL)
                                        ? clawt_config_schema_lookup(fleet_key)
                                        : NULL;

        if (fleet != NULL && fleet->default_value != NULL &&
            clawt_enum_from_nick(entry->enum_type(), fleet->default_value,
                                 &value))
            return value;
    }

    return 0;
}

static GStrv
node_to_strv(YamlNode *node)
{
    YamlSequence *sequence;
    GPtrArray *out;
    guint i;
    guint length;

    if (node == NULL)
        return NULL;

    /*
     * A lone scalar is a list of one.
     *
     * `allow_paths: /srv/data` is what a person writes by hand for a key
     * they have only ever seen hold one thing, and refusing it did not
     * fail -- it parsed the value, discarded it, and handed back the
     * schema default. For allow_paths that default is unset, so an
     * operator's confinement was silently replaced by an empty
     * allowlist.
     *
     * Not split on commas. The writer no longer produces a comma-joined
     * scalar for a list key, and a path is allowed to contain a comma --
     * splitting here would make such a path unwritable in exchange for
     * guessing at a spelling nothing emits.
     *
     * An empty scalar stays unset rather than becoming a list holding
     * "". `tools.allow:` with nothing after it is a key somebody started
     * and left, and the difference between an empty allowlist and an
     * absent one is the whole point of the key.
     */
    if (yaml_node_get_node_type(node) == YAML_NODE_SCALAR) {
        const gchar *only = yaml_node_get_string(node);

        if (only == NULL || *only == '\0')
            return NULL;

        out = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(out, g_strdup(only));
        g_ptr_array_add(out, NULL);
        return (GStrv)g_ptr_array_free(out, FALSE);
    }

    if (yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE)
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
    GStrv value;
    const gchar *fallback;

    g_return_val_if_fail(self != NULL, NULL);

    value = node_to_strv(node_at_path(self->node, key, FALSE));

    if (value != NULL)
        return value;

    /*
     * Then the fleet's value, for an option that has one.
     *
     * This step was missing here and present in every other getter, so
     * `memories.readers` set fleet-wide reached no agent at all -- the
     * only list among the options an agent can inherit, and the only
     * getter that did not look.
     */
    {
        const gchar *fleet_key = clawt_config_schema_fleet_key_for(key);

        if (fleet_key != NULL) {
            value = clawt_config_get_string_list(self->config, fleet_key);

            if (value != NULL)
                return value;
        }
    }

    /*
     * A list falls back to the schema's default like every other type.
     *
     * It used not to, and it was the only getter that did not -- so a
     * default declared in the schema appeared in the generated files,
     * was documented, and was never once handed to the code that asked
     * for it. Defaults are comma-separated in the table, which is the
     * same spelling the generator renders from.
     */
    fallback = agent_schema_default(key);

    if (fallback == NULL)
        return NULL;

    return g_strsplit(fallback, ",", -1);
}

gboolean
clawt_agent_config_validate_computer(ClawtAgentConfig *self, GError **error)
{
    g_autofree gchar *image = NULL;

    g_return_val_if_fail(self != NULL, FALSE);

    if (clawt_agent_config_get_enum(self, "computer.type") !=
        CLAWT_COMPUTER_VM)
        return TRUE;

    /*
     * Refused here rather than at the hypervisor, which reports a
     * malformed screen size as a domain that will not define -- an error
     * about XML, a long way from the line somebody typed.
     */
    {
        const gchar *resolution =
            clawt_agent_config_get_string(self, "computer.vm.resolution");

        if (resolution != NULL && *resolution != '\0' &&
            !clawt_vm_computer_parse_resolution(resolution, NULL, NULL)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "computer.vm.resolution is '%s', which is not a "
                        "screen size. Write it as WIDTHxHEIGHT, between "
                        "640x480 and 16384x16384 -- for example 1920x1080.",
                        resolution);
            return FALSE;
        }
    }

    image = clawt_agent_config_get_path_value(self, "computer.vm.image");

    if (image != NULL && *image != '\0')
        return TRUE;

    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "a VM needs a disk image to boot: set "
                        "computer.vm.image. `clawtilla image vm get "
                        "fedora-44` fetches one, or pick one from the Disk "
                        "image row when creating the agent.");

    return FALSE;
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
 * clawt_agent_config_set_string() cannot express a list -- it writes a
 * scalar at a dotted path. Without this the mount list could be read
 * but never written, so declaring a shared folder meant editing the
 * YAML by hand and no client offered it at all.
 *
 * This said "mounts are the only list an agent's configuration holds",
 * which was true when it was written and stopped being true the moment
 * persona.identity_files arrived. Nobody noticed, so every list added
 * after mounts was written as a scalar and read back as the schema
 * default. clawt_agent_config_set_string_list() is the general answer;
 * this stays because a mount is a mapping rather than a string.
 */
/*
 * Appends a mount to a `mounts:` sequence, creating it if absent.
 *
 * @owner_path names the mapping the list hangs off -- "computer" for an
 * agent, "defaults" for the fleet -- so one writer serves both. The
 * alternative was a second copy of thirty lines of yaml-glib, which is
 * where the (transfer none) trap below would have been reintroduced.
 */
static gboolean
add_mount_to_node(YamlNode    *root,
                  const gchar *owner_path,
                  ClawtMount  *mount)
{
    g_autoptr(YamlMapping) mapping = NULL;
    g_autoptr(YamlNode) element = NULL;
    YamlNode *computer;
    YamlNode *list;

    g_return_val_if_fail(mount != NULL, FALSE);
    g_return_val_if_fail(clawt_mount_get_target(mount) != NULL, FALSE);

    computer = node_at_path(root, owner_path, TRUE);

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

    /*
     * Only when they are not what an absent key already means, and
     * always when they are not.
     *
     * Both are read back by mounts_from_node() and both arrive on the
     * wire in clawt_daemon_mount_from_payload(), and neither was
     * written -- so `agent mount add --create --required=false` was
     * validated, saved and reported as added, and came back on the next
     * reload as `create: false, required: true`, which is the opposite
     * of both.  A mount is written here and read there; whatever one
     * end understands, the other has to be able to say.
     */
    if (clawt_mount_get_create(mount)) {
        g_autoptr(YamlNode) value = yaml_node_new_string("true");

        yaml_mapping_set_member(mapping, "create", value);
    }

    if (!clawt_mount_get_required(mount)) {
        g_autoptr(YamlNode) value = yaml_node_new_string("false");

        yaml_mapping_set_member(mapping, "required", value);
    }

    /*
     * Only when it is not ALL. Writing `scope: all` into every entry
     * would be noise in a file people read, and ALL is what an entry
     * without it already means.
     */
    if (clawt_mount_get_scope(mount) != CLAWT_SCOPE_ALL) {
        g_autoptr(YamlNode) value = yaml_node_new_string(
            clawt_enum_to_nick(CLAWT_TYPE_SCOPE,
                               clawt_mount_get_scope(mount)));

        yaml_mapping_set_member(mapping, "scope", value);
    }

    {
        static const struct {
            const gchar         *key;
            const gchar * const *(*get)(ClawtMount *);
        } lists[] = {
            { "agents", clawt_mount_get_agents },
            { "teams", clawt_mount_get_teams }
        };
        gsize l;

        for (l = 0; l < G_N_ELEMENTS(lists); l++) {
            const gchar *const *items = lists[l].get(mount);
            g_autoptr(YamlNode) seq = NULL;
            gsize k;

            if (items == NULL || items[0] == NULL)
                continue;

            seq = yaml_node_new_sequence(NULL);

            for (k = 0; items[k] != NULL; k++) {
                g_autoptr(YamlNode) item = yaml_node_new_string(items[k]);

                yaml_sequence_add_element(yaml_node_get_sequence(seq), item);
            }

            yaml_mapping_set_member(mapping, lists[l].key, seq);
        }
    }

    element = yaml_node_new_mapping(mapping);
    yaml_sequence_add_element(yaml_node_get_sequence(list), element);

    return TRUE;
}

gboolean
clawt_agent_config_add_mount(ClawtAgentConfig *self, ClawtMount *mount)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return add_mount_to_node(self->node, "computer", mount);
}

gboolean
clawt_config_add_default_mount(ClawtConfig *self, ClawtMount *mount)
{
    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);

    return add_mount_to_node(self->root, "defaults", mount);
}

/*
 * Removes the mount with this target.
 *
 * Keyed on the target rather than the source, because the target is
 * what has to be unique -- two sources cannot occupy one path inside
 * the computer, and validation already refuses that.
 */
static gboolean
remove_mount_from_node(YamlNode    *root,
                       const gchar *path,
                       const gchar *target)
{
    YamlNode *list;
    YamlSequence *sequence;
    guint i;

    g_return_val_if_fail(target != NULL, FALSE);

    list = node_at_path(root, path, FALSE);

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

            /*
             * And the key itself once the last one goes.
             *
             * `mounts: []` and no `mounts:` key mean the same thing to
             * every reader here -- mounts_from_node() answers with an
             * empty list either way, and the factory merges the fleet's
             * defaults in regardless -- but they do not read the same
             * to a person, and the file is one people edit.  Somebody
             * removing their only shared folder and finding `mounts:
             * []` where the key used to be read it as "this agent now
             * declares that it has none", spent an evening on a
             * disappearance that had not happened, and hand-edited the
             * file to get back a state the CLI had no way to express.
             *
             * So the client can express it: removing the last mount
             * leaves the key absent, which is what it was before the
             * first one was added.
             */
            if (yaml_sequence_get_length(sequence) == 0) {
                g_autofree gchar *parent_path = NULL;
                const gchar *leaf = strrchr(path, '.');
                YamlNode *parent;

                if (leaf != NULL) {
                    parent_path = g_strndup(path, (gsize)(leaf - path));
                    parent = node_at_path(root, parent_path, FALSE);
                    leaf++;
                } else {
                    parent = root;
                    leaf = path;
                }

                if (parent != NULL &&
                    yaml_node_get_node_type(parent) == YAML_NODE_MAPPING)
                    yaml_mapping_remove_member(yaml_node_get_mapping(parent),
                                                leaf);
            }

            return TRUE;
        }
    }

    return FALSE;
}

gboolean
clawt_agent_config_remove_mount(ClawtAgentConfig *self, const gchar *target)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return remove_mount_from_node(self->node, "computer.mounts", target);
}

gboolean
clawt_config_remove_default_mount(ClawtConfig *self, const gchar *target)
{
    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);

    return remove_mount_from_node(self->root, "defaults.mounts", target);
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

/*
 * Writes a dotted-path key as a YAML sequence.
 *
 * clawt_agent_config_set_string() writes a scalar, and
 * node_to_strv() refuses anything that is not a sequence -- so a list
 * set through it was accepted, saved to the file, and then silently
 * read back as the schema default. `agent set fai
 * persona.identity_files SOUL.md,USER.md` did exactly that: the CLI
 * echoed the value, clawtilla.yaml held it, and the agent loaded the
 * seven .org files it would have loaded anyway.
 *
 * The comment on clawt_agent_config_add_mount() saying mounts are the
 * only list an agent's configuration holds was true when it was
 * written and stopped being true when persona.identity_files was
 * added. A second list is what this exists for.
 */
gboolean
clawt_agent_config_set_string_list(ClawtAgentConfig   *self,
                                   const gchar        *key,
                                   const gchar *const *values)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;
    gsize i;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (values == NULL || values[0] == NULL) {
        node = yaml_node_new_null();
    } else {
        /*
         * NULL, not a fresh sequence: yaml_node_new_sequence() takes
         * its argument (transfer none) and refs it, so one made here
         * would leak this function's reference every call.
         */
        node = yaml_node_new_sequence(NULL);

        for (i = 0; values[i] != NULL; i++)
            yaml_sequence_add_string_element(yaml_node_get_sequence(node),
                                             values[i]);
    }

    schema_key = g_strdup_printf("agents.%s", key);

    return set_scalar(self->node, key, node, schema_key);
}

/*
 * Writes a value given as text, as whatever the schema says the key is.
 *
 * Every caller that takes a setting as a string -- the `agent set` IPC
 * handler, the create_agent tool -- had to decide for itself whether
 * that string was a list, and a caller that did not decide called
 * clawt_agent_config_set_string() and wrote a scalar. node_to_strv()
 * then read the key back as its default, so the value was accepted,
 * echoed to whoever set it, saved to the file, and never used.
 *
 * `agent set` grew the schema check for that reason; create_agent never
 * had it, so `allow_paths` given at creation reached the sandbox as an
 * empty allowlist. There is one right answer, the schema holds it, and
 * this is the one place that asks -- rather than a second copy that can
 * drift from the first.
 */
gboolean
clawt_agent_config_set_from_string(ClawtAgentConfig *self,
                                   const gchar      *key,
                                   const gchar      *value)
{
    g_autofree gchar *schema_key = NULL;
    const ClawtSchemaEntry *entry;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    schema_key = g_strdup_printf("agents.%s", key);
    entry = clawt_config_schema_lookup(schema_key);

    if (entry == NULL || entry->type != CLAWT_SCHEMA_STRING_LIST)
        return clawt_agent_config_set_string(self, key, value);

    /*
     * Comma-separated, which is how the schema table spells a list
     * default and therefore the one spelling a person has already seen.
     * Blanks are trimmed so "a, b" and "a,b" mean the same thing.
     */
    {
        g_auto(GStrv) values = NULL;

        if (value != NULL && *value != '\0') {
            guint i;

            values = g_strsplit(value, ",", -1);

            for (i = 0; values[i] != NULL; i++)
                g_strstrip(values[i]);
        }

        return clawt_agent_config_set_string_list(
            self, key, (const gchar *const *)values);
    }
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

/*
 * Reads a `mounts:` sequence, wherever it lives.
 *
 * One reader for the per-agent list and the fleet defaults, because two
 * would differ exactly once -- and the case they would differ on is the
 * relabel default below, where getting it wrong makes every shared
 * folder unreadable inside the container with an error that says
 * "permission denied" and nothing about labels.
 */
static GPtrArray *
mounts_from_node(YamlNode *root, const gchar *path)
{
    YamlNode *node;
    YamlSequence *sequence;
    GPtrArray *out;
    guint i;
    guint length;

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_mount_free);

    node = node_at_path(root, path, FALSE);
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

        /*
         * Who it is for. Absent is ALL, because a list under `defaults`
         * should mean what the word says -- but a scope that was
         * *written* and cannot be read reaches nobody and says so, which
         * is the same asymmetry integrations already record: a typo that
         * hands somebody's home directory to the whole fleet is far
         * worse than one that hands it to nothing.
         */
        nick = member_string(mapping, "scope");

        if (nick != NULL) {
            if (clawt_enum_from_nick(CLAWT_TYPE_SCOPE, nick, &value)) {
                clawt_mount_set_scope(mount, (ClawtScope)value);
            } else {
                g_warning("shared folder %s: '%s' is not a scope; "
                          "reaching nobody", target, nick);
                clawt_mount_set_scope(mount, CLAWT_SCOPE_NONE);
            }
        }

        {
            g_auto(GStrv) agents =
                node_to_strv(yaml_mapping_get_member(mapping, "agents"));
            g_auto(GStrv) teams =
                node_to_strv(yaml_mapping_get_member(mapping, "teams"));

            clawt_mount_set_agents(mount,
                                   (const gchar *const *)agents);
            clawt_mount_set_teams(mount, (const gchar *const *)teams);

            /*
             * Naming agents or teams without saying `scope` means
             * `selected`. Writing a list and having it ignored because
             * the scope defaulted to ALL would be a rule that reads
             * correctly and does the opposite.
             */
            if (nick == NULL &&
                ((agents != NULL && agents[0] != NULL) ||
                 (teams != NULL && teams[0] != NULL)))
                clawt_mount_set_scope(mount, CLAWT_SCOPE_SELECTED);
        }

        clawt_mount_set_create(
            mount, string_to_boolean(member_string(mapping, "create"), FALSE));
        clawt_mount_set_required(
            mount, string_to_boolean(member_string(mapping, "required"), TRUE));

        g_ptr_array_add(out, mount);
    }

    return out;
}

GPtrArray *
clawt_agent_config_get_mounts(ClawtAgentConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return mounts_from_node(self->node, "computer.mounts");
}

GPtrArray *
clawt_config_get_default_mounts(ClawtConfig *self)
{
    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    return mounts_from_node(self->root, "defaults.mounts");
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

/* ── teams ───────────────────────────────────────────────────────── */

void
clawt_team_spec_free(ClawtTeamSpec *self)
{
    if (self == NULL)
        return;

    g_free(self->id);
    g_free(self->name);
    g_free(self->description);
    g_free(self->color);
    g_free(self);
}

/*
 * Lowest order first, ties keeping the order the file has them in.
 *
 * Stable for the same reason the agent list is: teams nobody has ordered
 * all sit at the default, and a fleet that reshuffled its own sidebar on
 * every listing would be worse than one with no ordering at all.
 */
static gint
compare_teams(gconstpointer a, gconstpointer b)
{
    const ClawtTeamSpec *first = *(ClawtTeamSpec *const *)a;
    const ClawtTeamSpec *second = *(ClawtTeamSpec *const *)b;

    if (first->order == second->order)
        return 0;

    return first->order < second->order ? -1 : 1;
}

GPtrArray *
clawt_config_get_teams(ClawtConfig *self)
{
    GPtrArray *out;
    YamlNode *teams;
    YamlSequence *sequence;
    guint i;
    guint length;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_team_spec_free);

    teams = node_at_path(self->root, "teams", FALSE);
    if (teams == NULL || yaml_node_get_node_type(teams) != YAML_NODE_SEQUENCE)
        return out;

    sequence = yaml_node_get_sequence(teams);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *entry = yaml_sequence_get_element(sequence, i);
        ClawtTeamSpec *spec;
        const gchar *id;

        if (entry == NULL ||
            yaml_node_get_node_type(entry) != YAML_NODE_MAPPING)
            continue;

        id = member_string(yaml_node_get_mapping(entry), "id");

        if (id == NULL) {
            /*
             * A team with no id cannot be joined or addressed, so it is
             * skipped with a warning rather than given a generated one.
             */
            g_warning("teams[%u]: no id, so this team is ignored", i);
            continue;
        }

        spec = g_new0(ClawtTeamSpec, 1);
        spec->id = g_strdup(id);
        spec->name = g_strdup(member_string(yaml_node_get_mapping(entry),
                                            "name"));
        spec->description =
            g_strdup(member_string(yaml_node_get_mapping(entry),
                                   "description"));
        spec->color = g_strdup(member_string(yaml_node_get_mapping(entry),
                                             "color"));

        {
            YamlNode *node = node_at_path(entry, "order", FALSE);

            spec->order = (node != NULL) ? (gint)yaml_node_get_int(node) : 0;
        }

        g_ptr_array_add(out, spec);
    }

    g_ptr_array_sort(out, compare_teams);

    return out;
}

ClawtTeamSpec *
clawt_config_get_team(ClawtConfig *self, const gchar *team_id)
{
    g_autoptr(GPtrArray) teams = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    if (team_id == NULL || *team_id == '\0')
        return NULL;

    teams = clawt_config_get_teams(self);

    for (i = 0; i < teams->len; i++) {
        ClawtTeamSpec *spec = g_ptr_array_index(teams, i);

        if (g_strcmp0(spec->id, team_id) == 0) {
            /* Stolen out of the array rather than copied. */
            g_ptr_array_set_free_func(teams, NULL);
            g_ptr_array_remove_index(teams, i);
            g_ptr_array_set_free_func(
                teams, (GDestroyNotify)clawt_team_spec_free);
            return spec;
        }
    }

    return NULL;
}

gboolean
clawt_config_add_team(ClawtConfig *self, const gchar *team_id, GError **error)
{
    YamlNode *teams_node;
    g_autoptr(YamlNode) entry = NULL;
    g_autoptr(YamlNode) id_node = NULL;
    g_autoptr(ClawtTeamSpec) existing = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);

    if (!clawt_is_valid_id(team_id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable team id: ids may hold only "
                    "lowercase letters, digits, '-' and '_', and must not "
                    "start with punctuation",
                    team_id != NULL ? team_id : "");
        return FALSE;
    }

    existing = clawt_config_get_team(self, team_id);

    if (existing != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "a team called '%s' already exists", team_id);
        return FALSE;
    }

    teams_node = node_at_path(self->root, "teams", FALSE);

    if (teams_node == NULL ||
        yaml_node_get_node_type(teams_node) != YAML_NODE_SEQUENCE) {
        g_autoptr(YamlNode) fresh = yaml_node_new_sequence(NULL);

        yaml_mapping_set_member(yaml_node_get_mapping(self->root),
                                "teams", fresh);
        teams_node = node_at_path(self->root, "teams", FALSE);
        apply_schema_comment(teams_node, "teams");
    }

    entry = yaml_node_new_mapping(NULL);
    id_node = yaml_node_new_string(team_id);
    yaml_mapping_set_member(yaml_node_get_mapping(entry), "id", id_node);

    yaml_sequence_add_element(yaml_node_get_sequence(teams_node), entry);

    return TRUE;
}

/* The team's own mapping node, or NULL. */
static YamlNode *
team_node(ClawtConfig *self, const gchar *team_id)
{
    YamlNode *teams_node = node_at_path(self->root, "teams", FALSE);
    YamlSequence *sequence;
    guint i;
    guint length;

    if (teams_node == NULL ||
        yaml_node_get_node_type(teams_node) != YAML_NODE_SEQUENCE)
        return NULL;

    sequence = yaml_node_get_sequence(teams_node);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);

        if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
            continue;

        if (g_strcmp0(member_string(yaml_node_get_mapping(element), "id"),
                      team_id) == 0)
            return element;
    }

    return NULL;
}

gboolean
clawt_config_set_team_string(ClawtConfig *self,
                             const gchar *team_id,
                             const gchar *key,
                             const gchar *value)
{
    YamlNode *entry;
    g_autoptr(YamlNode) node = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    entry = team_node(self, team_id);

    if (entry == NULL)
        return FALSE;

    /*
     * The id is what everything else refers to the team by, so changing
     * it here would leave every agent naming a team that no longer
     * exists. Renaming is a create, a move and a remove, and it is not
     * this function's to do silently.
     */
    if (g_strcmp0(key, "id") == 0)
        return FALSE;

    if (value == NULL || *value == '\0') {
        yaml_mapping_remove_member(yaml_node_get_mapping(entry), key);
        return TRUE;
    }

    node = yaml_node_new_string(value);
    yaml_mapping_set_member(yaml_node_get_mapping(entry), key, node);

    {
        g_autofree gchar *schema_key = g_strdup_printf("teams.%s", key);

        apply_schema_comment(
            yaml_mapping_get_member(yaml_node_get_mapping(entry), key),
            schema_key);
    }

    return TRUE;
}

gboolean
clawt_config_set_team_string_list(ClawtConfig        *self,
                                  const gchar        *team_id,
                                  const gchar        *key,
                                  const gchar *const *values)
{
    YamlNode *entry;
    g_autoptr(YamlNode) node = NULL;
    gsize i;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    entry = team_node(self, team_id);

    if (entry == NULL)
        return FALSE;

    if (values == NULL || values[0] == NULL) {
        yaml_mapping_remove_member(yaml_node_get_mapping(entry), key);
        return TRUE;
    }

    node = yaml_node_new_sequence(NULL);

    for (i = 0; values[i] != NULL; i++)
        yaml_sequence_add_string_element(yaml_node_get_sequence(node),
                                         values[i]);

    yaml_mapping_set_member(yaml_node_get_mapping(entry), key, node);

    {
        g_autofree gchar *schema_key = g_strdup_printf("teams.%s", key);

        apply_schema_comment(
            yaml_mapping_get_member(yaml_node_get_mapping(entry), key),
            schema_key);
    }

    return TRUE;
}

GStrv
clawt_config_get_team_string_list(ClawtConfig *self,
                                  const gchar *team_id,
                                  const gchar *key)
{
    YamlNode *entry;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);
    g_return_val_if_fail(key != NULL, NULL);

    entry = team_node(self, team_id);

    if (entry == NULL)
        return NULL;

    return node_to_strv(
        yaml_mapping_get_member(yaml_node_get_mapping(entry), key));
}

gboolean
clawt_config_remove_team(ClawtConfig *self, const gchar *team_id)
{
    YamlNode *teams_node;
    YamlSequence *sequence;
    guint i;
    guint length;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);
    g_return_val_if_fail(team_id != NULL, FALSE);

    teams_node = node_at_path(self->root, "teams", FALSE);
    if (teams_node == NULL ||
        yaml_node_get_node_type(teams_node) != YAML_NODE_SEQUENCE)
        return FALSE;

    sequence = yaml_node_get_sequence(teams_node);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);

        if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
            continue;

        if (g_strcmp0(member_string(yaml_node_get_mapping(element), "id"),
                      team_id) == 0) {
            yaml_sequence_remove_element(sequence, i);
            return TRUE;
        }
    }

    return FALSE;
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
             * Whether the room said anything is carried separately.
             *
             * The schema default is `false`, which is right for a room
             * with two members and wrong for a standup: without a
             * mention rule every member takes a turn on every remark,
             * which is expensive and is never what anybody wanted.  So
             * a room that declares nothing lets #ClawtRoom answer from
             * its member count, and this flag is how it knows nobody
             * chose.
             */
            spec->require_mention_set = (node != NULL);
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

        {
            YamlNode *node = node_at_path(entry, "turn_timeout_seconds",
                                          FALSE);
            gint64 seconds;

            /*
             * The schema default when the room says nothing, the same way
             * require_mention above resolves -- a room that declares
             * nothing must behave as the documented default rather than
             * as its opposite, and 0 here means "no bound at all".
             */
            if (node != NULL) {
                seconds = yaml_node_get_int(node);
            } else {
                const gchar *fallback =
                    schema_default_for("rooms.turn_timeout_seconds");

                seconds = (fallback != NULL)
                    ? g_ascii_strtoll(fallback, NULL, 10) : 0;
            }

            /*
             * Floored rather than refused.  A minute is the shortest turn
             * worth bounding -- below it an agent that pauses to think is
             * indistinguishable from one that has stopped -- and a config
             * this daemon refuses to load is a worse answer than one it
             * corrects and says so.
             */
            if (seconds > 0 && seconds < 60) {
                g_warning("rooms[%u]: turn_timeout_seconds of %" G_GINT64_FORMAT
                          " is below the floor of 60; using 60", i, seconds);
                seconds = 60;
            }

            spec->turn_timeout_seconds =
                (seconds > 0) ? (guint)seconds : 0;
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
    g_clear_pointer(&self->integrations, g_ptr_array_unref);
    g_clear_pointer(&self->routines, g_ptr_array_unref);
    g_clear_pointer(&self->triggers, g_ptr_array_unref);
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
    self->integrations = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_integration_config_unref);
    self->routines = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_routine_unref);
    self->triggers = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_trigger_unref);
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
 * Is @key a member of one of the schema's lists of mappings?
 *
 * Those are reached element by element rather than by walking down from
 * the root, so a pass over the top of the file has to leave them alone.
 * The question is asked of the schema rather than by testing for known
 * prefixes, because the version that named "agents." and "rooms."
 * outright grew a bug the day a third list was added.
 */
static gboolean
key_inside_list_of(const gchar *key)
{
    const gchar *dot = strchr(key, '.');
    g_autofree gchar *head = NULL;
    const ClawtSchemaEntry *entry;

    if (dot == NULL)
        return FALSE;

    head = g_strndup(key, (gsize)(dot - key));
    entry = clawt_config_schema_lookup(head);

    return entry != NULL && entry->type == CLAWT_SCHEMA_LIST_OF;
}

/*
 * Two ways an option somebody set can do nothing, both of which used to
 * do it in silence.
 *
 * %CLAWT_SCHEMA_FLAG_INERT is the option this build accepts and does not
 * implement.  Nine of them were found at once -- accepted, saved,
 * reported back as set, and read by no code anywhere -- and the reason
 * they survived is that there is no symptom: the config parses, the
 * daemon starts, and the setting simply never happens.  Deleting the
 * keys would be worse, because then setting one produces "unknown
 * configuration key" and no clue where the option went, so instead the
 * loader says so out loud, naming the key.
 *
 * The second is a scalar option written as a YAML list.  That is not a
 * type error anywhere downstream: yaml_node_get_string() answers NULL
 * for a sequence and every getter then falls through to the default, so
 * `readers: [chief-of-staff]` is a permission somebody granted, saved,
 * and silently denied.  A denial nobody can see is the worst of the
 * outcomes available, which is why this is a warning rather than a
 * comment in the documentation.
 *
 * @section is the schema prefix @block's keys hang under -- "agents",
 * "rooms", "routines" -- or %NULL for the top of the file.  @where names
 * the element in the message, since "jitter_seconds does nothing" is no
 * use to somebody with six routines.
 */
static void
warn_block_keys(ClawtConfig *self,
                YamlNode    *block,
                const gchar *section,
                const gchar *where)
{
    const ClawtSchemaEntry *schema;
    gsize n_entries;
    gsize i;
    g_autofree gchar *prefix = (section != NULL)
                               ? g_strdup_printf("%s.", section)
                               : NULL;

    if (block == NULL)
        return;

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *path = NULL;
        YamlNode *node;
        gboolean inert = (entry->flags & CLAWT_SCHEMA_FLAG_INERT) != 0;
        gboolean scalar = (entry->type == CLAWT_SCHEMA_STRING) ||
                          (entry->type == CLAWT_SCHEMA_PATH) ||
                          (entry->type == CLAWT_SCHEMA_SECRET);

        if (!inert && !scalar)
            continue;

        if (prefix == NULL) {
            if (key_inside_list_of(entry->key))
                continue;

            path = entry->key;
        } else if (g_strcmp0(section, "agents") == 0) {
            /*
             * Asked of the schema, because an agent block spells some
             * fleet options differently -- `mailbox.max_depth` for
             * `orchestration.mailbox.max_depth` -- and a second answer
             * to what an option is called inside an agent is how nine
             * settings came to be unreachable once already.
             */
            path = clawt_config_schema_agent_name(entry);
        } else if (g_str_has_prefix(entry->key, prefix)) {
            path = entry->key + strlen(prefix);
        }

        if (path == NULL)
            continue;

        node = node_at_path(block, path, FALSE);
        if (node == NULL)
            continue;

        if (inert) {
            g_ptr_array_add(self->warnings,
                (where != NULL)
                ? g_strdup_printf("%s sets '%s', which nothing in this "
                                  "build reads; the value is accepted and "
                                  "has no effect", where, entry->key)
                : g_strdup_printf("'%s' is set, and nothing in this build "
                                  "reads it; the value is accepted and has "
                                  "no effect", entry->key));
            continue;
        }

        if (scalar &&
            yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE) {
            g_ptr_array_add(self->warnings,
                (where != NULL)
                ? g_strdup_printf("%s writes '%s' as a list; it is a "
                                  "comma-separated string, so as written "
                                  "it reads back as unset", where,
                                  entry->key)
                : g_strdup_printf("'%s' is written as a list; it is a "
                                  "comma-separated string, so as written "
                                  "it reads back as unset", entry->key));
        }
    }
}

/*
 * The same two checks inside every list of mappings the schema declares.
 *
 * A list is not a section, so warn_unknown_keys() never descends into
 * one and neither would the pass above -- which would leave a room's
 * max_hops and a routine's jitter_seconds, two of the nine, as the only
 * cases the whole mechanism could not see.  Driven from the schema so a
 * list added later is covered without anybody remembering to come back
 * here.
 */
static void
warn_block_keys_in_lists(ClawtConfig *self)
{
    const ClawtSchemaEntry *schema;
    gsize n_entries;
    gsize i;

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        YamlNode *list;
        YamlSequence *sequence;
        guint j;
        guint length;

        if (schema[i].type != CLAWT_SCHEMA_LIST_OF)
            continue;

        list = node_at_path(self->root, schema[i].key, FALSE);

        if (list == NULL ||
            yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE)
            continue;

        sequence = yaml_node_get_sequence(list);
        length = yaml_sequence_get_length(sequence);

        for (j = 0; j < length; j++) {
            YamlNode *element = yaml_sequence_get_element(sequence, j);
            const gchar *id;
            g_autofree gchar *where = NULL;

            if (element == NULL ||
                yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
                continue;

            id = member_string(yaml_node_get_mapping(element), "id");

            where = (id != NULL)
                    ? g_strdup_printf("%s '%s'", schema[i].key, id)
                    : g_strdup_printf("%s[%u]", schema[i].key, j);

            warn_block_keys(self, element, schema[i].key, where);
        }
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

        if (entry->type == CLAWT_SCHEMA_SECTION) {
            warn_unknown_keys(self, yaml_mapping_get_member(mapping, key),
                              full);
            continue;
        }

        /*
         * A value an enum does not have is worth the same warning as a
         * key it does not have, and for a stronger reason.
         *
         * An unknown key reaches nothing; a misspelt enum reaches
         * clawt_config_get_enum(), which could not parse it and
         * returned 0 -- and 0 is a real, documented value of every one
         * of these types.  `restart: sometimes` was silently `never`,
         * `overflow: reject` silently `block-sender`.  The getters now
         * fall back to the documented default instead, and this is
         * where somebody is told they typed it.
         */
        if (entry->type == CLAWT_SCHEMA_ENUM && entry->enum_type != NULL) {
            const gchar *written = lookup_string(self->root, full);
            gint parsed = 0;

            if (written != NULL &&
                !clawt_enum_from_nick(entry->enum_type(), written, &parsed)) {
                g_autofree gchar *allowed =
                    clawt_enum_nick_list(entry->enum_type());

                g_ptr_array_add(self->warnings,
                    g_strdup_printf("'%s' is not a value of '%s'; using the "
                                    "default '%s'. It is one of: %s",
                                    written, full,
                                    (entry->default_value != NULL)
                                    ? entry->default_value : "",
                                    allowed));
            }
        }
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
    warn_block_keys(self, self->root, NULL, NULL);
    warn_block_keys_in_lists(self);
    reload_agents(self);
    reload_integrations(self);
    reload_routines(self);
    reload_triggers(self);

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

    /*
     * A value the enum does not have falls back to the documented
     * default, exactly as an unset key does.
     *
     * Returning 0 was worse than returning nothing, because 0 is a real
     * value of each of these types and a perfectly ordinary one to
     * intend: a misspelt `restart` read as `never` and a misspelt
     * `overflow` as `block-sender`, with nothing anywhere saying so.
     * warn_unknown_keys() names it at load; this decides what the fleet
     * runs on, and the answer people can look up is the one in the
     * documentation.
     */
    if (entry->default_value != NULL &&
        clawt_enum_from_nick(entry->enum_type(), entry->default_value, &value))
        return value;

    return 0;
}

GStrv
clawt_config_get_string_list(ClawtConfig *self, const gchar *key)
{
    GStrv written;
    const gchar *fallback;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    written = node_to_strv(node_at_path(self->root, key, FALSE));

    if (written != NULL)
        return written;

    /*
     * A list falls back to the schema's default like every other type.
     *
     * clawt_agent_config_get_string_list() says above itself that it
     * "used not to, and it was the only getter that did not" -- and that
     * had stopped being true, because the fix was applied to the getter
     * somebody noticed rather than to the rule.  The fleet getter was
     * the remaining one, and it fed the only fleet list that declares a
     * default: orchestration.repeat_thresholds.  #ClawtRepeatWatch sets
     * "5,10,20" in its own init, clawt_daemon_turn_configure() runs at
     * every daemon start and passed NULL over it, and
     * clawt_repeat_watch_set_thresholds(NULL) empties the array -- so no
     * fleet that had not written the key by hand did any repeat
     * detection at all, while the schema, the generated config and the
     * docs all said 5, 10 and 20.
     *
     * Comma-separated in the table, which is the spelling the generator
     * renders from: one default, both readers.
     */
    fallback = schema_default_for(key);

    if (fallback == NULL)
        return NULL;

    return g_strsplit(fallback, ",", -1);
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

/*
 * A top-level list key, such as `defaults.skills`.
 *
 * A setter has to dispatch on what the schema says a key is: writing a
 * list through clawt_config_set_string() is accepted, echoed back,
 * saved, and then read as the default, because node_to_strv() refuses a
 * scalar it did not expect. `computer.host.deny_paths` denied nothing
 * for exactly that reason, so a list key gets a setter that writes a
 * sequence.
 */
gboolean
clawt_config_set_string_list(ClawtConfig        *self,
                             const gchar        *key,
                             const gchar *const *values)
{
    g_autoptr(YamlNode) node = NULL;
    gsize i;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (values == NULL || values[0] == NULL) {
        node = yaml_node_new_null();
    } else {
        /*
         * NULL rather than a fresh sequence: yaml_node_new_sequence()
         * takes its argument (transfer none) and refs it, so one made
         * here leaks this function's reference on every call.
         */
        node = yaml_node_new_sequence(NULL);

        for (i = 0; values[i] != NULL; i++)
            yaml_sequence_add_string_element(yaml_node_get_sequence(node),
                                             values[i]);
    }

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

    if (clawt_agent_id_is_reserved(id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is reserved: it is a sender name clawtilla's own "
                    "routing keys on. Pick another id.", id);
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

guint
clawt_config_adopt_libreclaw(ClawtAgentConfig *agent, const gchar *config_path)
{
    static const struct {
        const gchar *from;   /* dotted path in libreclaw's config */
        const gchar *to;     /* the clawtilla agent key */
    } wanted[] = {
        { "ai.provider",      "model.provider" },
        { "ai.model",         "model.model" },
        { "ai.default_effort", "model.effort" },
        { "agent.name",       "name" },
        { NULL, NULL }
    };
    g_autoptr(YamlParser) parser = NULL;
    g_autoptr(GError) error = NULL;
    YamlNode *root;
    guint adopted = 0;
    gsize i;

    g_return_val_if_fail(agent != NULL, 0);

    if (config_path == NULL || !g_file_test(config_path, G_FILE_TEST_EXISTS))
        return 0;

    parser = yaml_parser_new();

    if (!yaml_parser_load_from_file(parser, config_path, &error)) {
        g_info("import: %s could not be read (%s); using the fleet defaults",
               config_path, error->message);
        return 0;
    }

    root = yaml_parser_get_root(parser);

    if (root == NULL)
        return 0;

    for (i = 0; wanted[i].from != NULL; i++) {
        const gchar *value;

        /* Never overrides something the caller has already set. */
        if (clawt_agent_config_has_key(agent, wanted[i].to))
            continue;

        value = lookup_string(root, wanted[i].from);

        if (value == NULL || value[0] == '\0')
            continue;

        if (clawt_agent_config_set_string(agent, wanted[i].to, value))
            adopted++;
    }

    return adopted;
}

/* ── Integration instances ───────────────────────────────────────── */

struct _ClawtIntegrationConfig {
    gint         ref_count;

    ClawtConfig *config;      /* unowned; the config outlives its instances */
    gchar       *name;
    YamlNode    *node;        /* the instance's mapping, unowned */
    gchar       *shadow_reason;
};

static ClawtIntegrationConfig *
clawt_integration_config_new(ClawtConfig *config,
                             const gchar *name,
                             YamlNode    *node)
{
    ClawtIntegrationConfig *self = g_new0(ClawtIntegrationConfig, 1);

    self->ref_count = 1;
    self->config = config;
    self->name = g_strdup(name);
    self->node = node;

    return self;
}

ClawtIntegrationConfig *
clawt_integration_config_ref(ClawtIntegrationConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);

    return self;
}

void
clawt_integration_config_unref(ClawtIntegrationConfig *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->name);
    g_free(self->shadow_reason);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtIntegrationConfig, clawt_integration_config,
                    clawt_integration_config_ref,
                    clawt_integration_config_unref)

static void
integration_mark_shadow(ClawtIntegrationConfig *self,
                        const gchar            *format, ...) G_GNUC_PRINTF(2, 3);

static void
integration_mark_shadow(ClawtIntegrationConfig *self, const gchar *format, ...)
{
    va_list args;

    /* The first reason is kept: it is the one that caused the rest. */
    if (self->shadow_reason != NULL)
        return;

    va_start(args, format);
    self->shadow_reason = g_strdup_vprintf(format, args);
    va_end(args);
}

const gchar *
clawt_integration_config_get_name(ClawtIntegrationConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->name;
}

const gchar *
clawt_integration_config_get_type_id(ClawtIntegrationConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return member_string(yaml_node_get_mapping(self->node), "type");
}

gboolean
clawt_integration_config_is_shadow(ClawtIntegrationConfig *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->shadow_reason != NULL;
}

const gchar *
clawt_integration_config_get_shadow_reason(ClawtIntegrationConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->shadow_reason;
}

/*
 * Where a key is read from, in order: the agent's own override, then the
 * instance, then nothing.
 *
 * The schema fallback is applied by the callers rather than here, because
 * only they know what type to turn the default string into.
 */
static YamlNode *
integration_node_for(ClawtIntegrationConfig *self,
                     const gchar            *agent_id,
                     const gchar            *key)
{
    YamlNode *node;

    if (agent_id != NULL) {
        g_autofree gchar *path = g_strdup_printf("per_agent.%s.%s",
                                                 agent_id, key);

        node = node_at_path(self->node, path, FALSE);

        if (node != NULL)
            return node;
    }

    return node_at_path(self->node, key, FALSE);
}

/*
 * The schema key for one of an instance's own keys.
 *
 * Every key in the list shares the "integrations." prefix whatever the
 * instance's type is, exactly as "rooms.id" does -- the schema describes
 * the shape of an entry, and an entry is one flat mapping.
 */
static gchar *
integration_schema_key(const gchar *key)
{
    return g_strdup_printf("integrations.%s", key);
}

const gchar *
clawt_integration_config_get_string(ClawtIntegrationConfig *self,
                                    const gchar            *agent_id,
                                    const gchar            *key)
{
    YamlNode *node;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    node = integration_node_for(self, agent_id, key);

    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        return yaml_node_get_string(node);

    {
        g_autofree gchar *schema_key = integration_schema_key(key);

        return schema_default_for(schema_key);
    }
}

gboolean
clawt_integration_config_get_boolean(ClawtIntegrationConfig *self,
                                     const gchar            *agent_id,
                                     const gchar            *key)
{
    YamlNode *node;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    node = integration_node_for(self, agent_id, key);

    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        return yaml_node_get_boolean(node);

    schema_key = integration_schema_key(key);

    return string_to_boolean(schema_default_for(schema_key), FALSE);
}

gint64
clawt_integration_config_get_int(ClawtIntegrationConfig *self,
                                 const gchar            *agent_id,
                                 const gchar            *key)
{
    YamlNode *node;
    g_autofree gchar *schema_key = NULL;
    const gchar *fallback;

    g_return_val_if_fail(self != NULL, 0);
    g_return_val_if_fail(key != NULL, 0);

    node = integration_node_for(self, agent_id, key);

    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        return yaml_node_get_int(node);

    schema_key = integration_schema_key(key);
    fallback = schema_default_for(schema_key);

    return fallback != NULL ? g_ascii_strtoll(fallback, NULL, 10) : 0;
}

GStrv
clawt_integration_config_get_string_list(ClawtIntegrationConfig *self,
                                         const gchar            *agent_id,
                                         const gchar            *key)
{
    GStrv value;
    g_autofree gchar *schema_key = NULL;
    const gchar *fallback;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    value = node_to_strv(integration_node_for(self, agent_id, key));

    if (value != NULL)
        return value;

    /* Same schema fallback every other getter here has. */
    schema_key = integration_schema_key(key);
    fallback = schema_default_for(schema_key);

    if (fallback == NULL)
        return g_new0(gchar *, 1);

    return g_strsplit(fallback, ",", -1);
}

GHashTable *
clawt_integration_config_get_mapping(ClawtIntegrationConfig *self,
                                     const gchar            *agent_id,
                                     const gchar            *key)
{
    GHashTable *out;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    out = mapping_to_hash(node_at_path(self->node, key, FALSE));

    /*
     * Merged over the instance's own rather than replacing it.  An agent
     * that had to restate the whole `env` block to change one variable
     * would end up with three copies of it that drift.
     */
    if (agent_id != NULL) {
        g_autofree gchar *path = g_strdup_printf("per_agent.%s.%s",
                                                 agent_id, key);
        g_autoptr(GHashTable) overrides =
            mapping_to_hash(node_at_path(self->node, path, FALSE));
        GHashTableIter iter;
        gpointer k;
        gpointer v;

        g_hash_table_iter_init(&iter, overrides);

        while (g_hash_table_iter_next(&iter, &k, &v))
            g_hash_table_insert(out, g_strdup(k), g_strdup(v));
    }

    return out;
}

ClawtSecretRef *
clawt_integration_config_get_secret(ClawtIntegrationConfig *self,
                                    const gchar            *agent_id,
                                    const gchar            *key)
{
    YamlNode *node;
    ClawtSecretBackend default_backend;
    g_autoptr(GError) error = NULL;
    ClawtSecretRef *ref;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    node = integration_node_for(self, agent_id, key);

    if (node == NULL)
        return NULL;

    default_backend = (ClawtSecretBackend)
        clawt_config_get_enum(self->config, "secrets.default_backend");

    ref = clawt_secret_ref_parse(node, default_backend, &error);

    if (ref == NULL && error != NULL) {
        g_warning("integration %s: %s: %s", self->name, key, error->message);
        return NULL;
    }

    return ref;
}

gboolean
clawt_integration_config_has_key(ClawtIntegrationConfig *self,
                                 const gchar            *agent_id,
                                 const gchar            *key)
{
    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    return integration_node_for(self, agent_id, key) != NULL;
}

gboolean
clawt_integration_config_get_enabled(ClawtIntegrationConfig *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    if (self->shadow_reason != NULL)
        return FALSE;

    return clawt_integration_config_get_boolean(self, NULL, "enabled");
}

ClawtScope
clawt_integration_config_get_scope(ClawtIntegrationConfig *self)
{
    const gchar *nick;
    gint value = 0;

    g_return_val_if_fail(self != NULL, CLAWT_SCOPE_NONE);

    nick = clawt_integration_config_get_string(self, NULL, "scope");

    if (nick == NULL)
        return CLAWT_SCOPE_SELECTED;

    if (!clawt_enum_from_nick(CLAWT_TYPE_SCOPE, nick, &value)) {
        /*
         * An unrecognised scope reaches nobody rather than everybody.  The
         * two failure modes are not symmetric: a typo that hands a
         * credential to the whole fleet is a great deal worse than one
         * that hands it to nothing and says so.
         */
        g_warning("integration %s: '%s' is not a scope; reaching nobody",
                  self->name, nick);
        return CLAWT_SCOPE_NONE;
    }

    return (ClawtScope)value;
}

GStrv
clawt_integration_config_get_agents(ClawtIntegrationConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return node_to_strv(node_at_path(self->node, "agents", FALSE));
}

gboolean
clawt_integration_config_covers_on_team(ClawtIntegrationConfig *self,
                                        const gchar            *agent_id,
                                        const gchar            *team)
{
    g_auto(GStrv) agents = NULL;
    g_auto(GStrv) teams = NULL;

    g_return_val_if_fail(self != NULL, FALSE);

    if (!clawt_integration_config_get_enabled(self))
        return FALSE;

    agents = clawt_integration_config_get_agents(self);
    teams = node_to_strv(node_at_path(self->node, "teams", FALSE));

    /*
     * Through the shared rule rather than a copy of it. This was a copy,
     * and the fleet's shared folders were about to be a second -- two
     * implementations of "who gets this" that would have differed
     * exactly once.
     */
    return clawt_scope_covers(clawt_integration_config_get_scope(self),
                              (const gchar *const *)agents,
                              (const gchar *const *)teams,
                              agent_id, team);
}

gboolean
clawt_integration_config_covers(ClawtIntegrationConfig *self,
                                const gchar            *agent_id)
{
    g_auto(GStrv) teams = NULL;

    g_return_val_if_fail(self != NULL, FALSE);

    /*
     * A `teams:` entry cannot be judged without a team, and answering
     * FALSE for it silently is what made every team-scoped integration
     * reach nobody while reporting itself as configured.  Say so rather
     * than answering confidently about a scope this form cannot see.
     */
    teams = node_to_strv(node_at_path(self->node, "teams", FALSE));

    if (teams != NULL && teams[0] != NULL)
        g_warning("integration '%s' is scoped by team; asking whether it "
                  "covers '%s' without one cannot answer that",
                  clawt_integration_config_get_name(self),
                  agent_id != NULL ? agent_id : "(none)");

    return clawt_integration_config_covers_on_team(self, agent_id, NULL);
}

gboolean
clawt_integration_config_covers_agent(ClawtIntegrationConfig *self,
                                      ClawtAgentConfig       *agent)
{
    g_return_val_if_fail(self != NULL, FALSE);

    if (agent == NULL)
        return FALSE;

    return clawt_integration_config_covers_on_team(
        self, clawt_agent_config_get_id(agent),
        clawt_agent_config_get_string(agent, "team"));
}

/*
 * The mapping a write lands in: the instance itself, or the agent's own
 * block under per_agent, created on demand.
 */
static YamlNode *
integration_write_root(ClawtIntegrationConfig *self, const gchar *agent_id)
{
    g_autofree gchar *path = NULL;

    if (agent_id == NULL)
        return self->node;

    path = g_strdup_printf("per_agent.%s", agent_id);

    return node_at_path(self->node, path, TRUE);
}

/*
 * Removes a key, and then removes the per-agent block if that emptied it.
 *
 * Without the second half, clearing an override leaves `per_agent: {agent:
 * {}}` behind -- which reads, correctly but uselessly, as "this agent has
 * overrides" in every listing that shows them.
 */
static gboolean
integration_unset(ClawtIntegrationConfig *self,
                  const gchar            *agent_id,
                  const gchar            *key)
{
    YamlNode *root = integration_write_root(self, agent_id);
    YamlMapping *mapping;

    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
        return FALSE;

    mapping = yaml_node_get_mapping(root);

    if (yaml_mapping_get_member(mapping, key) == NULL)
        return FALSE;

    yaml_mapping_remove_member(mapping, key);

    if (agent_id != NULL && yaml_mapping_get_size(mapping) == 0) {
        YamlNode *per_agent = node_at_path(self->node, "per_agent", FALSE);

        if (per_agent != NULL &&
            yaml_node_get_node_type(per_agent) == YAML_NODE_MAPPING) {
            YamlMapping *outer = yaml_node_get_mapping(per_agent);

            yaml_mapping_remove_member(outer, agent_id);

            if (yaml_mapping_get_size(outer) == 0)
                yaml_mapping_remove_member(yaml_node_get_mapping(self->node),
                                           "per_agent");
        }
    }

    return TRUE;
}

gboolean
clawt_integration_config_set_string(ClawtIntegrationConfig *self,
                                    const gchar            *agent_id,
                                    const gchar            *key,
                                    const gchar            *value)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (value == NULL)
        return integration_unset(self, agent_id, key);

    node = yaml_node_new_string(value);
    schema_key = integration_schema_key(key);

    return set_scalar(integration_write_root(self, agent_id), key,
                      node, agent_id == NULL ? schema_key : NULL);
}

gboolean
clawt_integration_config_set_boolean(ClawtIntegrationConfig *self,
                                     const gchar            *agent_id,
                                     const gchar            *key,
                                     gboolean                value)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    node = yaml_node_new_boolean(value);
    schema_key = integration_schema_key(key);

    return set_scalar(integration_write_root(self, agent_id), key,
                      node, agent_id == NULL ? schema_key : NULL);
}

gboolean
clawt_integration_config_set_int(ClawtIntegrationConfig *self,
                                 const gchar            *agent_id,
                                 const gchar            *key,
                                 gint64                  value)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    node = yaml_node_new_int(value);
    schema_key = integration_schema_key(key);

    return set_scalar(integration_write_root(self, agent_id), key,
                      node, agent_id == NULL ? schema_key : NULL);
}

gboolean
clawt_integration_config_set_string_list(ClawtIntegrationConfig *self,
                                         const gchar            *agent_id,
                                         const gchar            *key,
                                         const gchar *const     *values)
{
    g_autoptr(YamlNode) node = NULL;
    g_autoptr(YamlSequence) sequence = NULL;
    g_autofree gchar *schema_key = NULL;
    guint i;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (values == NULL || values[0] == NULL)
        return integration_unset(self, agent_id, key);

    sequence = yaml_sequence_new();

    for (i = 0; values[i] != NULL; i++) {
        g_autoptr(YamlNode) element = yaml_node_new_string(values[i]);

        yaml_sequence_add_element(sequence, element);
    }

    node = yaml_node_new_sequence(sequence);
    schema_key = integration_schema_key(key);

    return set_scalar(integration_write_root(self, agent_id), key,
                      node, agent_id == NULL ? schema_key : NULL);
}

gboolean
clawt_integration_config_set_secret(ClawtIntegrationConfig *self,
                                    const gchar            *agent_id,
                                    const gchar            *key,
                                    ClawtSecretBackend      backend,
                                    const gchar            *locator)
{
    g_autoptr(YamlNode) node = NULL;
    g_autoptr(YamlMapping) mapping = NULL;
    g_autoptr(YamlNode) value = NULL;
    g_autofree gchar *schema_key = NULL;
    const gchar *backend_key;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (locator == NULL)
        return integration_unset(self, agent_id, key);

    switch (backend) {
    case CLAWT_SECRET_BACKEND_ENV:
        backend_key = "env";
        break;
    case CLAWT_SECRET_BACKEND_COMMAND:
        backend_key = "command";
        break;
    case CLAWT_SECRET_BACKEND_FILE:
    default:
        backend_key = "file";
        break;
    }

    mapping = yaml_mapping_new();
    value = yaml_node_new_string(locator);
    yaml_mapping_set_member(mapping, backend_key, value);
    node = yaml_node_new_mapping(mapping);
    schema_key = integration_schema_key(key);

    return set_scalar(integration_write_root(self, agent_id), key,
                      node, agent_id == NULL ? schema_key : NULL);
}

gboolean
clawt_integration_config_set_scope(ClawtIntegrationConfig *self,
                                   ClawtScope   scope,
                                   const gchar *const     *agents)
{
    const gchar *nick;

    g_return_val_if_fail(self != NULL, FALSE);

    nick = clawt_enum_to_nick(CLAWT_TYPE_SCOPE, (gint)scope);

    if (nick == NULL)
        return FALSE;

    if (!clawt_integration_config_set_string(self, NULL, "scope", nick))
        return FALSE;

    /*
     * The agent list is written whenever it is given, whatever the scope.
     * Keeping it under `all` is what lets the UI offer "back to these
     * three" instead of an empty box.
     */
    if (agents != NULL)
        clawt_integration_config_set_string_list(self, NULL, "agents", agents);

    return TRUE;
}

gboolean
clawt_integration_config_set_enabled(ClawtIntegrationConfig *self,
                                     gboolean                enabled)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return clawt_integration_config_set_boolean(self, NULL, "enabled",
                                                enabled);
}

/*
 * Rebuilds the instance list from the YAML tree, exactly as reload_agents()
 * does for agents and for the same reason.
 */
static void
reload_integrations(ClawtConfig *self)
{
    YamlNode *node;
    YamlSequence *sequence;
    g_autoptr(GHashTable) seen = NULL;
    guint i;
    guint length;

    g_ptr_array_set_size(self->integrations, 0);

    node = node_at_path(self->root, "integrations", FALSE);

    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE)
        return;

    seen = g_hash_table_new(g_str_hash, g_str_equal);
    sequence = yaml_node_get_sequence(node);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        ClawtIntegrationConfig *instance;
        const gchar *name;

        if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING) {
            g_ptr_array_add(self->warnings,
                g_strdup_printf("integrations[%u] is not a mapping; ignored",
                                i));
            continue;
        }

        name = member_string(yaml_node_get_mapping(element), "name");

        if (name == NULL) {
            g_ptr_array_add(self->warnings,
                g_strdup_printf("integrations[%u] has no name; ignored", i));
            continue;
        }

        instance = clawt_integration_config_new(self, name, element);

        if (g_hash_table_contains(seen, name))
            integration_mark_shadow(instance,
                                    "another integration already uses the "
                                    "name '%s'", name);
        else
            g_hash_table_add(seen, (gpointer)name);

        if (clawt_integration_config_get_type_id(instance) == NULL)
            integration_mark_shadow(instance, "no type is set");

        if (clawt_integration_config_is_shadow(instance))
            g_ptr_array_add(self->warnings,
                g_strdup_printf("integration '%s' disabled: %s", name,
                    clawt_integration_config_get_shadow_reason(instance)));

        g_ptr_array_add(self->integrations, instance);
    }
}

GPtrArray *
clawt_config_get_integrations(ClawtConfig *self)
{
    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    return self->integrations;
}

ClawtIntegrationConfig *
clawt_config_get_integration(ClawtConfig *self, const gchar *name)
{
    guint i;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);
    g_return_val_if_fail(name != NULL, NULL);

    for (i = 0; i < self->integrations->len; i++) {
        ClawtIntegrationConfig *instance =
            g_ptr_array_index(self->integrations, i);

        if (g_strcmp0(clawt_integration_config_get_name(instance), name) == 0)
            return instance;
    }

    return NULL;
}

ClawtIntegrationConfig *
clawt_config_add_integration(ClawtConfig  *self,
                             const gchar  *name,
                             const gchar  *type_id,
                             GError      **error)
{
    YamlNode *list;
    g_autoptr(YamlNode) entry = NULL;
    g_autoptr(YamlNode) name_node = NULL;
    g_autoptr(YamlNode) type_node = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    /*
     * The same id rules as an agent, because the name becomes a key in
     * the agent's .mcp.json and a file name under its credentials
     * directory.  A name with a slash in it would write outside both.
     */
    if (!clawt_is_valid_id(name)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable integration name: names may hold "
                    "only lowercase letters, digits, '-' and '_', and must "
                    "not start with punctuation",
                    name != NULL ? name : "");
        return NULL;
    }

    if (type_id == NULL || *type_id == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "an integration needs a type");
        return NULL;
    }

    if (clawt_config_get_integration(self, name) != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "an integration called '%s' already exists", name);
        return NULL;
    }

    list = node_at_path(self->root, "integrations", FALSE);

    if (list == NULL ||
        yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE) {
        g_autoptr(YamlNode) fresh = yaml_node_new_sequence(NULL);

        yaml_mapping_set_member(yaml_node_get_mapping(self->root),
                                "integrations", fresh);
        list = node_at_path(self->root, "integrations", FALSE);
        apply_schema_comment(list, "integrations");
    }

    entry = yaml_node_new_mapping(NULL);
    name_node = yaml_node_new_string(name);
    type_node = yaml_node_new_string(type_id);
    yaml_mapping_set_member(yaml_node_get_mapping(entry), "name", name_node);
    yaml_mapping_set_member(yaml_node_get_mapping(entry), "type", type_node);

    yaml_sequence_add_element(yaml_node_get_sequence(list), entry);

    reload_integrations(self);

    return clawt_config_get_integration(self, name);
}

gboolean
clawt_config_remove_integration(ClawtConfig *self, const gchar *name)
{
    YamlNode *list;
    YamlSequence *sequence;
    guint i;
    guint length;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);
    g_return_val_if_fail(name != NULL, FALSE);

    list = node_at_path(self->root, "integrations", FALSE);

    if (list == NULL || yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE)
        return FALSE;

    sequence = yaml_node_get_sequence(list);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);

        if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
            continue;

        if (g_strcmp0(member_string(yaml_node_get_mapping(element), "name"),
                      name) != 0)
            continue;

        yaml_sequence_remove_element(sequence, i);
        reload_integrations(self);
        return TRUE;
    }

    return FALSE;
}

/*
 * Resolves an `env` mapping, where a value may be a literal or a secret.
 *
 * The two are told apart by node type rather than by a marker, which is
 * how a secret reference is spelled everywhere else in this file: a
 * scalar is the value, a mapping is `{env: NAME}` or `{file: PATH}` and
 * is fetched.  Without this an MCP server needing an API key could only
 * be given one by writing it into clawtilla.yaml, which is the single
 * thing the secret machinery exists to prevent.
 */
GHashTable *
clawt_integration_config_resolve_env(ClawtIntegrationConfig  *self,
                                     const gchar             *agent_id,
                                     const gchar             *key,
                                     const gchar             *secrets_dir,
                                     GError                 **error)
{
    g_autoptr(GHashTable) out = NULL;
    ClawtSecretBackend default_backend;
    guint timeout;
    guint pass;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    out = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    default_backend = (ClawtSecretBackend)
        clawt_config_get_enum(self->config, "secrets.default_backend");
    timeout = (guint)clawt_config_get_int(self->config,
                                          "secrets.command_timeout_seconds");

    /*
     * The instance's own values first, then the agent's, so an override
     * replaces one variable without restating the block.
     */
    for (pass = 0; pass < 2; pass++) {
        YamlNode *node;
        YamlMapping *mapping;
        GList *members;
        GList *l;

        if (pass == 0) {
            node = node_at_path(self->node, key, FALSE);
        } else {
            g_autofree gchar *path = NULL;

            if (agent_id == NULL)
                break;

            path = g_strdup_printf("per_agent.%s.%s", agent_id, key);
            node = node_at_path(self->node, path, FALSE);
        }

        if (node == NULL ||
            yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
            continue;

        mapping = yaml_node_get_mapping(node);
        members = yaml_mapping_get_members(mapping);

        for (l = members; l != NULL; l = l->next) {
            const gchar *name = l->data;
            YamlNode *value = yaml_mapping_get_member(mapping, name);

            if (value == NULL)
                continue;

            if (yaml_node_get_node_type(value) == YAML_NODE_SCALAR) {
                g_hash_table_insert(out, g_strdup(name),
                                    g_strdup(yaml_node_get_string(value)));
                continue;
            }

            {
                g_autoptr(ClawtSecretRef) ref = NULL;
                g_autoptr(GError) local = NULL;
                gchar *resolved;

                ref = clawt_secret_ref_parse(value, default_backend, &local);

                if (ref == NULL) {
                    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "integration '%s': %s.%s is neither a value "
                                "nor a secret reference: %s",
                                self->name, key, name,
                                local != NULL ? local->message
                                              : "unknown reason");
                    g_list_free(members);
                    return NULL;
                }

                resolved = clawt_secret_ref_resolve(ref, secrets_dir, timeout,
                                                    &local);

                if (resolved == NULL) {
                    g_autofree gchar *described =
                        clawt_secret_ref_describe(ref);

                    /*
                     * Named by reference, never by value.  A failure here
                     * is one variable's worth of trouble and the message
                     * has to be enough to fix it without being enough to
                     * leak it.
                     */
                    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                                "integration '%s': %s.%s: could not resolve "
                                "%s: %s",
                                self->name, key, name, described,
                                local != NULL ? local->message
                                              : "unknown reason");
                    g_list_free(members);
                    return NULL;
                }

                g_hash_table_insert(out, g_strdup(name), resolved);
            }
        }

        g_list_free(members);
    }

    return g_steal_pointer(&out);
}

/* ── Routines ────────────────────────────────────────────────────── */

struct _ClawtRoutine {
    gint         ref_count;

    ClawtConfig *config;      /* unowned; the config outlives its routines */
    gchar       *id;
    YamlNode    *node;        /* the routine's mapping, unowned */
};

static ClawtRoutine *
clawt_routine_new(ClawtConfig *config, const gchar *id, YamlNode *node)
{
    ClawtRoutine *self = g_new0(ClawtRoutine, 1);

    self->ref_count = 1;
    self->config = config;
    self->id = g_strdup(id);
    self->node = node;

    return self;
}

ClawtRoutine *
clawt_routine_ref(ClawtRoutine *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);

    return self;
}

void
clawt_routine_unref(ClawtRoutine *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->id);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtRoutine, clawt_routine,
                    clawt_routine_ref, clawt_routine_unref)

const gchar *
clawt_routine_get_id(ClawtRoutine *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->id;
}

static gchar *
routine_schema_key(const gchar *key)
{
    return g_strdup_printf("routines.%s", key);
}

const gchar *
clawt_routine_get_string(ClawtRoutine *self, const gchar *key)
{
    YamlNode *node;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    node = node_at_path(self->node, key, FALSE);

    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        return yaml_node_get_string(node);

    schema_key = routine_schema_key(key);

    return schema_default_for(schema_key);
}

gboolean
clawt_routine_get_boolean(ClawtRoutine *self, const gchar *key)
{
    YamlNode *node;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    node = node_at_path(self->node, key, FALSE);

    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        return yaml_node_get_boolean(node);

    schema_key = routine_schema_key(key);

    return string_to_boolean(schema_default_for(schema_key), FALSE);
}

gint64
clawt_routine_get_int(ClawtRoutine *self, const gchar *key)
{
    YamlNode *node;
    g_autofree gchar *schema_key = NULL;
    const gchar *fallback;

    g_return_val_if_fail(self != NULL, 0);
    g_return_val_if_fail(key != NULL, 0);

    node = node_at_path(self->node, key, FALSE);

    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        return yaml_node_get_int(node);

    schema_key = routine_schema_key(key);
    fallback = schema_default_for(schema_key);

    return fallback != NULL ? g_ascii_strtoll(fallback, NULL, 10) : 0;
}

gboolean
clawt_routine_has_key(ClawtRoutine *self, const gchar *key)
{
    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    return node_at_path(self->node, key, FALSE) != NULL;
}

gboolean
clawt_routine_set_string(ClawtRoutine *self, const gchar *key,
                         const gchar *value)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (value == NULL) {
        YamlMapping *mapping = yaml_node_get_mapping(self->node);

        if (yaml_mapping_get_member(mapping, key) == NULL)
            return FALSE;

        yaml_mapping_remove_member(mapping, key);
        return TRUE;
    }

    node = yaml_node_new_string(value);
    schema_key = routine_schema_key(key);

    return set_scalar(self->node, key, node, schema_key);
}

gboolean
clawt_routine_set_boolean(ClawtRoutine *self, const gchar *key,
                          gboolean value)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    node = yaml_node_new_boolean(value);
    schema_key = routine_schema_key(key);

    return set_scalar(self->node, key, node, schema_key);
}

gboolean
clawt_routine_set_int(ClawtRoutine *self, const gchar *key, gint64 value)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    node = yaml_node_new_int(value);
    schema_key = routine_schema_key(key);

    return set_scalar(self->node, key, node, schema_key);
}

gchar *
clawt_routine_get_cron(ClawtRoutine *self, GError **error)
{
    g_return_val_if_fail(self != NULL, NULL);

    return clawt_cron_from_preset(clawt_routine_get_string(self, "schedule"),
                                  clawt_routine_get_string(self, "at"),
                                  clawt_routine_get_string(self, "weekday"),
                                  clawt_routine_get_string(self, "cron"),
                                  error);
}

static void
reload_routines(ClawtConfig *self)
{
    YamlNode *node;
    YamlSequence *sequence;
    g_autoptr(GHashTable) seen = NULL;
    guint i;
    guint length;

    g_ptr_array_set_size(self->routines, 0);

    node = node_at_path(self->root, "routines", FALSE);

    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE)
        return;

    seen = g_hash_table_new(g_str_hash, g_str_equal);
    sequence = yaml_node_get_sequence(node);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        const gchar *id;

        if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING) {
            g_ptr_array_add(self->warnings,
                g_strdup_printf("routines[%u] is not a mapping; ignored", i));
            continue;
        }

        id = member_string(yaml_node_get_mapping(element), "id");

        if (id == NULL) {
            g_ptr_array_add(self->warnings,
                g_strdup_printf("routines[%u] has no id; ignored", i));
            continue;
        }

        if (g_hash_table_contains(seen, id)) {
            g_ptr_array_add(self->warnings,
                g_strdup_printf("two routines are called '%s'; the second "
                                "is ignored", id));
            continue;
        }

        g_hash_table_add(seen, (gpointer)id);
        g_ptr_array_add(self->routines,
                        clawt_routine_new(self, id, element));

        /*
         * And whether the schedule it names can be turned into one.
         *
         * Checked here because a value that cannot be parsed is a
         * load-time fact.  It used to be discovered at every tick
         * instead: clawt_routine_runner's cron_for() warned each time it
         * failed, so one routine whose `at:` could not be read logged
         * roughly once a minute for as long as the daemon was up --
         * 1,368 lines a day from a single config value, on a daemon
         * nobody restarts, burying everything else in the unit's log.
         *
         * A warning rather than a refusal, like every other fleet-level
         * mistake here: a config is edited by hand and half-built states
         * are ordinary.  What is *not* acceptable is the routine then
         * reading as healthy, which is why `routine.list` reports the
         * same failure as `problem` and all three clients now draw it.
         */
        {
            ClawtRoutine *added = g_ptr_array_index(
                self->routines, self->routines->len - 1);
            g_autofree gchar *expression = NULL;
            g_autoptr(GError) schedule_error = NULL;

            expression = clawt_routine_get_cron(added, &schedule_error);

            if (expression == NULL && schedule_error != NULL)
                g_ptr_array_add(
                    self->warnings,
                    g_strdup_printf("routine '%s' will never run: %s", id,
                                    schedule_error->message));
        }
    }
}

GPtrArray *
clawt_config_get_routines(ClawtConfig *self)
{
    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    return self->routines;
}

ClawtRoutine *
clawt_config_get_routine(ClawtConfig *self, const gchar *id)
{
    guint i;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);
    g_return_val_if_fail(id != NULL, NULL);

    for (i = 0; i < self->routines->len; i++) {
        ClawtRoutine *routine = g_ptr_array_index(self->routines, i);

        if (g_strcmp0(clawt_routine_get_id(routine), id) == 0)
            return routine;
    }

    return NULL;
}

ClawtRoutine *
clawt_config_add_routine(ClawtConfig *self, const gchar *id, GError **error)
{
    YamlNode *list;
    g_autoptr(YamlNode) entry = NULL;
    g_autoptr(YamlNode) id_node = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), NULL);

    if (!clawt_is_valid_id(id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable routine id: ids may hold only "
                    "lowercase letters, digits, '-' and '_', and must not "
                    "start with punctuation", id != NULL ? id : "");
        return NULL;
    }

    if (clawt_config_get_routine(self, id) != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "a routine called '%s' already exists", id);
        return NULL;
    }

    list = node_at_path(self->root, "routines", FALSE);

    if (list == NULL || yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE) {
        g_autoptr(YamlNode) fresh = yaml_node_new_sequence(NULL);

        yaml_mapping_set_member(yaml_node_get_mapping(self->root),
                                "routines", fresh);
        list = node_at_path(self->root, "routines", FALSE);
        apply_schema_comment(list, "routines");
    }

    entry = yaml_node_new_mapping(NULL);
    id_node = yaml_node_new_string(id);
    yaml_mapping_set_member(yaml_node_get_mapping(entry), "id", id_node);
    yaml_sequence_add_element(yaml_node_get_sequence(list), entry);

    reload_routines(self);

    return clawt_config_get_routine(self, id);
}

gboolean
clawt_config_remove_routine(ClawtConfig *self, const gchar *id)
{
    YamlNode *list;
    YamlSequence *sequence;
    guint i;
    guint length;

    g_return_val_if_fail(CLAWT_IS_CONFIG(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    list = node_at_path(self->root, "routines", FALSE);

    if (list == NULL || yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE)
        return FALSE;

    sequence = yaml_node_get_sequence(list);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);

        if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
            continue;

        if (g_strcmp0(member_string(yaml_node_get_mapping(element), "id"),
                      id) != 0)
            continue;

        yaml_sequence_remove_element(sequence, i);
        reload_routines(self);
        return TRUE;
    }

    return FALSE;
}

/* ── Triggers ────────────────────────────────────────────────────── */

/*
 * The same handle shape as #ClawtRoutine, with two getters it does not
 * need: a trigger's `events` is a list and its `secret` is a reference.
 *
 * Both are here rather than folded into the string getter because a
 * setter that does not dispatch on what the schema says a key is has
 * already cost this codebase a working option -- `deny_paths` was
 * written as a scalar, accepted, saved, and read back as the default,
 * so it denied nothing.
 */
struct _ClawtTrigger {
    gint         ref_count;

    ClawtConfig *config;      /* unowned; the config outlives its triggers */
    gchar       *id;
    YamlNode    *node;        /* the trigger's mapping, unowned */
};

static ClawtTrigger *
clawt_trigger_new(ClawtConfig *config, const gchar *id, YamlNode *node)
{
    ClawtTrigger *self = g_new0(ClawtTrigger, 1);

    self->ref_count = 1;
    self->config = config;
    self->id = g_strdup(id);
    self->node = node;

    return self;
}

ClawtTrigger *
clawt_trigger_ref(ClawtTrigger *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);

    return self;
}

void
clawt_trigger_unref(ClawtTrigger *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->id);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtTrigger, clawt_trigger,
                    clawt_trigger_ref, clawt_trigger_unref)

const gchar *
clawt_trigger_get_id(ClawtTrigger *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->id;
}

static gchar *
trigger_schema_key(const gchar *key)
{
    return g_strdup_printf("triggers.%s", key);
}

const gchar *
clawt_trigger_get_string(ClawtTrigger *self, const gchar *key)
{
    YamlNode *node;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    node = node_at_path(self->node, key, FALSE);

    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        return yaml_node_get_string(node);

    schema_key = trigger_schema_key(key);

    return schema_default_for(schema_key);
}

gboolean
clawt_trigger_get_boolean(ClawtTrigger *self, const gchar *key)
{
    YamlNode *node;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    node = node_at_path(self->node, key, FALSE);

    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        return yaml_node_get_boolean(node);

    schema_key = trigger_schema_key(key);

    return string_to_boolean(schema_default_for(schema_key), FALSE);
}

gint64
clawt_trigger_get_int(ClawtTrigger *self, const gchar *key)
{
    YamlNode *node;
    g_autofree gchar *schema_key = NULL;
    const gchar *fallback;

    g_return_val_if_fail(self != NULL, 0);
    g_return_val_if_fail(key != NULL, 0);

    node = node_at_path(self->node, key, FALSE);

    if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
        return yaml_node_get_int(node);

    schema_key = trigger_schema_key(key);
    fallback = schema_default_for(schema_key);

    return fallback != NULL ? g_ascii_strtoll(fallback, NULL, 10) : 0;
}

GStrv
clawt_trigger_get_string_list(ClawtTrigger *self, const gchar *key)
{
    GStrv value;
    g_autofree gchar *schema_key = NULL;
    const gchar *fallback;

    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    value = node_to_strv(node_at_path(self->node, key, FALSE));

    if (value != NULL)
        return value;

    schema_key = trigger_schema_key(key);
    fallback = schema_default_for(schema_key);

    if (fallback == NULL)
        return g_new0(gchar *, 1);

    return g_strsplit(fallback, ",", -1);
}

ClawtSecretRef *
clawt_trigger_get_secret(ClawtTrigger *self, const gchar *key)
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

    /*
     * A reference that cannot be parsed is a warning and no secret, so
     * the trigger authenticates nothing.  Falling back to "no secret
     * required" would turn a typo into a public endpoint.
     */
    if (ref == NULL && error != NULL) {
        g_warning("trigger %s: %s: %s", self->id, key, error->message);
        return NULL;
    }

    return ref;
}

ClawtTriggerProvider
clawt_trigger_get_provider(ClawtTrigger *self)
{
    const gchar *nick;
    gint value;

    g_return_val_if_fail(self != NULL, CLAWT_TRIGGER_PROVIDER_GENERIC);

    nick = clawt_trigger_get_string(self, "provider");

    if (nick == NULL ||
        !clawt_enum_from_nick(CLAWT_TYPE_TRIGGER_PROVIDER, nick, &value))
        return CLAWT_TRIGGER_PROVIDER_GENERIC;

    return (ClawtTriggerProvider)value;
}

gboolean
clawt_trigger_has_key(ClawtTrigger *self, const gchar *key)
{
    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    return node_at_path(self->node, key, FALSE) != NULL;
}

gboolean
clawt_trigger_set_string(ClawtTrigger *self, const gchar *key,
                         const gchar *value)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (value == NULL) {
        YamlMapping *mapping = yaml_node_get_mapping(self->node);

        if (mapping != NULL)
            yaml_mapping_remove_member(mapping, key);

        return TRUE;
    }

    node = yaml_node_new_string(value);
    schema_key = trigger_schema_key(key);

    return set_scalar(self->node, key, node, schema_key);
}

gboolean
clawt_trigger_set_boolean(ClawtTrigger *self, const gchar *key,
                          gboolean value)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    node = yaml_node_new_boolean(value);
    schema_key = trigger_schema_key(key);

    return set_scalar(self->node, key, node, schema_key);
}

gboolean
clawt_trigger_set_int(ClawtTrigger *self, const gchar *key, gint64 value)
{
    g_autoptr(YamlNode) node = NULL;
    g_autofree gchar *schema_key = NULL;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    node = yaml_node_new_int(value);
    schema_key = trigger_schema_key(key);

    return set_scalar(self->node, key, node, schema_key);
}

gboolean
clawt_trigger_set_string_list(ClawtTrigger       *self,
                              const gchar        *key,
                              const gchar *const *values)
{
    g_autoptr(YamlNode) node = NULL;
    g_autoptr(YamlSequence) sequence = NULL;
    g_autofree gchar *schema_key = NULL;
    guint i;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (values == NULL || values[0] == NULL)
        return clawt_trigger_set_string(self, key, NULL);

    sequence = yaml_sequence_new();

    for (i = 0; values[i] != NULL; i++) {
        g_autoptr(YamlNode) element = yaml_node_new_string(values[i]);

        yaml_sequence_add_element(sequence, element);
    }

    node = yaml_node_new_sequence(sequence);
    schema_key = trigger_schema_key(key);

    return set_scalar(self->node, key, node, schema_key);
}

gboolean
clawt_trigger_set_secret(ClawtTrigger       *self,
                         const gchar        *key,
                         ClawtSecretBackend  backend,
                         const gchar        *locator)
{
    g_autoptr(YamlNode) node = NULL;
    g_autoptr(YamlMapping) mapping = NULL;
    g_autoptr(YamlNode) value = NULL;
    g_autofree gchar *schema_key = NULL;
    const gchar *backend_key;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (locator == NULL)
        return clawt_trigger_set_string(self, key, NULL);

    switch (backend) {
    case CLAWT_SECRET_BACKEND_ENV:
        backend_key = "env";
        break;
    case CLAWT_SECRET_BACKEND_COMMAND:
        backend_key = "command";
        break;
    case CLAWT_SECRET_BACKEND_FILE:
    default:
        backend_key = "file";
        break;
    }

    mapping = yaml_mapping_new();
    value = yaml_node_new_string(locator);
    yaml_mapping_set_member(mapping, backend_key, value);
    node = yaml_node_new_mapping(mapping);
    schema_key = trigger_schema_key(key);

    return set_scalar(self->node, key, node, schema_key);
}

/*
 * Rebuilt from the file, with the same three refusals a routine gets.
 *
 * A trigger with no id is worse than a routine with none: an endpoint is
 * derived from it, so an entry nobody can name is an entry nobody can
 * rotate or remove.
 */
static void
reload_triggers(ClawtConfig *self)
{
    YamlNode *list;
    YamlSequence *sequence;
    guint i;
    guint length;

    g_ptr_array_set_size(self->triggers, 0);

    list = node_at_path(self->root, "triggers", FALSE);

    if (list == NULL || yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE)
        return;

    sequence = yaml_node_get_sequence(list);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        YamlMapping *mapping;
        const gchar *id;

        if (element == NULL ||
            yaml_node_get_node_type(element) != YAML_NODE_MAPPING) {
            g_ptr_array_add(self->warnings,
                            g_strdup_printf("triggers[%u] is not a mapping; "
                                            "ignored", i));
            continue;
        }

        mapping = yaml_node_get_mapping(element);
        id = member_string(mapping, "id");

        if (id == NULL || *id == '\0') {
            g_ptr_array_add(self->warnings,
                            g_strdup_printf("triggers[%u] has no id; "
                                            "ignored", i));
            continue;
        }

        if (clawt_config_get_trigger(self, id) != NULL) {
            g_ptr_array_add(self->warnings,
                            g_strdup_printf("two triggers are called '%s'; "
                                            "the second is ignored", id));
            continue;
        }

        g_ptr_array_add(self->triggers,
                        clawt_trigger_new(self, id, element));
    }
}

GPtrArray *
clawt_config_get_triggers(ClawtConfig *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->triggers;
}

ClawtTrigger *
clawt_config_get_trigger(ClawtConfig *self, const gchar *id)
{
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    if (id == NULL)
        return NULL;

    for (i = 0; i < self->triggers->len; i++) {
        ClawtTrigger *trigger = g_ptr_array_index(self->triggers, i);

        if (g_strcmp0(trigger->id, id) == 0)
            return trigger;
    }

    return NULL;
}

ClawtTrigger *
clawt_config_add_trigger(ClawtConfig  *self,
                         const gchar  *id,
                         GError      **error)
{
    YamlNode *list;
    g_autoptr(YamlNode) entry = NULL;
    g_autoptr(YamlNode) id_node = NULL;

    g_return_val_if_fail(self != NULL, NULL);

    if (!clawt_is_valid_id(id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable trigger id: ids may hold only "
                    "lowercase letters, digits, '-' and '_', and must not "
                    "start with punctuation", id != NULL ? id : "");
        return NULL;
    }

    if (clawt_config_get_trigger(self, id) != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "a trigger called '%s' already exists", id);
        return NULL;
    }

    list = node_at_path(self->root, "triggers", FALSE);

    if (list == NULL || yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE) {
        g_autoptr(YamlNode) fresh = yaml_node_new_sequence(NULL);

        yaml_mapping_set_member(yaml_node_get_mapping(self->root),
                                "triggers", fresh);
        list = node_at_path(self->root, "triggers", FALSE);
        apply_schema_comment(list, "triggers");
    }

    entry = yaml_node_new_mapping(NULL);
    id_node = yaml_node_new_string(id);
    yaml_mapping_set_member(yaml_node_get_mapping(entry), "id", id_node);
    yaml_sequence_add_element(yaml_node_get_sequence(list), entry);

    reload_triggers(self);

    return clawt_config_get_trigger(self, id);
}

gboolean
clawt_config_remove_trigger(ClawtConfig *self, const gchar *id)
{
    YamlNode *list;
    YamlSequence *sequence;
    guint i;
    guint length;

    g_return_val_if_fail(self != NULL, FALSE);

    if (id == NULL)
        return FALSE;

    list = node_at_path(self->root, "triggers", FALSE);

    if (list == NULL || yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE)
        return FALSE;

    sequence = yaml_node_get_sequence(list);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        YamlMapping *mapping;

        if (element == NULL ||
            yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
            continue;

        mapping = yaml_node_get_mapping(element);

        if (g_strcmp0(member_string(mapping, "id"), id) != 0)
            continue;

        yaml_sequence_remove_element(sequence, i);
        reload_triggers(self);
        return TRUE;
    }

    return FALSE;
}
