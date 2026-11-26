/*
 * clawt-selinux-level.h - Which SELinux MCS level a container is made at
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * podman gives every container a random pair of MCS categories, and a
 * file created inside one container is labelled with that container's
 * pair.  On a bind mount several containers share -- ~/source, relabelled
 * :z for exactly that purpose -- a file one agent writes from inside its
 * container is then unreadable from inside every other one, even at mode
 * 0644: it reads as a permissions bug and is a labelling one.
 *
 * computer.container.selinux_level decides the level instead: a team's
 * containers share one stored pair by default, so a team working on one
 * tree can read what each of them writes.  Everything here that decides a
 * level is a pure function of the configuration and of the team pairs
 * passed in; the one piece of state, the stored pairs, belongs to the
 * daemon and is reached through clawt_selinux_level_set_team_func().
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * CLAWT_SELINUX_LEVEL_KEY:
 *
 * The agent-relative config key, spelled once.
 */
#define CLAWT_SELINUX_LEVEL_KEY "computer.container.selinux_level"

/**
 * CLAWT_SELINUX_CATEGORY_MAX:
 *
 * The highest MCS category podman's default policy allows (c0..c1023).
 */
#define CLAWT_SELINUX_CATEGORY_MAX (1023)

/**
 * ClawtSelinuxLevelMode:
 * @CLAWT_SELINUX_LEVEL_AUTO: the agent's team's stored pair; podman's
 *   random pair for an agent on no team
 * @CLAWT_SELINUX_LEVEL_RANDOM: podman's random pair, always
 * @CLAWT_SELINUX_LEVEL_EXPLICIT: the level the configuration names
 *
 * The three forms computer.container.selinux_level takes.
 */
typedef enum {
    CLAWT_SELINUX_LEVEL_AUTO = 0,
    CLAWT_SELINUX_LEVEL_RANDOM,
    CLAWT_SELINUX_LEVEL_EXPLICIT
} ClawtSelinuxLevelMode;

/**
 * ClawtSelinuxTeamLevelFunc:
 * @team: the team that needs its level
 * @user_data: what was registered with the function
 * @error: return location for a #GError
 *
 * Answers a team's stored level, allocating and storing one the first
 * time the team needs it.
 *
 * Returns: (transfer full) (nullable): the level, or %NULL with @error set
 */
typedef gchar *(*ClawtSelinuxTeamLevelFunc)(const gchar  *team,
                                            gpointer      user_data,
                                            GError      **error);

/**
 * clawt_selinux_level_is_valid:
 * @level: (nullable): a candidate level
 *
 * Whether @level is exactly "s0:cN" or "s0:cN,cM": decimal categories in
 * 0..1023 with no leading zeros ("c0" is the only zero form) and N < M.
 * The rule podomation's container module applies to its selinux_level
 * parameter, so nothing this accepts is refused there.
 *
 * Returns: %TRUE when podman would be handed a level it accepts
 */
gboolean clawt_selinux_level_is_valid(const gchar *level);

/**
 * clawt_selinux_level_of_label:
 * @label: (nullable): a process label as podman reports it, such as
 *   "system_u:system_r:container_t:s0:c100,c200"
 *
 * The MCS level a container actually runs at, read from the
 * ProcessLabel of `podman container inspect`, so an existing container
 * can be compared with the level it should have.
 *
 * Returns: (transfer full) (nullable): the level, such as "s0:c100,c200",
 *   or %NULL when @label is empty (a host without SELinux) or carries
 *   anything but one valid level
 */
gchar *clawt_selinux_level_of_label(const gchar *label);

/**
 * clawt_selinux_level_parse:
 * @value: (nullable): computer.container.selinux_level as written;
 *   %NULL or "" mean "auto"
 * @mode: (out) (optional): which form it is
 * @error: return location for a #GError
 *
 * Reads the key.  "disable" in any spelling is refused by name: podman's
 * label=disable turns SELinux confinement off for the whole container,
 * which is a different decision from choosing its level.
 *
 * Returns: %TRUE when @value is auto, random or a valid level
 */
gboolean clawt_selinux_level_parse(const gchar            *value,
                                   ClawtSelinuxLevelMode  *mode,
                                   GError                **error);

/**
 * clawt_selinux_level_allocate:
 * @taken: (nullable) (array zero-terminated=1): every level already in
 *   use -- the other teams' stored pairs and every explicit level in the
 *   configuration
 * @rand: (nullable): the generator to draw from, so a test can seed it;
 *   %NULL for a fresh one
 *
 * Picks a new pair of two distinct categories in c0..c1023.  The pair
 * neither equals nor dominates any level in @taken: a pair holding both
 * of another pair's categories -- or the one category of a single-category
 * level -- could read that level's files, which would defeat keeping the
 * two apart.
 *
 * Returns: (transfer full) (nullable): "s0:cN,cM", or %NULL when no pair
 *   is left
 */
gchar *clawt_selinux_level_allocate(const gchar * const *taken,
                                    GRand               *rand);

/**
 * clawt_selinux_level_set_team_func:
 * @func: (nullable) (scope forever): what answers a team's level
 * @user_data: (nullable): passed to @func
 *
 * Registers where a team's stored level comes from.  The daemon owns the
 * store, so it registers itself; with nothing registered an agent under
 * "auto" gets podman's random pair, which is what a container computer
 * built outside a daemon has always had.
 */
void clawt_selinux_level_set_team_func(ClawtSelinuxTeamLevelFunc func,
                                       gpointer                  user_data);

/**
 * clawt_selinux_level_clear_team_func:
 * @user_data: (nullable): the data the function was registered with
 *
 * Unregisters the team function, but only when it was registered with
 * @user_data -- so a daemon going away cannot clear another one's.
 */
void clawt_selinux_level_clear_team_func(gpointer user_data);

/**
 * clawt_selinux_level_machine_team:
 * @agent_config: the agent, or any agent on a shared machine
 * @users: (nullable) (element-type ClawtAgentConfig): every agent on the
 *   shared machine, or %NULL for an agent's own one
 *
 * The team whose level a machine takes under "auto": the agent's own
 * team, or for a shared machine the one team every agent on it is on.
 * Agents on different teams share no team pair, so their machine gets
 * none.
 *
 * Returns: (transfer none) (nullable): the team, or %NULL for none
 */
const gchar *clawt_selinux_level_machine_team(ClawtAgentConfig *agent_config,
                                              GPtrArray        *users);

/**
 * clawt_selinux_level_wanted:
 * @agent_config: the agent, or any agent on a shared machine
 * @users: (nullable) (element-type ClawtAgentConfig): every agent on the
 *   shared machine, or %NULL for an agent's own one
 * @error: return location for a #GError
 *
 * The level a container built from @agent_config is to be created at:
 * the explicit level, the team's stored pair under "auto", or %NULL for
 * podman's random pair.  A team pair the registered function cannot
 * provide -- its store would not open -- is logged and answered with
 * %NULL rather than failing the start: the container then has what it
 * had before this key existed, and the warning says why.
 *
 * Returns: (transfer full) (nullable): the level, or %NULL for random;
 *   %NULL with @error set when the key itself is unusable
 */
gchar *clawt_selinux_level_wanted(ClawtAgentConfig  *agent_config,
                                  GPtrArray         *users,
                                  GError           **error);

/**
 * clawt_selinux_level_effective:
 * @agent_config: the agent, or any agent on a shared machine
 * @users: (nullable) (element-type ClawtAgentConfig): every agent on the
 *   shared machine, or %NULL for an agent's own one
 * @team_levels: (nullable) (element-type utf8 utf8): team id to stored
 *   level, for the teams that have one
 *
 * What the machine's level is, for comparing two machines or one machine
 * across a reload -- without allocating anything.  An explicit level is
 * itself; a team under "auto" is its stored pair when @team_levels has
 * one and "team:<id>" when it does not (no two teams share a pair, so
 * the name is as good as the pair for telling them apart); podman's
 * random pair is %NULL.
 *
 * Returns: (transfer full) (nullable): the level or "team:<id>", or %NULL
 *   for random or an unusable value
 */
gchar *clawt_selinux_level_effective(ClawtAgentConfig *agent_config,
                                     GPtrArray        *users,
                                     GHashTable       *team_levels);

/**
 * clawt_selinux_level_explicit_levels:
 * @config: the fleet's configuration
 *
 * Every explicit level the configuration names, on agents and on shared
 * machines alike: what a newly allocated team pair has to stay clear of.
 *
 * Returns: (transfer full) (array zero-terminated=1): the levels, each
 *   once; empty when there are none
 */
GStrv clawt_selinux_level_explicit_levels(ClawtConfig *config);

/**
 * clawt_selinux_level_shared_mount_warnings:
 * @config: the fleet's configuration
 * @team_levels: (nullable) (element-type utf8 utf8): team id to stored
 *   level, when the caller has the store; %NULL compares teams by name
 *
 * The labelling trap, found before it bites: two container computers
 * that bind the same host directory -- or one inside the other -- at
 * different SELinux levels, with at least one of them able to write it.
 * A file written inside one is unreadable inside the other.  Podman's
 * random pair always counts as different, including from another random
 * one.
 *
 * Only the mounts the configuration declares are compared: the agent's
 * own and the shared folders that reach it.  clawtilla's own per-agent
 * workspace and exchange mounts are left out -- the workspace belongs to
 * one agent, and the exchange is a known limitation the documentation
 * states.
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): one
 *   warning per mount source, naming it and the agents; %NULL when there
 *   is nothing to say
 */
GStrv clawt_selinux_level_shared_mount_warnings(ClawtConfig *config,
                                                GHashTable  *team_levels);

G_END_DECLS
