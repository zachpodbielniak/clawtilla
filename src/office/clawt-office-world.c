/*
 * clawt-office-world.c - Office world packs and the choreography that fills them
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "office/clawt-office-world.h"

#include <yaml-glib.h>
#include <string.h>

/* ── Zone tags ──────────────────────────────────────────────────── */

/*
 * The tags something in clawtilla sends people to.  A pack may carry
 * others (they are kept, and warned about once, since the usual cause is
 * a typo); `meeting` and `team_area` take an argument after a colon.
 */
static const struct {
    const gchar *tag;
    const gchar *description;
} zone_tags[] = {
    { "desk", "where an agent works; `team` on the zone keeps a team's desks together" },
    { "team_area", "a team's own corner (`team_area:<team>`)" },
    { "lead_office", "where team leads sit" },
    { "chief_office", "where the chief of staff sits" },
    { "lounge", "where idle agents rest; also the fallback for any missing zone" },
    { "breakroom", "a second idle spot, used when the lounge is full" },
    { "meeting", "a meeting room (`meeting:<capacity>`)" },
    { "town_hall", "the big room a round of every lead goes to" },
    { "phone_booth", "where a call is taken when there is one" },
    { "bug_corner", "where an agent whose turn broke stands" },
    { "library", "reading, searching and research" },
    { "lab", "running commands and tests" },
    { "whiteboard", "planning, tasks and delegation" },
    { "terminal", "driving a browser, a screen or a computer" },
    { "server_room", "decoration with blinking lights; optional" },
    { "visitor", "where remote workers (Grok bots, other fleets) appear" },
    { "entrance", "where ephemeral helpers arrive and leave" },
    { "usage_board", "the wall the plan allowances are drawn on" },
};

guint
clawt_office_zone_tag_count(void)
{
    return G_N_ELEMENTS(zone_tags);
}

const gchar *
clawt_office_zone_tag_nth(guint n, const gchar **out_description)
{
    if (n >= G_N_ELEMENTS(zone_tags))
        return NULL;
    if (out_description != NULL)
        *out_description = zone_tags[n].description;
    return zone_tags[n].tag;
}

/* The tag without its `:argument`. */
static gchar *
tag_base(const gchar *tag)
{
    const gchar *colon = strchr(tag, ':');

    return colon != NULL ? g_strndup(tag, colon - tag) : g_strdup(tag);
}

gboolean
clawt_office_zone_tag_is_known(const gchar *tag)
{
    g_autofree gchar *base = NULL;
    guint i;

    if (tag == NULL)
        return FALSE;
    base = tag_base(tag);
    for (i = 0; i < G_N_ELEMENTS(zone_tags); i++)
        if (g_strcmp0(zone_tags[i].tag, base) == 0)
            return TRUE;
    return FALSE;
}

/* ── The world ──────────────────────────────────────────────────── */

struct _ClawtOfficeWorld {
    GObject    parent_instance;
    gchar     *id;
    gchar     *directory;
    gchar     *name;
    gchar     *description;
    gchar     *license;
    gchar     *map_2d;        /* relative */
    gchar     *level_3d;      /* relative */
    gchar     *choreography;  /* YAML, or NULL */
    GPtrArray *zones;
    GPtrArray *assets;
    GPtrArray *warnings;
    GStrv      districts;
};

G_DEFINE_TYPE(ClawtOfficeWorld, clawt_office_world, G_TYPE_OBJECT)

static void
zone_free(ClawtOfficeZone *zone)
{
    g_free(zone->id);
    g_free(zone->name);
    g_strfreev(zone->tags);
    g_free(zone->team);
    g_array_unref(zone->anchors_2d);
    g_array_unref(zone->anchors_3d);
    g_free(zone);
}

static void
asset_free(ClawtOfficeAsset *asset)
{
    g_free(asset->path);
    g_free(asset->sha256);
    g_free(asset->license);
    g_free(asset->source);
    g_free(asset);
}

static void
clawt_office_world_finalize(GObject *object)
{
    ClawtOfficeWorld *self = CLAWT_OFFICE_WORLD(object);

    g_free(self->id);
    g_free(self->directory);
    g_free(self->name);
    g_free(self->description);
    g_free(self->license);
    g_free(self->map_2d);
    g_free(self->level_3d);
    g_free(self->choreography);
    g_ptr_array_unref(self->zones);
    g_ptr_array_unref(self->assets);
    g_ptr_array_unref(self->warnings);
    g_strfreev(self->districts);
    G_OBJECT_CLASS(clawt_office_world_parent_class)->finalize(object);
}

static void
clawt_office_world_class_init(ClawtOfficeWorldClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_office_world_finalize;
}

static void
clawt_office_world_init(ClawtOfficeWorld *self)
{
    self->zones = g_ptr_array_new_with_free_func((GDestroyNotify)zone_free);
    self->assets = g_ptr_array_new_with_free_func((GDestroyNotify)asset_free);
    self->warnings = g_ptr_array_new_with_free_func(g_free);
}

static const gchar *
string_member(YamlMapping *mapping, const gchar *name)
{
    YamlNode *node;

    if (mapping == NULL || !yaml_mapping_has_member(mapping, name))
        return NULL;
    node = yaml_mapping_get_member(mapping, name);
    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
        return NULL;
    return yaml_node_get_string(node);
}

static YamlMapping *
mapping_member(YamlMapping *mapping, const gchar *name)
{
    YamlNode *node;

    if (mapping == NULL || !yaml_mapping_has_member(mapping, name))
        return NULL;
    node = yaml_mapping_get_member(mapping, name);
    return node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING
        ? yaml_node_get_mapping(node) : NULL;
}

static YamlSequence *
sequence_member(YamlMapping *mapping, const gchar *name)
{
    YamlNode *node;

    if (mapping == NULL || !yaml_mapping_has_member(mapping, name))
        return NULL;
    node = yaml_mapping_get_member(mapping, name);
    return node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE
        ? yaml_node_get_sequence(node) : NULL;
}

static GStrv
strv_member(YamlMapping *mapping, const gchar *name)
{
    YamlSequence *sequence = sequence_member(mapping, name);
    GPtrArray *out;
    guint i;

    if (sequence == NULL)
        return NULL;
    out = g_ptr_array_new();
    for (i = 0; i < yaml_sequence_get_length(sequence); i++) {
        YamlNode *node = yaml_sequence_get_element(sequence, i);

        if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SCALAR)
            g_ptr_array_add(out, g_strdup(yaml_node_get_string(node)));
    }
    g_ptr_array_add(out, NULL);
    return (GStrv)g_ptr_array_free(out, FALSE);
}

/*
 * A path inside the pack, or NULL: no absolute path, no `..`.  Checked
 * on the text, before anything touches the disk, because a pack synced
 * from another fleet is that fleet's input.
 */
static gboolean
relative_inside(const gchar *path)
{
    g_auto(GStrv) parts = NULL;
    gsize i;

    if (path == NULL || *path == '\0' || g_path_is_absolute(path))
        return FALSE;
    parts = g_strsplit(path, "/", -1);
    for (i = 0; parts[i] != NULL; i++)
        if (g_strcmp0(parts[i], "..") == 0)
            return FALSE;
    return TRUE;
}

/* Each anchor a list of numbers, flattened; `want` numbers each. */
static GArray *
anchors_member(YamlMapping *mapping, const gchar *name, guint want)
{
    YamlSequence *sequence = sequence_member(mapping, name);
    GArray *out = g_array_new(FALSE, FALSE, sizeof(gdouble));
    guint i;

    for (i = 0; sequence != NULL && i < yaml_sequence_get_length(sequence); i++) {
        YamlNode *point = yaml_sequence_get_element(sequence, i);
        YamlSequence *xyz;
        guint j;

        if (point == NULL || yaml_node_get_node_type(point) != YAML_NODE_SEQUENCE)
            continue;
        xyz = yaml_node_get_sequence(point);
        if (yaml_sequence_get_length(xyz) != want)
            continue;
        for (j = 0; j < want; j++) {
            gdouble v = yaml_node_get_double(yaml_sequence_get_element(xyz, j));

            g_array_append_val(out, v);
        }
    }
    return out;
}

ClawtOfficeWorld *
clawt_office_world_load(const gchar *directory, GError **error)
{
    g_autoptr(ClawtOfficeWorld) self = NULL;
    g_autoptr(YamlParser) parser = NULL;
    g_autofree gchar *path = NULL;
    YamlNode *root;
    YamlMapping *top;
    YamlMapping *renderers;
    YamlSequence *zones;
    YamlSequence *assets;
    guint i;

    g_return_val_if_fail(directory != NULL, NULL);

    path = g_build_filename(directory, "office.yaml", NULL);
    parser = yaml_parser_new();
    if (!yaml_parser_load_from_file(parser, path, error))
        return NULL;
    root = yaml_parser_get_root(parser);
    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "%s: the document is not a mapping", path);
        return NULL;
    }
    top = yaml_node_get_mapping(root);

    self = g_object_new(CLAWT_TYPE_OFFICE_WORLD, NULL);
    self->directory = g_canonicalize_filename(directory, NULL);
    self->id = g_path_get_basename(self->directory);
    self->name = g_strdup(string_member(top, "name") != NULL
                          ? string_member(top, "name") : self->id);
    self->description = g_strdup(string_member(top, "description"));
    self->license = g_strdup(string_member(top, "license"));
    self->districts = strv_member(top, "districts");

    /* without a licence the pack could not be shared with anybody */
    if (self->license == NULL || *self->license == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "%s: a world pack needs a `license` (e.g. CC0-1.0)", path);
        return NULL;
    }

    renderers = mapping_member(top, "renderers");
    {
        YamlMapping *two = mapping_member(renderers, "2d");
        YamlMapping *three = mapping_member(renderers, "3d");

        self->map_2d = g_strdup(string_member(two, "map"));
        self->level_3d = g_strdup(string_member(three, "level"));
    }
    if (self->map_2d == NULL && self->level_3d == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "%s: `renderers` names neither a 2d map nor a 3d level", path);
        return NULL;
    }
    if ((self->map_2d != NULL && !relative_inside(self->map_2d)) ||
        (self->level_3d != NULL && !relative_inside(self->level_3d))) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "%s: a renderer file must be a relative path inside the pack",
                    path);
        return NULL;
    }

    zones = sequence_member(top, "zones");
    for (i = 0; zones != NULL && i < yaml_sequence_get_length(zones); i++) {
        YamlNode *node = yaml_sequence_get_element(zones, i);
        YamlMapping *m;
        ClawtOfficeZone *zone;
        const gchar *capacity;
        gsize t;

        if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
            continue;
        m = yaml_node_get_mapping(node);
        if (string_member(m, "id") == NULL) {
            g_ptr_array_add(self->warnings,
                            g_strdup_printf("zones[%u] has no id and is ignored", i));
            continue;
        }
        zone = g_new0(ClawtOfficeZone, 1);
        zone->id = g_strdup(string_member(m, "id"));
        zone->name = g_strdup(string_member(m, "name"));
        zone->team = g_strdup(string_member(m, "team"));
        zone->tags = strv_member(m, "tags");
        if (zone->tags == NULL)
            zone->tags = g_new0(gchar *, 1);
        capacity = string_member(m, "capacity");
        zone->capacity = capacity != NULL ? (guint)g_ascii_strtoull(capacity, NULL, 10) : 0;
        zone->anchors_2d = anchors_member(m, "anchors_2d", 2);
        zone->anchors_3d = anchors_member(m, "anchors_3d", 3);

        for (t = 0; zone->tags[t] != NULL; t++)
            if (!clawt_office_zone_tag_is_known(zone->tags[t]))
                g_ptr_array_add(self->warnings, g_strdup_printf(
                    "zone '%s' has the tag '%s', which nothing sends anybody to",
                    zone->id, zone->tags[t]));
        if (zone->anchors_2d->len == 0 && zone->anchors_3d->len == 0)
            g_ptr_array_add(self->warnings, g_strdup_printf(
                "zone '%s' has no anchors, so nobody can stand in it", zone->id));
        g_ptr_array_add(self->zones, zone);
    }

    assets = sequence_member(top, "assets");
    for (i = 0; assets != NULL && i < yaml_sequence_get_length(assets); i++) {
        YamlNode *node = yaml_sequence_get_element(assets, i);
        YamlMapping *m;
        ClawtOfficeAsset *asset;

        if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
            continue;
        m = yaml_node_get_mapping(node);
        if (!relative_inside(string_member(m, "path"))) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "%s: assets[%u] is not a relative path inside the pack",
                        path, i);
            return NULL;
        }
        asset = g_new0(ClawtOfficeAsset, 1);
        asset->path = g_strdup(string_member(m, "path"));
        asset->sha256 = g_strdup(string_member(m, "sha256"));
        asset->license = g_strdup(string_member(m, "license"));
        asset->source = g_strdup(string_member(m, "source"));
        g_ptr_array_add(self->assets, asset);
    }

    /* the pack's own choreography overrides, kept as text for the merge */
    {
        g_autofree gchar *chor_path = g_build_filename(self->directory,
                                                       "choreography.yaml", NULL);

        g_file_get_contents(chor_path, &self->choreography, NULL, NULL);
    }

    if (clawt_office_world_find_zone(self, "lounge", NULL, 0) == NULL)
        g_ptr_array_add(self->warnings, g_strdup(
            "there is no zone tagged `lounge`, which is where anything with no "
            "zone of its own goes"));

    return g_steal_pointer(&self);
}

const gchar *
clawt_office_world_get_id(ClawtOfficeWorld *self)
{
    return self->id;
}

const gchar *
clawt_office_world_get_name(ClawtOfficeWorld *self)
{
    return self->name;
}

const gchar *
clawt_office_world_get_directory(ClawtOfficeWorld *self)
{
    return self->directory;
}

const gchar *
clawt_office_world_get_license(ClawtOfficeWorld *self)
{
    return self->license;
}

gchar *
clawt_office_world_get_renderer(ClawtOfficeWorld *self, const gchar *dimension)
{
    const gchar *relative = g_strcmp0(dimension, "3d") == 0 ? self->level_3d
                          : g_strcmp0(dimension, "2d") == 0 ? self->map_2d : NULL;

    return relative != NULL ? g_build_filename(self->directory, relative, NULL) : NULL;
}

GPtrArray *
clawt_office_world_get_zones(ClawtOfficeWorld *self)
{
    return self->zones;
}

GPtrArray *
clawt_office_world_get_assets(ClawtOfficeWorld *self)
{
    return self->assets;
}

GPtrArray *
clawt_office_world_get_warnings(ClawtOfficeWorld *self)
{
    return self->warnings;
}

const gchar *
clawt_office_world_get_choreography_yaml(ClawtOfficeWorld *self)
{
    return self->choreography;
}

static gboolean
zone_has_tag(const ClawtOfficeZone *zone, const gchar *tag, guint *argument)
{
    gsize i;

    for (i = 0; zone->tags[i] != NULL; i++) {
        const gchar *t = zone->tags[i];
        gsize n = strlen(tag);

        if (g_strcmp0(t, tag) == 0) {
            *argument = 0;
            return TRUE;
        }
        if (strncmp(t, tag, n) == 0 && t[n] == ':') {
            *argument = (guint)g_ascii_strtoull(t + n + 1, NULL, 10);
            return TRUE;
        }
    }
    return FALSE;
}

const ClawtOfficeZone *
clawt_office_world_find_zone(ClawtOfficeWorld *self,
                             const gchar      *tag,
                             const gchar      *team,
                             guint             need)
{
    const ClawtOfficeZone *best = NULL;
    guint best_room = G_MAXUINT;
    gboolean best_team = FALSE;
    guint i;

    g_return_val_if_fail(CLAWT_IS_OFFICE_WORLD(self), NULL);

    if (tag == NULL)
        return NULL;
    for (i = 0; i < self->zones->len; i++) {
        const ClawtOfficeZone *zone = g_ptr_array_index(self->zones, i);
        guint argument = 0;
        guint room;
        gboolean ours;

        if (!zone_has_tag(zone, tag, &argument))
            continue;
        /* `meeting:8` holds eight; a plain capacity says the same */
        room = argument > 0 ? argument : (zone->capacity > 0 ? zone->capacity : G_MAXUINT - 1);
        if (need > 0 && room < need)
            continue;
        ours = team != NULL && g_strcmp0(zone->team, team) == 0;
        /* a team's own zone first, then the smallest that fits */
        if (best == NULL || (ours && !best_team) ||
            (ours == best_team && room < best_room)) {
            best = zone;
            best_room = room;
            best_team = ours;
        }
    }
    return best;
}

gboolean
clawt_office_world_verify_assets(ClawtOfficeWorld *self, GError **error)
{
    guint i;

    for (i = 0; i < self->assets->len; i++) {
        ClawtOfficeAsset *asset = g_ptr_array_index(self->assets, i);
        g_autofree gchar *path = g_build_filename(self->directory, asset->path, NULL);
        g_autofree gchar *data = NULL;
        g_autofree gchar *sum = NULL;
        gsize length = 0;

        if (!g_file_get_contents(path, &data, &length, error))
            return FALSE;
        if (asset->sha256 == NULL)
            continue;
        sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)data, length);
        if (g_ascii_strcasecmp(sum, asset->sha256) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "%s: sha256 is %s, the pack says %s", asset->path, sum,
                        asset->sha256);
            return FALSE;
        }
    }
    return TRUE;
}

static void
add_string(JsonBuilder *builder, const gchar *name, const gchar *value)
{
    if (value == NULL)
        return;
    json_builder_set_member_name(builder, name);
    json_builder_add_string_value(builder, value);
}

static void
add_numbers(JsonBuilder *builder, const gchar *name, GArray *values, guint each)
{
    guint i;

    json_builder_set_member_name(builder, name);
    json_builder_begin_array(builder);
    for (i = 0; i + each <= values->len; i += each) {
        guint j;

        json_builder_begin_array(builder);
        for (j = 0; j < each; j++)
            json_builder_add_double_value(builder, g_array_index(values, gdouble, i + j));
        json_builder_end_array(builder);
    }
    json_builder_end_array(builder);
}

JsonNode *
clawt_office_world_to_json(ClawtOfficeWorld *self)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    guint i;

    json_builder_begin_object(builder);
    add_string(builder, "id", self->id);
    add_string(builder, "name", self->name);
    add_string(builder, "description", self->description);
    add_string(builder, "license", self->license);
    /* relative paths only: the requester may not be on this machine */
    add_string(builder, "map_2d", self->map_2d);
    add_string(builder, "level_3d", self->level_3d);

    json_builder_set_member_name(builder, "zones");
    json_builder_begin_array(builder);
    for (i = 0; i < self->zones->len; i++) {
        ClawtOfficeZone *zone = g_ptr_array_index(self->zones, i);
        gsize t;

        json_builder_begin_object(builder);
        add_string(builder, "id", zone->id);
        add_string(builder, "name", zone->name);
        add_string(builder, "team", zone->team);
        json_builder_set_member_name(builder, "capacity");
        json_builder_add_int_value(builder, zone->capacity);
        json_builder_set_member_name(builder, "tags");
        json_builder_begin_array(builder);
        for (t = 0; zone->tags[t] != NULL; t++)
            json_builder_add_string_value(builder, zone->tags[t]);
        json_builder_end_array(builder);
        add_numbers(builder, "anchors_2d", zone->anchors_2d, 2);
        add_numbers(builder, "anchors_3d", zone->anchors_3d, 3);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);

    json_builder_set_member_name(builder, "assets");
    json_builder_begin_array(builder);
    for (i = 0; i < self->assets->len; i++) {
        ClawtOfficeAsset *asset = g_ptr_array_index(self->assets, i);

        json_builder_begin_object(builder);
        add_string(builder, "path", asset->path);
        add_string(builder, "sha256", asset->sha256);
        add_string(builder, "license", asset->license);
        add_string(builder, "source", asset->source);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);

    json_builder_set_member_name(builder, "warnings");
    json_builder_begin_array(builder);
    for (i = 0; i < self->warnings->len; i++)
        json_builder_add_string_value(builder, g_ptr_array_index(self->warnings, i));
    json_builder_end_array(builder);

    json_builder_set_member_name(builder, "districts");
    json_builder_begin_array(builder);
    for (i = 0; self->districts != NULL && self->districts[i] != NULL; i++)
        json_builder_add_string_value(builder, self->districts[i]);
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    return json_builder_get_root(builder);
}

GPtrArray *
clawt_office_worlds_discover(const gchar *const *directories, GPtrArray *refused)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, NULL);
    gsize d;

    for (d = 0; directories != NULL && directories[d] != NULL; d++) {
        g_autoptr(GDir) dir = g_dir_open(directories[d], 0, NULL);
        const gchar *entry;

        if (dir == NULL)
            continue;
        while ((entry = g_dir_read_name(dir)) != NULL) {
            g_autofree gchar *pack = g_build_filename(directories[d], entry, NULL);
            g_autofree gchar *manifest = g_build_filename(pack, "office.yaml", NULL);
            g_autoptr(GError) error = NULL;
            ClawtOfficeWorld *world;

            if (!g_file_test(manifest, G_FILE_TEST_IS_REGULAR) ||
                g_hash_table_contains(seen, entry))
                continue;
            world = clawt_office_world_load(pack, &error);
            if (world == NULL) {
                if (refused != NULL)
                    g_ptr_array_add(refused, g_strdup_printf("%s: %s", pack,
                                                             error->message));
                continue;
            }
            g_hash_table_add(seen, g_strdup(entry));
            g_ptr_array_add(out, world);
        }
    }
    return out;
}

/* ── Choreography ───────────────────────────────────────────────── */

/*
 * The built-in rules.  One per activity, plus one per station for
 * `working`.  This text is the default and the documentation of the
 * format at once: `clawtilla office choreography` prints it.
 */
static const gchar default_choreography[] =
    "choreography:\n"
    "  - { activity: offline,        zone: entrance,    pose: stand }\n"
    "  - { activity: idle,           zone: lounge,      pose: sit }\n"
    "  - { activity: thinking,       zone: desk,        pose: sit,     emote: thought }\n"
    "  - { activity: working,        zone: desk,        pose: type,    prop: laptop }\n"
    "  - { activity: working, station: desk,       zone: desk,       pose: type,    prop: laptop }\n"
    "  - { activity: working, station: library,    zone: library,    pose: read,    prop: book }\n"
    "  - { activity: working, station: lab,        zone: lab,        pose: type,    prop: terminal }\n"
    "  - { activity: working, station: whiteboard, zone: whiteboard, pose: present, prop: marker }\n"
    "  - { activity: working, station: terminal,   zone: terminal,   pose: type,    prop: screen }\n"
    "  - { activity: on_call,        zone: phone_booth, pose: phone,   prop: phone }\n"
    "  - { activity: in_meeting,     zone: meeting,     pose: sit }\n"
    "  - { activity: delivering,     zone: desk,        pose: walk,    prop: folder }\n"
    "  - { activity: waiting_on_you, zone: desk,        pose: stand,   emote: question }\n"
    "  - { activity: error,          zone: bug_corner,  pose: stand,   emote: alert }\n"
    "  - { activity: arriving,       zone: entrance,    pose: walk }\n"
    "  - { activity: leaving,        zone: entrance,    pose: walk,    emote: wave }\n";

struct _ClawtOfficeChoreography {
    GObject     parent_instance;
    GHashTable *cues;   /* "activity" or "activity/station" -> ClawtOfficeCue */
};

G_DEFINE_TYPE(ClawtOfficeChoreography, clawt_office_choreography, G_TYPE_OBJECT)

static void
cue_free(ClawtOfficeCue *cue)
{
    g_free(cue->zone);
    g_free(cue->pose);
    g_free(cue->prop);
    g_free(cue->emote);
    g_free(cue);
}

static void
clawt_office_choreography_finalize(GObject *object)
{
    g_hash_table_unref(CLAWT_OFFICE_CHOREOGRAPHY(object)->cues);
    G_OBJECT_CLASS(clawt_office_choreography_parent_class)->finalize(object);
}

static void
clawt_office_choreography_class_init(ClawtOfficeChoreographyClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_office_choreography_finalize;
}

static void
clawt_office_choreography_init(ClawtOfficeChoreography *self)
{
    self->cues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify)cue_free);
}

const gchar *
clawt_office_choreography_default_yaml(void)
{
    return default_choreography;
}

ClawtOfficeChoreography *
clawt_office_choreography_new_default(void)
{
    ClawtOfficeChoreography *self = g_object_new(CLAWT_TYPE_OFFICE_CHOREOGRAPHY, NULL);
    g_autoptr(GError) error = NULL;

    if (!clawt_office_choreography_override(self, default_choreography, &error))
        g_critical("office: the built-in choreography does not load: %s", error->message);
    return self;
}

gboolean
clawt_office_choreography_override(ClawtOfficeChoreography *self,
                                   const gchar             *yaml,
                                   GError                 **error)
{
    g_autoptr(YamlParser) parser = yaml_parser_new();
    YamlNode *root;
    YamlSequence *rules;
    g_autoptr(GHashTable) staged = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_OFFICE_CHOREOGRAPHY(self), FALSE);

    if (yaml == NULL)
        return TRUE;
    if (!yaml_parser_load_from_data(parser, yaml, -1, error))
        return FALSE;
    root = yaml_parser_get_root(parser);
    rules = root != NULL && yaml_node_get_node_type(root) == YAML_NODE_MAPPING
        ? sequence_member(yaml_node_get_mapping(root), "choreography") : NULL;
    if (rules == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "a choreography is a `choreography:` list");
        return FALSE;
    }

    /* every rule checked before any is applied: a half-applied override is worse */
    staged = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify)cue_free);
    for (i = 0; i < yaml_sequence_get_length(rules); i++) {
        YamlNode *node = yaml_sequence_get_element(rules, i);
        YamlMapping *m;
        const gchar *activity;
        const gchar *station;
        ClawtOfficeActivity parsed;
        ClawtOfficeCue *cue;

        if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
            continue;
        m = yaml_node_get_mapping(node);
        activity = string_member(m, "activity");
        station = string_member(m, "station");
        if (!clawt_office_activity_from_nick(activity, &parsed)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "choreography[%u]: '%s' is not an activity", i,
                        activity != NULL ? activity : "");
            return FALSE;
        }
        if (station != NULL &&
            clawt_office_station_from_nick(station) == CLAWT_OFFICE_STATION_NONE &&
            g_strcmp0(station, "none") != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "choreography[%u]: '%s' is not a station", i, station);
            return FALSE;
        }
        if (string_member(m, "zone") == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "choreography[%u]: every rule needs a zone", i);
            return FALSE;
        }
        cue = g_new0(ClawtOfficeCue, 1);
        cue->zone = g_strdup(string_member(m, "zone"));
        cue->pose = g_strdup(string_member(m, "pose") != NULL
                             ? string_member(m, "pose") : "stand");
        cue->prop = g_strdup(string_member(m, "prop"));
        cue->emote = g_strdup(string_member(m, "emote"));
        g_hash_table_replace(staged,
            station != NULL ? g_strdup_printf("%s/%s", activity, station)
                            : g_strdup(activity), cue);
    }

    {
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_iter_init(&iter, staged);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            g_hash_table_replace(self->cues, g_strdup(key), value);
            g_hash_table_iter_steal(&iter);
            g_free(key);
        }
    }
    return TRUE;
}

const ClawtOfficeCue *
clawt_office_choreography_cue(ClawtOfficeChoreography *self,
                              ClawtOfficeActivity      activity,
                              ClawtOfficeStation       station)
{
    static const ClawtOfficeCue lounge = { (gchar *)"lounge", (gchar *)"stand", NULL, NULL };
    const ClawtOfficeCue *cue = NULL;

    if (station != CLAWT_OFFICE_STATION_NONE) {
        g_autofree gchar *key = g_strdup_printf("%s/%s",
                                                clawt_office_activity_nick(activity),
                                                clawt_office_station_nick(station));

        cue = g_hash_table_lookup(self->cues, key);
    }
    if (cue == NULL)
        cue = g_hash_table_lookup(self->cues, clawt_office_activity_nick(activity));
    return cue != NULL ? cue : &lounge;
}

JsonNode *
clawt_office_choreography_to_json(ClawtOfficeChoreography *self)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GList) keys = g_hash_table_get_keys(self->cues);
    GList *l;

    keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
    json_builder_begin_object(builder);
    for (l = keys; l != NULL; l = l->next) {
        ClawtOfficeCue *cue = g_hash_table_lookup(self->cues, l->data);

        json_builder_set_member_name(builder, l->data);
        json_builder_begin_object(builder);
        add_string(builder, "zone", cue->zone);
        add_string(builder, "pose", cue->pose);
        add_string(builder, "prop", cue->prop);
        add_string(builder, "emote", cue->emote);
        json_builder_end_object(builder);
    }
    json_builder_end_object(builder);
    return json_builder_get_root(builder);
}
