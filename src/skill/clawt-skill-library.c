/*
 * clawt-skill-library.c - Every skill the fleet has, once
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "skill/clawt-skill-library.h"
#include "skill/clawt-skill-scan.h"

#include <glib/gstdio.h>
#include <string.h>

#include <yaml-glib.h>

/*
 * How long to wait after a file event before rescanning.
 *
 * An editor writes a file as several operations, so one save arrives as
 * four events; without this every one of them would rescan the whole
 * library and emit ::changed, which both clients redraw on.
 */
#define RESCAN_DEBOUNCE_MS (250)

/*
 * clawtilla's own record, beside the skill rather than inside SKILL.md.
 *
 * Inside would mean these keys were front matter, which every harness
 * reads and none of them understands -- and `enabled: false` sitting in
 * a file the harness loads anyway would read as a setting that does
 * something, which it does not.  Beside it, and a dotfile, so it is
 * carried by the symlink and ignored by everyone.
 */
#define SIDECAR_NAME ".clawtilla.yaml"

struct _ClawtSkillLibrary {
    GObject       parent_instance;

    gchar        *directory;
    GHashTable   *skills;      /* gchar* -> ClawtSkill* */
    GPtrArray    *order;       /* gchar*, names in sorted order */
    GPtrArray    *problems;    /* gchar* */

    gboolean      watching;
    GFileMonitor *monitor;
    GSource      *rescan;

    /*
     * The context the monitor and its debounce belong to.
     *
     * Captured in set_watching(), which is the function that attaches
     * the source.  Naming it at the caller has failed here five times
     * over: a daemon embedded in another program runs its own loop, and
     * a timer left on the global default in that arrangement is a timer
     * nobody ever dispatches.
     */
    GMainContext *context;
};

enum {
    PROP_0,
    PROP_DIRECTORY,
    PROP_WATCHING,
    N_PROPS
};

enum {
    SIGNAL_CHANGED,
    SIGNAL_SKILL_ADDED,
    SIGNAL_SKILL_REMOVED,
    N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint       signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(ClawtSkillLibrary, clawt_skill_library, G_TYPE_OBJECT)

/* ── The sidecar ─────────────────────────────────────────────────── */

static void
load_sidecar(ClawtSkill *skill, const gchar *directory)
{
    g_autofree gchar *path = g_build_filename(directory, SIDECAR_NAME, NULL);
    g_autoptr(YamlParser) parser = NULL;
    g_autoptr(GError) error = NULL;
    YamlNode *root;
    YamlMapping *fields;
    YamlNode *node;

    /*
     * No sidecar means a person put this directory here themselves,
     * which is the review.  That default has to be this way round: the
     * only skills that arrive without somebody's hand on them are the
     * imported and synthesized ones, and both of those write a sidecar
     * saying `enabled: false` *before* the SKILL.md they describe.
     */
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return;

    parser = yaml_parser_new();

    if (!yaml_parser_load_from_file(parser, path, &error)) {
        g_warning("skills: %s could not be read (%s); treating '%s' as "
                  "written here by hand", path, error->message,
                  clawt_skill_get_name(skill));
        return;
    }

    root = yaml_parser_get_root(parser);

    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
        return;

    fields = yaml_node_get_mapping(root);

    node = yaml_mapping_get_member(fields, "enabled");
    if (node != NULL)
        clawt_skill_set_enabled(skill, yaml_node_get_boolean(node));

    node = yaml_mapping_get_member(fields, "source");
    if (node != NULL) {
        gint value = 0;

        if (clawt_enum_from_nick(CLAWT_TYPE_SKILL_SOURCE,
                                 yaml_node_get_string(node), &value))
            clawt_skill_set_source(skill, (ClawtSkillSource)value);
    }

    node = yaml_mapping_get_member(fields, "origin");
    if (node != NULL)
        clawt_skill_set_origin_url(skill, yaml_node_get_string(node));

    node = yaml_mapping_get_member(fields, "sha256");
    if (node != NULL)
        clawt_skill_set_digest(skill, yaml_node_get_string(node));

    node = yaml_mapping_get_member(fields, "imported_at");
    if (node != NULL)
        clawt_skill_set_imported_at(skill, yaml_node_get_int(node));

    node = yaml_mapping_get_member(fields, "skipped");
    if (node != NULL &&
        yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE) {
        YamlSequence *sequence = yaml_node_get_sequence(node);
        guint i;

        for (i = 0; i < yaml_sequence_get_length(sequence); i++) {
            YamlNode *element = yaml_sequence_get_element(sequence, i);
            const gchar *value = yaml_node_get_string(element);

            if (value != NULL)
                clawt_skill_add_skipped(skill, value);
        }
    }
}

static gboolean
save_sidecar(ClawtSkill *skill, const gchar *directory, GError **error)
{
    g_autofree gchar *path = g_build_filename(directory, SIDECAR_NAME, NULL);
    g_autoptr(YamlNode) root = yaml_node_new_mapping(NULL);
    YamlMapping *fields = yaml_node_get_mapping(root);
    g_autoptr(YamlGenerator) generator = NULL;
    g_autofree gchar *text = NULL;
    GPtrArray *skipped;

    {
        g_autoptr(YamlNode) enabled =
            yaml_node_new_boolean(clawt_skill_get_enabled(skill));
        g_autoptr(YamlNode) source = yaml_node_new_string(
            clawt_enum_to_nick(CLAWT_TYPE_SKILL_SOURCE,
                               (gint)clawt_skill_get_source(skill)));

        yaml_mapping_set_member(fields, "enabled", enabled);
        yaml_mapping_set_member(fields, "source", source);
    }

    if (clawt_skill_get_origin_url(skill) != NULL) {
        g_autoptr(YamlNode) origin =
            yaml_node_new_string(clawt_skill_get_origin_url(skill));

        yaml_mapping_set_member(fields, "origin", origin);
    }

    if (clawt_skill_get_digest(skill) != NULL) {
        g_autoptr(YamlNode) digest =
            yaml_node_new_string(clawt_skill_get_digest(skill));

        yaml_mapping_set_member(fields, "sha256", digest);
    }

    if (clawt_skill_get_imported_at(skill) != 0) {
        g_autoptr(YamlNode) stamp =
            yaml_node_new_int(clawt_skill_get_imported_at(skill));

        yaml_mapping_set_member(fields, "imported_at", stamp);
    }

    skipped = clawt_skill_get_skipped(skill);

    if (skipped != NULL && skipped->len > 0) {
        g_autoptr(YamlNode) list = yaml_node_new_sequence(NULL);
        YamlSequence *sequence = yaml_node_get_sequence(list);
        guint i;

        for (i = 0; i < skipped->len; i++) {
            g_autoptr(YamlNode) element =
                yaml_node_new_string(g_ptr_array_index(skipped, i));

            yaml_sequence_add_element(sequence, element);
        }

        yaml_mapping_set_member(fields, "skipped", list);
    }

    generator = yaml_generator_new();
    yaml_generator_set_root(generator, root);
    text = yaml_generator_to_data(generator, NULL, error);

    if (text == NULL)
        return FALSE;

    return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}

/* ── Scanning ────────────────────────────────────────────────────── */

static void
library_note_problem(ClawtSkillLibrary *self,
                     const gchar       *name,
                     const gchar       *message)
{
    g_ptr_array_add(self->problems,
                    g_strdup_printf("%s: %s", name, message));
}

static ClawtSkill *
library_read_one(ClawtSkillLibrary *self,
                 const gchar       *name,
                 GError           **error)
{
    g_autofree gchar *directory = NULL;
    g_autofree gchar *file = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(ClawtSkill) skill = NULL;
    g_autofree gchar *digest = NULL;

    directory = clawt_skill_directory_for(self->directory, name);

    if (directory == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable skill name", name);
        return NULL;
    }

    file = g_build_filename(directory, "SKILL.md", NULL);

    if (!g_file_get_contents(file, &text, NULL, error))
        return NULL;

    skill = clawt_skill_parse(text, -1, name, error);

    if (skill == NULL)
        return NULL;

    clawt_skill_set_directory(skill, directory);
    load_sidecar(skill, directory);

    /*
     * Provenance is checked, not merely stored.
     *
     * A digest that was recorded at import and no longer matches means
     * the file changed after review, which is precisely the case the
     * record exists to catch -- and it is a *warning* rather than a
     * refusal, because the ordinary reason for it is that somebody
     * edited the skill on purpose.
     */
    digest = clawt_skill_digest(text);

    if (clawt_skill_get_digest(skill) != NULL &&
        g_strcmp0(clawt_skill_get_digest(skill), digest) != 0)
        clawt_skill_add_warning(skill,
            "This file has changed since it was imported. If that was not "
            "you, read it again before relying on it.");

    return g_steal_pointer(&skill);
}

static gint
compare_names(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

void
clawt_skill_library_scan(ClawtSkillLibrary *self)
{
    g_autoptr(GDir) dir = NULL;
    g_autoptr(GError) open_error = NULL;
    g_autoptr(GHashTable) previous = NULL;
    const gchar *entry;
    guint i;

    g_return_if_fail(CLAWT_IS_SKILL_LIBRARY(self));

    /*
     * The old set is kept so that ::skill-added and ::skill-removed can
     * say what actually changed.  A client that only had ::changed
     * would rebuild its whole list on every keystroke in an editor.
     */
    previous = g_steal_pointer(&self->skills);
    self->skills = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)clawt_skill_free);
    g_ptr_array_set_size(self->order, 0);
    g_ptr_array_set_size(self->problems, 0);

    if (self->directory == NULL)
        goto compare;

    dir = g_dir_open(self->directory, 0, &open_error);

    if (dir == NULL) {
        /*
         * Absent is the ordinary case and is not a problem worth
         * reporting: `skills.dir` has a default and most fleets never
         * create it.  Unreadable is different, and is worth saying,
         * because it looks identical from every other view.
         */
        if (!g_error_matches(open_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            library_note_problem(self, self->directory, open_error->message);

        goto compare;
    }

    while ((entry = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *path = NULL;
        g_autofree gchar *marker = NULL;
        g_autoptr(GError) error = NULL;
        ClawtSkill *skill;

        path = g_build_filename(self->directory, entry, NULL);

        if (!g_file_test(path, G_FILE_TEST_IS_DIR))
            continue;

        marker = g_build_filename(path, "SKILL.md", NULL);

        if (!g_file_test(marker, G_FILE_TEST_IS_REGULAR))
            continue;

        /*
         * The name gate again, on the way *in* from the filesystem.
         *
         * A directory somebody created by hand is untrusted input as
         * much as a name off the wire: this is where a `Legal-Review`
         * or a `my skill` is caught, and it is caught with a sentence
         * rather than by being silently absent from every listing.
         */
        if (!clawt_skill_name_is_valid(entry)) {
            library_note_problem(self, entry,
                "not a usable skill directory name -- use lowercase "
                "letters, digits and single hyphens");
            continue;
        }

        skill = library_read_one(self, entry, &error);

        if (skill == NULL) {
            library_note_problem(self, entry, error->message);
            continue;
        }

        g_hash_table_replace(self->skills, g_strdup(entry), skill);
        g_ptr_array_add(self->order, g_strdup(entry));
    }

    g_ptr_array_sort(self->order, compare_names);

compare:
    {
        GHashTableIter iter;
        gpointer key;

        for (i = 0; i < self->order->len; i++) {
            const gchar *name = g_ptr_array_index(self->order, i);

            if (previous == NULL || !g_hash_table_contains(previous, name))
                g_signal_emit(self, signals[SIGNAL_SKILL_ADDED], 0, name);
        }

        if (previous != NULL) {
            g_hash_table_iter_init(&iter, previous);

            while (g_hash_table_iter_next(&iter, &key, NULL)) {
                if (!g_hash_table_contains(self->skills, key))
                    g_signal_emit(self, signals[SIGNAL_SKILL_REMOVED], 0, key);
            }
        }
    }
}

/* ── Watching ────────────────────────────────────────────────────── */

static gboolean
on_rescan(gpointer user_data)
{
    ClawtSkillLibrary *self = user_data;

    g_clear_pointer(&self->rescan, g_source_unref);

    clawt_skill_library_scan(self);
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);

    return G_SOURCE_REMOVE;
}

static void
on_directory_changed(GFileMonitor      *monitor,
                     GFile             *file,
                     GFile             *other,
                     GFileMonitorEvent  event,
                     gpointer           user_data)
{
    ClawtSkillLibrary *self = user_data;
    GSource *source;

    (void)monitor;
    (void)file;
    (void)other;
    (void)event;

    if (self->rescan != NULL)
        return;

    source = g_timeout_source_new(RESCAN_DEBOUNCE_MS);
    g_source_set_callback(source, on_rescan, self, NULL);

    /*
     * Attached to the captured context, not to whatever is
     * thread-default right now.  This runs from a signal dispatch, and
     * dispatching a source does not push that source's own context --
     * so asking here would get the global default and the timer would
     * never fire under an embedded daemon.
     */
    g_source_attach(source, self->context);
    self->rescan = source;
}

void
clawt_skill_library_set_watching(ClawtSkillLibrary *self, gboolean watching)
{
    g_return_if_fail(CLAWT_IS_SKILL_LIBRARY(self));

    if (self->watching == watching)
        return;

    self->watching = watching;

    if (!watching) {
        if (self->rescan != NULL) {
            g_source_destroy(self->rescan);
            g_clear_pointer(&self->rescan, g_source_unref);
        }

        if (self->monitor != NULL) {
            g_signal_handlers_disconnect_by_data(self->monitor, self);
            g_file_monitor_cancel(self->monitor);
            g_clear_object(&self->monitor);
        }

        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WATCHING]);
        return;
    }

    /*
     * Captured here, in the function that attaches the sources -- both
     * the monitor's own emission and the debounce below hang off it.
     */
    g_clear_pointer(&self->context, g_main_context_unref);
    self->context = g_main_context_ref_thread_default();

    if (self->directory != NULL &&
        g_file_test(self->directory, G_FILE_TEST_IS_DIR)) {
        g_autoptr(GFile) dir = g_file_new_for_path(self->directory);
        g_autoptr(GError) error = NULL;

        self->monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES,
                                                 NULL, &error);

        if (self->monitor == NULL) {
            /*
             * A warning and not a failure.  Watching is a convenience;
             * the library still works, it just needs `skill reload`.
             * Refusing here would mean a filesystem with no inotify
             * left the fleet with no skills at all.
             */
            g_warning("skills: cannot watch %s (%s); edits will need "
                      "'clawtilla skill reload'", self->directory,
                      error->message);
        } else {
            g_signal_connect(self->monitor, "changed",
                             G_CALLBACK(on_directory_changed), self);
        }
    }

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WATCHING]);
}

gboolean
clawt_skill_library_get_watching(ClawtSkillLibrary *self)
{
    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), FALSE);

    return self->watching;
}

/* ── Reading ─────────────────────────────────────────────────────── */

GPtrArray *
clawt_skill_library_list(ClawtSkillLibrary *self)
{
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), NULL);

    out = g_ptr_array_new();

    for (i = 0; i < self->order->len; i++) {
        const gchar *name = g_ptr_array_index(self->order, i);

        g_ptr_array_add(out, g_hash_table_lookup(self->skills, name));
    }

    return out;
}

ClawtSkill *
clawt_skill_library_lookup(ClawtSkillLibrary *self, const gchar *name)
{
    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), NULL);

    if (name == NULL)
        return NULL;

    return g_hash_table_lookup(self->skills, name);
}

GPtrArray *
clawt_skill_library_get_problems(ClawtSkillLibrary *self)
{
    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), NULL);

    return self->problems;
}

const gchar *
clawt_skill_library_get_directory(ClawtSkillLibrary *self)
{
    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), NULL);

    return self->directory;
}

/* ── Writing ─────────────────────────────────────────────────────── */

/*
 * Write a skill's two files into its directory.
 *
 * The sidecar goes first, deliberately.  A crash between the two leaves
 * a directory with clawtilla's record and no SKILL.md, which is not a
 * skill and is skipped; the other order would leave a SKILL.md with no
 * record, which reads as "somebody wrote this here by hand" -- enabled.
 * The failure has to fall on the safe side of that.
 */
static gboolean
library_write(ClawtSkillLibrary  *self,
              ClawtSkill         *skill,
              const gchar        *directory,
              GError            **error)
{
    g_autofree gchar *file = NULL;
    g_autofree gchar *text = NULL;

    (void)self;

    if (!clawt_ensure_dir(directory, 0700, error))
        return FALSE;

    if (!save_sidecar(skill, directory, error))
        return FALSE;

    file = g_build_filename(directory, "SKILL.md", NULL);
    text = clawt_skill_render(skill);

    return clawt_write_file_atomic(file, text, -1, 0600, FALSE, error);
}

/*
 * The one write path, whoever asked for it.
 *
 * `create` and `create_taught` differ in exactly two values -- the
 * provenance and whether the skill is enabled -- and in nothing else.
 * A separate implementation for the AI-written one would be a second
 * copy of the name rules, the description bound and the traversal gate,
 * and it would be the copy nobody read.
 *
 * Enabled follows from the source rather than being a parameter: a
 * person who sat and wrote a skill has reviewed it, and nothing else
 * has. Making that a flag a caller passes would be an invitation to
 * pass TRUE.
 */
static ClawtSkill *
library_create(ClawtSkillLibrary  *self,
               const gchar        *name,
               const gchar        *description,
               const gchar        *body,
               ClawtSkillSource    source,
               const gchar        *origin,
               GError            **error)
{
    g_autofree gchar *directory = NULL;
    g_autofree gchar *rendered = NULL;
    g_autofree gchar *digest = NULL;
    g_autoptr(ClawtSkill) skill = NULL;

    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), NULL);

    if (self->directory == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "no skills directory is configured");
        return NULL;
    }

    directory = clawt_skill_directory_for(self->directory, name);

    if (directory == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a skill name: use lowercase letters, digits "
                    "and single hyphens, at most %d characters",
                    name != NULL ? name : "", CLAWT_SKILL_MAX_NAME);
        return NULL;
    }

    /*
     * Checked before anything is created, so a refusal leaves no
     * half-made directory behind for the next scan to report as a
     * broken skill.
     */
    if (description == NULL || *description == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "a skill needs a description; it is the only part an "
                    "agent sees before opening it");
        return NULL;
    }

    if (g_file_test(directory, G_FILE_TEST_EXISTS)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "a skill called '%s' is already here", name);
        return NULL;
    }

    skill = clawt_skill_new(name);
    clawt_skill_set_description(skill, description);
    clawt_skill_set_source(skill, source);
    clawt_skill_set_enabled(skill, source == CLAWT_SKILL_SOURCE_USER);
    clawt_skill_set_origin_url(skill, origin);

    if (source != CLAWT_SKILL_SOURCE_USER)
        clawt_skill_set_imported_at(skill, g_get_real_time());
    clawt_skill_set_body(skill,
        (body != NULL && *body != '\0')
            ? body
            : "Say what this procedure is for, then the steps.\n"
              "\n"
              "The description in the front matter is the only part an\n"
              "agent reads before deciding to open this, so write it as\n"
              "\"use this when ...\" rather than as a title.\n");

    rendered = clawt_skill_render(skill);
    digest = clawt_skill_digest(rendered);
    clawt_skill_set_digest(skill, digest);

    if (!library_write(self, skill, directory, error))
        return NULL;

    clawt_skill_library_scan(self);
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);

    return clawt_skill_library_lookup(self, name);
}

ClawtSkill *
clawt_skill_library_create(ClawtSkillLibrary  *self,
                           const gchar        *name,
                           const gchar        *description,
                           const gchar        *body,
                           GError            **error)
{
    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), NULL);

    return library_create(self, name, description, body,
                          CLAWT_SKILL_SOURCE_USER, NULL, error);
}

ClawtSkill *
clawt_skill_library_create_taught(ClawtSkillLibrary  *self,
                                  const gchar        *name,
                                  const gchar        *description,
                                  const gchar        *body,
                                  const gchar        *origin,
                                  GError            **error)
{
    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), NULL);

    return library_create(self, name, description, body,
                          CLAWT_SKILL_SOURCE_TAUGHT, origin, error);
}

/*
 * What a sibling file's presence means, in one place.
 *
 * Markdown comes across; everything else is recorded and left behind.
 * Not a denylist of dangerous extensions: the audit that motivates this
 * found payloads in `.py`, `.sh`, `.js` and a `Makefile`, and any list
 * of what to refuse is a list somebody adds a new extension to.
 */
static gboolean
is_importable(const gchar *basename)
{
    return g_str_has_suffix(basename, ".md") ||
           g_str_has_suffix(basename, ".markdown");
}

ClawtSkill *
clawt_skill_library_import(ClawtSkillLibrary  *self,
                           const gchar        *source,
                           const gchar        *origin,
                           GError            **error)
{
    g_autofree gchar *source_dir = NULL;
    g_autofree gchar *marker = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *directory = NULL;
    g_autofree gchar *rendered = NULL;
    g_autofree gchar *digest = NULL;
    g_autoptr(ClawtSkill) skill = NULL;
    g_autoptr(GDir) dir = NULL;
    const gchar *entry;
    const gchar *name;

    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), NULL);
    g_return_val_if_fail(source != NULL, NULL);

    if (self->directory == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "no skills directory is configured");
        return NULL;
    }

    if (g_file_test(source, G_FILE_TEST_IS_DIR)) {
        source_dir = g_strdup(source);
        marker = g_build_filename(source_dir, "SKILL.md", NULL);
    } else {
        source_dir = g_path_get_dirname(source);
        marker = g_strdup(source);
    }

    if (!g_file_test(marker, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "%s holds no SKILL.md", source);
        return NULL;
    }

    if (!g_file_get_contents(marker, &text, NULL, error))
        return NULL;

    /*
     * Parsed and validated *before* anything is written, so a malformed
     * import leaves no half-made directory for the next scan to find
     * and report as a broken skill.
     */
    skill = clawt_skill_parse(text, -1, NULL, error);

    if (skill == NULL)
        return NULL;

    name = clawt_skill_get_name(skill);
    directory = clawt_skill_directory_for(self->directory, name);

    if (directory == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable skill name", name);
        return NULL;
    }

    if (g_file_test(directory, G_FILE_TEST_EXISTS)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "a skill called '%s' is already here; remove it first if "
                    "you mean to replace it", name);
        return NULL;
    }

    /*
     * Disabled, and the reason is written where somebody will read it.
     * Nothing in this file reaches any prompt until a person has said
     * so.
     */
    clawt_skill_set_enabled(skill, FALSE);
    clawt_skill_set_source(skill, CLAWT_SKILL_SOURCE_IMPORTED);
    clawt_skill_set_origin_url(skill, origin != NULL ? origin : source);
    clawt_skill_set_imported_at(skill, g_get_real_time() / G_USEC_PER_SEC);

    rendered = clawt_skill_render(skill);
    digest = clawt_skill_digest(rendered);
    clawt_skill_set_digest(skill, digest);

    /*
     * Everything else in the source directory, recorded and not copied.
     *
     * Listed rather than logged: a skill whose steps say "run
     * scripts/setup.sh" will fail, and a person who was not told the
     * script was left behind will read that as clawtilla being broken.
     */
    dir = g_dir_open(source_dir, 0, NULL);

    while (dir != NULL && (entry = g_dir_read_name(dir)) != NULL) {
        if (g_strcmp0(entry, "SKILL.md") == 0)
            continue;

        if (is_importable(entry))
            continue;

        clawt_skill_add_skipped(skill, entry);
    }

    if (!library_write(self, skill, directory, error))
        return NULL;

    /*
     * The sibling markdown, after the two files that matter.  A
     * reference document a skill links to is text and carries none of
     * the risk a script does.
     */
    if (dir != NULL) {
        g_dir_rewind(dir);

        while ((entry = g_dir_read_name(dir)) != NULL) {
            g_autofree gchar *from = NULL;
            g_autofree gchar *to = NULL;
            g_autofree gchar *contents = NULL;
            gsize length = 0;

            if (g_strcmp0(entry, "SKILL.md") == 0 || !is_importable(entry))
                continue;

            from = g_build_filename(source_dir, entry, NULL);

            if (!g_file_test(from, G_FILE_TEST_IS_REGULAR))
                continue;

            if (!g_file_get_contents(from, &contents, &length, NULL))
                continue;

            to = g_build_filename(directory, entry, NULL);
            clawt_write_file_atomic(to, contents, (gssize)length, 0600, FALSE,
                                    NULL);
        }
    }

    clawt_skill_library_scan(self);
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);

    return clawt_skill_library_lookup(self, name);
}

gboolean
clawt_skill_library_set_enabled(ClawtSkillLibrary  *self,
                                const gchar        *name,
                                gboolean            enabled,
                                GError            **error)
{
    ClawtSkill *skill;

    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), FALSE);

    skill = clawt_skill_library_lookup(self, name);

    if (skill == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no skill called '%s'", name != NULL ? name : "");
        return FALSE;
    }

    clawt_skill_set_enabled(skill, enabled);

    if (!save_sidecar(skill, clawt_skill_get_directory(skill), error))
        return FALSE;

    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);

    return TRUE;
}

gboolean
clawt_skill_library_remove(ClawtSkillLibrary  *self,
                           const gchar        *name,
                           GError            **error)
{
    g_autofree gchar *directory = NULL;

    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(self), FALSE);

    if (self->directory == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "no skills directory is configured");
        return FALSE;
    }

    directory = clawt_skill_directory_for(self->directory, name);

    if (directory == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a skill name", name != NULL ? name : "");
        return FALSE;
    }

    if (!g_file_test(directory, G_FILE_TEST_IS_DIR)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no skill called '%s'", name);
        return FALSE;
    }

    /*
     * Bounded by the library's own root, checked canonically per child.
     * The name came from a client and the root came from a config file;
     * only the first of those has been through the name gate.
     */
    if (!clawt_remove_tree(directory, self->directory, error))
        return FALSE;

    clawt_skill_library_scan(self);
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);

    return TRUE;
}

/* ── GObject plumbing ────────────────────────────────────────────── */

static void
clawt_skill_library_dispose(GObject *object)
{
    ClawtSkillLibrary *self = CLAWT_SKILL_LIBRARY(object);

    /*
     * The debounce goes with the monitor.  A timeout that fired between
     * dispose and finalize would rescan a half-torn object.
     */
    clawt_skill_library_set_watching(self, FALSE);

    G_OBJECT_CLASS(clawt_skill_library_parent_class)->dispose(object);
}

static void
clawt_skill_library_finalize(GObject *object)
{
    ClawtSkillLibrary *self = CLAWT_SKILL_LIBRARY(object);

    g_clear_pointer(&self->directory, g_free);
    g_clear_pointer(&self->skills, g_hash_table_unref);
    g_clear_pointer(&self->order, g_ptr_array_unref);
    g_clear_pointer(&self->problems, g_ptr_array_unref);
    g_clear_pointer(&self->context, g_main_context_unref);

    G_OBJECT_CLASS(clawt_skill_library_parent_class)->finalize(object);
}

static void
clawt_skill_library_get_property(GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
    ClawtSkillLibrary *self = CLAWT_SKILL_LIBRARY(object);

    switch (prop_id) {
    case PROP_DIRECTORY:
        g_value_set_string(value, self->directory);
        break;

    case PROP_WATCHING:
        g_value_set_boolean(value, self->watching);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
clawt_skill_library_set_property(GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
    ClawtSkillLibrary *self = CLAWT_SKILL_LIBRARY(object);

    switch (prop_id) {
    case PROP_DIRECTORY:
        g_free(self->directory);
        self->directory = g_value_dup_string(value);
        break;

    case PROP_WATCHING:
        clawt_skill_library_set_watching(self, g_value_get_boolean(value));
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
clawt_skill_library_class_init(ClawtSkillLibraryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_skill_library_dispose;
    object_class->finalize = clawt_skill_library_finalize;
    object_class->get_property = clawt_skill_library_get_property;
    object_class->set_property = clawt_skill_library_set_property;

    properties[PROP_DIRECTORY] = g_param_spec_string(
        "directory", "Directory", "Where the skills live", NULL,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    properties[PROP_WATCHING] = g_param_spec_boolean(
        "watching", "Watching", "Whether the directory is followed", FALSE,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);

    /**
     * ClawtSkillLibrary::changed:
     *
     * Something about the library is different: a skill appeared,
     * vanished, was edited, or was enabled.
     */
    signals[SIGNAL_CHANGED] = g_signal_new(
        "changed", CLAWT_TYPE_SKILL_LIBRARY, G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 0);

    /**
     * ClawtSkillLibrary::skill-added:
     * @self: the library
     * @name: the skill's name
     */
    signals[SIGNAL_SKILL_ADDED] = g_signal_new(
        "skill-added", CLAWT_TYPE_SKILL_LIBRARY, G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * ClawtSkillLibrary::skill-removed:
     * @self: the library
     * @name: the skill's name
     */
    signals[SIGNAL_SKILL_REMOVED] = g_signal_new(
        "skill-removed", CLAWT_TYPE_SKILL_LIBRARY, G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
clawt_skill_library_init(ClawtSkillLibrary *self)
{
    self->skills = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)clawt_skill_free);
    self->order = g_ptr_array_new_with_free_func(g_free);
    self->problems = g_ptr_array_new_with_free_func(g_free);
}

ClawtSkillLibrary *
clawt_skill_library_new(const gchar *directory)
{
    return g_object_new(CLAWT_TYPE_SKILL_LIBRARY,
                        "directory", directory,
                        NULL);
}
