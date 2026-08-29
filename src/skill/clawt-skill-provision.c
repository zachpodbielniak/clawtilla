/*
 * clawt-skill-provision.c - Putting an agent's skills where its CLI looks
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "skill/clawt-skill-provision.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include <libreclaw.h>
#include <ai-glib.h>

/*
 * Which registry origin each of libreclaw's CLI backends is, and
 * whether it takes a manifest rather than a symlink.
 *
 * Two columns and nothing more, because everything else -- the actual
 * directories -- is asked of the registry.  The first column exists
 * because libreclaw and ai-glib name the same harnesses differently and
 * neither library has the other's spelling; the second is clawtilla's
 * own decision, recorded once.
 *
 * The keys are what lc_provider_type_normalize() returns, so an agent
 * configured for something libreclaw does not drive lands here as
 * whatever libreclaw is actually going to run for it.
 */
static const struct {
    const gchar *normalized;
    const gchar *origin;
    gboolean     manifest;
} PROVIDER_ORIGINS[] = {
    { "claude-code",  "claude",      FALSE },

    /* The same CLI in a tmux pane, so the same directories. */
    { "claude-tmux",  "claude",      FALSE },

    { "opencode",     "opencode",    FALSE },
    { "grok-build",   "grok",        FALSE },
    { "cursor",       "cursor",      FALSE },

    /*
     * Antigravity gets `.agents/skills.json`, not a link.
     *
     * Its documentation describes a customization root with an explicit
     * `entries[].path` indirection and says nothing at all about
     * symlinks; a link there might work today and stop working on the
     * next release, and the failure would be a skill that quietly is
     * not loaded.  Using the mechanism the vendor built is the durable
     * answer.
     */
    { "antigravity",  "antigravity", TRUE }
};

/*
 * The front-matter key that says a command file is ours.
 *
 * The `.mcp.json` precedent exactly: clawtilla removes only what
 * clawtilla wrote, and the marker is in the artefact rather than in a
 * side record that could disagree with it.  A skill link needs no such
 * marker because where it *points* already says who made it.
 */
#define COMMAND_MARKER "clawtilla-skill"

const gchar *
clawt_skill_provider_origin(const gchar *provider)
{
    const gchar *normalized;
    gsize i;

    normalized = lc_provider_type_normalize(provider, "clawtilla");

    for (i = 0; i < G_N_ELEMENTS(PROVIDER_ORIGINS); i++) {
        if (g_strcmp0(PROVIDER_ORIGINS[i].normalized, normalized) == 0)
            return PROVIDER_ORIGINS[i].origin;
    }

    /*
     * Unreachable while the table covers what normalize() returns, and
     * a test pins exactly that.  If it ever is reached, claude's
     * directories are the right guess: normalize() itself falls back to
     * claude-code, so the CLI being run will be looking there.
     */
    return "claude";
}

static gboolean
origin_uses_manifest(const gchar *origin)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(PROVIDER_ORIGINS); i++) {
        if (g_strcmp0(PROVIDER_ORIGINS[i].origin, origin) == 0)
            return PROVIDER_ORIGINS[i].manifest;
    }

    return FALSE;
}

/* ── Asking the registry ─────────────────────────────────────────── */

/*
 * The directories this origin searches *inside* @workspace.
 *
 * The registry answers with project directories first and then the
 * user's own; only the project ones are ours to write into.  Filtering
 * by containment rather than by counting is what makes that robust
 * against the table gaining a row.
 */
static GStrv
paths_for(const gchar    *workspace,
          const gchar    *origin,
          AiResourceKind  kind)
{
    g_autoptr(AiResourceRegistry) registry = NULL;
    g_auto(GStrv) all = NULL;
    GPtrArray *out;
    gsize i;

    if (workspace == NULL || origin == NULL)
        return NULL;

    registry = ai_resource_registry_new();
    ai_resource_registry_set_working_directory(registry, workspace);

    all = ai_resource_registry_get_search_paths_for_origin(registry, origin,
                                                           kind);
    out = g_ptr_array_new();

    for (i = 0; all != NULL && all[i] != NULL; i++) {
        if (!clawt_path_is_within(all[i], workspace))
            continue;

        g_ptr_array_add(out, g_strdup(all[i]));
    }

    if (out->len == 0) {
        g_ptr_array_free(out, TRUE);
        return NULL;
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}

GStrv
clawt_skill_provision_paths(ClawtAgentConfig *agent, gboolean kind_commands)
{
    g_autofree gchar *workspace = NULL;
    const gchar *origin;

    g_return_val_if_fail(agent != NULL, NULL);

    workspace = clawt_agent_config_get_workspace(agent);
    origin = clawt_skill_provider_origin(
        clawt_agent_config_get_string(agent, "model.provider"));

    /*
     * A manifest provider has no directory to link into, and saying so
     * is the point: grok and antigravity have no commands concept at
     * all, and answering with a plausible-looking path would produce a
     * file nothing reads.
     */
    if (kind_commands && origin_uses_manifest(origin))
        return NULL;

    return paths_for(workspace, origin,
                     kind_commands ? AI_RESOURCE_COMMAND : AI_RESOURCE_SKILL);
}

/* ── Symlinks ────────────────────────────────────────────────────── */

/*
 * Is this entry one clawtilla made?
 *
 * Decided by where it points, and deliberately not by a manifest.  A
 * manifest is a second record of the same fact, and this codebase has
 * been bitten before by a record that outlived what it described: a
 * link removed by hand would stay in the manifest for ever, and a link
 * added by hand would never be recognised.  Pointing into the library
 * is a property of the link itself and cannot go stale.
 *
 * A *dangling* link still answers TRUE, which is the case that matters:
 * a broken symlink enumerates as a symlink and is skipped in silence by
 * every reader, so leaving one is leaving a skill that looks installed
 * and loads nothing.
 */
static gboolean
link_is_ours(const gchar *path, const gchar *skills_dir)
{
    g_autofree gchar *target = NULL;

    if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK))
        return FALSE;

    target = g_file_read_link(path, NULL);

    if (target == NULL)
        return FALSE;

    if (!g_path_is_absolute(target)) {
        g_autofree gchar *dir = g_path_get_dirname(path);
        gchar *joined = g_build_filename(dir, target, NULL);

        g_free(target);
        target = joined;
    }

    return clawt_path_is_within(target, skills_dir) ||
           g_strcmp0(target, skills_dir) == 0;
}

/*
 * Make @path a link to @target, replacing whatever we own there.
 *
 * The unlink-then-symlink is not laziness about atomicity: the case it
 * has to handle is a link that already exists and points at the right
 * place, which must not be recreated -- provisioning twice has to leave
 * the workspace byte-identical, or every daemon start would look like
 * an edit to whatever is watching the directory.
 */
static gboolean
ensure_link(const gchar  *path,
            const gchar  *target,
            GError      **error)
{
    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        g_autofree gchar *existing = g_file_read_link(path, NULL);

        if (g_strcmp0(existing, target) == 0 &&
            g_file_test(path, G_FILE_TEST_EXISTS))
            return TRUE;

        if (g_unlink(path) != 0 && errno != ENOENT) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "could not replace the link at %s: %s",
                        path, g_strerror(errno));
            return FALSE;
        }
    }

    if (symlink(target, path) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not link %s to %s: %s", path, target,
                    g_strerror(errno));
        return FALSE;
    }

    return TRUE;
}

/*
 * Take away every link of ours that is not in @wanted.
 *
 * Anything that is not a link of ours is left exactly as it is.  A real
 * directory at one of these paths is somebody's own skill, written into
 * the workspace by hand, and clawtilla deleting it because it was not on
 * a list would be destroying work nobody asked us to manage.
 */
static void
prune_links(const gchar *directory,
            const gchar *skills_dir,
            GHashTable  *wanted,
            GPtrArray   *warnings)
{
    g_autoptr(GDir) dir = NULL;
    const gchar *entry;

    dir = g_dir_open(directory, 0, NULL);

    if (dir == NULL)
        return;

    while ((entry = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *path = g_build_filename(directory, entry, NULL);

        if (!link_is_ours(path, skills_dir)) {
            if (g_hash_table_contains(wanted, entry) && warnings != NULL)
                g_ptr_array_add(warnings, g_strdup_printf(
                    "%s already exists and is not a link clawtilla made, so "
                    "the skill '%s' was not installed there; move it aside "
                    "if you want clawtilla to manage it",
                    path, entry));

            continue;
        }

        if (g_hash_table_contains(wanted, entry))
            continue;

        if (g_unlink(path) != 0 && warnings != NULL)
            g_ptr_array_add(warnings, g_strdup_printf(
                "could not remove the stale skill link %s: %s",
                path, g_strerror(errno)));
    }
}

/* ── Command files ───────────────────────────────────────────────── */

/*
 * What `/name` expands to.
 *
 * Short on purpose.  The skill's own text is what should be read, and
 * duplicating it here would give the harness two copies to disagree
 * about -- one of which nobody would remember to update.  `$ARGUMENTS`
 * is the one substitution every harness that has commands supports.
 */
static gchar *
render_command(ClawtSkill *skill)
{
    const gchar *description = clawt_skill_get_description(skill);

    return g_strdup_printf(
        "---\n"
        "description: %s\n"
        "%s: %s\n"
        "argument-hint: \"[what to apply it to]\"\n"
        "---\n"
        "\n"
        "Use your `%s` skill for what follows, reading its SKILL.md first.\n"
        "\n"
        "$ARGUMENTS\n",
        description != NULL ? description : "",
        COMMAND_MARKER, clawt_skill_get_name(skill),
        clawt_skill_get_name(skill));
}

static gboolean
command_is_ours(const gchar *path)
{
    g_autofree gchar *contents = NULL;
    g_autofree gchar *needle = NULL;

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return FALSE;

    needle = g_strconcat("\n", COMMAND_MARKER, ":", NULL);

    return strstr(contents, needle) != NULL;
}

static gboolean
write_commands(const gchar  *directory,
               GPtrArray    *bindings,
               GPtrArray    *warnings,
               GError      **error)
{
    g_autoptr(GHashTable) wanted = NULL;
    g_autoptr(GDir) dir = NULL;
    const gchar *entry;
    guint i;

    if (!clawt_ensure_dir(directory, 0700, error))
        return FALSE;

    wanted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (i = 0; i < bindings->len; i++) {
        ClawtSkillBinding *binding = g_ptr_array_index(bindings, i);
        g_autofree gchar *basename = NULL;
        g_autofree gchar *path = NULL;
        g_autofree gchar *text = NULL;
        g_autofree gchar *existing = NULL;

        if (!clawt_skill_binding_is_active(binding))
            continue;

        basename = g_strconcat(clawt_skill_binding_get_name(binding), ".md",
                               NULL);
        path = g_build_filename(directory, basename, NULL);
        g_hash_table_add(wanted, g_strdup(basename));

        if (g_file_test(path, G_FILE_TEST_EXISTS) && !command_is_ours(path)) {
            if (warnings != NULL)
                g_ptr_array_add(warnings, g_strdup_printf(
                    "%s already exists and clawtilla did not write it, so "
                    "/%s is left as you have it",
                    path, clawt_skill_binding_get_name(binding)));

            continue;
        }

        text = render_command(clawt_skill_binding_get_skill(binding));

        /*
         * Skipped when it has not changed, so a second provision writes
         * nothing.  A harness watching this directory would otherwise
         * see a change on every daemon start.
         */
        if (g_file_get_contents(path, &existing, NULL, NULL) &&
            g_strcmp0(existing, text) == 0)
            continue;

        if (!clawt_write_file_atomic(path, text, -1, 0600, FALSE, error))
            return FALSE;
    }

    dir = g_dir_open(directory, 0, NULL);

    while (dir != NULL && (entry = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *path = NULL;

        if (g_hash_table_contains(wanted, entry))
            continue;

        path = g_build_filename(directory, entry, NULL);

        if (!command_is_ours(path))
            continue;

        g_unlink(path);
    }

    return TRUE;
}

/* ── The manifest, for antigravity ───────────────────────────────── */

/*
 * `.agents/skills.json`, rewritten the way `.mcp.json` is.
 *
 * Entries pointing into the library are ours and are replaced wholesale;
 * everything else in the file is carried across untouched, because this
 * is a file a person may also have written in.
 */
static gboolean
write_manifest(const gchar  *directory,
               const gchar  *skills_dir,
               GPtrArray    *bindings,
               GError      **error)
{
    g_autofree gchar *path = NULL;
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *existing = NULL;
    JsonArray *carried = NULL;
    guint i;

    if (!clawt_ensure_dir(directory, 0700, error))
        return FALSE;

    path = g_build_filename(directory, "skills.json", NULL);
    parser = json_parser_new();

    if (json_parser_load_from_file(parser, path, NULL)) {
        JsonNode *root = json_parser_get_root(parser);

        if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *object = json_node_get_object(root);

            if (object != NULL && json_object_has_member(object, "entries"))
                carried = json_object_get_array_member(object, "entries");
        }
    }

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "entries");
    json_builder_begin_array(builder);

    for (i = 0; i < bindings->len; i++) {
        ClawtSkillBinding *binding = g_ptr_array_index(bindings, i);
        ClawtSkill *skill;

        if (!clawt_skill_binding_is_active(binding))
            continue;

        skill = clawt_skill_binding_get_skill(binding);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, clawt_skill_get_name(skill));
        json_builder_set_member_name(builder, "path");
        json_builder_add_string_value(builder,
                                      clawt_skill_get_directory(skill));
        json_builder_end_object(builder);
    }

    for (i = 0; carried != NULL && i < json_array_get_length(carried); i++) {
        JsonNode *element = json_array_get_element(carried, i);
        JsonObject *object;
        const gchar *entry_path;

        if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element))
            continue;

        object = json_node_get_object(element);

        if (object == NULL)
            continue;

        entry_path = json_object_has_member(object, "path")
                     ? json_object_get_string_member(object, "path") : NULL;

        /*
         * Ours by where it points, exactly as a symlink is.  Dropping
         * it here is what makes an unassigned skill disappear from the
         * file rather than lingering as an entry naming a directory
         * this agent is no longer meant to see.
         */
        if (entry_path != NULL && clawt_path_is_within(entry_path, skills_dir))
            continue;

        json_builder_add_value(builder, json_node_ref(element));
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    generator = json_generator_new();

    /*
     * json_builder_get_root() copies, and json_generator_set_root()
     * takes its own reference -- so the copy has to be held and dropped
     * here. Passing it straight in leaks one node per manifest write,
     * which is once per agent per start.
     */
    {
        g_autoptr(JsonNode) root = json_builder_get_root(builder);

        json_generator_set_root(generator, root);
    }

    json_generator_set_pretty(generator, TRUE);
    text = json_generator_to_data(generator, NULL);

    if (g_file_get_contents(path, &existing, NULL, NULL) &&
        g_strcmp0(existing, text) == 0)
        return TRUE;

    return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}

/* ── The whole pass ──────────────────────────────────────────────── */

gboolean
clawt_skill_provision(ClawtConfig        *config,
                      ClawtAgentConfig   *agent,
                      ClawtSkillLibrary  *library,
                      GPtrArray         **warnings,
                      GError            **error)
{
    g_autoptr(GPtrArray) bindings = NULL;
    g_autoptr(GPtrArray) found = NULL;
    g_autoptr(GHashTable) wanted = NULL;
    g_auto(GStrv) skill_dirs = NULL;
    g_auto(GStrv) command_dirs = NULL;
    g_autofree gchar *workspace = NULL;
    const gchar *skills_dir;
    const gchar *origin;
    guint i;

    g_return_val_if_fail(agent != NULL, FALSE);
    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(library), FALSE);

    found = g_ptr_array_new_with_free_func(g_free);
    workspace = clawt_agent_config_get_workspace(agent);
    skills_dir = clawt_skill_library_get_directory(library);

    if (workspace == NULL || skills_dir == NULL) {
        if (warnings != NULL)
            *warnings = g_steal_pointer(&found);

        return TRUE;
    }

    bindings = clawt_skill_resolve_for_agent(config, agent, library);

    {
        g_autoptr(GPtrArray) resolution =
            clawt_skill_bindings_warnings(
                bindings, clawt_agent_config_get_id(agent));

        for (i = 0; i < resolution->len; i++)
            g_ptr_array_add(found,
                            g_strdup(g_ptr_array_index(resolution, i)));
    }

    origin = clawt_skill_provider_origin(
        clawt_agent_config_get_string(agent, "model.provider"));

    skill_dirs = clawt_skill_provision_paths(agent, FALSE);

    if (skill_dirs == NULL || skill_dirs[0] == NULL) {
        if (warnings != NULL)
            *warnings = g_steal_pointer(&found);

        return TRUE;
    }

    if (origin_uses_manifest(origin)) {
        /*
         * The manifest lives beside the directory the registry named,
         * derived from it rather than written down: `.agents/skills`
         * is the search path, `.agents/skills.json` is the file.  One
         * source of truth for both.
         */
        g_autofree gchar *parent = g_path_get_dirname(skill_dirs[0]);

        if (!write_manifest(parent, skills_dir, bindings, error))
            return FALSE;

        if (warnings != NULL)
            *warnings = g_steal_pointer(&found);

        return TRUE;
    }

    if (!clawt_ensure_dir(skill_dirs[0], 0700, error))
        return FALSE;

    wanted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (i = 0; i < bindings->len; i++) {
        ClawtSkillBinding *binding = g_ptr_array_index(bindings, i);
        g_autofree gchar *path = NULL;
        const gchar *name;

        if (!clawt_skill_binding_is_active(binding))
            continue;

        name = clawt_skill_binding_get_name(binding);
        g_hash_table_add(wanted, g_strdup(name));
        path = g_build_filename(skill_dirs[0], name, NULL);

        /*
         * A real directory here is somebody's work and is never
         * replaced.  prune_links() reports it; this only has to not
         * clobber it.
         */
        if (g_file_test(path, G_FILE_TEST_EXISTS) &&
            !link_is_ours(path, skills_dir))
            continue;

        if (!ensure_link(path,
                         clawt_skill_get_directory(
                             clawt_skill_binding_get_skill(binding)),
                         error))
            return FALSE;
    }

    prune_links(skill_dirs[0], skills_dir, wanted, found);

    command_dirs = clawt_skill_provision_paths(agent, TRUE);

    if (command_dirs != NULL && command_dirs[0] != NULL) {
        if (!write_commands(command_dirs[0], bindings, found, error))
            return FALSE;
    }

    if (warnings != NULL)
        *warnings = g_steal_pointer(&found);

    return TRUE;
}

gchar *
clawt_skill_provision_describe(GPtrArray *bindings)
{
    g_autoptr(GString) out = NULL;
    guint active = 0;
    guint i;

    out = g_string_new("* Your skills\n\n");

    for (i = 0; bindings != NULL && i < bindings->len; i++) {
        if (clawt_skill_binding_is_active(g_ptr_array_index(bindings, i)))
            active++;
    }

    if (active == 0) {
        /*
         * Said rather than left blank.  An empty section reads as
         * "clawtilla has not worked this out yet", and an agent that
         * suspects it has procedures it cannot see will go looking for
         * them instead of doing the work.
         */
        g_string_append(out,
            "You have none. Nothing in this fleet has written a procedure\n"
            "for you to follow, so work from your own files and what you\n"
            "are asked.\n");

        return g_string_free(g_steal_pointer(&out), FALSE);
    }

    g_string_append(out,
        "Procedures written for you to follow. Read the whole SKILL.md\n"
        "before working from one -- the description below is the summary\n"
        "you are shown, not the procedure.\n\n");

    for (i = 0; i < bindings->len; i++) {
        ClawtSkillBinding *binding = g_ptr_array_index(bindings, i);
        ClawtSkill *skill;
        const gchar *description;

        if (!clawt_skill_binding_is_active(binding))
            continue;

        skill = clawt_skill_binding_get_skill(binding);
        description = clawt_skill_get_description(skill);

        g_string_append_printf(out, "- ~%s~ -- %s\n",
                               clawt_skill_get_name(skill),
                               description != NULL ? description : "");
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}
