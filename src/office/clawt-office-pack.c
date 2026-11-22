/*
 * clawt-office-pack.c - Changing a world pack on disk, safely
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The custodial team edits world packs through the daemon, on behalf of
 * agents that may be confined -- so every path is checked here, in the
 * library, rather than at the tool that happens to be calling: a rule
 * enforced at one call site is a rule about that call site.  A file is
 * written only inside its pack, a write that would leave a pack refusing
 * to load is undone, and an asset arrives with its checksum and where it
 * came from or not at all.
 */

#include "clawtilla.h"
#include "office/clawt-office-pack.h"
#include "office/clawt-office-world.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <yaml-glib.h>

gboolean
clawt_office_pack_path_is_safe(const gchar *relative)
{
    g_auto(GStrv) parts = NULL;
    gsize i;

    if (relative == NULL || *relative == '\0' || g_path_is_absolute(relative) ||
        strchr(relative, '\\') != NULL)
        return FALSE;
    parts = g_strsplit(relative, "/", -1);
    for (i = 0; parts[i] != NULL; i++) {
        /* `..` leaves the pack; a hidden name is somewhere nobody looks */
        if (*parts[i] == '\0' || parts[i][0] == '.')
            return FALSE;
    }
    return TRUE;
}

/*
 * The directory a file would land in, resolved through any symbolic link
 * on the way, must still be the pack or inside it.  Lexical checks alone
 * (g_canonicalize_filename()) do not see a link planted in a pack; the
 * kernel's answer does.
 */
static gboolean
parent_is_inside(const gchar *pack, const gchar *file, GError **error)
{
    g_autofree gchar *parent = g_path_get_dirname(file);
    char root_real[PATH_MAX];
    char parent_real[PATH_MAX];
    gsize root_length;

    if (realpath(pack, root_real) == NULL || realpath(parent, parent_real) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not resolve %s: %s", parent, g_strerror(errno));
        return FALSE;
    }
    root_length = strlen(root_real);
    if (strncmp(parent_real, root_real, root_length) != 0 ||
        (parent_real[root_length] != '\0' && parent_real[root_length] != '/')) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "%s is outside the pack", file);
        return FALSE;
    }
    return TRUE;
}

/* ── Forking ─────────────────────────────────────────────────────── */

static gboolean
copy_tree(const gchar *source, const gchar *destination, guint depth, GError **error)
{
    g_autoptr(GDir) dir = NULL;
    const gchar *name;

    /* a pack is a few directories deep; anything deeper is not a pack */
    if (depth > 8) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "%s is nested too deeply to be a world pack", source);
        return FALSE;
    }
    if (g_mkdir_with_parents(destination, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "could not create %s: %s",
                    destination, g_strerror(errno));
        return FALSE;
    }
    dir = g_dir_open(source, 0, error);
    if (dir == NULL)
        return FALSE;
    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *from = g_build_filename(source, name, NULL);
        g_autofree gchar *to = g_build_filename(destination, name, NULL);

        /* never a link: it could name anything on the machine */
        if (g_file_test(from, G_FILE_TEST_IS_SYMLINK))
            continue;
        if (g_file_test(from, G_FILE_TEST_IS_DIR)) {
            if (!copy_tree(from, to, depth + 1, error))
                return FALSE;
        } else if (g_file_test(from, G_FILE_TEST_IS_REGULAR)) {
            g_autofree gchar *data = NULL;
            gsize length = 0;

            if (!g_file_get_contents(from, &data, &length, error) ||
                !g_file_set_contents_full(to, data, (gssize)length,
                                          G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
                return FALSE;
        }
    }
    return TRUE;
}

gboolean
clawt_office_pack_fork(const gchar *source, const gchar *destination, GError **error)
{
    g_return_val_if_fail(source != NULL && destination != NULL, FALSE);

    if (g_file_test(destination, G_FILE_TEST_EXISTS)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "%s already exists", destination);
        return FALSE;
    }
    if (!copy_tree(source, destination, 0, error)) {
        /* half a pack is not a pack: take back what was written */
        clawt_remove_tree(destination, destination, NULL);
        return FALSE;
    }
    return TRUE;
}

/* ── Writing one file ────────────────────────────────────────────── */

/* Whether the pack, as it now stands, would be accepted by the office. */
static gboolean
pack_accepted(const gchar *pack, const gchar *relative, GError **error)
{
    g_autoptr(ClawtOfficeWorld) world = NULL;

    world = clawt_office_world_load(pack, error);
    if (world == NULL)
        return FALSE;
    if (g_strcmp0(relative, "choreography.yaml") == 0) {
        g_autoptr(ClawtOfficeChoreography) rules = clawt_office_choreography_new_default();

        return clawt_office_choreography_override(
            rules, clawt_office_world_get_choreography_yaml(world), error);
    }
    return TRUE;
}

gboolean
clawt_office_pack_write_file(const gchar  *pack,
                             const gchar  *relative,
                             const gchar  *contents,
                             gsize         length,
                             GError      **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *parent = NULL;
    g_autofree gchar *before = NULL;
    gsize before_length = 0;
    gboolean checked;
    g_autoptr(GError) refusal = NULL;

    g_return_val_if_fail(pack != NULL, FALSE);
    g_return_val_if_fail(contents != NULL || length == 0, FALSE);

    if (!clawt_office_pack_path_is_safe(relative)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a path inside the pack: relative, no '..' and "
                    "no hidden names", relative != NULL ? relative : "");
        return FALSE;
    }
    if (length > CLAWT_OFFICE_PACK_MAX_TEXT) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "%s is larger than %d bytes; import a large file as an asset",
                    relative, CLAWT_OFFICE_PACK_MAX_TEXT);
        return FALSE;
    }

    path = g_build_filename(pack, relative, NULL);
    parent = g_path_get_dirname(path);
    if (g_mkdir_with_parents(parent, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "could not create %s: %s",
                    parent, g_strerror(errno));
        return FALSE;
    }
    if (!parent_is_inside(pack, path, error))
        return FALSE;

    checked = g_strcmp0(relative, "office.yaml") == 0 ||
              g_strcmp0(relative, "choreography.yaml") == 0;
    if (checked)
        g_file_get_contents(path, &before, &before_length, NULL);

    /* by rename, so a link already at the name is replaced, not followed */
    if (!g_file_set_contents_full(path, contents, (gssize)length,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
        return FALSE;
    if (!checked || pack_accepted(pack, relative, &refusal))
        return TRUE;

    /* refused: put the pack back as it was, and say why */
    if (before != NULL)
        g_file_set_contents_full(path, before, (gssize)before_length,
                                 G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
    else
        g_unlink(path);
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                "%s was not kept, because the pack would not load with it: %s",
                relative, refusal->message);
    return FALSE;
}

/* ── Assets ──────────────────────────────────────────────────────── */

gchar *
clawt_office_file_sha256(const gchar *path, GError **error)
{
    g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInputStream) in = g_file_read(file, NULL, error);
    guchar buffer[65536];
    gssize got;

    if (in == NULL)
        return NULL;
    while ((got = g_input_stream_read(G_INPUT_STREAM(in), buffer, sizeof(buffer),
                                      NULL, error)) > 0)
        g_checksum_update(sum, buffer, got);
    if (got < 0)
        return NULL;
    return g_strdup(g_checksum_get_string(sum));
}

/*
 * The manifest, with one entry in `assets` for @relative -- replaced if
 * there was one, appended if not.  Rewritten through yaml-glib rather
 * than by appending text, because an appended block under a manifest
 * that already has `assets:` is a second key, and YAML keeps the last.
 */
static gboolean
record_asset(const gchar *pack,
             const gchar *manifest,
             const gchar *relative,
             const gchar *sha256,
             const gchar *license,
             const gchar *provenance,
             GError     **error)
{
    g_autofree gchar *path = g_build_filename(pack, manifest, NULL);
    g_autoptr(YamlParser) parser = yaml_parser_new();
    g_autoptr(YamlGenerator) generator = NULL;
    g_autofree gchar *text = NULL;
    YamlNode *root;
    YamlMapping *top;
    YamlSequence *assets = NULL;
    g_autoptr(YamlMapping) entry = yaml_mapping_new();
    gsize length = 0;
    guint i;

    if (!yaml_parser_load_from_file(parser, path, error))
        return FALSE;
    root = yaml_parser_get_root(parser);
    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "%s is not a mapping", path);
        return FALSE;
    }
    top = yaml_node_get_mapping(root);
    if (yaml_mapping_has_member(top, "assets")) {
        YamlNode *node = yaml_mapping_get_member(top, "assets");

        if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE)
            assets = yaml_node_get_sequence(node);
    }
    if (assets == NULL) {
        g_autoptr(YamlSequence) fresh = yaml_sequence_new();

        yaml_mapping_set_sequence_member(top, "assets", fresh);
        assets = yaml_mapping_get_sequence_member(top, "assets");
    }

    /* the same path twice would be two answers to one question */
    for (i = yaml_sequence_get_length(assets); i > 0; i--) {
        YamlNode *node = yaml_sequence_get_element(assets, i - 1);
        YamlMapping *m;

        if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
            continue;
        m = yaml_node_get_mapping(node);
        if (g_strcmp0(yaml_mapping_get_string_member(m, "path"), relative) == 0)
            yaml_sequence_remove_element(assets, i - 1);
    }

    yaml_mapping_set_string_member(entry, "path", relative);
    yaml_mapping_set_string_member(entry, "sha256", sha256);
    if (license != NULL && *license != '\0')
        yaml_mapping_set_string_member(entry, "license", license);
    yaml_mapping_set_string_member(entry, "source", provenance);
    yaml_sequence_add_mapping_element(assets, entry);

    generator = yaml_generator_new();
    yaml_generator_set_root(generator, root);
    text = yaml_generator_to_data(generator, &length, error);
    if (text == NULL)
        return FALSE;
    return clawt_office_pack_write_file(pack, manifest, text, length, error);
}

static gboolean
import_into(const gchar  *pack,
            const gchar  *manifest,
            const gchar  *source_path,
                               const gchar  *relative,
                               const gchar  *license,
                               const gchar  *provenance,
                               gchar       **out_sha256,
                               GError      **error)
{
    g_autofree gchar *destination = NULL;
    g_autofree gchar *parent = NULL;
    g_autofree gchar *data = NULL;
    g_autofree gchar *sum = NULL;
    gsize length = 0;
    GStatBuf st;

    g_return_val_if_fail(pack != NULL && source_path != NULL, FALSE);

    if (provenance == NULL || *provenance == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "say where the asset came from (a URL, an author, or a "
                            "generator with its model and terms); a pack keeps that "
                            "with every file");
        return FALSE;
    }
    if (!clawt_office_pack_path_is_safe(relative)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a path inside the pack", relative != NULL ? relative : "");
        return FALSE;
    }
    if (g_strcmp0(relative, "office.yaml") == 0 ||
        g_strcmp0(relative, "choreography.yaml") == 0 ||
        g_strcmp0(relative, "character.yaml") == 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "%s is the pack's own description, not an asset", relative);
        return FALSE;
    }
    /* lstat: a link given as the source is refused, not followed */
    if (g_lstat(source_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "%s is not a regular file", source_path);
        return FALSE;
    }
    if (st.st_size > CLAWT_OFFICE_PACK_MAX_ASSET) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "%s is larger than %d bytes", source_path, CLAWT_OFFICE_PACK_MAX_ASSET);
        return FALSE;
    }
    if (!g_file_get_contents(source_path, &data, &length, error))
        return FALSE;

    destination = g_build_filename(pack, relative, NULL);
    parent = g_path_get_dirname(destination);
    if (g_mkdir_with_parents(parent, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "could not create %s: %s",
                    parent, g_strerror(errno));
        return FALSE;
    }
    if (!parent_is_inside(pack, destination, error) ||
        !g_file_set_contents_full(destination, data, (gssize)length,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
        return FALSE;

    sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)data, length);
    if (!record_asset(pack, manifest, relative, sum, license, provenance, error))
        return FALSE;
    if (out_sha256 != NULL)
        *out_sha256 = g_steal_pointer(&sum);
    return TRUE;
}

gboolean
clawt_office_pack_import_asset(const gchar  *pack,
                               const gchar  *source_path,
                               const gchar  *relative,
                               const gchar  *license,
                               const gchar  *provenance,
                               gchar       **out_sha256,
                               GError      **error)
{
    return import_into(pack, "office.yaml", source_path, relative, license, provenance,
                       out_sha256, error);
}

gboolean
clawt_office_character_import_asset(const gchar  *characters,
                                    const gchar  *agent_id,
                                    const gchar  *source_path,
                                    const gchar  *relative,
                                    const gchar  *license,
                                    const gchar  *provenance,
                                    gchar       **out_sha256,
                                    GError      **error)
{
    g_autofree gchar *dir = NULL;
    g_autofree gchar *manifest = NULL;

    g_return_val_if_fail(characters != NULL, FALSE);

    if (agent_id == NULL || !clawt_office_pack_path_is_safe(agent_id) ||
        strchr(agent_id, '/') != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not an agent id", agent_id != NULL ? agent_id : "");
        return FALSE;
    }
    dir = g_build_filename(characters, agent_id, NULL);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "could not create %s: %s", dir,
                    g_strerror(errno));
        return FALSE;
    }
    /* a character's own manifest, made on its first file */
    manifest = g_build_filename(dir, "character.yaml", NULL);
    if (!g_file_test(manifest, G_FILE_TEST_EXISTS)) {
        g_autofree gchar *text = g_strdup_printf("agent: %s\nassets: []\n", agent_id);

        if (!clawt_office_pack_write_file(dir, "character.yaml", text, strlen(text), error))
            return FALSE;
    }
    return import_into(dir, "character.yaml", source_path, relative, license, provenance,
                       out_sha256, error);
}
