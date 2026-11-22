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
#include <stdio.h>
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

/* ── Characters ──────────────────────────────────────────────────── */

static void
animation_free(ClawtOfficeAnimation *animation)
{
    g_free(animation->name);
    g_free(animation);
}

static ClawtOfficeCharacter *
character_new(void)
{
    ClawtOfficeCharacter *c = g_new0(ClawtOfficeCharacter, 1);

    c->animations = g_ptr_array_new_with_free_func((GDestroyNotify)animation_free);
    return c;
}

ClawtOfficeCharacter *
clawt_office_character_copy(const ClawtOfficeCharacter *character)
{
    ClawtOfficeCharacter *c;
    guint i;

    g_return_val_if_fail(character != NULL, NULL);

    c = character_new();
    c->agent = g_strdup(character->agent);
    c->sheet = g_strdup(character->sheet);
    c->sheet_sha256 = g_strdup(character->sheet_sha256);
    c->frame_width = character->frame_width;
    c->frame_height = character->frame_height;
    for (i = 0; i < character->animations->len; i++) {
        const ClawtOfficeAnimation *from = g_ptr_array_index(character->animations, i);
        ClawtOfficeAnimation *to = g_new0(ClawtOfficeAnimation, 1);

        *to = *from;
        to->name = g_strdup(from->name);
        g_ptr_array_add(c->animations, to);
    }
    return c;
}

void
clawt_office_character_free(ClawtOfficeCharacter *character)
{
    if (character == NULL)
        return;
    g_free(character->agent);
    g_free(character->sheet);
    g_free(character->sheet_sha256);
    g_ptr_array_unref(character->animations);
    g_free(character);
}

G_DEFINE_BOXED_TYPE(ClawtOfficeCharacter, clawt_office_character,
                    clawt_office_character_copy, clawt_office_character_free)

gboolean
clawt_office_png_size(const gchar *path, guint *width, guint *height)
{
    static const guchar signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    guchar head[24];
    FILE *f;
    gsize got;

    g_return_val_if_fail(path != NULL && width != NULL && height != NULL, FALSE);

    f = fopen(path, "rb");
    if (f == NULL)
        return FALSE;
    got = fread(head, 1, sizeof(head), f);
    fclose(f);
    /* the signature, then IHDR first, as the format requires */
    if (got != sizeof(head) || memcmp(head, signature, 8) != 0 || memcmp(head + 12, "IHDR", 4) != 0)
        return FALSE;
    *width = ((guint)head[16] << 24) | ((guint)head[17] << 16) | ((guint)head[18] << 8) | head[19];
    *height = ((guint)head[20] << 24) | ((guint)head[21] << 16) | ((guint)head[22] << 8) | head[23];
    return *width > 0 && *height > 0;
}

/* A YAML scalar member as a number, or @fallback. */
static gdouble
number_member(YamlMapping *mapping, const gchar *name, gdouble fallback)
{
    YamlNode *node;
    const gchar *text;
    gchar *end = NULL;
    gdouble value;

    if (mapping == NULL || !yaml_mapping_has_member(mapping, name))
        return fallback;
    node = yaml_mapping_get_member(mapping, name);
    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
        return fallback;
    text = yaml_node_get_string(node);
    value = text != NULL ? g_ascii_strtod(text, &end) : fallback;
    return end != NULL && end != text && *end == '\0' ? value : fallback;
}

ClawtOfficeCharacter *
clawt_office_character_load(const gchar *dir, GError **error)
{
    g_autofree gchar *path = NULL;
    g_autoptr(YamlParser) parser = yaml_parser_new();
    g_autoptr(ClawtOfficeCharacter) c = NULL;
    YamlNode *root;
    YamlMapping *top;
    YamlMapping *animations;
    YamlSequence *frame;
    YamlSequence *assets;
    guint i;

    g_return_val_if_fail(dir != NULL, NULL);

    path = g_build_filename(dir, "character.yaml", NULL);
    if (!yaml_parser_load_from_file(parser, path, error))
        return NULL;
    root = yaml_parser_get_root(parser);
    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID, "%s is not a mapping", path);
        return NULL;
    }
    top = yaml_node_get_mapping(root);
    c = character_new();
    c->agent = g_strdup(yaml_mapping_get_string_member(top, "agent"));
    c->sheet = g_strdup(yaml_mapping_get_string_member(top, "sheet"));
    if (c->sheet != NULL && !clawt_office_pack_path_is_safe(c->sheet)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "%s: sheet is not a path inside the character", path);
        return NULL;
    }
    frame = yaml_mapping_has_member(top, "frame")
        ? yaml_mapping_get_sequence_member(top, "frame") : NULL;
    if (frame != NULL && yaml_sequence_get_length(frame) == 2) {
        c->frame_width = (guint)g_ascii_strtoull(yaml_sequence_get_string_element(frame, 0),
                                                 NULL, 10);
        c->frame_height = (guint)g_ascii_strtoull(yaml_sequence_get_string_element(frame, 1),
                                                  NULL, 10);
    }
    animations = yaml_mapping_has_member(top, "animations")
        ? yaml_mapping_get_mapping_member(top, "animations") : NULL;
    for (i = 0; animations != NULL && i < yaml_mapping_get_size(animations); i++) {
        const gchar *name = yaml_mapping_get_key(animations, i);
        YamlNode *value = yaml_mapping_get_value(animations, i);
        YamlMapping *m;
        ClawtOfficeAnimation *a;

        if (value == NULL || yaml_node_get_node_type(value) != YAML_NODE_MAPPING)
            continue;
        m = yaml_node_get_mapping(value);
        a = g_new0(ClawtOfficeAnimation, 1);
        a->name = g_strdup(name);
        a->row = (guint)MAX(0.0, number_member(m, "row", 0.0));
        a->frames = (guint)MAX(1.0, number_member(m, "frames", 1.0));
        a->fps = MAX(0.0, number_member(m, "fps", 0.0));
        g_ptr_array_add(c->animations, a);
    }
    /* the sheet's checksum, from the assets the importer recorded */
    assets = yaml_mapping_has_member(top, "assets")
        ? yaml_mapping_get_sequence_member(top, "assets") : NULL;
    for (i = 0; c->sheet != NULL && assets != NULL && i < yaml_sequence_get_length(assets); i++) {
        YamlNode *node = yaml_sequence_get_element(assets, i);
        YamlMapping *m;

        if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
            continue;
        m = yaml_node_get_mapping(node);
        if (g_strcmp0(yaml_mapping_get_string_member(m, "path"), c->sheet) == 0)
            c->sheet_sha256 = g_strdup(yaml_mapping_get_string_member(m, "sha256"));
    }
    return g_steal_pointer(&c);
}

JsonNode *
clawt_office_character_to_json(const ClawtOfficeCharacter *character)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    guint i;

    g_return_val_if_fail(character != NULL, NULL);

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "agent");
    json_builder_add_string_value(b, character->agent);
    if (character->sheet != NULL) {
        json_builder_set_member_name(b, "sheet");
        json_builder_add_string_value(b, character->sheet);
    }
    if (character->sheet_sha256 != NULL) {
        json_builder_set_member_name(b, "sheet_sha256");
        json_builder_add_string_value(b, character->sheet_sha256);
    }
    json_builder_set_member_name(b, "frame");
    json_builder_begin_array(b);
    json_builder_add_int_value(b, character->frame_width);
    json_builder_add_int_value(b, character->frame_height);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "animations");
    json_builder_begin_object(b);
    for (i = 0; i < character->animations->len; i++) {
        const ClawtOfficeAnimation *a = g_ptr_array_index(character->animations, i);

        json_builder_set_member_name(b, a->name);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "row");
        json_builder_add_int_value(b, a->row);
        json_builder_set_member_name(b, "frames");
        json_builder_add_int_value(b, a->frames);
        json_builder_set_member_name(b, "fps");
        json_builder_add_double_value(b, a->fps);
        json_builder_end_object(b);
    }
    json_builder_end_object(b);
    json_builder_end_object(b);
    return json_builder_get_root(b);
}

/* An animation name is a word: lower-case letters, digits and underscores. */
static gboolean
animation_name_is_valid(const gchar *name)
{
    const gchar *p;

    if (name == NULL || *name == '\0' || strlen(name) > 32)
        return FALSE;
    for (p = name; *p != '\0'; p++)
        if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) && *p != '_')
            return FALSE;
    return TRUE;
}

gboolean
clawt_office_character_set_sheet(const gchar  *characters,
                                 const gchar  *agent_id,
                                 const gchar  *sheet,
                                 guint         frame_width,
                                 guint         frame_height,
                                 JsonObject   *animations,
                                 GError      **error)
{
    g_autofree gchar *dir = NULL;
    g_autofree gchar *manifest = NULL;
    g_autofree gchar *image = NULL;
    g_autoptr(ClawtOfficeCharacter) current = NULL;
    g_autoptr(YamlParser) parser = yaml_parser_new();
    g_autoptr(YamlGenerator) generator = NULL;
    g_autoptr(YamlMapping) rows = yaml_mapping_new();
    g_autoptr(YamlSequence) frame = yaml_sequence_new();
    g_autofree gchar *text = NULL;
    YamlNode *root;
    YamlMapping *top;
    JsonObjectIter iter;
    const gchar *name;
    JsonNode *value;
    guint width = 0;
    guint height = 0;
    gsize length = 0;
    guint count = 0;

    g_return_val_if_fail(characters != NULL, FALSE);

    if (agent_id == NULL || !clawt_office_pack_path_is_safe(agent_id) ||
        strchr(agent_id, '/') != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT, "'%s' is not an agent id",
                    agent_id != NULL ? agent_id : "");
        return FALSE;
    }
    dir = g_build_filename(characters, agent_id, NULL);
    manifest = g_build_filename(dir, "character.yaml", NULL);
    current = clawt_office_character_load(dir, NULL);
    if (current == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "%s has no character yet: import its sprite sheet first "
                    "(clawtilla_office_asset_import, character \"%s\")", agent_id, agent_id);
        return FALSE;
    }
    if (sheet == NULL || !clawt_office_pack_path_is_safe(sheet)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "the sheet must be a path inside the character");
        return FALSE;
    }
    image = g_build_filename(dir, sheet, NULL);
    if (!parent_is_inside(dir, image, error))
        return FALSE;
    if (!clawt_office_png_size(image, &width, &height)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "%s is not a PNG in %s's character; import it first", sheet, agent_id);
        return FALSE;
    }
    if (frame_width < 8 || frame_height < 8 || frame_width > 512 || frame_height > 512) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a frame is 8 to 512 pixels on each side");
        return FALSE;
    }

    /* every row and frame inside the image, or a renderer reads past it */
    json_object_iter_init(&iter, animations);
    while (json_object_iter_next(&iter, &name, &value)) {
        JsonObject *a = JSON_NODE_HOLDS_OBJECT(value) ? json_node_get_object(value) : NULL;
        gint64 row = a != NULL ? json_object_get_int_member_with_default(a, "row", -1) : -1;
        gint64 frames = a != NULL ? json_object_get_int_member_with_default(a, "frames", 1) : 0;
        gdouble fps = a != NULL ? json_object_get_double_member_with_default(a, "fps", 0.0) : 0;
        g_autoptr(YamlMapping) entry = yaml_mapping_new();

        if (!animation_name_is_valid(name) || a == NULL || row < 0 || frames < 1 ||
            fps < 0.0 || fps > 60.0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "animation '%s' needs a name of lower-case letters, digits and '_', "
                        "a row from 0, frames from 1 and fps from 0 to 60", name);
            return FALSE;
        }
        if ((guint64)(row + 1) * frame_height > height || (guint64)frames * frame_width > width) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "animation '%s' (row %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
                        " frames of %ux%u) runs past the %ux%u sheet", name, row, frames,
                        frame_width, frame_height, width, height);
            return FALSE;
        }
        yaml_mapping_set_int_member(entry, "row", row);
        yaml_mapping_set_int_member(entry, "frames", frames);
        yaml_mapping_set_double_member(entry, "fps", fps);
        yaml_mapping_set_mapping_member(rows, name, entry);
        count++;
    }
    if (count == 0) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "name at least one animation (idle, at the least)");
        return FALSE;
    }

    /* the layout written into the manifest; its agent and assets kept */
    if (!yaml_parser_load_from_file(parser, manifest, error))
        return FALSE;
    root = yaml_parser_get_root(parser);
    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID, "%s is not a mapping",
                    manifest);
        return FALSE;
    }
    top = yaml_node_get_mapping(root);
    yaml_mapping_set_string_member(top, "sheet", sheet);
    yaml_sequence_add_int_element(frame, frame_width);
    yaml_sequence_add_int_element(frame, frame_height);
    yaml_mapping_set_sequence_member(top, "frame", frame);
    yaml_mapping_set_mapping_member(top, "animations", rows);
    generator = yaml_generator_new();
    yaml_generator_set_root(generator, root);
    text = yaml_generator_to_data(generator, &length, error);
    if (text == NULL)
        return FALSE;
    return clawt_office_pack_write_file(dir, "character.yaml", text, length, error);
}
