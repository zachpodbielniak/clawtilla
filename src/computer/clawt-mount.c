/*
 * clawt-mount.c - A host path shared into an agent's computer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-mount.h"

#include <string.h>

struct _ClawtMount {
    gint            ref_count;

    gchar          *source;
    gchar          *target;
    gchar          *size;

    ClawtMountMode  mode;
    ClawtMountType  type;
    ClawtRelabel    relabel;

    gboolean        create;
    gboolean        required;

    /*
     * Who the mount is for, read only from `defaults.mounts`. A mount an
     * agent declared for itself is already agent-scoped, so these stay
     * at ALL/NULL there and clawt_mount_covers() answers TRUE.
     */
    ClawtScope      scope;
    GStrv           agents;
    GStrv           teams;
};

static ClawtMount *
clawt_mount_ref(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

G_DEFINE_BOXED_TYPE(ClawtMount, clawt_mount, clawt_mount_ref, clawt_mount_free)

ClawtMount *
clawt_mount_new(const gchar *source, const gchar *target)
{
    ClawtMount *self;

    g_return_val_if_fail(target != NULL, NULL);

    self = g_new0(ClawtMount, 1);
    self->ref_count = 1;
    self->source = g_strdup(source);
    self->target = g_strdup(target);

    /*
     * Read-only by default.  An agent that only needs to read your notes
     * should not be able to rewrite them, and the safe default is the one
     * that has to be overridden deliberately.
     */
    self->mode = CLAWT_MOUNT_MODE_RO;
    self->type = CLAWT_MOUNT_BIND;
    self->relabel = CLAWT_RELABEL_NONE;
    self->create = FALSE;
    self->required = TRUE;

    /*
     * ALL, so a mount that says nothing about scope is a default in the
     * ordinary sense of the word.
     *
     * SELECTED would be the fail-closed choice and is wrong here. The
     * safe direction for a *credential* is nobody -- which is why an
     * unrecognised integration scope reaches nobody -- but a list
     * somebody wrote under `defaults` should mean what the word says,
     * and a shared folder that silently reached no agent would look
     * exactly like the feature not working.
     *
     * The fail-closed rule still applies to a scope that was written and
     * could not be read: see mounts_from_node().
     */
    self->scope = CLAWT_SCOPE_ALL;

    return self;
}

ClawtMount *
clawt_mount_copy(ClawtMount *self)
{
    ClawtMount *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = clawt_mount_new(self->source, self->target);
    copy->size = g_strdup(self->size);
    copy->mode = self->mode;
    copy->type = self->type;
    copy->relabel = self->relabel;
    copy->create = self->create;
    copy->required = self->required;
    copy->scope = self->scope;
    copy->agents = g_strdupv(self->agents);
    copy->teams = g_strdupv(self->teams);

    return copy;
}

void
clawt_mount_free(ClawtMount *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->source);
    g_free(self->target);
    g_free(self->size);
    g_strfreev(self->agents);
    g_strfreev(self->teams);
    g_free(self);
}

const gchar *
clawt_mount_get_source(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->source;
}

const gchar *
clawt_mount_get_target(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->target;
}

ClawtMountMode
clawt_mount_get_mode(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_MOUNT_MODE_RO);
    return self->mode;
}

ClawtMountType
clawt_mount_get_mount_type(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_MOUNT_BIND);
    return self->type;
}

ClawtRelabel
clawt_mount_get_relabel(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_RELABEL_NONE);
    return self->relabel;
}

const gchar *
clawt_mount_get_size(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->size;
}

gboolean
clawt_mount_get_create(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, FALSE);
    return self->create;
}

gboolean
clawt_mount_get_required(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, TRUE);
    return self->required;
}

void
clawt_mount_set_mode(ClawtMount *self, ClawtMountMode mode)
{
    g_return_if_fail(self != NULL);
    self->mode = mode;
}

void
clawt_mount_set_mount_type(ClawtMount *self, ClawtMountType type)
{
    g_return_if_fail(self != NULL);
    self->type = type;
}

void
clawt_mount_set_relabel(ClawtMount *self, ClawtRelabel relabel)
{
    g_return_if_fail(self != NULL);
    self->relabel = relabel;
}

void
clawt_mount_set_size(ClawtMount *self, const gchar *size)
{
    g_return_if_fail(self != NULL);

    g_free(self->size);
    self->size = g_strdup(size);
}

void
clawt_mount_set_create(ClawtMount *self, gboolean create)
{
    g_return_if_fail(self != NULL);
    self->create = create;
}

void
clawt_mount_set_required(ClawtMount *self, gboolean required)
{
    g_return_if_fail(self != NULL);
    self->required = required;
}

gchar *
clawt_mount_resolved_source(ClawtMount *self)
{
    g_autofree gchar *expanded = NULL;
    gchar *real;

    g_return_val_if_fail(self != NULL, NULL);

    if (self->source == NULL)
        return NULL;

    expanded = clawt_expand_path(self->source);

    /*
     * realpath() rather than the literal string, because the resolved path
     * is what gets mounted and what has to be compared against the places an
     * agent must never reach.  A symlink in the source pointing at ~/.ssh is
     * a mount of ~/.ssh however the config spelled it.
     */
    real = realpath(expanded, NULL);
    if (real == NULL)
        return g_steal_pointer(&expanded);

    {
        gchar *owned = g_strdup(real);
        free(real);
        return owned;
    }
}

/*
 * Directories no mount may expose.  Owned here, set by the daemon.
 */
static gchar **forbidden_sources;

void
clawt_mount_set_forbidden_sources(const gchar * const *paths)
{
    g_strfreev(forbidden_sources);
    forbidden_sources = g_strdupv((gchar **)paths);
}

/*
 * Whether this mount would expose a directory that must stay private.
 *
 * Both directions are checked: mounting the state directory itself is
 * refused, and so is mounting any parent of it, since a parent exposes
 * everything below.
 */
static gboolean
source_is_forbidden(const gchar *source, const gchar **which)
{
    g_autofree gchar *resolved = clawt_canonicalize_missing(source);
    gsize i;

    if (resolved == NULL)
        return FALSE;

    for (i = 0; forbidden_sources != NULL && forbidden_sources[i] != NULL;
         i++) {
        g_autofree gchar *guarded =
            clawt_canonicalize_missing(forbidden_sources[i]);

        if (guarded == NULL)
            continue;

        if (clawt_path_is_within(resolved, guarded) ||
            clawt_path_is_within(guarded, resolved)) {
            *which = forbidden_sources[i];
            return TRUE;
        }
    }

    return FALSE;
}

gboolean
clawt_mount_validate(ClawtMount *self, GError **error)
{
    g_autofree gchar *expanded_source = NULL;

    g_return_val_if_fail(self != NULL, FALSE);

    if (self->target == NULL || self->target[0] == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_MOUNT,
                            "mount has no target");
        return FALSE;
    }

    /*
     * A relative target is meaningless: there is no working directory to
     * resolve it against inside a container that has not started yet.
     */
    if (!g_path_is_absolute(self->target)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_MOUNT,
                    "mount target '%s' must be an absolute path",
                    self->target);
        return FALSE;
    }

    if (g_strcmp0(self->target, "/") == 0) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_MOUNT,
                            "mount target must not be the root directory");
        return FALSE;
    }

    if (strstr(self->target, "/../") != NULL ||
        g_str_has_suffix(self->target, "/..")) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_MOUNT,
                    "mount target '%s' must not contain '..'",
                    self->target);
        return FALSE;
    }

    /*
     * Before the type dispatch, so it covers every kind of mount that has
     * a source.  It used to sit *inside* the tmpfs branch, guarded by
     * `self->source != NULL` -- which for a tmpfs is already an error two
     * lines below it.  So the one check standing between an agent and
     * `daemon.state_dir` ran only in a case that was refused anyway, and
     * never once for the bind mount somebody would actually write.  The
     * daemon populates the list at start and the header describes the
     * refusal; only the braces were wrong, which is why it read as
     * working.
     */
    if (self->source != NULL) {
        const gchar *which = NULL;

        if (source_is_forbidden(self->source, &which)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_MOUNT,
                        "'%s' cannot be mounted into a computer: it holds "
                        "%s, which contains every agent's token and "
                        "credentials", self->source, which);
            return FALSE;
        }
    }

    if (self->type == CLAWT_MOUNT_TMPFS) {
        /* A tmpfs starts empty, so a source would have nothing to mean. */
        if (self->source != NULL) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_MOUNT,
                                "a tmpfs mount takes no source");
            return FALSE;
        }
        return TRUE;
    }

    if (self->type == CLAWT_MOUNT_VOLUME) {
        if (self->source == NULL || self->source[0] == '\0') {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_MOUNT,
                                "a volume mount needs a volume name as its "
                                "source");
            return FALSE;
        }
        return TRUE;
    }

    if (self->source == NULL || self->source[0] == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_MOUNT,
                    "mount of '%s' has no source", self->target);
        return FALSE;
    }

    expanded_source = clawt_expand_path(self->source);

    if (!g_file_test(expanded_source, G_FILE_TEST_EXISTS)) {
        if (self->create) {
            if (!clawt_ensure_dir(expanded_source, 0700, error))
                return FALSE;
        } else if (self->required) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_MOUNT,
                        "mount source '%s' does not exist; set create: true "
                        "to have it made, or required: false to skip it",
                        expanded_source);
            return FALSE;
        }
    }

    return TRUE;
}

/* ── The name the guest mounts a share by ────────────────────────── */

/*
 * Readable, bounded, and stable.
 *
 * The three matter for different reasons.  Readable, because somebody
 * reading `findmnt` in the guest or `virsh dumpxml` on the host should
 * be able to tell which share they are looking at.  Bounded, because
 * qemu refuses a tag over 36 bytes and refuses the whole *device* with
 * it -- the domain does not start, and the error names a property nobody
 * set by hand.  Stable, because the tag is written into the guest's
 * fstab at first boot and into the domain XML on every provision: a tag
 * that moved would leave the guest mounting something that is not there
 * any more, and `nofail` makes that silent.
 *
 * The hash is always present rather than only when the readable part
 * had to be cut.  It is what makes two different targets produce two
 * different tags, and a branch that only runs for long paths is a branch
 * that is exercised by nobody until the day it matters.
 */
gchar *
clawt_mount_tag(const gchar *target)
{
    g_autofree gchar *digest = NULL;
    g_autoptr(GString) slug = NULL;
    const gchar *p;
    gsize keep;

    g_return_val_if_fail(target != NULL, NULL);

    digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, target, -1);

    /*
     * The common prefix carries no information -- everything clawtilla
     * mounts is under it -- and spending 14 of 36 bytes on it would push
     * the part that identifies the share off the end.
     */
    p = target;

    if (g_str_has_prefix(p, "/mnt/clawtilla/"))
        p += strlen("/mnt/clawtilla/");

    slug = g_string_new(NULL);

    for (; *p != '\0'; p++) {
        if (g_ascii_isalnum(*p) || *p == '-' || *p == '_' || *p == '.')
            g_string_append_c(slug, *p);
        else if (slug->len > 0 && slug->str[slug->len - 1] != '-')
            g_string_append_c(slug, '-');
    }

    while (slug->len > 0 && slug->str[slug->len - 1] == '-')
        g_string_truncate(slug, slug->len - 1);

    if (slug->len == 0)
        g_string_append(slug, "share");

    /* Seven for the hash and its separator, the rest for the name. */
    keep = CLAWT_MOUNT_TAG_MAX - 7;

    if (slug->len > keep)
        g_string_truncate(slug, keep);

    return g_strdup_printf("%s-%.6s", slug->str, digest);
}

GPtrArray *
clawt_mount_merge_defaults(GPtrArray *defaults, GPtrArray *own)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_mount_free);
    g_autoptr(GHashTable) taken = g_hash_table_new(g_str_hash, g_str_equal);
    guint i;

    /*
     * The agent's own targets first, so a default at the same path is
     * skipped rather than added beside it.  Both mounted at one path is
     * refused by validation, so the agent would stop starting -- and the
     * error would name a path the person had deliberately customised.
     */
    for (i = 0; own != NULL && i < own->len; i++) {
        ClawtMount *mount = g_ptr_array_index(own, i);
        const gchar *target = clawt_mount_get_target(mount);

        if (target != NULL)
            g_hash_table_add(taken, (gpointer)target);
    }

    for (i = 0; defaults != NULL && i < defaults->len; i++) {
        ClawtMount *mount = g_ptr_array_index(defaults, i);
        const gchar *target = clawt_mount_get_target(mount);

        if (target != NULL && g_hash_table_contains(taken, target))
            continue;

        g_ptr_array_add(out, clawt_mount_copy(mount));
    }

    for (i = 0; own != NULL && i < own->len; i++)
        g_ptr_array_add(out, clawt_mount_copy(g_ptr_array_index(own, i)));

    return out;
}

ClawtScope
clawt_mount_get_scope(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_SCOPE_ALL);

    return self->scope;
}

void
clawt_mount_set_scope(ClawtMount *self, ClawtScope scope)
{
    g_return_if_fail(self != NULL);

    self->scope = scope;
}

const gchar * const *
clawt_mount_get_agents(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return (const gchar *const *)self->agents;
}

void
clawt_mount_set_agents(ClawtMount *self, const gchar * const *agents)
{
    g_return_if_fail(self != NULL);

    g_strfreev(self->agents);
    self->agents = g_strdupv((GStrv)agents);
}

const gchar * const *
clawt_mount_get_teams(ClawtMount *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return (const gchar *const *)self->teams;
}

void
clawt_mount_set_teams(ClawtMount *self, const gchar * const *teams)
{
    g_return_if_fail(self != NULL);

    g_strfreev(self->teams);
    self->teams = g_strdupv((GStrv)teams);
}

gboolean
clawt_mount_covers(ClawtMount *self, const gchar *agent_id, const gchar *team)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return clawt_scope_covers(self->scope,
                              (const gchar *const *)self->agents,
                              (const gchar *const *)self->teams,
                              agent_id, team);
}

/* The team an agent is on, or NULL. An empty string is not a team. */
static const gchar *
agent_team(ClawtAgentConfig *agent)
{
    const gchar *team = clawt_agent_config_get_string(agent, "team");

    return (team != NULL && *team != '\0') ? team : NULL;
}

static gboolean
fleet_has_agent(GPtrArray *agents, const gchar *id)
{
    guint i;

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(agents, i);

        if (g_strcmp0(clawt_agent_config_get_id(agent), id) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * A team is one somebody declared *or* one an agent claims to be on.
 * Both, because clawt_mount_covers() matches on the string an agent
 * carries rather than on the declaration -- so warning about an
 * undeclared team that agents are nonetheless on would be a warning
 * contradicting what the daemon goes on to do. The team validator is
 * what says the declaration is missing; that is its job, not this one's.
 */
static gboolean
fleet_has_team(GPtrArray *teams, GPtrArray *agents, const gchar *id)
{
    guint i;

    for (i = 0; teams != NULL && i < teams->len; i++) {
        ClawtTeamSpec *spec = g_ptr_array_index(teams, i);

        if (g_strcmp0(spec->id, id) == 0)
            return TRUE;
    }

    for (i = 0; agents != NULL && i < agents->len; i++) {
        if (g_strcmp0(agent_team(g_ptr_array_index(agents, i)), id) == 0)
            return TRUE;
    }

    return FALSE;
}

void
clawt_mount_sort_scope(ClawtConfig          *config,
                       const gchar * const  *who,
                       GStrv                *out_agents,
                       GStrv                *out_teams)
{
    g_autoptr(GPtrArray) agents_out = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) teams_out = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) teams = NULL;
    GPtrArray *agents;
    guint i;

    if (out_agents != NULL)
        *out_agents = NULL;
    if (out_teams != NULL)
        *out_teams = NULL;

    g_return_if_fail(CLAWT_IS_CONFIG(config));

    if (who == NULL)
        return;

    teams = clawt_config_get_teams(config);
    agents = clawt_config_get_agents(config);

    for (i = 0; who[i] != NULL; i++) {
        g_autofree gchar *name = g_strdup(who[i]);

        g_strstrip(name);

        if (*name == '\0')
            continue;

        if (!fleet_has_agent(agents, name) &&
            fleet_has_team(teams, agents, name))
            g_ptr_array_add(teams_out, g_steal_pointer(&name));
        else
            g_ptr_array_add(agents_out, g_steal_pointer(&name));
    }

    if (out_agents != NULL && agents_out->len > 0) {
        g_ptr_array_add(agents_out, NULL);
        *out_agents = (GStrv)g_ptr_array_free(g_steal_pointer(&agents_out),
                                              FALSE);
    }

    if (out_teams != NULL && teams_out->len > 0) {
        g_ptr_array_add(teams_out, NULL);
        *out_teams = (GStrv)g_ptr_array_free(g_steal_pointer(&teams_out),
                                             FALSE);
    }
}

gboolean
clawt_mount_validate_fleet(ClawtConfig *config, GStrv *warnings)
{
    g_autoptr(GPtrArray) found = NULL;
    g_autoptr(GPtrArray) mounts = NULL;
    g_autoptr(GPtrArray) teams = NULL;
    GPtrArray *agents;
    guint i;

    if (warnings != NULL)
        *warnings = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), TRUE);

    found = g_ptr_array_new_with_free_func(g_free);
    mounts = clawt_config_get_default_mounts(config);
    teams = clawt_config_get_teams(config);
    agents = clawt_config_get_agents(config);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        const gchar *target = clawt_mount_get_target(mount);
        const gchar * const *named;
        guint said = found->len;
        guint j;
        gboolean reaches = FALSE;

        /*
         * `all` covers the fleet by construction and `none` is a parked
         * entry somebody chose to keep. Only `selected` can be wrong
         * about who it names.
         */
        if (clawt_mount_get_scope(mount) != CLAWT_SCOPE_SELECTED)
            continue;

        named = clawt_mount_get_agents(mount);

        for (j = 0; named != NULL && named[j] != NULL; j++) {
            if (fleet_has_agent(agents, named[j]))
                continue;

            if (fleet_has_team(teams, agents, named[j]))
                g_ptr_array_add(found, g_strdup_printf(
                    "shared folder %s lists '%s' under agents:, but '%s' is "
                    "a team. Only agent ids match there, so it reaches "
                    "nobody -- move it to teams:.",
                    target, named[j], named[j]));
            else
                g_ptr_array_add(found, g_strdup_printf(
                    "shared folder %s lists '%s' under agents:, which is "
                    "neither an agent nor a team.", target, named[j]));
        }

        named = clawt_mount_get_teams(mount);

        for (j = 0; named != NULL && named[j] != NULL; j++) {
            if (fleet_has_team(teams, agents, named[j]))
                continue;

            if (fleet_has_agent(agents, named[j]))
                g_ptr_array_add(found, g_strdup_printf(
                    "shared folder %s lists '%s' under teams:, but '%s' is "
                    "an agent. Only team ids match there, so it reaches "
                    "nobody -- move it to agents:.",
                    target, named[j], named[j]));
            else
                g_ptr_array_add(found, g_strdup_printf(
                    "shared folder %s lists '%s' under teams:, which is "
                    "neither a team nor an agent.", target, named[j]));
        }

        for (j = 0; agents != NULL && j < agents->len && !reaches; j++) {
            ClawtAgentConfig *agent = g_ptr_array_index(agents, j);

            reaches = clawt_mount_covers(mount,
                                         clawt_agent_config_get_id(agent),
                                         agent_team(agent));
        }

        /*
         * Only when nothing above already said why. An entry naming one
         * bad id has been explained; repeating the consequence would
         * make the useful line the second one.
         */
        if (!reaches && found->len == said)
            g_ptr_array_add(found, g_strdup_printf(
                "shared folder %s is scoped to selected agents and reaches "
                "none of them.", target));
    }

    if (found->len == 0)
        return TRUE;

    if (warnings != NULL) {
        g_ptr_array_add(found, NULL);
        *warnings = (GStrv)g_ptr_array_free(g_steal_pointer(&found), FALSE);
    }

    return FALSE;
}
