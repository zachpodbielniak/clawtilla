/*
 * clawt-selinux-level.c - Which SELinux MCS level a container is made at
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-selinux-level.h"

#include <string.h>

/*
 * How many random draws clawt_selinux_level_allocate() makes before it
 * walks every pair in order.  With a few dozen levels taken nearly every
 * draw is free, so the walk exists only to make "no pair left" an answer
 * rather than a loop that never ends.
 */
#define SELINUX_RANDOM_TRIES (4096)

/* The registered team function; see clawt_selinux_level_set_team_func(). */
G_LOCK_DEFINE_STATIC(team_func);
static ClawtSelinuxTeamLevelFunc team_func = NULL;
static gpointer team_func_data = NULL;

/*
 * The categories of @level into @cats, returning how many there are: 1
 * or 2 for a valid level, 0 for anything else.
 *
 * The same rule podomation's container module applies, written out here
 * rather than called because podomation keeps it private -- and because
 * the config has to be refusable without the module loaded at all.
 */
static gint
level_categories(const gchar *level, gint cats[2])
{
    const gchar *p;
    gint count = 0;

    if (level == NULL || !g_str_has_prefix(level, "s0:"))
        return 0;

    p = level + 3;

    while (TRUE) {
        gint value = 0;
        gint digits = 0;

        if (*p != 'c')
            return 0;
        p++;

        /* No leading zeros: "c0" is fine, "c01" is not. */
        if (*p == '0' && g_ascii_isdigit(p[1]))
            return 0;

        while (g_ascii_isdigit(*p)) {
            value = value * 10 + (*p - '0');
            digits++;
            p++;

            if (digits > 4 || value > CLAWT_SELINUX_CATEGORY_MAX)
                return 0;
        }

        if (digits == 0)
            return 0;

        cats[count++] = value;

        if (*p == '\0')
            break;

        /* A comma may follow only the first category. */
        if (*p != ',' || count >= 2)
            return 0;
        p++;
    }

    if (count == 2 && cats[0] >= cats[1])
        return 0;

    return count;
}

gboolean
clawt_selinux_level_is_valid(const gchar *level)
{
    gint cats[2];

    return level_categories(level, cats) > 0;
}

gchar *
clawt_selinux_level_of_label(const gchar *label)
{
    g_auto(GStrv) fields = NULL;

    if (label == NULL || *label == '\0')
        return NULL;

    /*
     * user:role:type:level, and the level itself has a colon in it
     * ("s0:c1,c2"), so the split stops at four fields and the last one
     * is the whole level.  Anything that is not a single valid level --
     * a range such as spc_t's "s0-s0:c0.c1023", or no level at all -- is
     * not one this module ever asks for, and is answered as unknown.
     */
    fields = g_strsplit(label, ":", 4);

    if (g_strv_length(fields) != 4 ||
        !clawt_selinux_level_is_valid(fields[3]))
        return NULL;

    return g_strdup(fields[3]);
}

gboolean
clawt_selinux_level_parse(const gchar            *value,
                          ClawtSelinuxLevelMode  *mode,
                          GError                **error)
{
    ClawtSelinuxLevelMode found;

    if (value == NULL || *value == '\0' || g_strcmp0(value, "auto") == 0) {
        found = CLAWT_SELINUX_LEVEL_AUTO;
    } else if (g_strcmp0(value, "random") == 0) {
        found = CLAWT_SELINUX_LEVEL_RANDOM;
    } else if (clawt_selinux_level_is_valid(value)) {
        found = CLAWT_SELINUX_LEVEL_EXPLICIT;
    } else {
        g_autofree gchar *lower = g_ascii_strdown(value, -1);

        /*
         * Refused by name rather than as one more malformed level.
         * Somebody writing "disable" has usually just been told on a
         * forum that label=disable makes the permission error go away --
         * and it does, by switching SELinux off for that container
         * entirely, which is not what this key is for.
         */
        if (strstr(lower, "disable") != NULL)
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "%s: '%s' is refused: label=disable would turn "
                        "SELinux confinement off for the whole container "
                        "rather than choose its level. Use auto, random, "
                        "or a level such as s0:c100,c200",
                        CLAWT_SELINUX_LEVEL_KEY, value);
        else
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "%s: '%s' is not auto, random or an MCS level such "
                        "as s0:c100,c200 -- one or two categories in "
                        "c0..c%d, the second larger than the first, with "
                        "no leading zeros", CLAWT_SELINUX_LEVEL_KEY, value,
                        CLAWT_SELINUX_CATEGORY_MAX);
        return FALSE;
    }

    if (mode != NULL)
        *mode = found;

    return TRUE;
}

/*
 * The taken levels, parsed once: a single category blocks every pair
 * holding it, a pair blocks only itself.  A pair (a, b) can read files at
 * a level exactly when it holds all of that level's categories, so those
 * are the two ways a new pair could reach somebody else's files.
 */
typedef struct {
    gboolean  single[CLAWT_SELINUX_CATEGORY_MAX + 1];
    GArray   *pairs;    /* gint, two per taken pair */
} Taken;

static void
taken_init(Taken *self, const gchar * const *taken)
{
    gsize i;

    memset(self->single, 0, sizeof(self->single));
    self->pairs = g_array_new(FALSE, FALSE, sizeof(gint));

    /* An unparseable level blocks nothing; there is no level to meet. */
    for (i = 0; taken != NULL && taken[i] != NULL; i++) {
        gint cats[2];
        gint count = level_categories(taken[i], cats);

        if (count == 1)
            self->single[cats[0]] = TRUE;
        else if (count == 2)
            g_array_append_vals(self->pairs, cats, 2);
    }
}

/* Whether the pair @a < @b is clear of every taken level. */
static gboolean
pair_is_free(const Taken *self, gint a, gint b)
{
    guint i;

    if (self->single[a] || self->single[b])
        return FALSE;

    for (i = 0; i + 1 < self->pairs->len; i += 2) {
        if (g_array_index(self->pairs, gint, i) == a &&
            g_array_index(self->pairs, gint, i + 1) == b)
            return FALSE;
    }

    return TRUE;
}

gchar *
clawt_selinux_level_allocate(const gchar * const *taken, GRand *rand)
{
    g_autoptr(GRand) own = NULL;
    Taken blocked;
    gchar *found = NULL;
    gint tries;
    gint total;
    gint start;
    gint n;

    if (rand == NULL) {
        own = g_rand_new();
        rand = own;
    }

    taken_init(&blocked, taken);

    /*
     * Random first, so two fleets -- or two runs of one fleet after the
     * store was deleted -- do not all land on c0,c1 and become each
     * other's neighbours by construction.
     */
    for (tries = 0; tries < SELINUX_RANDOM_TRIES && found == NULL; tries++) {
        gint a = g_rand_int_range(rand, 0, CLAWT_SELINUX_CATEGORY_MAX + 1);
        gint b = g_rand_int_range(rand, 0, CLAWT_SELINUX_CATEGORY_MAX + 1);

        if (a == b)
            continue;

        if (a > b) {
            gint swap = a;

            a = b;
            b = swap;
        }

        if (pair_is_free(&blocked, a, b))
            found = g_strdup_printf("s0:c%d,c%d", a, b);
    }

    /*
     * Then every pair, from a random place, so "none left" is a real
     * answer.  The pairs a < b are numbered 0..total-1 row by row: row a
     * holds (a, a+1) .. (a, 1023).
     */
    total = (CLAWT_SELINUX_CATEGORY_MAX + 1) * CLAWT_SELINUX_CATEGORY_MAX / 2;
    start = g_rand_int_range(rand, 0, total);

    {
        gint a = 0;
        gint b;
        gint row = CLAWT_SELINUX_CATEGORY_MAX;

        /* The starting pair, found once; then stepped, wrapping at the end. */
        while (start >= row) {
            start -= row;
            a++;
            row--;
        }
        b = a + 1 + start;

        for (n = 0; n < total && found == NULL; n++) {
            if (pair_is_free(&blocked, a, b))
                found = g_strdup_printf("s0:c%d,c%d", a, b);

            if (++b > CLAWT_SELINUX_CATEGORY_MAX) {
                a = (a + 1 < CLAWT_SELINUX_CATEGORY_MAX) ? a + 1 : 0;
                b = a + 1;
            }
        }
    }

    g_array_unref(blocked.pairs);

    return found;
}

void
clawt_selinux_level_set_team_func(ClawtSelinuxTeamLevelFunc func,
                                  gpointer                  user_data)
{
    G_LOCK(team_func);
    team_func = func;
    team_func_data = (func != NULL) ? user_data : NULL;
    G_UNLOCK(team_func);
}

void
clawt_selinux_level_clear_team_func(gpointer user_data)
{
    G_LOCK(team_func);

    if (team_func_data == user_data) {
        team_func = NULL;
        team_func_data = NULL;
    }

    G_UNLOCK(team_func);
}

/* The agent's team, or NULL: an empty string is not a team. */
static const gchar *
agent_team(ClawtAgentConfig *agent_config)
{
    const gchar *team = clawt_agent_config_get_string(agent_config, "team");

    return (team != NULL && *team != '\0') ? team : NULL;
}

const gchar *
clawt_selinux_level_machine_team(ClawtAgentConfig *agent_config,
                                 GPtrArray        *users)
{
    const gchar *team;
    guint i;

    g_return_val_if_fail(agent_config != NULL, NULL);

    if (users == NULL || users->len == 0)
        return agent_team(agent_config);

    /*
     * One team for the whole machine, or none.  Taking the first user's
     * would put the second team's agent at the first team's level, and
     * its own team's containers could no longer read what it wrote.
     */
    team = agent_team(g_ptr_array_index(users, 0));

    for (i = 1; team != NULL && i < users->len; i++) {
        if (g_strcmp0(agent_team(g_ptr_array_index(users, i)), team) != 0)
            return NULL;
    }

    return team;
}

gchar *
clawt_selinux_level_wanted(ClawtAgentConfig  *agent_config,
                           GPtrArray         *users,
                           GError           **error)
{
    const gchar *value;
    const gchar *team;
    ClawtSelinuxLevelMode mode = CLAWT_SELINUX_LEVEL_AUTO;
    ClawtSelinuxTeamLevelFunc func;
    gpointer data;
    g_autoptr(GError) local = NULL;
    g_autofree gchar *level = NULL;

    g_return_val_if_fail(agent_config != NULL, NULL);

    value = clawt_agent_config_get_string(agent_config,
                                          CLAWT_SELINUX_LEVEL_KEY);

    if (!clawt_selinux_level_parse(value, &mode, error))
        return NULL;

    if (mode == CLAWT_SELINUX_LEVEL_EXPLICIT)
        return g_strdup(value);

    if (mode == CLAWT_SELINUX_LEVEL_RANDOM)
        return NULL;

    team = clawt_selinux_level_machine_team(agent_config, users);

    if (team == NULL)
        return NULL;

    /* Copied out, so the store is not asked while the lock is held. */
    G_LOCK(team_func);
    func = team_func;
    data = team_func_data;
    G_UNLOCK(team_func);

    if (func == NULL)
        return NULL;

    level = func(team, data, &local);

    /*
     * A level the store cannot give is not a reason to refuse the
     * machine: it then comes up exactly as it did before this key
     * existed, and the warning names the team and the reason so the
     * shared-folder trap is not a surprise.
     */
    if (level == NULL || !clawt_selinux_level_is_valid(level)) {
        g_warning("agent %s: team %s has no usable SELinux level (%s); its "
                  "container gets podman's random pair, so files it writes "
                  "into a shared folder may be unreadable in its "
                  "teammates' containers",
                  clawt_agent_config_get_id(agent_config), team,
                  local != NULL ? local->message
                  : level != NULL ? "the stored level is malformed"
                  : "no reason given");
        return NULL;
    }

    return g_steal_pointer(&level);
}

gchar *
clawt_selinux_level_effective(ClawtAgentConfig *agent_config,
                              GPtrArray        *users,
                              GHashTable       *team_levels)
{
    const gchar *value;
    const gchar *team;
    const gchar *stored;
    ClawtSelinuxLevelMode mode = CLAWT_SELINUX_LEVEL_AUTO;

    g_return_val_if_fail(agent_config != NULL, NULL);

    value = clawt_agent_config_get_string(agent_config,
                                          CLAWT_SELINUX_LEVEL_KEY);

    if (!clawt_selinux_level_parse(value, &mode, NULL))
        return NULL;

    if (mode == CLAWT_SELINUX_LEVEL_EXPLICIT)
        return g_strdup(value);

    if (mode == CLAWT_SELINUX_LEVEL_RANDOM)
        return NULL;

    team = clawt_selinux_level_machine_team(agent_config, users);

    if (team == NULL)
        return NULL;

    stored = (team_levels != NULL) ? g_hash_table_lookup(team_levels, team)
                                   : NULL;

    return (stored != NULL) ? g_strdup(stored)
                            : g_strdup_printf("team:%s", team);
}

GStrv
clawt_selinux_level_explicit_levels(ClawtConfig *config)
{
    g_autoptr(GHashTable) seen = NULL;
    g_autoptr(GPtrArray) ids = NULL;
    GPtrArray *agents;
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

    seen = g_hash_table_new(g_str_hash, g_str_equal);
    out = g_ptr_array_new();

    /*
     * The agents first, read through their own getter so an agent on a
     * shared machine reports the machine's level; then every shared
     * machine directly, for one nobody has joined yet -- its level is
     * still in the file and still a level a team pair must not take.
     */
    agents = clawt_config_get_agents(config);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        const gchar *value = clawt_agent_config_get_string(
            g_ptr_array_index(agents, i), CLAWT_SELINUX_LEVEL_KEY);

        if (clawt_selinux_level_is_valid(value) &&
            g_hash_table_add(seen, (gpointer)value))
            g_ptr_array_add(out, g_strdup(value));
    }

    ids = clawt_config_get_computer_ids(config);

    for (i = 0; i < ids->len; i++) {
        const gchar *value = clawt_config_get_computer_string(
            config, g_ptr_array_index(ids, i), "container.selinux_level");

        if (clawt_selinux_level_is_valid(value) &&
            g_hash_table_add(seen, (gpointer)value))
            g_ptr_array_add(out, g_strdup(value));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}

/* ── The shared-folder warning ───────────────────────────────────── */

/*
 * One container: an agent's own, or a shared machine with every agent
 * on it.  The agents of one machine never warn about each other -- they
 * are one container, at one level, whatever it is.
 */
typedef struct {
    gchar     *key;       /* "a:<agent>" or "c:<computer>" */
    gchar     *shared;    /* the computers: id, or NULL */
    GPtrArray *users;     /* ClawtAgentConfig *, not owned */
    gchar     *level;     /* clawt_selinux_level_effective(); NULL = random */
    gchar     *said;      /* how the level is described in a warning */
    GPtrArray *mounts;    /* ClawtMount *, owned */
} Machine;

static void
machine_free(gpointer data)
{
    Machine *machine = data;

    g_free(machine->key);
    g_free(machine->shared);
    g_clear_pointer(&machine->users, g_ptr_array_unref);
    g_free(machine->level);
    g_free(machine->said);
    g_clear_pointer(&machine->mounts, g_ptr_array_unref);
    g_free(machine);
}

/* "forge-a" or "computer 'build' (forge-a, forge-b)". */
static gchar *
machine_name(Machine *machine)
{
    g_autoptr(GString) out = g_string_new(NULL);
    guint i;

    if (machine->shared == NULL)
        return g_strdup(clawt_agent_config_get_id(
            g_ptr_array_index(machine->users, 0)));

    g_string_append_printf(out, "computer '%s' (", machine->shared);

    for (i = 0; i < machine->users->len; i++)
        g_string_append_printf(out, "%s%s", i > 0 ? ", " : "",
                               clawt_agent_config_get_id(
                                   g_ptr_array_index(machine->users, i)));

    g_string_append_c(out, ')');

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/* The level as a person reads it, for the warning. */
static gchar *
machine_level_said(Machine *machine)
{
    ClawtAgentConfig *first = g_ptr_array_index(machine->users, 0);
    const gchar *value = clawt_agent_config_get_string(
        first, CLAWT_SELINUX_LEVEL_KEY);
    ClawtSelinuxLevelMode mode = CLAWT_SELINUX_LEVEL_AUTO;
    const gchar *team;

    clawt_selinux_level_parse(value, &mode, NULL);

    if (mode == CLAWT_SELINUX_LEVEL_EXPLICIT)
        return g_strdup(value);

    team = (mode == CLAWT_SELINUX_LEVEL_AUTO)
           ? clawt_selinux_level_machine_team(
                 first, machine->shared != NULL ? machine->users : NULL)
           : NULL;

    if (team == NULL)
        return g_strdup("podman's random pair");

    if (machine->level != NULL && !g_str_has_prefix(machine->level, "team:"))
        return g_strdup_printf("team %s's %s", team, machine->level);

    return g_strdup_printf("team %s's level", team);
}

/*
 * The mounts a machine is built with, as far as the configuration says:
 * its own, and the shared folders that reach any agent on it.  The same
 * rule apply_mounts() and apply_shared_mounts() use, minus the workspace
 * and exchange the daemon adds -- which this warning leaves out on
 * purpose.
 */
static GPtrArray *
machine_mounts(Machine *machine, GPtrArray *defaults)
{
    ClawtAgentConfig *first = g_ptr_array_index(machine->users, 0);
    g_autoptr(GPtrArray) own = clawt_agent_config_get_mounts(first);
    g_autoptr(GPtrArray) mine = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_mount_free);
    guint d;

    if (defaults == NULL ||
        !clawt_agent_config_get_boolean(first, "computer.default_mounts"))
        return g_ptr_array_ref(own);

    for (d = 0; d < defaults->len; d++) {
        ClawtMount *candidate = g_ptr_array_index(defaults, d);
        guint u;

        for (u = 0; u < machine->users->len; u++) {
            ClawtAgentConfig *user = g_ptr_array_index(machine->users, u);

            if (clawt_mount_covers(candidate,
                                   clawt_agent_config_get_id(user),
                                   agent_team(user))) {
                g_ptr_array_add(mine, clawt_mount_copy(candidate));
                break;
            }
        }
    }

    return clawt_mount_merge_defaults(mine, own);
}

/*
 * Whether @inner is @outer or a path beneath it.  Compared on path
 * components, so /src/a is not inside /src/ab.
 */
static gboolean
path_within(const gchar *inner, const gchar *outer)
{
    gsize length = strlen(outer);

    if (g_strcmp0(inner, outer) == 0)
        return TRUE;

    if (length == 0 || strncmp(inner, outer, length) != 0)
        return FALSE;

    return outer[length - 1] == '/' || inner[length] == '/';
}

/* Same level, so a file one writes the other reads.  Random never is. */
static gboolean
levels_match(Machine *a, Machine *b)
{
    return a->level != NULL && b->level != NULL &&
           g_strcmp0(a->level, b->level) == 0;
}

/* One source the trap applies to, and who is caught in it. */
typedef struct {
    gchar      *source;
    GPtrArray  *machines;   /* Machine *, not owned, each once */
} Trap;

static void
trap_free(gpointer data)
{
    Trap *trap = data;

    g_free(trap->source);
    g_ptr_array_unref(trap->machines);
    g_free(trap);
}

static void
trap_add_machine(Trap *trap, Machine *machine)
{
    guint i;

    for (i = 0; i < trap->machines->len; i++) {
        if (g_ptr_array_index(trap->machines, i) == machine)
            return;
    }

    g_ptr_array_add(trap->machines, machine);
}

/*
 * The trap for @source, made the first time it is needed.  Keyed on the
 * outer of two nested sources, so ~/source and ~/source/clawtilla shared
 * at different levels are one warning about ~/source rather than one per
 * spelling.
 */
static Trap *
trap_for(GPtrArray *traps, const gchar *source)
{
    Trap *trap;
    guint i;

    for (i = 0; i < traps->len; i++) {
        trap = g_ptr_array_index(traps, i);

        if (g_strcmp0(trap->source, source) == 0)
            return trap;
    }

    trap = g_new0(Trap, 1);
    trap->source = g_strdup(source);
    trap->machines = g_ptr_array_new();
    g_ptr_array_add(traps, trap);

    return trap;
}

/* Every machine with a container computer, grouped by who shares one. */
static GPtrArray *
collect_machines(ClawtConfig *config, GHashTable *team_levels)
{
    GPtrArray *machines = g_ptr_array_new_with_free_func(machine_free);
    g_autoptr(GPtrArray) defaults = NULL;
    GPtrArray *agents = clawt_config_get_agents(config);
    guint i;

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(agents, i);
        const gchar *use;
        g_autofree gchar *key = NULL;
        Machine *machine = NULL;
        guint m;

        /* A disabled agent starts nothing, so it shares nothing. */
        if (clawt_agent_config_is_shadow(agent))
            continue;

        if (clawt_agent_config_get_enum(agent, "computer.type") !=
            CLAWT_COMPUTER_CONTAINER)
            continue;

        use = clawt_agent_config_get_string(agent, "computer.use");
        key = (use != NULL && *use != '\0')
              ? g_strdup_printf("c:%s", use)
              : g_strdup_printf("a:%s", clawt_agent_config_get_id(agent));

        for (m = 0; m < machines->len && machine == NULL; m++) {
            Machine *candidate = g_ptr_array_index(machines, m);

            if (g_strcmp0(candidate->key, key) == 0)
                machine = candidate;
        }

        if (machine == NULL) {
            machine = g_new0(Machine, 1);
            machine->key = g_steal_pointer(&key);
            machine->shared = (use != NULL && *use != '\0') ? g_strdup(use)
                                                            : NULL;
            machine->users = g_ptr_array_new();
            g_ptr_array_add(machines, machine);
        }

        g_ptr_array_add(machine->users, agent);
    }

    /*
     * One machine has nobody to disagree with, so nothing more is read.
     * The shared folders in particular: reading them says out loud what
     * is wrong with each (a scope that is not one), and that is the
     * daemon's to say once when it builds a computer, not this check's
     * to repeat for a fleet it has nothing to report about.
     */
    if (machines->len < 2)
        return machines;

    defaults = clawt_config_get_default_mounts(config);

    /* Settled once every user is known: a shared machine's team needs all. */
    for (i = 0; i < machines->len; i++) {
        Machine *machine = g_ptr_array_index(machines, i);
        ClawtAgentConfig *first = g_ptr_array_index(machine->users, 0);

        machine->level = clawt_selinux_level_effective(
            first, machine->shared != NULL ? machine->users : NULL,
            team_levels);
        machine->said = machine_level_said(machine);
        machine->mounts = machine_mounts(machine, defaults);
    }

    return machines;
}

/*
 * Whether @a and @b meet on a host source that at least one of them can
 * write, recording the outer source in @traps when they do.  One writer
 * is enough: a reader at another level cannot open what the writer made.
 */
static void
compare_machines(Machine *a, Machine *b, GPtrArray *traps)
{
    guint i;

    for (i = 0; i < a->mounts->len; i++) {
        ClawtMount *ma = g_ptr_array_index(a->mounts, i);
        g_autofree gchar *sa = NULL;
        guint j;

        if (clawt_mount_get_mount_type(ma) != CLAWT_MOUNT_BIND)
            continue;

        sa = clawt_mount_resolved_source(ma);
        if (sa == NULL)
            continue;

        for (j = 0; j < b->mounts->len; j++) {
            ClawtMount *mb = g_ptr_array_index(b->mounts, j);
            g_autofree gchar *sb = NULL;
            const gchar *outer;
            Trap *trap;

            if (clawt_mount_get_mount_type(mb) != CLAWT_MOUNT_BIND)
                continue;

            if (clawt_mount_get_mode(ma) != CLAWT_MOUNT_MODE_RW &&
                clawt_mount_get_mode(mb) != CLAWT_MOUNT_MODE_RW)
                continue;

            sb = clawt_mount_resolved_source(mb);
            if (sb == NULL)
                continue;

            if (path_within(sa, sb))
                outer = sb;
            else if (path_within(sb, sa))
                outer = sa;
            else
                continue;

            trap = trap_for(traps, outer);
            trap_add_machine(trap, a);
            trap_add_machine(trap, b);
        }
    }
}

GStrv
clawt_selinux_level_shared_mount_warnings(ClawtConfig *config,
                                          GHashTable  *team_levels)
{
    g_autoptr(GPtrArray) machines = NULL;
    g_autoptr(GPtrArray) traps = NULL;
    GPtrArray *out;
    guint i;
    guint j;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

    machines = collect_machines(config, team_levels);
    traps = g_ptr_array_new_with_free_func(trap_free);

    for (i = 0; i < machines->len; i++) {
        for (j = i + 1; j < machines->len; j++) {
            Machine *a = g_ptr_array_index(machines, i);
            Machine *b = g_ptr_array_index(machines, j);

            if (!levels_match(a, b))
                compare_machines(a, b, traps);
        }
    }

    if (traps->len == 0)
        return NULL;

    out = g_ptr_array_new();

    for (i = 0; i < traps->len; i++) {
        Trap *trap = g_ptr_array_index(traps, i);
        g_autoptr(GString) who = g_string_new(NULL);

        for (j = 0; j < trap->machines->len; j++) {
            Machine *machine = g_ptr_array_index(trap->machines, j);
            g_autofree gchar *name = machine_name(machine);

            g_string_append_printf(who, "%s%s at %s",
                                   j > 0 ? ", " : "", name, machine->said);
        }

        g_ptr_array_add(out, g_strdup_printf(
            "SELinux labelling: %s is a shared folder of container "
            "computers at different SELinux levels -- %s. A file written "
            "into it from inside one of them is labelled with that "
            "container's level and cannot be read from inside the others, "
            "even at mode 0644: a labelling problem, not a permissions one. "
            "Put them on one level with %s (agents on one team share their "
            "team's level under auto), then recreate their containers",
            trap->source, who->str, CLAWT_SELINUX_LEVEL_KEY));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}
