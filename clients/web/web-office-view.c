/*
 * web-office-view.c - The drawn office on the web Office page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A canvas renderer of the web client's own, in every build: it needs
 * nothing but a browser, so unlike the GTK and lrg views it is not behind
 * OFFICE=1.  The drawing is data/web/office.js; this file serves it what
 * to draw.
 *
 * The page carries its first scene in the markup -- the director's
 * layout, the world pack, the Tiled map with its tilesets inlined as data:
 * URIs, every participant's face -- so opening Office fetches nothing
 * (the web client's rule: it must work on a tailnet with no route out,
 * and a page that drives the fleet loads nothing it did not bring).
 * After that the script polls /office/layout.json while the tab is
 * visible, and asks /office/world.json only when the layout names a world
 * other than the one it holds.
 *
 * The same scene carries the agents' characters (office.characters, each
 * sheet held to its manifest's sha256 here and inlined) and the usage
 * board (usage.providers as it stands, labelled by libclawt here); a
 * changed sheet afterwards comes from /office/character/:agent.
 *
 * Nothing here decides where anybody stands.  The positions are the
 * director's (office.layout), the activity words and tones libclawt's
 * enumeration, the faces clawt_web_face_svg()'s -- one answer each,
 * shared with GTK and lrg.
 */

#include "web-pages.h"

#include <string.h>

/* What the script polls, and the most a tileset may add to the page. */
#define OFFICE_LAYOUT_PATH        "/office/layout.json"
#define OFFICE_WORLD_PATH         "/office/world.json"
#define OFFICE_INLINE_IMAGE_MAX   (4 * 1024 * 1024)
#define OFFICE_CHARACTER_PATH     "/office/character/"

/*
 * The Content-Type for a pack file, by its extension.
 *
 * An allowlist, and SVG is deliberately not on it: a pack is written by
 * agents (the custodial team) and synced between fleets, and an SVG
 * served as image/svg+xml from this origin is a document that can run
 * script with the page's cookies when somebody opens its URL.  Anything
 * unlisted goes out as bytes with nosniff, which a browser will neither
 * render nor execute.
 */
static const gchar *
office_file_type(const gchar *path)
{
    static const struct {
        const gchar *suffix;
        const gchar *type;
    } types[] = {
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".webp", "image/webp" },
        { ".tmj",  "application/json" },
        { ".tsj",  "application/json" },
        { ".json", "application/json" },
        { ".yaml", "text/plain; charset=utf-8" },
        { ".yml",  "text/plain; charset=utf-8" },
    };
    g_autofree gchar *lower = NULL;
    guint i;

    if (path == NULL)
        return "application/octet-stream";

    lower = g_ascii_strdown(path, -1);

    for (i = 0; i < G_N_ELEMENTS(types); i++)
        if (g_str_has_suffix(lower, types[i].suffix))
            return types[i].type;

    return "application/octet-stream";
}

/*
 * Whether @world can name a pack: one path segment the pack-path rule
 * accepts.  The daemon resolves the id against the packs it discovered,
 * so this is not the containment check -- it is refusing, here, a value
 * no pack could have, before it costs a request.
 */
static gboolean
office_world_id_is_safe(const gchar *world)
{
    return world != NULL && clawt_office_pack_path_is_safe(world) &&
           strchr(world, '/') == NULL;
}

/*
 * Whether @agent can name a character: the same one-segment rule, so
 * office.character_file is never asked about a path.  The daemon checks
 * containment again on the canonical path.
 */
static gboolean
office_agent_id_is_safe(const gchar *agent)
{
    return office_world_id_is_safe(agent);
}

/* One file of a pack, as office.file answers it (base64), or NULL. */
static JsonNode *
office_file_reply(ClawtWebApp *app, const gchar *world, const gchar *path)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();

    clawt_web_payload_set(payload, "world", world);
    clawt_web_payload_set(payload, "path", path);

    return clawt_web_app_call(app, "office.file",
                              clawt_web_payload_take(g_steal_pointer(&payload)));
}

/*
 * The directory part of a pack-relative path, with its separator, or ""
 * -- a tileset's image is named relative to the map that names it.
 */
static gchar *
office_dir_of(const gchar *relative)
{
    const gchar *slash = strrchr(relative, '/');

    if (slash == NULL)
        return g_strdup("");

    return g_strndup(relative, (gsize)(slash - relative) + 1);
}

/*
 * A tileset made drawable: its image fetched through the daemon and
 * written back as a data: URI in `src`, so the canvas needs no request.
 * An external tileset (`source`, a .tsj) is read and merged first,
 * keeping the map's own firstgid.  An image larger than
 * OFFICE_INLINE_IMAGE_MAX is left as a URL to /office/file instead, so a
 * pack with a vast sheet costs a request rather than a vast page.
 */
static void
office_resolve_tileset(ClawtWebApp *app, const gchar *world,
                       const gchar *map_dir, JsonObject *tileset)
{
    const gchar *source = clawt_web_member(tileset, "source", NULL);
    g_autofree gchar *image_dir = g_strdup(map_dir);
    g_autofree gchar *image_path = NULL;
    g_autoptr(JsonNode) image = NULL;
    const gchar *encoded;
    const gchar *image_name;

    if (source != NULL) {
        g_autofree gchar *path = g_strconcat(map_dir, source, NULL);
        g_autoptr(JsonNode) reply = NULL;
        g_autoptr(JsonParser) parser = json_parser_new();
        g_autofree guchar *raw = NULL;
        gsize length = 0;
        JsonObject *external;
        GList *members;
        GList *l;

        if (!clawt_office_pack_path_is_safe(path))
            return;
        reply = office_file_reply(app, world, path);
        encoded = clawt_web_member(clawt_web_root(reply), "base64", NULL);
        if (encoded == NULL)
            return;
        raw = g_base64_decode(encoded, &length);
        if (!json_parser_load_from_data(parser, (const gchar *)raw,
                                        (gssize)length, NULL) ||
            !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
            return;
        external = json_node_get_object(json_parser_get_root(parser));
        members = json_object_get_members(external);
        for (l = members; l != NULL; l = l->next)
            if (g_strcmp0(l->data, "firstgid") != 0)
                json_object_set_member(tileset, l->data, json_node_copy(
                    json_object_get_member(external, l->data)));
        g_list_free(members);
        json_object_remove_member(tileset, "source");
        g_free(image_dir);
        image_dir = office_dir_of(path);
    }

    image_name = clawt_web_member(tileset, "image", NULL);
    if (image_name == NULL)
        return;
    image_path = g_strconcat(image_dir, image_name, NULL);
    if (!clawt_office_pack_path_is_safe(image_path))
        return;

    image = office_file_reply(app, world, image_path);
    encoded = clawt_web_member(clawt_web_root(image), "base64", NULL);

    if (encoded != NULL && strlen(encoded) / 4 * 3 <= OFFICE_INLINE_IMAGE_MAX) {
        g_autofree gchar *uri = g_strdup_printf(
            "data:%s;base64,%s", office_file_type(image_path), encoded);

        json_object_set_string_member(tileset, "src", uri);
    } else {
        g_autofree gchar *escaped_world = g_uri_escape_string(world, NULL, FALSE);
        g_autofree gchar *escaped_path = g_uri_escape_string(image_path, NULL, FALSE);
        g_autofree gchar *url = g_strdup_printf("/office/file/%s?path=%s",
                                                escaped_world, escaped_path);

        json_object_set_string_member(tileset, "src", url);
    }
}

/*
 * The world half of a scene: office.world's answer, plus its Tiled map
 * parsed here with every tileset made drawable.  NULL when the daemon
 * would not say; a world with no 2D map still answers, with no `map`, and
 * the script draws the zones' anchors on a plain floor instead.
 */
static JsonObject *
office_scene_world(ClawtWebApp *app, const gchar *world_id)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *world;
    JsonObject *scene;
    const gchar *id;
    const gchar *map_2d;

    if (world_id != NULL)
        clawt_web_payload_set(payload, "world", world_id);
    reply = clawt_web_app_call(app, "office.world",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    world = clawt_web_root(reply);
    if (world == NULL)
        return NULL;

    scene = json_object_new();
    json_object_set_object_member(scene, "world", json_object_ref(world));
    id = clawt_web_member(world, "id", world_id);
    map_2d = clawt_web_member(world, "map_2d", NULL);

    if (id != NULL && map_2d != NULL && clawt_office_pack_path_is_safe(map_2d)) {
        g_autoptr(JsonNode) file = office_file_reply(app, id, map_2d);
        const gchar *encoded = clawt_web_member(clawt_web_root(file), "base64", NULL);
        g_autoptr(JsonParser) parser = json_parser_new();
        g_autofree guchar *raw = NULL;
        gsize length = 0;

        if (encoded != NULL)
            raw = g_base64_decode(encoded, &length);
        if (raw != NULL &&
            json_parser_load_from_data(parser, (const gchar *)raw,
                                       (gssize)length, NULL) &&
            JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
            JsonObject *map = json_node_get_object(json_parser_get_root(parser));
            JsonArray *tilesets = clawt_web_member_array(map, "tilesets");
            g_autofree gchar *map_dir = office_dir_of(map_2d);
            guint i;

            for (i = 0; tilesets != NULL && i < json_array_get_length(tilesets); i++) {
                JsonNode *node = json_array_get_element(tilesets, i);

                if (JSON_NODE_HOLDS_OBJECT(node))
                    office_resolve_tileset(app, id, map_dir,
                                           json_node_get_object(node));
            }
            json_object_set_object_member(scene, "map", json_object_ref(map));
        }
    }
    return scene;
}

/*
 * Who looks like what: per participant its body colour and its face --
 * the generated SVG, the picture's URL, or the letter for a disc -- from
 * agent.list, through the same functions clawt_web_avatar() uses.  A
 * Grok bot is not an agent and gets the derived colour and face of its id.
 */
static JsonObject *
office_faces(ClawtWebApp *app, JsonArray *participants)
{
    g_autoptr(JsonNode) listed = clawt_web_app_call(app, "agent.list", NULL);
    JsonArray *agents = clawt_web_member_array(clawt_web_root(listed), "agents");
    g_autoptr(GHashTable) by_id = g_hash_table_new(g_str_hash, g_str_equal);
    JsonObject *faces = json_object_new();
    guint i;

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *id = clawt_web_member(agent, "id", NULL);

        if (id == NULL)
            continue;
        g_hash_table_insert(by_id, (gpointer)id, agent);
        clawt_web_face_note(id, clawt_web_member(agent, "face", NULL));
    }

    for (i = 0; participants != NULL && i < json_array_get_length(participants); i++) {
        JsonObject *p = json_array_get_object_element(participants, i);
        const gchar *id = clawt_web_member(p, "id", NULL);
        const gchar *name = clawt_web_member(p, "name", id);
        JsonObject *agent;
        const gchar *color = NULL;
        g_autofree gchar *svg = NULL;
        JsonObject *face;
        gunichar first;
        gchar initial[8] = { 0 };

        if (id == NULL)
            continue;
        agent = g_hash_table_lookup(by_id, id);
        if (agent != NULL)
            color = clawt_web_member(agent, "color", NULL);
        /* a visitor's colour comes with it: no agent here to read it from */
        if (color == NULL)
            color = clawt_web_member(p, "color", NULL);

        face = json_object_new();
        json_object_set_string_member(face, "color",
                                      clawt_avatar_color(id, name, color));
        if (agent != NULL && clawt_web_member_bool(agent, "has_avatar", FALSE)) {
            g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
            g_autofree gchar *url = g_strdup_printf("/a/%s/avatar", escaped);

            json_object_set_string_member(face, "img", url);
        }
        svg = clawt_web_face_svg(name, id, color, 64, FALSE);
        if (svg != NULL)
            json_object_set_string_member(face, "svg", svg);
        first = g_utf8_get_char_validated(name, -1);
        if (first != (gunichar)-1 && first != (gunichar)-2)
            g_unichar_to_utf8(g_unichar_toupper(first), initial);
        json_object_set_string_member(face, "initial", initial);
        json_object_set_object_member(faces, id, face);
    }
    return faces;
}

/* One file of an agent's character, as office.character_file answers it. */
static JsonNode *
office_character_file_reply(ClawtWebApp *app, const gchar *agent, const gchar *path)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();

    clawt_web_payload_set(payload, "agent", agent);
    clawt_web_payload_set(payload, "path", path);

    return clawt_web_app_call(app, "office.character_file",
                              clawt_web_payload_take(g_steal_pointer(&payload)));
}

/*
 * Whether @bytes are the file a character's manifest recorded.  A
 * manifest with no checksum has nothing to hold the bytes to, which is
 * the daemon's answer and the lrg feed's too: drawn.
 */
static gboolean
office_sheet_matches(GBytes *bytes, const gchar *want)
{
    g_autofree gchar *sum = NULL;

    if (want == NULL || *want == '\0')
        return TRUE;
    sum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
    return sum != NULL && g_ascii_strcasecmp(sum, want) == 0;
}

/*
 * The characters the daemon has whole (office.characters), each with a
 * `src` the script draws the sheet from.
 *
 * With @inline_sheets the sheet is read here, held to its manifest's
 * sha256, and written in as a data: URI -- the first page fetches
 * nothing, like the tilesets.  A sheet that does not match is left out
 * rather than drawn wrong, and one over OFFICE_INLINE_IMAGE_MAX becomes a
 * URL instead.  Without it (the poll, every 2.5 s) `src` is always the
 * URL, carrying the checksum, which the route holds the bytes to; the
 * script loads a sheet again only when its checksum changes.
 *
 * Returns NULL when the daemon did not answer, so a poll that failed
 * leaves the characters the page has.
 */
static JsonArray *
office_characters(ClawtWebApp *app, gboolean inline_sheets)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "office.characters", NULL);
    JsonArray *listed;
    JsonArray *out;
    guint i;

    if (clawt_web_root(reply) == NULL)
        return NULL;
    listed = clawt_web_member_array(clawt_web_root(reply), "characters");
    out = json_array_new();

    for (i = 0; listed != NULL && i < json_array_get_length(listed); i++) {
        JsonNode *node = json_array_get_element(listed, i);
        JsonObject *character;
        const gchar *agent;
        const gchar *sheet;
        const gchar *sha;
        g_autofree gchar *src = NULL;

        if (!JSON_NODE_HOLDS_OBJECT(node))
            continue;
        character = json_node_get_object(node);
        agent = clawt_web_member(character, "agent", NULL);
        sheet = clawt_web_member(character, "sheet", NULL);
        sha = clawt_web_member(character, "sheet_sha256", NULL);
        if (!office_agent_id_is_safe(agent) || !clawt_office_pack_path_is_safe(sheet))
            continue;

        if (inline_sheets) {
            g_autoptr(JsonNode) file = office_character_file_reply(app, agent, sheet);
            const gchar *encoded = clawt_web_member(clawt_web_root(file), "base64", NULL);
            guchar *raw;
            gsize length = 0;
            g_autoptr(GBytes) bytes = NULL;

            if (encoded == NULL)
                continue;
            raw = g_base64_decode(encoded, &length);
            bytes = g_bytes_new_take(raw, length);
            if (!office_sheet_matches(bytes, sha))
                continue;
            if (length <= OFFICE_INLINE_IMAGE_MAX)
                src = g_strdup_printf("data:%s;base64,%s", office_file_type(sheet), encoded);
        }
        if (src == NULL) {
            g_autofree gchar *escaped_agent = g_uri_escape_string(agent, NULL, FALSE);
            g_autofree gchar *escaped_path = g_uri_escape_string(sheet, NULL, FALSE);
            /* the manifest is a file in a directory agents write to */
            g_autofree gchar *escaped_sha = sha != NULL
                ? g_uri_escape_string(sha, NULL, FALSE) : NULL;

            src = g_strdup_printf("%s%s?path=%s%s%s", OFFICE_CHARACTER_PATH,
                                  escaped_agent, escaped_path,
                                  escaped_sha != NULL ? "&sha256=" : "",
                                  escaped_sha != NULL ? escaped_sha : "");
        }
        node = json_node_copy(node);
        json_object_set_string_member(json_node_get_object(node), "src", src);
        json_array_add_element(out, node);
    }
    return out;
}

/*
 * The usage board, from usage.providers' answer (@usage): every login and
 * every window, worded by libclawt (clawt_office_board_new(), which is
 * clawt_usage_login_title(), _login_status_label(), _window_value_label(),
 * _window_reset_label(), _window_remaining() and the level's tone -- the
 * Fleet page's words) and sent as clawt_office_board_to_json() has them.
 * The script holds no copy of the rules, only how to lay them out: what
 * does not fit on the wall it counts, and the details panel lists all.
 *
 * Returns an object whose `logins` may be empty; the script then draws no
 * board, as the lrg scene does not.
 */
static JsonObject *
office_usage_board(JsonObject *usage)
{
    g_autoptr(ClawtOfficeBoard) board = clawt_office_board_new(
        usage, g_get_real_time() / G_USEC_PER_SEC);

    return clawt_office_board_to_json(board);
}

/*
 * usage.providers as it stands -- never with `refresh`: reading an
 * allowance spawns the provider's CLI, and a wall display polled every
 * few seconds must not.  The Fleet page refreshes; the board shows the
 * result.  NULL when the daemon did not answer.
 */
static JsonObject *
office_board(ClawtWebApp *app)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "usage.providers", NULL);

    if (clawt_web_root(reply) == NULL)
        return NULL;
    return office_usage_board(clawt_web_root(reply));
}

/*
 * The activity words and tones, walked from libclawt's enumeration --
 * never a list kept here, which is how two clients come to call one
 * state different things.
 */
static JsonObject *
office_vocabulary(void)
{
    JsonObject *words = json_object_new();
    guint i;

    for (i = 0; i < clawt_office_activity_count(); i++) {
        ClawtOfficeActivity activity = (ClawtOfficeActivity)i;
        JsonObject *word = json_object_new();

        json_object_set_string_member(word, "label",
                                      clawt_office_activity_label(activity));
        json_object_set_string_member(word, "tone",
                                      clawt_office_activity_tone(activity));
        json_object_set_object_member(words,
                                      clawt_office_activity_nick(activity), word);
    }
    return words;
}

/*
 * The moving half of a scene: the director's layout, the faces of
 * everybody in it and each one's roster line (libclawt's, so the tooltip
 * and the roster under the canvas say the same sentence).
 *
 * Returns NULL with @out_off set when the office is off, and NULL with it
 * clear when the daemon did not answer.
 */
static JsonObject *
office_scene_layout(ClawtWebApp *app, const gchar *world_id, gboolean inline_sheets,
                    gboolean *out_off)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *layout;
    JsonObject *scene;
    JsonObject *lines;
    JsonObject *board;
    JsonArray *people;
    JsonArray *characters;
    gint64 now;
    guint i;

    *out_off = FALSE;
    if (world_id != NULL)
        clawt_web_payload_set(payload, "world", world_id);
    reply = clawt_web_app_call(app, "office.layout",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    layout = clawt_web_root(reply);
    if (layout == NULL)
        return NULL;
    if (!clawt_web_member_bool(layout, "enabled", TRUE)) {
        *out_off = TRUE;
        return NULL;
    }

    people = clawt_web_member_array(layout, "participants");
    now = clawt_web_member_int(layout, "now", g_get_real_time() / G_USEC_PER_SEC);
    lines = json_object_new();
    for (i = 0; people != NULL && i < json_array_get_length(people); i++) {
        JsonObject *p = json_array_get_object_element(people, i);
        const gchar *id = clawt_web_member(p, "id", NULL);
        g_autofree gchar *line = NULL;

        if (id == NULL)
            continue;
        line = clawt_office_roster_line(p, now);
        json_object_set_string_member(lines, id, line != NULL ? line : "");
    }

    scene = json_object_new();
    json_object_set_object_member(scene, "layout", json_object_ref(layout));
    json_object_set_object_member(scene, "faces", office_faces(app, people));
    json_object_set_object_member(scene, "lines", lines);
    json_object_set_boolean_member(scene, "enabled", TRUE);
    /* absent when the daemon would not say: the script keeps what it has */
    characters = office_characters(app, inline_sheets);
    if (characters != NULL)
        json_object_set_array_member(scene, "characters", characters);
    board = office_board(app);
    if (board != NULL)
        json_object_set_object_member(scene, "board", board);
    return scene;
}

/*
 * @object as JSON that can sit inside a <script> element.
 *
 * json-glib escapes quotes and control characters but not `<`, and a
 * participant's detail is somebody's words: "</script><script>..." in a
 * tool step would end the data block and start a script of its own.
 * `<`, `>` and `&` only ever occur inside strings in JSON, where the
 * \u escapes mean the same characters to JSON.parse.
 */
static gchar *
office_json_for_script(JsonObject *object)
{
    g_autoptr(JsonNode) node = json_node_alloc();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autofree gchar *text = NULL;
    GString *safe;
    const gchar *c;

    json_node_init_object(node, object);
    json_generator_set_root(generator, node);
    text = json_generator_to_data(generator, NULL);
    safe = g_string_sized_new(strlen(text) + 64);
    for (c = text; *c != '\0'; c++) {
        switch (*c) {
        case '<':
            g_string_append(safe, "\\u003c");
            break;
        case '>':
            g_string_append(safe, "\\u003e");
            break;
        case '&':
            g_string_append(safe, "\\u0026");
            break;
        default:
            g_string_append_c(safe, *c);
            break;
        }
    }
    return g_string_free(safe, FALSE);
}

/* A JSON body for the script's polls; never cached, it is a moment. */
static HtmxResponse *
office_json_response(JsonObject *object, guint status)
{
    g_autoptr(JsonNode) node = json_node_alloc();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autofree gchar *text = NULL;
    HtmxResponse *response;

    json_node_init_object(node, object);
    json_generator_set_root(generator, node);
    text = json_generator_to_data(generator, NULL);
    response = htmx_response_new_with_content(text);
    htmx_response_set_status(response, status);
    htmx_response_set_content_type(response, "application/json");
    htmx_response_add_header(response, "Cache-Control", "no-store");
    htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
    return response;
}

/* {"error": @message}, for a poll the daemon could not answer. */
static HtmxResponse *
office_json_error(const gchar *message, guint status)
{
    g_autoptr(JsonObject) body = json_object_new();

    json_object_set_string_member(body, "error",
                                  message != NULL ? message : "the daemon did not answer");
    return office_json_response(body, status);
}

/* A button for the toolbar; the script gives it its behaviour. */
static HtmxButton *
office_toggle(const gchar *label, const gchar *data_name, const gchar *hint)
{
    g_autoptr(HtmxButton) button = clawt_web_button(label, NULL);

    htmx_element_add_class(HTMX_ELEMENT(button), "office-toggle");
    htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "button");
    htmx_element_set_attribute(HTMX_ELEMENT(button), data_name, "");
    htmx_element_set_attribute(HTMX_ELEMENT(button), "aria-pressed", "false");
    htmx_element_set_attribute(HTMX_ELEMENT(button), "title", hint);
    return (HtmxButton *)g_steal_pointer(&button);
}

void
clawt_web_office_view_add(ClawtWebApp *app, HtmxElement *parent)
{
    g_autoptr(JsonObject) scene = NULL;
    g_autoptr(JsonObject) world = NULL;
    g_autoptr(HtmxElement) section = NULL;
    g_autoptr(HtmxDiv) bar = htmx_div_new();
    g_autoptr(HtmxDiv) controls = htmx_div_new();
    g_autoptr(HtmxDiv) stage = htmx_div_new();
    g_autoptr(HtmxCanvas) floor = htmx_canvas_new();
    g_autoptr(HtmxCanvas) actors = htmx_canvas_new();
    g_autoptr(HtmxDiv) tip = htmx_div_new();
    g_autoptr(HtmxDiv) light = htmx_div_new();
    g_autoptr(HtmxScript) data = htmx_script_new();
    g_autoptr(HtmxScript) code = htmx_script_new_with_src("/static/office.js");
    g_autoptr(HtmxSpan) title = htmx_span_new();
    g_autoptr(HtmxSpan) simulation = NULL;
    g_autofree gchar *json = NULL;
    gboolean off = FALSE;

    g_return_if_fail(parent != NULL);

    /*
     * Off, or a daemon that did not answer: nothing at all.  The settings
     * card above says how to turn the office on and the roster below says
     * the daemon is unreachable; a third sentence would only repeat one.
     */
    scene = office_scene_layout(app, NULL, TRUE, &off);
    if (scene == NULL)
        return;
    world = office_scene_world(app, clawt_web_member(
        clawt_web_member_object(scene, "layout"), "world", NULL));
    if (world == NULL)
        return;
    json_object_set_object_member(scene, "world",
                                  json_object_ref(clawt_web_member_object(world, "world")));
    if (json_object_has_member(world, "map"))
        json_object_set_object_member(scene, "map",
                                      json_object_ref(clawt_web_member_object(world, "map")));
    json_object_set_object_member(scene, "words", office_vocabulary());
    json_object_set_string_member(scene, "layout_url", OFFICE_LAYOUT_PATH);
    json_object_set_string_member(scene, "world_url", OFFICE_WORLD_PATH);

    section = HTMX_ELEMENT(htmx_section_new());
    htmx_element_set_id(section, "office-view");
    htmx_element_add_class(section, "office-view");

    /* the toolbar: which world, the simulation label, the two toggles */
    htmx_element_add_class(HTMX_ELEMENT(bar), "office-bar");
    htmx_element_add_class(HTMX_ELEMENT(title), "office-world-name");
    htmx_node_set_text_content(HTMX_NODE(title), clawt_web_member(
        clawt_web_member_object(world, "world"), "name", "The office"));
    htmx_node_add_child(HTMX_NODE(bar), HTMX_NODE(title));
    simulation = clawt_web_badge("Simulation -- invented here, never sent", "warn");
    htmx_element_add_class(HTMX_ELEMENT(simulation), "office-sim");
    htmx_element_set_attribute(HTMX_ELEMENT(simulation), "hidden", "");
    htmx_node_add_child(HTMX_NODE(bar), HTMX_NODE(simulation));
    htmx_element_add_class(HTMX_ELEMENT(controls), "btn-row");
    htmx_element_add_class(HTMX_ELEMENT(controls), "office-controls");
    clawt_web_add(controls, office_toggle(
        "Tilt", "data-office-tilt", "Lean the floor back and stand everybody up"));
    clawt_web_add(controls, office_toggle(
        "Rooms", "data-office-rooms", "Name the rooms on the floor"));
    /* where provider usage goes: on the wall, in the corner, off -- the script cycles it */
    clawt_web_add(controls, office_toggle(
        "Usage: on the wall", "data-office-usage",
        "Where provider usage is shown: on the usage board in the office (hover or "
        "click it for details), as a panel in the corner, or not at all"));
    htmx_node_add_child(HTMX_NODE(bar), HTMX_NODE(controls));
    htmx_node_add_child(HTMX_NODE(section), HTMX_NODE(bar));

    /*
     * Two canvases: the floor, which leans back under a CSS transform in
     * the tilted view, and the people, drawn upright over it at the
     * points the same transform puts their seats.
     */
    htmx_element_add_class(HTMX_ELEMENT(stage), "office-stage");
    htmx_element_add_class(HTMX_ELEMENT(floor), "office-floor");
    htmx_element_set_attribute(HTMX_ELEMENT(floor), "role", "img");
    htmx_element_set_attribute(HTMX_ELEMENT(floor), "aria-label",
        "The office, drawn: who is where. The roster below says the same in words.");
    htmx_element_add_class(HTMX_ELEMENT(actors), "office-actors");
    htmx_element_set_attribute(HTMX_ELEMENT(actors), "aria-hidden", "true");
    htmx_element_add_class(HTMX_ELEMENT(light), "office-light");
    htmx_element_set_attribute(HTMX_ELEMENT(light), "aria-hidden", "true");
    htmx_element_add_class(HTMX_ELEMENT(tip), "office-tip");
    htmx_element_set_attribute(HTMX_ELEMENT(tip), "role", "tooltip");
    htmx_element_set_attribute(HTMX_ELEMENT(tip), "hidden", "");
    htmx_node_add_child(HTMX_NODE(stage), HTMX_NODE(floor));
    htmx_node_add_child(HTMX_NODE(stage), HTMX_NODE(light));
    htmx_node_add_child(HTMX_NODE(stage), HTMX_NODE(actors));
    htmx_node_add_child(HTMX_NODE(stage), HTMX_NODE(tip));
    htmx_node_add_child(HTMX_NODE(section), HTMX_NODE(stage));

    /* the first scene, so opening the page fetches nothing */
    json = office_json_for_script(scene);
    htmx_script_set_type(data, "application/json");
    htmx_element_set_id(HTMX_ELEMENT(data), "office-scene");
    htmx_node_set_html_content(HTMX_NODE(data), json);
    htmx_node_add_child(HTMX_NODE(section), HTMX_NODE(data));
    htmx_script_set_defer(code, TRUE);
    htmx_node_add_child(HTMX_NODE(section), HTMX_NODE(code));

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(section));
}

/*
 * GET /office/layout.json[?world=] -- the moving half of the scene, for
 * the script's poll.  `{"enabled": false}` when the office was turned
 * off under it, so the script can stop rather than retry.
 */
static HtmxResponse *
on_layout_json(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *world = htmx_request_get_query_param(request, "world");
    g_autoptr(JsonObject) scene = NULL;
    gboolean off = FALSE;

    (void)params;
    if (world != NULL && !office_world_id_is_safe(world))
        return office_json_error("no such world", SOUP_STATUS_BAD_REQUEST);
    scene = office_scene_layout(app, world, FALSE, &off);
    if (scene == NULL && off) {
        g_autoptr(JsonObject) body = json_object_new();

        json_object_set_boolean_member(body, "enabled", FALSE);
        return office_json_response(body, SOUP_STATUS_OK);
    }
    if (scene == NULL)
        return office_json_error(clawt_web_app_last_error(app), SOUP_STATUS_BAD_GATEWAY);
    return office_json_response(scene, SOUP_STATUS_OK);
}

/* GET /office/world.json[?world=] -- the pack and its map, tilesets inlined. */
static HtmxResponse *
on_world_json(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *world = htmx_request_get_query_param(request, "world");
    g_autoptr(JsonObject) scene = NULL;

    (void)params;
    if (world != NULL && !office_world_id_is_safe(world))
        return office_json_error("no such world", SOUP_STATUS_BAD_REQUEST);
    scene = office_scene_world(app, world);
    if (scene == NULL)
        return office_json_error(clawt_web_app_last_error(app), SOUP_STATUS_NOT_FOUND);
    return office_json_response(scene, SOUP_STATUS_OK);
}

/*
 * GET /office/file/:world?path=<relative> -- one pack file, as bytes.
 *
 * The path is refused here when the pack-path rule would refuse it
 * (absolute, `..`, hidden, a doubled or back slash), before the daemon is
 * asked; the daemon checks containment again on the canonical path,
 * because it is the one that knows where the pack is.
 */
static HtmxResponse *
on_file(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    /* a route parameter arrives still escaped; clawt_web_param() does this */
    g_autofree gchar *world = (params != NULL && g_hash_table_lookup(params, "world") != NULL)
        ? g_uri_unescape_string(g_hash_table_lookup(params, "world"), NULL) : NULL;
    const gchar *path = htmx_request_get_query_param(request, "path");
    g_autoptr(JsonNode) reply = NULL;
    const gchar *encoded;
    guchar *raw;
    gsize length = 0;
    g_autoptr(GBytes) bytes = NULL;
    HtmxResponse *response;

    if (!office_world_id_is_safe(world) || !clawt_office_pack_path_is_safe(path)) {
        response = htmx_response_new_with_content("that path is outside the pack\n");
        htmx_response_set_status(response, SOUP_STATUS_FORBIDDEN);
        htmx_response_set_content_type(response, "text/plain; charset=utf-8");
        return response;
    }

    reply = office_file_reply(app, world, path);
    encoded = clawt_web_member(clawt_web_root(reply), "base64", NULL);
    if (encoded == NULL)
        return htmx_response_not_found();

    raw = g_base64_decode(encoded, &length);
    bytes = g_bytes_new_take(raw, length);
    response = htmx_response_new();
    htmx_response_set_bytes(response, bytes);
    htmx_response_set_content_type(response, office_file_type(path));
    htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
    htmx_response_add_header(response, "Cache-Control", "no-cache");
    return response;
}

/* A refusal the script will never show anybody, as plain text. */
static HtmxResponse *
office_text_response(const gchar *text, guint status)
{
    HtmxResponse *response = htmx_response_new_with_content(text);

    htmx_response_set_status(response, status);
    htmx_response_set_content_type(response, "text/plain; charset=utf-8");
    htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
    return response;
}

/*
 * GET /office/character/:agent?path=<relative>[&sha256=<hex>] -- one file
 * of an agent's character (its sprite sheet), as bytes.
 *
 * Refused here, before the daemon is asked, when the agent id is not one
 * path segment or the path is one the pack rule refuses; the daemon
 * resolves it again inside <state_dir>/office/characters/<agent>/ on the
 * canonical path.  With `sha256` (the script always sends the manifest's)
 * bytes that do not match are a 409, never an image: a sheet that does
 * not match its manifest is not drawn, rather than drawn wrong.
 */
static HtmxResponse *
on_character(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent = (params != NULL && g_hash_table_lookup(params, "agent") != NULL)
        ? g_uri_unescape_string(g_hash_table_lookup(params, "agent"), NULL) : NULL;
    const gchar *path = htmx_request_get_query_param(request, "path");
    const gchar *sha = htmx_request_get_query_param(request, "sha256");
    g_autoptr(JsonNode) reply = NULL;
    const gchar *encoded;
    guchar *raw;
    gsize length = 0;
    g_autoptr(GBytes) bytes = NULL;
    HtmxResponse *response;

    if (!office_agent_id_is_safe(agent) || !clawt_office_pack_path_is_safe(path))
        return office_text_response("that path is outside the character\n",
                                    SOUP_STATUS_FORBIDDEN);

    reply = office_character_file_reply(app, agent, path);
    encoded = clawt_web_member(clawt_web_root(reply), "base64", NULL);
    if (encoded == NULL)
        return htmx_response_not_found();

    raw = g_base64_decode(encoded, &length);
    bytes = g_bytes_new_take(raw, length);
    if (!office_sheet_matches(bytes, sha))
        return office_text_response("that file does not match its manifest\n",
                                    SOUP_STATUS_CONFLICT);
    response = htmx_response_new();
    htmx_response_set_bytes(response, bytes);
    htmx_response_set_content_type(response, office_file_type(path));
    htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
    htmx_response_add_header(response, "Cache-Control", "no-cache");
    return response;
}

void
clawt_web_office_view_register(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, OFFICE_LAYOUT_PATH, on_layout_json, app);
    htmx_router_get(router, OFFICE_WORLD_PATH, on_world_json, app);
    htmx_router_get(router, "/office/file/:world", on_file, app);
    htmx_router_get(router, OFFICE_CHARACTER_PATH ":agent", on_character, app);
}
