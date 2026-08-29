/*
 * clawt-skill.c - One reusable procedure, as a directory on disk
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "skill/clawt-skill.h"

#include <string.h>

struct _ClawtSkill {
    gint              ref_count;

    gchar            *name;
    gchar            *description;
    gchar            *body;
    gchar            *directory;

    /*
     * Where it came from and what it looked like then.
     *
     * The digest is over the rendered SKILL.md, not over the body, so a
     * front-matter edit is a change too.  Somebody who rewrites a
     * skill's `description` has changed what every agent sees before
     * deciding whether to open it, which is exactly the part a reviewer
     * would want to be told about again.
     */
    gchar            *origin_url;
    gchar            *digest;
    gint64            imported_at;
    ClawtSkillSource  source;

    gboolean          enabled;

    /* Front matter we do not act on, kept in file order. */
    GHashTable       *meta;      /* gchar* -> gchar* */
    GPtrArray        *meta_order;

    GPtrArray        *warnings;  /* gchar* */
    GPtrArray        *skipped;   /* gchar* */
};

static ClawtSkill *
clawt_skill_ref(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

G_DEFINE_BOXED_TYPE(ClawtSkill, clawt_skill, clawt_skill_ref, clawt_skill_free)

/* ── The name gate ───────────────────────────────────────────────── */

gboolean
clawt_skill_name_is_valid(const gchar *name)
{
    gsize length;
    gsize i;

    if (name == NULL || *name == '\0')
        return FALSE;

    length = strlen(name);

    if (length > CLAWT_SKILL_MAX_NAME)
        return FALSE;

    /*
     * A hyphen may not begin or end the name, which also settles the
     * two shapes a shell would treat as an option.
     */
    if (name[0] == '-' || name[length - 1] == '-')
        return FALSE;

    for (i = 0; i < length; i++) {
        gchar c = name[i];

        if (c >= 'a' && c <= 'z')
            continue;

        if (c >= '0' && c <= '9')
            continue;

        if (c == '-') {
            /*
             * Single interior hyphens only.  Consecutive ones are
             * refused because two names differing only by how many
             * hyphens they have between two words are, for every human
             * purpose, the same name -- and both would be a directory.
             */
            if (name[i + 1] == '-')
                return FALSE;

            continue;
        }

        return FALSE;
    }

    return TRUE;
}

gchar *
clawt_skill_name_from_wire(const gchar *raw, GError **error)
{
    g_autofree gchar *decoded = NULL;

    if (raw == NULL || *raw == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a skill name is required");
        return NULL;
    }

    /*
     * Decoded first, always -- and returned decoded.
     *
     * The allowlist below already refuses anything containing a `%`, so
     * a single encoded traversal is caught either way round.  Doing the
     * decode first is what makes the refusal say `../` instead of
     * `%2e%2e%2f`, and what stops a still-encoded string being handed
     * to something downstream that decodes it a second time.
     *
     * g_uri_unescape_string() returns NULL for an illegal escape, which
     * is itself a refusal: a name nobody can decode is not a name.  The
     * `illegal_characters` argument is left NULL because the validation
     * below is the allowlist, and two overlapping filters would be two
     * places to keep in step.
     */
    decoded = g_uri_unescape_string(raw, NULL);

    if (decoded == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable skill name", raw);
        return NULL;
    }

    if (!clawt_skill_name_is_valid(decoded)) {
        /*
         * The decoded form is named in the message when it differs, so
         * somebody who sent `%2e%2e%2f` is told what it became rather
         * than being told the string they typed looks fine.
         */
        if (g_strcmp0(decoded, raw) != 0)
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' decodes to '%s', which is not a skill name: "
                        "use lowercase letters, digits and single hyphens, "
                        "at most %d characters",
                        raw, decoded, CLAWT_SKILL_MAX_NAME);
        else
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' is not a skill name: use lowercase letters, "
                        "digits and single hyphens, at most %d characters",
                        raw, CLAWT_SKILL_MAX_NAME);

        return NULL;
    }

    return g_steal_pointer(&decoded);
}

gchar *
clawt_skill_directory_for(const gchar *skills_dir, const gchar *name)
{
    g_return_val_if_fail(skills_dir != NULL, NULL);

    /*
     * Refuses rather than sanitises.  A path built from a name this has
     * not checked is the traversal, and "build it anyway and hope the
     * caller checked" is how one gets built.
     */
    if (!clawt_skill_name_is_valid(name))
        return NULL;

    return g_build_filename(skills_dir, name, NULL);
}

/* ── The record ──────────────────────────────────────────────────── */

ClawtSkill *
clawt_skill_new(const gchar *name)
{
    ClawtSkill *self;

    g_return_val_if_fail(clawt_skill_name_is_valid(name), NULL);

    self = g_new0(ClawtSkill, 1);
    self->ref_count = 1;
    self->name = g_strdup(name);
    self->source = CLAWT_SKILL_SOURCE_USER;

    /*
     * Enabled by default here, and *not* by the import path, which
     * clears it.  A skill somebody wrote in the library is theirs
     * already; one that arrived from elsewhere has not been read yet.
     */
    self->enabled = TRUE;

    self->meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       g_free);
    self->meta_order = g_ptr_array_new_with_free_func(g_free);
    self->warnings = g_ptr_array_new_with_free_func(g_free);
    self->skipped = g_ptr_array_new_with_free_func(g_free);

    return self;
}

void
clawt_skill_free(ClawtSkill *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->name);
    g_free(self->description);
    g_free(self->body);
    g_free(self->directory);
    g_free(self->origin_url);
    g_free(self->digest);
    g_hash_table_unref(self->meta);
    g_ptr_array_unref(self->meta_order);
    g_ptr_array_unref(self->warnings);
    g_ptr_array_unref(self->skipped);
    g_free(self);
}

ClawtSkill *
clawt_skill_copy(ClawtSkill *self)
{
    ClawtSkill *copy;
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    copy = clawt_skill_new(self->name);

    if (copy == NULL)
        return NULL;

    copy->description = g_strdup(self->description);
    copy->body = g_strdup(self->body);
    copy->directory = g_strdup(self->directory);
    copy->origin_url = g_strdup(self->origin_url);
    copy->digest = g_strdup(self->digest);
    copy->imported_at = self->imported_at;
    copy->source = self->source;
    copy->enabled = self->enabled;

    for (i = 0; i < self->meta_order->len; i++) {
        const gchar *key = g_ptr_array_index(self->meta_order, i);

        clawt_skill_set_meta(copy, key,
                             g_hash_table_lookup(self->meta, key));
    }

    for (i = 0; i < self->warnings->len; i++)
        clawt_skill_add_warning(copy, g_ptr_array_index(self->warnings, i));

    for (i = 0; i < self->skipped->len; i++)
        clawt_skill_add_skipped(copy, g_ptr_array_index(self->skipped, i));

    return copy;
}

const gchar *
clawt_skill_get_name(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->name;
}

const gchar *
clawt_skill_get_description(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->description;
}

const gchar *
clawt_skill_get_body(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->body;
}

const gchar *
clawt_skill_get_directory(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->directory;
}

const gchar *
clawt_skill_get_origin_url(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->origin_url;
}

const gchar *
clawt_skill_get_digest(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->digest;
}

gint64
clawt_skill_get_imported_at(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->imported_at;
}

ClawtSkillSource
clawt_skill_get_source(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_SKILL_SOURCE_USER);

    return self->source;
}

gboolean
clawt_skill_get_enabled(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->enabled;
}

void
clawt_skill_set_description(ClawtSkill *self, const gchar *description)
{
    g_return_if_fail(self != NULL);

    g_free(self->description);
    self->description = g_strdup(description);
}

void
clawt_skill_set_body(ClawtSkill *self, const gchar *body)
{
    g_return_if_fail(self != NULL);

    g_free(self->body);
    self->body = g_strdup(body);
}

void
clawt_skill_set_directory(ClawtSkill *self, const gchar *directory)
{
    g_return_if_fail(self != NULL);

    g_free(self->directory);
    self->directory = g_strdup(directory);
}

void
clawt_skill_set_origin_url(ClawtSkill *self, const gchar *url)
{
    g_return_if_fail(self != NULL);

    g_free(self->origin_url);
    self->origin_url = g_strdup(url);
}

void
clawt_skill_set_digest(ClawtSkill *self, const gchar *digest)
{
    g_return_if_fail(self != NULL);

    g_free(self->digest);
    self->digest = g_strdup(digest);
}

void
clawt_skill_set_imported_at(ClawtSkill *self, gint64 stamp)
{
    g_return_if_fail(self != NULL);

    self->imported_at = stamp;
}

void
clawt_skill_set_source(ClawtSkill *self, ClawtSkillSource source)
{
    g_return_if_fail(self != NULL);

    self->source = source;
}

void
clawt_skill_set_enabled(ClawtSkill *self, gboolean enabled)
{
    g_return_if_fail(self != NULL);

    self->enabled = enabled;
}

const gchar *
clawt_skill_get_meta(ClawtSkill *self, const gchar *key)
{
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    return g_hash_table_lookup(self->meta, key);
}

void
clawt_skill_set_meta(ClawtSkill *self, const gchar *key, const gchar *value)
{
    guint i;

    g_return_if_fail(self != NULL);
    g_return_if_fail(key != NULL);

    if (value == NULL) {
        g_hash_table_remove(self->meta, key);

        for (i = 0; i < self->meta_order->len; i++) {
            if (g_strcmp0(g_ptr_array_index(self->meta_order, i), key) == 0) {
                g_ptr_array_remove_index(self->meta_order, i);
                break;
            }
        }

        return;
    }

    if (!g_hash_table_contains(self->meta, key))
        g_ptr_array_add(self->meta_order, g_strdup(key));

    g_hash_table_replace(self->meta, g_strdup(key), g_strdup(value));
}

GStrv
clawt_skill_get_meta_keys(ClawtSkill *self)
{
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    out = g_ptr_array_new();

    for (i = 0; i < self->meta_order->len; i++)
        g_ptr_array_add(out, g_strdup(g_ptr_array_index(self->meta_order, i)));

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}

GPtrArray *
clawt_skill_get_warnings(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->warnings;
}

void
clawt_skill_add_warning(ClawtSkill *self, const gchar *warning)
{
    guint i;

    g_return_if_fail(self != NULL);
    g_return_if_fail(warning != NULL);

    /*
     * Deduplicated, because the same pattern can match several times in
     * one file and a reviewer reading the same sentence eleven times
     * learns nothing on the second.
     */
    for (i = 0; i < self->warnings->len; i++) {
        if (g_strcmp0(g_ptr_array_index(self->warnings, i), warning) == 0)
            return;
    }

    g_ptr_array_add(self->warnings, g_strdup(warning));
}

GPtrArray *
clawt_skill_get_skipped(ClawtSkill *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->skipped;
}

void
clawt_skill_add_skipped(ClawtSkill *self, const gchar *filename)
{
    g_return_if_fail(self != NULL);
    g_return_if_fail(filename != NULL);

    g_ptr_array_add(self->skipped, g_strdup(filename));
}

/*
 * One front-matter value, quoted only when it has to be.
 *
 * YAML would read an unquoted value containing a colon-space as a
 * nested mapping and one containing a leading `#` as a comment, so a
 * description written in ordinary prose -- "Use this when: the build
 * fails" -- would come back as something other than what went in.
 */
static void
append_meta_value(GString *out, const gchar *key, const gchar *value)
{
    gboolean needs_quotes;

    if (value == NULL)
        return;

    needs_quotes = *value == '\0' ||
                   strstr(value, ": ") != NULL ||
                   strchr(value, '\n') != NULL ||
                   strchr(value, '"') != NULL ||
                   value[0] == '#' || value[0] == '&' || value[0] == '*' ||
                   value[0] == '[' || value[0] == '{' || value[0] == '!' ||
                   value[0] == '%' || value[0] == '@' || value[0] == '`' ||
                   value[0] == '>' || value[0] == '|' || value[0] == '\'' ||
                   value[0] == ' ' ||
                   g_str_has_suffix(value, ":") ||
                   g_str_has_suffix(value, " ");

    if (!needs_quotes) {
        g_string_append_printf(out, "%s: %s\n", key, value);
        return;
    }

    g_string_append_printf(out, "%s: \"", key);

    for (; *value != '\0'; value++) {
        switch (*value) {
        case '"':
            g_string_append(out, "\\\"");
            break;

        case '\\':
            g_string_append(out, "\\\\");
            break;

        case '\n':
            g_string_append(out, "\\n");
            break;

        default:
            g_string_append_c(out, *value);
            break;
        }
    }

    g_string_append(out, "\"\n");
}

gchar *
clawt_skill_render(ClawtSkill *self)
{
    g_autoptr(GString) out = NULL;
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    out = g_string_new("---\n");

    append_meta_value(out, "name", self->name);
    append_meta_value(out, "description",
                      self->description != NULL ? self->description : "");

    for (i = 0; i < self->meta_order->len; i++) {
        const gchar *key = g_ptr_array_index(self->meta_order, i);

        append_meta_value(out, key, g_hash_table_lookup(self->meta, key));
    }

    g_string_append(out, "---\n");

    if (self->body != NULL && *self->body != '\0') {
        g_string_append_c(out, '\n');
        g_string_append(out, self->body);

        if (!g_str_has_suffix(self->body, "\n"))
            g_string_append_c(out, '\n');
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}
