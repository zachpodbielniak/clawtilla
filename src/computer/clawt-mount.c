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

    if (self->type == CLAWT_MOUNT_TMPFS) {
        /* A tmpfs starts empty, so a source would have nothing to mean. */
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
