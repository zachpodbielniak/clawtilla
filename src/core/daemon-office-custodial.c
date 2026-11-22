/*
 * daemon-office-custodial.c - The team that keeps the office, and its tools
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three things, all optional and none automatic at daemon start:
 *
 *   - installing a custodial team: a team carrying the `office-custodial`
 *     tag (named whatever the operator likes) with a lead, an asset
 *     steward and a set designer;
 *   - the office tools (clawtilla_office_*), which agents with
 *     `tools.office` use to read, change and apply world packs -- the
 *     chief has it by role, the custodians by template;
 *   - commissioning a character: when an agent is created while the
 *     office is on and `office.asset_generator` names a generator, a task
 *     goes to the custodial lead (or the chief) to make that agent's
 *     character from its picture.  Filed, never generated silently, and
 *     never in the way of the agent being created.
 *
 * Every path an agent names is checked in the library
 * (clawt-office-pack.c), not here: the tools are one caller of it.
 */

#include "clawtilla.h"
#include "clawt-daemon-private.h"
#include "office/clawt-office-pack.h"
#include "office/clawt-office-world.h"
#include "agent/clawt-face.h"
#include "agent/clawt-team.h"

#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#define WORLD_READ_MAX (256 * 1024)

/* ── Where things live ──────────────────────────────────────────── */

static gchar *
own_worlds_dir(ClawtDaemon *self)
{
    return g_build_filename(self->state_dir, "office", "worlds", NULL);
}

gchar *
clawt_daemon_office_characters_dir(ClawtDaemon *self)
{
    return g_build_filename(self->state_dir, "office", "characters", NULL);
}

/*
 * A world's id is a directory name; the same rule as an agent id, so it
 * cannot be a path.
 */
static gboolean
world_id_is_valid(const gchar *id)
{
    const gchar *p;

    if (id == NULL || *id == '\0' || strlen(id) > 64 || *id == '-')
        return FALSE;
    for (p = id; *p != '\0'; p++)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            return FALSE;
    return TRUE;
}

static ClawtOfficeWorld *
find_world_by_id(ClawtDaemon *self, const gchar *id)
{
    g_auto(GStrv) dirs = clawt_daemon_office_world_dirs(self);
    g_autoptr(GPtrArray) worlds = clawt_office_worlds_discover((const gchar *const *)dirs,
                                                               NULL);
    guint i;

    for (i = 0; i < worlds->len; i++) {
        ClawtOfficeWorld *world = g_ptr_array_index(worlds, i);

        if (g_strcmp0(clawt_office_world_get_id(world), id) == 0)
            return g_object_ref(world);
    }
    return NULL;
}

/* Whether @directory is in one of the operator's own office.world_dirs. */
static gboolean
in_operator_dirs(ClawtDaemon *self, const gchar *directory)
{
    g_auto(GStrv) extra = clawt_config_get_string_list(self->config, "office.world_dirs");
    gsize i;

    for (i = 0; extra != NULL && extra[i] != NULL; i++) {
        g_autofree gchar *expanded = clawt_expand_path(extra[i]);
        g_autofree gchar *root = g_canonicalize_filename(expanded, NULL);
        g_autofree gchar *prefix = g_strconcat(root, G_DIR_SEPARATOR_S, NULL);

        if (g_str_has_prefix(directory, prefix))
            return TRUE;
    }
    return FALSE;
}

/*
 * The directory a change to @id is written in, always under
 * `<state_dir>/office/worlds/`:
 *
 *   - the pack already there, when it is;
 *   - a copy of the *shipped* pack of that id, made on the first change
 *     -- the state directory is searched before the shipped one, so the
 *     copy is what the office uses from then on;
 *   - a copy of @from under the new id, when the id is new;
 *   - a new empty pack otherwise.
 *
 * Never the operator's own: a pack in office.world_dirs outranks the
 * state directory, so a copy of it there would never be drawn -- a write
 * that reports success and changes nothing.  That is refused, naming the
 * way that works (a new id, copied with from).
 */
static gchar *
writable_pack(ClawtDaemon *self, const gchar *id, const gchar *from,
              gboolean *out_forked, GError **error)
{
    g_autofree gchar *root = own_worlds_dir(self);
    g_autofree gchar *mine = g_build_filename(root, id != NULL ? id : "", NULL);
    g_autoptr(ClawtOfficeWorld) existing = NULL;
    g_autoptr(ClawtOfficeWorld) source = NULL;

    if (out_forked != NULL)
        *out_forked = FALSE;
    if (!world_id_is_valid(id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' cannot be a world id: letters, digits, '-' and '_'",
                    id != NULL ? id : "");
        return NULL;
    }
    if (g_file_test(mine, G_FILE_TEST_IS_DIR))
        return g_steal_pointer(&mine);

    existing = find_world_by_id(self, id);
    if (existing != NULL &&
        in_operator_dirs(self, clawt_office_world_get_directory(existing))) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "'%s' is the operator's own pack (%s), which is never written "
                    "here; make a new world from it instead (a new id, with from: "
                    "\"%s\") and apply that", id,
                    clawt_office_world_get_directory(existing), id);
        return NULL;
    }
    if (existing == NULL && from != NULL) {
        source = find_world_by_id(self, from);
        if (source == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                        "there is no world '%s' to copy", from);
            return NULL;
        }
    }
    if (g_mkdir_with_parents(root, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "could not create %s", root);
        return NULL;
    }
    if (existing != NULL || source != NULL) {
        ClawtOfficeWorld *copy_of = existing != NULL ? existing : source;

        if (!clawt_office_pack_fork(clawt_office_world_get_directory(copy_of), mine,
                                    error))
            return NULL;
        if (out_forked != NULL)
            *out_forked = TRUE;
    } else if (g_mkdir(mine, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "could not create %s", mine);
        return NULL;
    }
    return g_steal_pointer(&mine);
}

/* ── The custodial team ─────────────────────────────────────────── */

/*
 * The custodians' parts, as data: the id suffix, the role in the team,
 * and the mission written into each one's SOUL.org.  The lead takes
 * requests and delegates; the steward makes and imports assets; the
 * designer edits zones and choreography.
 */
static const struct {
    const gchar *suffix;
    const gchar *role;
    const gchar *title;
    const gchar *mission;
} custodians[] = {
    { "lead", "lead", "Facilities lead",
      "You run the office: the world the fleet is drawn in.  Take requests "
      "from the operator and the chief (\"add a war room\", \"make it "
      "winter\", \"give the new agent a character\") and hand each to the "
      "member whose work it is -- assets and characters to the asset "
      "steward, rooms, zones and choreography to the set designer.  Check "
      "the result with clawtilla_office_preview before you report it done, "
      "and apply a world (clawtilla_office_world_apply) only when asked." },
    { "assets", "member", "Asset steward",
      "You make and import the office's assets: tiles, furniture, props and "
      "each agent's character.  Use the generator the operator configured "
      "(office.asset_generator) when you have it, and check the licence of "
      "anything you did not make.  Every file goes into a pack with "
      "clawtilla_office_asset_import and its provenance -- where it came "
      "from, the generator and model, and their terms -- because a pack "
      "travels and that question has to stay answerable.  A paid generator "
      "asks a person before each call; that is deliberate." },
    { "designer", "member", "Set designer",
      "You edit the office's world packs: office.yaml (zones, their tags "
      "and seats), choreography.yaml (where each activity sends people) "
      "and the maps.  Read before you write (clawtilla_office_world_read), "
      "write with clawtilla_office_world_write -- a write that would stop "
      "the pack loading is refused and undone, and the refusal says why -- "
      "and check every change with clawtilla_office_preview." },
};

gchar *
clawt_daemon_office_custodial_team(ClawtDaemon *self)
{
    return clawt_team_find_by_tag(self->config, CLAWT_TEAM_TAG_OFFICE_CUSTODIAL);
}

/* The lead of @team_id, if it has one. */
static gchar *
team_lead(ClawtDaemon *self, const gchar *team_id)
{
    GPtrArray *agents = clawt_config_get_agents(self->config);
    guint i;

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(agents, i);

        if (g_strcmp0(clawt_agent_config_get_string(agent, "team"), team_id) == 0 &&
            g_strcmp0(clawt_agent_config_get_string(agent, "team_role"), "lead") == 0)
            return g_strdup(clawt_agent_config_get_id(agent));
    }
    return NULL;
}

gboolean
clawt_daemon_office_custodians_install(ClawtDaemon  *self,
                                       const gchar  *team,
                                       gboolean      start,
                                       GPtrArray    *out_created,
                                       GError      **error)
{
    g_autofree gchar *existing = clawt_daemon_office_custodial_team(self);
    const gchar *team_id = team != NULL && *team != '\0' ? team : "facilities";
    static const gchar *const tags[] = { CLAWT_TEAM_TAG_OFFICE_CUSTODIAL, NULL };
    gsize i;

    if (existing != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "the office already has a custodial team: '%s' carries the "
                    "%s tag", existing, CLAWT_TEAM_TAG_OFFICE_CUSTODIAL);
        return FALSE;
    }

    /* every id free before anything is written: half a team is worse */
    if (clawt_config_get_team(self->config, team_id) != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "there is already a team called '%s'; name another, or tag that "
                    "one %s", team_id, CLAWT_TEAM_TAG_OFFICE_CUSTODIAL);
        return FALSE;
    }
    for (i = 0; i < G_N_ELEMENTS(custodians); i++) {
        g_autofree gchar *id = g_strdup_printf("%s-%s", team_id, custodians[i].suffix);

        if (clawt_config_get_agent(self->config, id) != NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                        "there is already an agent called '%s'; name the team "
                        "something else", id);
            return FALSE;
        }
    }

    if (!clawt_config_add_team(self->config, team_id, error))
        return FALSE;
    clawt_config_set_team_string(self->config, team_id, "name", "Facilities");
    clawt_config_set_team_string(self->config, team_id, "description",
                                 "Keeps the office: its worlds, assets and characters.");
    clawt_config_set_team_string_list(self->config, team_id, "tags", tags);
    if (!clawt_config_save(self->config, error))
        return FALSE;
    clawt_event_bus_emit(self->bus, "team.changed", team_id);

    for (i = 0; i < G_N_ELEMENTS(custodians); i++) {
        g_autofree gchar *id = g_strdup_printf("%s-%s", team_id, custodians[i].suffix);
        g_autoptr(GHashTable) fields = g_hash_table_new(g_str_hash, g_str_equal);
        g_autofree gchar *rule = NULL;
        const gchar *generator = clawt_config_get_string(self->config,
                                                         "office.asset_generator");

        g_hash_table_insert(fields, (gpointer)"name", (gpointer)custodians[i].title);
        g_hash_table_insert(fields, (gpointer)"team", (gpointer)team_id);
        g_hash_table_insert(fields, (gpointer)"team_role", (gpointer)custodians[i].role);
        g_hash_table_insert(fields, (gpointer)"tools.office", (gpointer)"true");

        /*
         * The steward holds the generator, and every call to it asks a
         * person first: a generator that charges must not spend because a
         * model decided to.  Through the action rules, so the approval is
         * the ordinary decision card and the operator can loosen it.
         */
        if (g_strcmp0(custodians[i].suffix, "assets") == 0 &&
            generator != NULL && *generator != '\0') {
            rule = g_strdup_printf("mcp__clawtilla-%s__*=ask", generator);
            g_hash_table_insert(fields, (gpointer)"policy.rules", rule);
        }

        if (clawt_daemon_create_agent(self, id, fields, custodians[i].mission, NULL,
                                      error) == NULL)
            return FALSE;
        if (out_created != NULL)
            g_ptr_array_add(out_created, g_strdup(id));
        if (start)
            clawt_daemon_start_agent_background(self, id);
    }
    return TRUE;
}

/* ── Characters ─────────────────────────────────────────────────── */

/*
 * Who a character is commissioned from: the custodial lead, else the
 * chief when its office tools are on.  NULL means nobody can, and then
 * nothing is filed -- the agent is drawn with the default body and its
 * own face, which is never wrong, only plain.
 */
static gchar *
character_maker(ClawtDaemon *self)
{
    g_autofree gchar *team = clawt_daemon_office_custodial_team(self);
    g_autofree gchar *lead = team != NULL ? team_lead(self, team) : NULL;
    const gchar *chief;
    ClawtAgentConfig *config;

    if (lead != NULL)
        return g_steal_pointer(&lead);

    /* both spellings of the chief, as the tools.office resolver reads them */
    chief = clawt_config_get_string(self->config, "orchestration.chief_of_staff");
    config = chief != NULL ? clawt_config_get_agent(self->config, chief) : NULL;
    if (config == NULL) {
        GPtrArray *agents = clawt_config_get_agents(self->config);
        guint i;

        for (i = 0; agents != NULL && i < agents->len && config == NULL; i++)
            if (clawt_agent_config_get_boolean(g_ptr_array_index(agents, i),
                                               "chief_of_staff"))
                config = g_ptr_array_index(agents, i);
    }
    if (config != NULL && clawt_agent_config_get_boolean(config, "tools.office"))
        return g_strdup(clawt_agent_config_get_id(config));
    return NULL;
}

/*
 * The file an agent's picture is in, or NULL: agents.avatar, else the
 * profile picture a client stored in its workspace -- the answer every
 * client's sidebar draws.  Both roads to a character ask this; each
 * used to read agents.avatar alone, so a picture set from a client was
 * never the one a character was made from.
 */
gchar *
clawt_daemon_office_agent_picture(ClawtAgentConfig *agent)
{
    g_autofree gchar *workspace = clawt_agent_config_get_workspace(agent);

    if (workspace == NULL)
        return NULL;
    return clawt_avatar_resolve_path(clawt_agent_config_get_string(agent, "avatar"),
                                     workspace);
}

/*
 * The picture a character is made from: the agent's own avatar when it
 * has one, otherwise its generated face as an SVG -- the same face every
 * client draws, so the character looks like the agent people know.
 * Answered as bytes and a file name, staged by the caller.
 */
static gboolean
reference_picture(ClawtAgentConfig *agent, gchar **out_name, gchar **out_data,
                  gsize *out_length, GError **error)
{
    const gchar *id = clawt_agent_config_get_id(agent);
    g_autofree gchar *avatar = clawt_daemon_office_agent_picture(agent);

    if (avatar != NULL) {
        if (!g_file_get_contents(avatar, out_data, out_length, error))
            return FALSE;
        *out_name = g_path_get_basename(avatar);
        return TRUE;
    }
    {
        const gchar *name = clawt_agent_config_get_string(agent, "name");
        const gchar *fill = clawt_avatar_color(id, name,
                                               clawt_agent_config_get_string(agent, "color"));
        gint shape = clawt_face_resolve(id, clawt_agent_config_get_string(agent, "face"));
        gchar *svg = clawt_face_svg(shape, fill, 512, NULL, clawt_face_seed(id), FALSE);

        if (svg == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "could not draw %s's face", id);
            return FALSE;
        }
        *out_data = svg;
        *out_length = strlen(svg);
        *out_name = g_strdup("face.svg");
        return TRUE;
    }
}

/*
 * Put the picture where the people making the character can read it:
 * the workspace of the maker and of every member of the custodial team,
 * because the lead hands the job on and a confined steward cannot read
 * the lead's workspace, let alone `<state_dir>`.  Answers the path
 * relative to a workspace, which is the same in all of them.
 */
static gchar *
stage_reference(ClawtDaemon *self, const gchar *maker, ClawtAgentConfig *agent,
                GError **error)
{
    g_autofree gchar *team = clawt_daemon_office_custodial_team(self);
    g_autofree gchar *name = NULL;
    g_autofree gchar *data = NULL;
    g_autofree gchar *relative = NULL;
    GPtrArray *agents = clawt_config_get_agents(self->config);
    gsize length = 0;
    guint staged = 0;
    guint i;

    if (!reference_picture(agent, &name, &data, &length, error))
        return NULL;
    relative = g_build_filename("office-characters", clawt_agent_config_get_id(agent),
                                name, NULL);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *to = g_ptr_array_index(agents, i);
        const gchar *to_id = clawt_agent_config_get_id(to);
        g_autofree gchar *workspace = NULL;
        g_autofree gchar *path = NULL;
        g_autofree gchar *dir = NULL;

        if (g_strcmp0(to_id, maker) != 0 &&
            (team == NULL || g_strcmp0(clawt_agent_config_get_string(to, "team"), team) != 0))
            continue;
        workspace = clawt_agent_config_get_workspace(to);
        if (workspace == NULL)
            continue;
        path = g_build_filename(workspace, relative, NULL);
        dir = g_path_get_dirname(path);
        if (g_mkdir_with_parents(dir, 0700) != 0 ||
            !g_file_set_contents_full(path, data, (gssize)length,
                                      G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL))
            continue;
        staged++;
    }
    if (staged == 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "the picture could not be put in '%s''s workspace", maker);
        return NULL;
    }
    return g_steal_pointer(&relative);
}

gchar *
clawt_daemon_office_commission_character(ClawtDaemon  *self,
                                         const gchar  *agent_id,
                                         GError      **error)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    const gchar *generator = clawt_config_get_string(self->config, "office.asset_generator");
    g_autofree gchar *maker = NULL;
    g_autofree gchar *reference = NULL;
    g_autofree gchar *prompt = NULL;
    g_autofree gchar *title = NULL;
    ClawtTask *task;

    if (agent == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND, "there is no agent '%s'",
                    agent_id != NULL ? agent_id : "");
        return NULL;
    }
    if (generator == NULL || *generator == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "no asset generator is configured (office.asset_generator)");
        return NULL;
    }
    maker = character_maker(self);
    if (maker == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                            "nobody keeps the office: install a custodial team "
                            "(`clawtilla office custodians install`) or turn on the "
                            "chief's tools.office");
        return NULL;
    }
    reference = stage_reference(self, maker, agent, error);
    if (reference == NULL)
        return NULL;

    prompt = g_strdup_printf(
        "Make an office character for %s (%s).\n\n"
        "Reference picture: %s, in your workspace (and every office custodian's) "
        "-- the character must read as this agent at a "
        "glance: same colour (%s), same face.\n"
        "Generator: the connector '%s'.  Every call to it asks the operator "
        "first, with its cost; ask for what you need in as few calls as you can.\n\n"
        "What to make:\n"
        "1. a consistent turnaround of the character, from the reference;\n"
        "2. short clips for: walk (four directions), idle, sit and type, phone, "
        "present;\n"
        "3. frames from those clips, background removed, as a sprite sheet with "
        "one named row per animation, 32x48 pixels a frame.\n\n"
        "Import each file with clawtilla_office_asset_import, character \"%s\", "
        "with its provenance (generator, model, terms), then say how the sheet is "
        "laid out with clawtilla_office_character_layout (frame size, and a row "
        "per animation: idle, walk_down, walk_up, walk_left, walk_right, sit, "
        "type, phone, present).  The office draws the character from then on "
        "(it lives in <state_dir>/office/characters/%s/); until then the agent "
        "keeps the default body with its own face.\n"
        "If you lead a team, hand this to the asset steward.",
        clawt_agent_config_get_string(agent, "name") != NULL
            ? clawt_agent_config_get_string(agent, "name") : agent_id,
        agent_id, reference,
        clawt_avatar_color(agent_id, clawt_agent_config_get_string(agent, "name"),
                           clawt_agent_config_get_string(agent, "color")),
        generator, agent_id, agent_id);

    task = clawt_task_manager_create(self->tasks, "user", maker, prompt, NULL, error);
    if (task == NULL)
        return NULL;
    title = g_strdup_printf("Office character for %s", agent_id);
    clawt_task_set_title(task, title);
    clawt_task_manager_save(self->tasks, task, NULL);
    if (!clawt_daemon_deliver_operator_task(self, task, error))
        return NULL;
    return g_strdup(clawt_task_get_id(task));
}

/* ── The office tools ───────────────────────────────────────────── */

static gchar *
tool_worlds(ClawtDaemon *self)
{
    g_auto(GStrv) dirs = clawt_daemon_office_world_dirs(self);
    g_autoptr(GPtrArray) refused = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) worlds = clawt_office_worlds_discover((const gchar *const *)dirs,
                                                               refused);
    g_autofree gchar *mine = own_worlds_dir(self);
    GString *out = g_string_new(NULL);
    guint i;

    g_string_append_printf(out, "The office is %s; its world is %s.\n\n",
        self->office != NULL ? "on" : "off",
        clawt_config_get_string(self->config, "office.world"));
    for (i = 0; i < worlds->len; i++) {
        ClawtOfficeWorld *world = g_ptr_array_index(worlds, i);
        gboolean editable = g_str_has_prefix(clawt_office_world_get_directory(world), mine);

        g_string_append_printf(out, "- %s (%s), %s, %s%s\n",
            clawt_office_world_get_id(world), clawt_office_world_get_name(world),
            clawt_office_world_get_license(world),
            editable ? "written here" : "shipped or the operator's own (a change makes "
                                        "a copy under the state directory)",
            clawt_office_world_get_warnings(world)->len > 0 ? ", with warnings" : "");
    }
    for (i = 0; i < refused->len; i++)
        g_string_append_printf(out, "- refused: %s\n", (gchar *)g_ptr_array_index(refused, i));
    return g_string_free(out, FALSE);
}

static gchar *
tool_world_read(ClawtDaemon *self, JsonObject *args, gboolean *is_error)
{
    const gchar *id = clawt_ipc_payload_string(args, "world");
    const gchar *path = clawt_ipc_payload_string(args, "path");
    g_autoptr(ClawtOfficeWorld) world = NULL;
    g_autofree gchar *full = NULL;
    g_autofree gchar *data = NULL;
    gsize length = 0;
    guint i;

    world = find_world_by_id(self, id != NULL ? id
                             : clawt_config_get_string(self->config, "office.world"));
    if (world == NULL) {
        *is_error = TRUE;
        return g_strdup("There is no such world; clawtilla_office_worlds lists them.");
    }
    if (path == NULL)
        path = "office.yaml";
    if (!clawt_office_pack_path_is_safe(path)) {
        *is_error = TRUE;
        return g_strdup("That is not a path inside the pack.");
    }
    full = g_build_filename(clawt_office_world_get_directory(world), path, NULL);
    if (!g_file_get_contents(full, &data, &length, NULL)) {
        *is_error = TRUE;
        return g_strdup_printf("%s has no file %s.", clawt_office_world_get_id(world), path);
    }
    if (length > WORLD_READ_MAX || !g_utf8_validate(data, (gssize)length, NULL)) {
        *is_error = TRUE;
        return g_strdup_printf("%s is %s; only text up to %d bytes is read here.", path,
                               length > WORLD_READ_MAX ? "too large" : "not text",
                               WORLD_READ_MAX);
    }
    {
        GString *out = g_string_new(NULL);
        GPtrArray *warnings = clawt_office_world_get_warnings(world);

        g_string_append_printf(out, "%s/%s:\n\n%s", clawt_office_world_get_id(world),
                               path, data);
        for (i = 0; warnings != NULL && i < warnings->len; i++)
            g_string_append_printf(out, "%swarning: %s\n", i == 0 ? "\n" : "",
                                   (gchar *)g_ptr_array_index(warnings, i));
        return g_string_free(out, FALSE);
    }
}

static gchar *
tool_world_write(ClawtDaemon *self, JsonObject *args, gboolean *is_error)
{
    const gchar *id = clawt_ipc_payload_string(args, "world");
    const gchar *path = clawt_ipc_payload_string(args, "path");
    const gchar *content = clawt_ipc_payload_string(args, "content");
    g_autoptr(GError) error = NULL;
    g_autofree gchar *pack = NULL;
    gboolean forked = FALSE;

    if (id == NULL || path == NULL || content == NULL) {
        *is_error = TRUE;
        return g_strdup("world, path and content are required.");
    }
    pack = writable_pack(self, id, clawt_ipc_payload_string(args, "from"), &forked, &error);
    if (pack == NULL ||
        !clawt_office_pack_write_file(pack, path, content, strlen(content), &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }
    clawt_event_bus_emit(self->bus, "office.changed", id);
    return g_strdup_printf("Wrote %s/%s%s.", id, path,
        forked ? " (the pack was copied under the state directory first, and the copy is "
                 "what the office uses for this id)" : "");
}

static gchar *
tool_world_apply(ClawtDaemon *self, JsonObject *args, gboolean *is_error)
{
    const gchar *id = clawt_ipc_payload_string(args, "world");
    g_autoptr(ClawtOfficeWorld) world = id != NULL ? find_world_by_id(self, id) : NULL;
    g_autoptr(GError) error = NULL;

    if (world == NULL) {
        *is_error = TRUE;
        return g_strdup("There is no such world (it may have been refused: "
                        "clawtilla_office_worlds says why).");
    }
    clawt_config_set_string(self->config, "office.world", id);
    if (!clawt_config_save(self->config, &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }
    clawt_daemon_office_reconfigure(self);
    clawt_event_bus_emit(self->bus, "office.changed", NULL);
    return g_strdup_printf("The office is now drawn in %s.", id);
}

static gchar *
tool_preview(ClawtDaemon *self, JsonObject *args, gboolean *is_error)
{
    const gchar *id = clawt_ipc_payload_string(args, "world");
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) layout = clawt_daemon_office_layout(self, id, &error);
    JsonObject *root;
    JsonArray *placements;
    GString *out;
    guint i;

    if (layout == NULL) {
        *is_error = TRUE;
        if (self->office == NULL)
            return g_strdup("The office is off, so nobody is seated; the operator "
                            "turns it on (clawtilla office on).");
        return g_strdup(error->message);
    }
    root = json_node_get_object(layout);
    out = g_string_new(NULL);
    g_string_append_printf(out, "Who would sit where in %s:\n",
                           id != NULL ? id : clawt_config_get_string(self->config,
                                                                     "office.world"));
    placements = json_object_has_member(root, "placements")
        ? json_object_get_array_member(root, "placements") : NULL;
    for (i = 0; placements != NULL && i < json_array_get_length(placements); i++) {
        JsonObject *p = json_array_get_object_element(placements, i);
        const gchar *fallback = clawt_ipc_payload_string(p, "fallback");
        const gchar *zone = clawt_ipc_payload_string(p, "zone");

        g_string_append_printf(out, "- %s: %s, %s in %s%s%s\n",
            clawt_ipc_payload_string(p, "id"), clawt_ipc_payload_string(p, "activity"),
            clawt_ipc_payload_string(p, "pose"), zone != NULL ? zone : "(nowhere)",
            fallback != NULL ? " -- " : "", fallback != NULL ? fallback : "");
    }
    if (placements == NULL || json_array_get_length(placements) == 0)
        g_string_append(out, "(nobody is in the office yet)\n");
    return g_string_free(out, FALSE);
}

/*
 * A file an agent hands over must be one it could have made: inside its
 * own workspace.  The daemon reads with its own rights, so without this
 * a confined agent could import any file the daemon can read.
 */
static gboolean
source_is_agents_own(ClawtDaemon *self, const gchar *agent_id, const gchar *source,
                     GError **error)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    g_autofree gchar *workspace = agent != NULL ? clawt_agent_config_get_workspace(agent)
                                                : NULL;
    g_autofree gchar *real_root = workspace != NULL ? realpath(workspace, NULL) : NULL;
    g_autofree gchar *real_source = source != NULL ? realpath(source, NULL) : NULL;
    gsize length;

    if (real_root == NULL || real_source == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND, "%s does not exist",
                    source != NULL ? source : "the source");
        return FALSE;
    }
    length = strlen(real_root);
    if (strncmp(real_source, real_root, length) != 0 || real_source[length] != '/') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "%s is not in your workspace (%s); copy it there first",
                    source, workspace);
        return FALSE;
    }
    return TRUE;
}

static gchar *
tool_asset_import(ClawtDaemon *self, const gchar *agent_id, JsonObject *args,
                  gboolean *is_error)
{
    const gchar *world = clawt_ipc_payload_string(args, "world");
    const gchar *character = clawt_ipc_payload_string(args, "character");
    const gchar *source = clawt_ipc_payload_string(args, "source");
    const gchar *path = clawt_ipc_payload_string(args, "path");
    g_autoptr(GError) error = NULL;
    g_autofree gchar *pack = NULL;
    g_autofree gchar *sum = NULL;

    if ((world == NULL) == (character == NULL) || source == NULL || path == NULL) {
        *is_error = TRUE;
        return g_strdup("Name a world or a character (one of them), the source file "
                        "and the path it takes inside.");
    }
    if (!source_is_agents_own(self, agent_id, source, &error))
        goto failed;
    if (world != NULL) {
        pack = writable_pack(self, world, NULL, NULL, &error);
        if (pack == NULL ||
            !clawt_office_pack_import_asset(pack, source, path,
                                            clawt_ipc_payload_string(args, "license"),
                                            clawt_ipc_payload_string(args, "provenance"),
                                            &sum, &error))
            goto failed;
    } else {
        g_autofree gchar *root = clawt_daemon_office_characters_dir(self);

        if (clawt_config_get_agent(self->config, character) == NULL) {
            g_set_error(&error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                        "there is no agent '%s' to make a character for", character);
            goto failed;
        }
        if (!clawt_office_character_import_asset(root, character, source, path,
                clawt_ipc_payload_string(args, "license"),
                clawt_ipc_payload_string(args, "provenance"), &sum, &error))
            goto failed;
    }
    clawt_event_bus_emit(self->bus, "office.changed", world != NULL ? world : character);
    return g_strdup_printf("Imported %s into %s %s (sha256 %s).", path,
                           world != NULL ? "world" : "the character of",
                           world != NULL ? world : character, sum);

failed:
    *is_error = TRUE;
    return g_strdup(error->message);
}

static gchar *
tool_character_layout(ClawtDaemon *self, JsonObject *args, gboolean *is_error)
{
    const gchar *agent = clawt_ipc_payload_string(args, "agent");
    JsonObject *animations = json_object_has_member(args, "animations") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(args, "animations"))
        ? json_object_get_object_member(args, "animations") : NULL;
    g_autofree gchar *root = clawt_daemon_office_characters_dir(self);
    g_autoptr(GError) error = NULL;

    const gchar *background = clawt_ipc_payload_string(args, "background");

    if (agent == NULL || animations == NULL) {
        *is_error = TRUE;
        return g_strdup("agent, sheet, frame_width, frame_height and animations are required.");
    }
    /* refused before anything is written, so a bad colour leaves the old layout */
    if (!clawt_office_character_background_is_valid(background)) {
        *is_error = TRUE;
        return g_strdup_printf("background must be corners or #rrggbb, not '%s'.", background);
    }
    /* the backdrop is the sheet's, so a new sheet says its own (or has none) */
    if (!clawt_office_character_set_sheet(root, agent,
            clawt_ipc_payload_string(args, "sheet"),
            (guint)json_object_get_int_member_with_default(args, "frame_width", 0),
            (guint)json_object_get_int_member_with_default(args, "frame_height", 0),
            animations, &error) ||
        !clawt_office_character_set_background(root, agent, background, &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }
    clawt_event_bus_emit(self->bus, "office.changed", agent);
    return g_strdup_printf("%s's character is ready: the office draws it from now on.", agent);
}

static gchar *
tool_character_mesh(ClawtDaemon *self, JsonObject *args, gboolean *is_error)
{
    const gchar *agent = clawt_ipc_payload_string(args, "agent");
    g_autofree gchar *root = clawt_daemon_office_characters_dir(self);
    g_autoptr(GError) error = NULL;

    if (!clawt_office_character_set_mesh(root, agent, clawt_ipc_payload_string(args, "mesh"),
                                         &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }
    clawt_event_bus_emit(self->bus, "office.changed", agent);
    return g_strdup_printf("%s has a 3D model now: the 3D view draws it.", agent);
}

static gchar *
tool_custodians_install(ClawtDaemon *self, const gchar *agent_id, JsonObject *args,
                        gboolean *is_error)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    g_autoptr(GPtrArray) created = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *joined = NULL;

    /* making agents is fleet administration, whoever asks */
    if (agent == NULL || !clawt_agent_config_get_boolean(agent, "tools.manage_fleet")) {
        *is_error = TRUE;
        return g_strdup("Installing the custodial team creates agents, which needs "
                        "tools.manage_fleet as well as tools.office; ask the operator.");
    }
    if (!clawt_daemon_office_custodians_install(self, clawt_ipc_payload_string(args, "team"),
                                                TRUE, created, &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }
    g_ptr_array_add(created, NULL);
    joined = g_strjoinv(", ", (gchar **)created->pdata);
    return g_strdup_printf("Installed the custodial team: %s.  The lead takes office "
                           "requests from here on.", joined);
}

static gchar *
hook_call(const gchar *agent_id,
          const gchar *tool,
          JsonObject  *arguments,
          gboolean    *is_error,
          gpointer     user_data)
{
    ClawtDaemon *self = user_data;

    *is_error = FALSE;
    if (g_strcmp0(tool, "clawtilla_office_worlds") == 0)
        return tool_worlds(self);
    if (g_strcmp0(tool, "clawtilla_office_world_read") == 0)
        return tool_world_read(self, arguments, is_error);
    if (g_strcmp0(tool, "clawtilla_office_world_write") == 0)
        return tool_world_write(self, arguments, is_error);
    if (g_strcmp0(tool, "clawtilla_office_world_apply") == 0)
        return tool_world_apply(self, arguments, is_error);
    if (g_strcmp0(tool, "clawtilla_office_preview") == 0)
        return tool_preview(self, arguments, is_error);
    if (g_strcmp0(tool, "clawtilla_office_asset_import") == 0)
        return tool_asset_import(self, agent_id, arguments, is_error);
    if (g_strcmp0(tool, "clawtilla_office_character_mesh") == 0)
        return tool_character_mesh(self, arguments, is_error);
    if (g_strcmp0(tool, "clawtilla_office_character_layout") == 0)
        return tool_character_layout(self, arguments, is_error);
    if (g_strcmp0(tool, "clawtilla_office_custodians_install") == 0)
        return tool_custodians_install(self, agent_id, arguments, is_error);
    *is_error = TRUE;
    return g_strdup_printf("%s is not an office tool.", tool);
}

void
clawt_daemon_office_custodial_setup(ClawtDaemon *self)
{
    static const ClawtMcpOfficeHooks hooks = { hook_call };

    if (self->mcp_tools != NULL)
        clawt_mcp_tools_set_office_hooks(self->mcp_tools, &hooks, self, NULL);
}
