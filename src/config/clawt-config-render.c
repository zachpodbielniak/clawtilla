/*
 * clawt-config-render.c - Rendering an agent's libreclaw configuration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "config/clawt-config-render.h"

#include <yaml-glib.h>

#include <glib/gstdio.h>

#include <string.h>

gchar *
clawt_config_agent_state_dir(ClawtConfig *config, const gchar *agent_id)
{
    g_autofree gchar *state_dir = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    state_dir = clawt_config_get_path_value(config, "daemon.state_dir");
    if (state_dir == NULL)
        state_dir = clawt_expand_path("~/.clawtilla");

    return g_build_filename(state_dir, "agents", agent_id, NULL);
}

/*
 * YAML quoting for the values we emit.
 *
 * Everything scalar goes out double-quoted rather than bare, because a
 * value like `no`, `1.0` or `@agent:example.com` changes meaning unquoted
 * and the failure shows up as a puzzling type error much later.
 */
static void
append_quoted(GString *out, const gchar *value)
{
    const gchar *p;

    g_string_append_c(out, '"');

    for (p = value; p != NULL && *p != '\0'; p++) {
        switch (*p) {
        case '"':
            g_string_append(out, "\\\"");
            break;
        case '\\':
            g_string_append(out, "\\\\");
            break;
        case '\n':
            g_string_append(out, "\\n");
            break;
        case '\r':
            /*
             * A lone CR is a YAML line break: emitted raw it is folded to
             * a space on reparse, so a CRLF-authored system prompt came
             * back subtly different from what was written.
             */
            g_string_append(out, "\\r");
            break;
        case '\t':
            g_string_append(out, "\\t");
            break;
        default:
            g_string_append_c(out, *p);
            break;
        }
    }

    g_string_append_c(out, '"');
}

static void
append_key_value(GString *out, guint indent, const gchar *key,
                 const gchar *value)
{
    guint i;

    if (value == NULL)
        return;

    for (i = 0; i < indent; i++)
        g_string_append_c(out, ' ');

    g_string_append_printf(out, "%s: ", key);
    append_quoted(out, value);
    g_string_append_c(out, '\n');
}

static void
append_key_bool(GString *out, guint indent, const gchar *key, gboolean value)
{
    guint i;

    for (i = 0; i < indent; i++)
        g_string_append_c(out, ' ');

    g_string_append_printf(out, "%s: %s\n", key, value ? "true" : "false");
}

static void
append_key_int(GString *out, guint indent, const gchar *key, gint64 value)
{
    guint i;

    for (i = 0; i < indent; i++)
        g_string_append_c(out, ' ');

    g_string_append_printf(out, "%s: %" G_GINT64_FORMAT "\n", key, value);
}

static void
append_string_list(GString *out, guint indent, const gchar *key, GStrv list)
{
    guint i;
    gsize j;

    if (list == NULL || list[0] == NULL)
        return;

    for (i = 0; i < indent; i++)
        g_string_append_c(out, ' ');

    g_string_append_printf(out, "%s:\n", key);

    for (j = 0; list[j] != NULL; j++) {
        for (i = 0; i < indent + 2; i++)
            g_string_append_c(out, ' ');

        g_string_append(out, "- ");
        append_quoted(out, list[j]);
        g_string_append_c(out, '\n');
    }
}

/*
 * Re-indents a serialised subtree so it can be dropped in under a key.
 *
 * The generator emits at column zero; nesting it under `session:` or a
 * channel needs every line pushed right by the same amount, blank lines
 * left alone so they do not become trailing whitespace.
 */
static void
append_block(GString *out, guint indent, const gchar *yaml)
{
    g_auto(GStrv) lines = NULL;
    gsize i;

    if (yaml == NULL)
        return;

    lines = g_strsplit(yaml, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        guint j;

        /* Document markers would end the document we are nesting into. */
        if (g_str_has_prefix(lines[i], "---") ||
            g_str_has_prefix(lines[i], "..."))
            continue;

        if (lines[i][0] == '\0') {
            /* A blank line stays blank rather than becoming indentation. */
            if (lines[i + 1] != NULL)
                g_string_append_c(out, '\n');
            continue;
        }

        for (j = 0; j < indent; j++)
            g_string_append_c(out, ' ');

        g_string_append(out, lines[i]);
        g_string_append_c(out, '\n');
    }
}

/*
 * Writes one secret to its own file, 0600.
 *
 * The value never goes into the rendered YAML: that file is meant to be
 * readable, copied into bug reports and diffed, and a token in it would
 * leak by the most ordinary possible route.
 */
static gboolean
write_secret_file(ClawtConfig             *config,
                  ClawtIntegrationBinding *binding,
                  const gchar             *key,
                  const gchar             *base_dir,
                  const gchar             *dir,
                  const gchar             *filename,
                  const gchar             *json_member,
                  gchar                  **out_path,
                  GError                 **error)
{
    g_autoptr(ClawtSecretRef) ref = NULL;
    g_autofree gchar *value = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr(GError) local = NULL;
    guint timeout;

    *out_path = NULL;

    if (binding == NULL)
        return TRUE;

    ref = clawt_integration_binding_get_secret(binding, key);

    /*
     * Absent is only acceptable when nothing depends on it.  The channel
     * block is rendered pointing at this file regardless, so returning
     * success here produced an agent whose configuration named a
     * credential file that was never written -- it started cleanly and
     * then never authenticated.
     */
    if (ref == NULL)
        return TRUE;

    timeout = (guint)clawt_config_get_int(config,
                                          "secrets.command_timeout_seconds");

    value = clawt_secret_ref_resolve(ref, base_dir, timeout, &local);
    if (value == NULL) {
        g_autofree gchar *described = clawt_secret_ref_describe(ref);

        /*
         * Named by reference, never by value, and reported as a failure of
         * that one credential -- an agent whose Matrix token cannot be
         * fetched should say so rather than start silently deaf.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "%s (%s): could not resolve %s: %s",
                    key, clawt_integration_binding_get_name(binding),
                    described,
                    local != NULL ? local->message : "unknown reason");
        return FALSE;
    }

    path = g_build_filename(dir, filename, NULL);

    if (json_member != NULL) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(JsonGenerator) generator = json_generator_new();
        g_autoptr(JsonNode) root = NULL;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, json_member);
        json_builder_add_string_value(builder, value);
        json_builder_end_object(builder);

        root = json_builder_get_root(builder);
        json_generator_set_root(generator, root);
        contents = json_generator_to_data(generator, NULL);
    } else {
        contents = g_strdup(value);
    }

    /*
     * 0600 and no backup: a .bak of a credential file is a second copy of
     * the secret nobody asked for, and it outlives the rotation that was
     * supposed to retire it.
     */
    if (!clawt_write_file_atomic(path, contents, -1, 0600, FALSE, error))
        return FALSE;

    *out_path = g_steal_pointer(&path);
    return TRUE;
}

/*
 * Writes a two-value credential file, as the email channel expects.
 */
static gboolean
write_login_file(ClawtConfig             *config,
                 ClawtIntegrationBinding *binding,
                 const gchar             *username,
                 const gchar             *password_key,
                 const gchar             *base_dir,
                 const gchar             *dir,
                 const gchar             *filename,
                 gchar                  **out_path,
                 GError                 **error)
{
    g_autoptr(ClawtSecretRef) ref = NULL;
    g_autofree gchar *password = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonNode) root = NULL;
    g_autoptr(GError) local = NULL;

    *out_path = NULL;

    if (binding == NULL)
        return TRUE;

    ref = clawt_integration_binding_get_secret(binding, password_key);
    if (ref == NULL || username == NULL)
        return TRUE;

    password = clawt_secret_ref_resolve(
        ref, base_dir,
        (guint)clawt_config_get_int(config, "secrets.command_timeout_seconds"),
        &local);

    if (password == NULL) {
        g_autofree gchar *described = clawt_secret_ref_describe(ref);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "%s (%s): could not resolve %s: %s",
                    password_key, clawt_integration_binding_get_name(binding),
                    described,
                    local != NULL ? local->message : "unknown reason");
        return FALSE;
    }

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "username");
    json_builder_add_string_value(builder, username);
    json_builder_set_member_name(builder, "password");
    json_builder_add_string_value(builder, password);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    contents = json_generator_to_data(generator, NULL);

    path = g_build_filename(dir, filename, NULL);

    if (!clawt_write_file_atomic(path, contents, -1, 0600, FALSE, error))
        return FALSE;

    *out_path = g_steal_pointer(&path);
    return TRUE;
}

/* ── Channel blocks ──────────────────────────────────────────────── */

static void
render_clawtilla_channel(GString          *out,
                         ClawtAgentConfig *agent,
                         const gchar      *link_socket,
                         const gchar      *state_dir)
{
    g_autofree gchar *token_file = g_build_filename(state_dir, "token", NULL);

    g_string_append(out, "  clawtilla:\n");
    append_key_bool(out, 4, "enabled", TRUE);
    append_key_value(out, 4, "socket", link_socket);
    append_key_value(out, 4, "agent_id", clawt_agent_config_get_id(agent));
    append_key_value(out, 4, "token_file", token_file);

    /*
     * Reconnection is on by default and matters more here than for most
     * channels: the daemon restarts whenever the fleet config changes, and
     * an agent that gave up on the first drop would sit idle until
     * somebody noticed it had.
     */
    append_key_int(out, 4, "reconnect_backoff_seconds", 5);

    /*
     * No "Still working..." notes.  libreclaw posts one every five
     * minutes by default on every channel that is not email, into the
     * room *and the thread* of the message being worked on -- and a
     * thread here is a clawtilla task id, so each note arrived looking
     * exactly like the agent's answer.  A routine therefore reported
     * `completed` with the text "Still working..." as its result while
     * the work had not started.  The daemon guards against that now, but
     * a message nothing can tell from an answer is better not generated.
     *
     * Nothing is lost by it: the same turn already raises the typing
     * indicator, which both clients show as a live activity line for as
     * long as it runs.  The note was a second, worse answer to the same
     * question, and it landed in the operator's transcript.
     */
    append_key_bool(out, 4, "progress_enabled", FALSE);
}

/*
 * Every channel below reads through a binding rather than out of the
 * agent's own block, so a Matrix account configured once at the top level
 * and pointed at four agents renders exactly what four inline blocks
 * would have.  There is one code path, so there is one behaviour.
 */
static void
render_matrix_channel(GString                 *out,
                      ClawtIntegrationBinding *binding,
                      const gchar             *token_path)
{
    g_auto(GStrv) rooms = NULL;

    if (binding == NULL)
        return;

    g_string_append(out, "  matrix:\n");
    append_key_bool(out, 4, "enabled", TRUE);
    append_key_value(out, 4, "homeserver",
                     clawt_integration_binding_get_string(binding,
                                                          "homeserver"));
    append_key_value(out, 4, "user_id",
                     clawt_integration_binding_get_string(binding, "user_id"));
    append_key_value(out, 4, "access_token_file", token_path);

    rooms = clawt_integration_binding_get_string_list(binding, "rooms");
    append_string_list(out, 4, "rooms", rooms);

    append_key_bool(out, 4, "require_mention",
                    clawt_integration_binding_get_boolean(binding,
                                                          "require_mention"));
}

static void
render_email_channel(GString                 *out,
                     ClawtIntegrationBinding *binding,
                     const gchar             *imap_path,
                     const gchar             *smtp_path)
{
    g_auto(GStrv) folders = NULL;

    if (binding == NULL)
        return;

    g_string_append(out, "  email:\n");
    append_key_bool(out, 4, "enabled", TRUE);
    append_key_value(out, 4, "imap_host",
                     clawt_integration_binding_get_string(binding,
                                                          "imap_host"));
    append_key_int(out, 4, "imap_port",
                   clawt_integration_binding_get_int(binding, "imap_port"));
    append_key_value(out, 4, "imap_credentials_file", imap_path);

    folders = clawt_integration_binding_get_string_list(binding, "folders");
    append_string_list(out, 4, "folders", folders);

    append_key_value(out, 4, "smtp_host",
                     clawt_integration_binding_get_string(binding,
                                                          "smtp_host"));
    append_key_int(out, 4, "smtp_port",
                   clawt_integration_binding_get_int(binding, "smtp_port"));
    append_key_value(out, 4, "smtp_credentials_file", smtp_path);
}

/*
 * The channels a `libreclaw:` passthrough declares, keyed by channel id.
 *
 * clawtilla refuses a passthrough that redeclares a section it renders
 * itself -- YAML keeps the last of two identical top-level keys, so
 * clawtilla's own block would be silently discarded.  That refusal is
 * right and it made `channels:` unreachable: libreclaw's webhook routing
 * lives at `channels.webhook.endpoints`, which is a list of objects with
 * nested targets and has no sensible spelling in a flat schema.  So the
 * listener could be configured, scoped and health-checked, and could
 * never receive anything.
 *
 * The collision hazard is **per key**, not per section, so that is where
 * the check belongs.  Each channel's own keys are merged into the block
 * clawtilla rendered, and a key clawtilla already wrote is refused by
 * name rather than the whole edit.
 *
 * Each value is the channel's keys already rendered at four spaces,
 * which is the indent they land at -- produced by yaml-glib rather than
 * by re-emitting the nodes here, so a nested sequence of mappings comes
 * out as YAML rather than as an approximation of it.
 *
 * Returns: (transfer full) (nullable): id -> rendered keys, or %NULL
 */
static GHashTable *
passthrough_channels(const gchar *passthrough, GHashTable **out_keys,
                     GError **error)
{
    g_autoptr(YamlParser) parser = NULL;
    g_autoptr(YamlDocument) document = NULL;
    YamlNode *root;
    YamlNode *channels;
    YamlMapping *mapping;
    g_autoptr(GList) ids = NULL;
    GList *walk;
    GHashTable *blocks;

    *out_keys = NULL;

    if (passthrough == NULL || *passthrough == '\0')
        return NULL;

    parser = yaml_parser_new();

    if (!yaml_parser_load_from_data(parser, passthrough, -1, error))
        return NULL;

    document = yaml_parser_dup_document(parser, 0);

    if (document == NULL)
        return NULL;

    root = yaml_document_get_root(document);

    if (root == NULL ||
        yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
        return NULL;

    channels = yaml_mapping_get_member(yaml_node_get_mapping(root), "channels");

    if (channels == NULL ||
        yaml_node_get_node_type(channels) != YAML_NODE_MAPPING)
        return NULL;

    mapping = yaml_node_get_mapping(channels);
    ids = yaml_mapping_get_members(mapping);

    blocks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    *out_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify)g_strfreev);

    for (walk = ids; walk != NULL; walk = walk->next) {
        const gchar *id = walk->data;
        YamlNode *channel = yaml_mapping_get_member(mapping, id);
        g_autoptr(YamlMapping) wrapper_inner = NULL;
        g_autoptr(YamlMapping) wrapper_outer = NULL;
        g_autoptr(YamlNode) inner_node = NULL;
        g_autoptr(YamlNode) outer_node = NULL;
        g_autoptr(YamlGenerator) generator = NULL;
        g_autofree gchar *text = NULL;
        g_auto(GStrv) lines = NULL;
        g_autoptr(GString) body = NULL;
        gsize i;

        if (channel == NULL ||
            yaml_node_get_node_type(channel) != YAML_NODE_MAPPING)
            continue;

        /*
         * Generated inside `channels: <id>:` and then the two heading
         * lines are dropped, so the indentation is yaml-glib's own
         * rather than something counted by hand here.
         */
        wrapper_inner = yaml_mapping_new();
        yaml_mapping_set_member(wrapper_inner, id, channel);
        inner_node = yaml_node_new_mapping(wrapper_inner);

        wrapper_outer = yaml_mapping_new();
        yaml_mapping_set_member(wrapper_outer, "channels", inner_node);
        outer_node = yaml_node_new_mapping(wrapper_outer);

        generator = yaml_generator_new();
        yaml_generator_set_root(generator, outer_node);
        text = yaml_generator_to_data(generator, NULL, NULL);

        if (text == NULL)
            continue;

        lines = g_strsplit(text, "\n", -1);
        body = g_string_new(NULL);

        for (i = 0; lines[i] != NULL; i++) {
            /* "channels:" and "  <id>:" are ours, not the channel's. */
            if (i < 2 || *lines[i] == '\0')
                continue;

            g_string_append(body, lines[i]);
            g_string_append_c(body, '\n');
        }

        g_hash_table_insert(blocks, g_strdup(id),
                            g_strdup(body->str));

        {
            g_autoptr(GList) keys = yaml_mapping_get_members(
                yaml_node_get_mapping(channel));
            g_autoptr(GPtrArray) names = g_ptr_array_new();
            GList *key;

            for (key = keys; key != NULL; key = key->next)
                g_ptr_array_add(names, g_strdup(key->data));

            g_ptr_array_add(names, NULL);
            g_hash_table_insert(*out_keys, g_strdup(id),
                                g_strdupv((GStrv)names->pdata));

            for (i = 0; i + 1 < names->len; i++)
                g_free(g_ptr_array_index(names, i));
        }
    }

    return blocks;
}

/*
 * The passthrough with its `channels:` taken out.
 *
 * Its channels went into clawtilla's own block, and left here as well
 * they would be a second top-level `channels:` -- YAML keeps the last,
 * which is the exact collision the whole-section refusal existed to
 * prevent.  Line-based rather than a reparse-and-regenerate: everything
 * else in the passthrough is copied across verbatim on purpose, and
 * round-tripping it through a parser would quietly reformat somebody's
 * file.
 */
static gchar *
passthrough_without_channels(const gchar *passthrough)
{
    g_auto(GStrv) lines = g_strsplit(passthrough, "\n", -1);
    g_autoptr(GString) out = g_string_new(NULL);
    gboolean skipping = FALSE;
    gsize i;

    for (i = 0; lines[i] != NULL; i++) {
        if (g_str_has_prefix(lines[i], "channels:")) {
            skipping = TRUE;
            continue;
        }

        /* Anything back at the left margin ends the section. */
        if (skipping) {
            if (lines[i][0] == '\0' || g_ascii_isspace(lines[i][0]))
                continue;

            skipping = FALSE;
        }

        g_string_append(out, lines[i]);
        g_string_append_c(out, '\n');
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * Splices those channels into the block clawtilla rendered.
 *
 * A key clawtilla already wrote for that channel is refused by name --
 * that is the collision the whole-section refusal was protecting against,
 * checked where it actually is.  The check reads the text that was
 * *emitted* rather than a list of what the renderers write, because a
 * hand-maintained list of an option's keys is exactly what drifts.
 */
static gboolean
merge_passthrough_channels(GString *channels, GHashTable *blocks,
                           GHashTable *keys, GError **error)
{
    GHashTableIter iter;
    gpointer id;
    gpointer text;

    if (blocks == NULL)
        return TRUE;

    g_hash_table_iter_init(&iter, blocks);

    while (g_hash_table_iter_next(&iter, &id, &text)) {
        g_autofree gchar *heading = g_strdup_printf("\n  %s:\n",
                                                    (const gchar *)id);
        const gchar *start = strstr(channels->str, heading);
        GStrv names = g_hash_table_lookup(keys, id);
        gsize insert_at;
        gsize i;

        if (start == NULL) {
            /* A channel clawtilla does not render at all: emitted whole. */
            g_string_append_printf(channels, "  %s:\n%s", (const gchar *)id,
                                   (const gchar *)text);
            continue;
        }

        /*
         * The block runs to the next line at two-space indent, or to the
         * end of the section.
         */
        {
            const gchar *scan = start + strlen(heading);
            const gchar *end = scan;

            while (*end != '\0') {
                const gchar *line_end = strchr(end, '\n');

                if (end[0] != ' ' || end[1] != ' ')
                    break;

                if (end[2] != ' ' && end[2] != '\0' && end[2] != '\n')
                    break;

                if (line_end == NULL) {
                    end += strlen(end);
                    break;
                }

                end = line_end + 1;
            }

            for (i = 0; names != NULL && names[i] != NULL; i++) {
                g_autofree gchar *needle = g_strdup_printf("    %s:",
                                                            names[i]);
                const gchar *found = strstr(scan, needle);

                if (found != NULL && found < end) {
                    g_set_error(error, CLAWT_ERROR,
                                CLAWT_ERROR_CONFIG_INVALID,
                                "the libreclaw: block sets "
                                "channels.%s.%s, which clawtilla renders "
                                "itself; set it through the integration "
                                "instead", (const gchar *)id, names[i]);
                    return FALSE;
                }
            }

            insert_at = (gsize)(end - channels->str);
        }

        g_string_insert(channels, (gssize)insert_at, (const gchar *)text);
    }

    return TRUE;
}

static void
render_webhook_channel(GString *out, ClawtIntegrationBinding *binding)
{
    if (binding == NULL)
        return;

    g_string_append(out, "  webhook:\n");
    append_key_bool(out, 4, "enabled", TRUE);
    append_key_int(out, 4, "listen_port",
                   clawt_integration_binding_get_int(binding, "port"));

    /*
     * The loopback unless somebody widened it.  libreclaw's own default
     * is every interface, which is the behaviour it has always had --
     * clawtilla names an address so an agent's endpoint, which can run a
     * shell command, is not reachable from wherever the machine happens
     * to be reachable from because nobody said otherwise.
     */
    {
        const gchar *address =
            clawt_integration_binding_get_string(binding, "bind_address");

        append_key_value(out, 4, "bind_address",
                         (address != NULL && *address != '\0')
                             ? address : "127.0.0.1");
    }
}


/*
 * The standing rule an agent with a computer needs on every turn.
 *
 * An agent runs as a libreclaw process on the *host*.  Its own bash,
 * read and write tools therefore touch the host filesystem, and its
 * container is reachable only through clawtilla_computer_exec.  Nothing
 * told it that, so an agent with a perfectly good container sat in its
 * workspace running commands on the machine clawtilla runs on and
 * reporting, correctly, that it did not appear to be in a container.
 *
 * It goes in the per-turn suffix rather than only the system prompt
 * because a resumed CLI session never re-receives the system prompt and
 * a long conversation drifts away from anything said once at the start.
 * This is the rule that has to hold on turn 200.
 */
static gchar *
render_computer_directive(ClawtAgentConfig *for_agent)
{
    ClawtComputerType type =
        (ClawtComputerType)clawt_agent_config_get_enum(for_agent,
                                                       "computer.type");
    const gchar *id = clawt_agent_config_get_id(for_agent);

    switch (type) {
    case CLAWT_COMPUTER_CONTAINER:
        return g_strdup_printf(
            "[clawtilla] Your computer is the container 'clawt-%s'. Run "
            "every shell command in it with clawtilla_computer_exec. Your "
            "own bash, read and write tools run on the host, outside it, "
            "which is not where your work belongs. Touch the host only "
            "when the user asks, and say so when you do.", id);

    case CLAWT_COMPUTER_VM:
        return g_strdup_printf(
            "[clawtilla] Your computer is the virtual machine 'clawt-%s'. "
            "Run every shell command in it with clawtilla_computer_exec. "
            "Your own bash, read and write tools run on the host, outside "
            "it, which is not where your work belongs. Touch the host "
            "only when the user asks, and say so when you do.", id);

    case CLAWT_COMPUTER_HOST:
        /*
         * No redirection to give -- the host is its computer.  What it
         * needs instead is that this is somebody's real machine, which
         * the confinement mode alone does not convey.
         */
        return g_strdup(
            "[clawtilla] Your computer is the real machine clawtilla runs "
            "on. Confinement is in force and will refuse paths outside "
            "it. Every command affects a machine somebody uses.");

    case CLAWT_COMPUTER_NONE:
    default:
        return NULL;
    }
}

gchar *
clawt_config_render_agent(ClawtConfig       *config,
                          ClawtAgentConfig  *agent,
                          const gchar       *link_socket,
                          const gchar       *state_dir,
                          GError           **error)
{
    g_autoptr(GString) out = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *passthrough = NULL;
    g_auto(GStrv) identity_files = NULL;
    const gchar *name;
    const gchar *system_prompt;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);
    g_return_val_if_fail(agent != NULL, NULL);
    g_return_val_if_fail(link_socket != NULL, NULL);
    g_return_val_if_fail(state_dir != NULL, NULL);

    if (clawt_agent_config_is_shadow(agent)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "agent %s cannot be rendered: %s",
                    clawt_agent_config_get_id(agent),
                    clawt_agent_config_get_shadow_reason(agent));
        return NULL;
    }

    out = g_string_new(NULL);

    /*
     * Said plainly and first, because the natural thing to do with a
     * config file is edit it, and every edit here is silently lost on the
     * next daemon start.
     */
    g_string_append(out,
        "# Generated by clawtilla. Do not edit.\n"
        "#\n"
        "# This file is rendered from the agent's block in clawtilla.yaml\n"
        "# every time the daemon starts or reloads its configuration, so\n"
        "# any change made here is overwritten without warning.\n"
        "#\n");
    g_string_append_printf(out, "# Agent:  %s\n",
                           clawt_agent_config_get_id(agent));
    g_string_append_printf(out, "# Source: %s\n",
                           clawt_config_get_path(config) != NULL
                               ? clawt_config_get_path(config)
                               : "(in memory)");
    g_string_append(out, "\n");

    /* ── agent ── */
    name = clawt_agent_config_get_string(agent, "name");
    workspace = clawt_agent_config_get_workspace(agent);

    g_string_append(out, "agent:\n");
    append_key_value(out, 2, "name",
                     name != NULL ? name : clawt_agent_config_get_id(agent));
    append_key_value(out, 2, "workspace", workspace);

    /*
     * Joined with anything the user set rather than replacing it: a
     * standing rule of ours should not silently discard one of theirs.
     */
    {
        g_autofree gchar *directive = render_computer_directive(agent);
        const gchar *configured =
            clawt_agent_config_get_string(agent, "prompt_suffix");
        g_autofree gchar *suffix = NULL;

        if (directive != NULL && configured != NULL)
            suffix = g_strconcat(directive, "\n\n", configured, NULL);
        else if (directive != NULL)
            suffix = g_strdup(directive);
        else if (configured != NULL)
            suffix = g_strdup(configured);

        if (suffix != NULL)
            append_key_value(out, 2, "prompt_suffix", suffix);
    }

    /*
     * The standard set when nothing was configured.
     *
     * The files are scaffolded into every workspace, so leaving this
     * empty produced an agent surrounded by its own identity files with
     * none of them loaded -- which looks exactly like the files being
     * ignored, because they were.  An explicit list still wins, and an
     * inline system_prompt makes the whole thing moot.
     *
     * Asked of the workspace rather than decided here, because the
     * scaffolder needs the same answer: it writes these files, and one
     * of them deciding differently is how a workspace fills up with
     * templates nothing loads.
     */
    identity_files = clawt_workspace_effective_identity_files(agent);

    append_string_list(out, 2, "identity_files", identity_files);

    system_prompt = clawt_agent_config_get_string(agent,
                                                  "persona.system_prompt");
    if (system_prompt != NULL)
        append_key_value(out, 2, "system_prompt", system_prompt);

    g_string_append(out, "\n");

    /* ── ai ── */
    g_string_append(out, "ai:\n");
    append_key_value(out, 2, "provider",
                     clawt_agent_config_get_string(agent, "model.provider"));
    append_key_value(out, 2, "model",
                     clawt_agent_config_get_string(agent, "model.model"));
    append_key_value(out, 2, "default_effort",
                     clawt_agent_config_get_string(agent, "model.effort"));
    g_string_append(out, "\n");

    /*
     * Per-agent session and database paths.
     *
     * These must differ per agent even when two agents run embedded in one
     * process: sharing a persist_dir would let one agent resume another's
     * session, which looks like the model hallucinating a conversation it
     * never had.
     */
    {
        g_autofree gchar *sessions = g_build_filename(state_dir, "sessions",
                                                      NULL);
        g_autofree gchar *database = clawt_usage_database_path(state_dir);
        /*
         * Skills come from the *workspace*, not the state directory.
         *
         * They are authored content -- somebody writes a skill and edits
         * it, the same as SOUL.org -- so they belong beside the persona
         * rather than beside the mailbox. Building this from state_dir
         * was invisible for as long as the two were the same directory,
         * which they are for every agent that does not set
         * `agents.workspace`. The moment one does, skills.dir named a
         * directory clawtilla has never created and nothing else in the
         * tree ever writes to, and the agent silently had no skills.
         *
         * The `workspace` this reads is the function's own, resolved at
         * the top. A second one here shadowed it -- same value, and a
         * warning, which this project treats as a latent bug rather than
         * noise.
         */
        g_autofree gchar *skills =
            (workspace != NULL) ? g_build_filename(workspace, "skills", NULL)
                                : g_build_filename(state_dir, "skills", NULL);

        g_string_append(out, "session:\n");
        append_key_value(out, 2, "persist_dir", sessions);
        g_string_append(out, "\n");

        /*
         * Written to where libreclaw actually puts it, which is inside
         * persist_dir rather than beside it.  The sqlite backend builds
         * the filename from `session.persist_dir` and never reads this
         * key, so a different value here would be a line in every
         * agent's config naming a file that does not exist -- which is
         * exactly how `/reset` came to look in the wrong place.
         */
        g_string_append(out, "database:\n");
        append_key_value(out, 2, "path", database);
        g_string_append(out, "\n");

        g_string_append(out, "skills:\n");
        append_key_value(out, 2, "dir", skills);
        g_string_append(out, "\n");
    }

    /* ── channels ── */
    {
        /*
         * Built on its own so the passthrough's channels can be merged
         * into it before it reaches the document -- the collision check
         * reads the text that was *emitted* rather than a list of what
         * the renderers write, and a hand-maintained list of an option's
         * keys is exactly what drifts.
         */
        g_autoptr(GString) channels = g_string_new("channels:\n");
        g_autoptr(GHashTable) blocks = NULL;
        g_autoptr(GHashTable) merged_keys = NULL;

        render_clawtilla_channel(channels, agent, link_socket, state_dir);

    {
        g_autoptr(GPtrArray) bindings =
            clawt_integration_resolve_for_agent(config, agent);
        g_autofree gchar *matrix_token =
            g_build_filename(state_dir, "credentials",
                             "matrix_credentials.json", NULL);
        g_autofree gchar *imap =
            g_build_filename(state_dir, "credentials",
                             "imap_credentials.json", NULL);
        g_autofree gchar *smtp =
            g_build_filename(state_dir, "credentials",
                             "smtp_credentials.json", NULL);

        render_matrix_channel(channels,
                              clawt_integration_find_binding(bindings,
                                                             "matrix"),
                              matrix_token);
        render_email_channel(channels,
                             clawt_integration_find_binding(bindings, "email"),
                             imap, smtp);
        render_webhook_channel(channels,
                               clawt_integration_find_binding(bindings,
                                                              "webhook"));

        /*
         * channels.local owns stdin and stdout.  Two agents in one process
         * would fight over the terminal, and an agent started by the daemon
         * has no terminal at all, so it is only ever rendered when asked for
         * explicitly.
         */
        if (clawt_integration_find_binding(bindings, "local") != NULL) {
            g_string_append(channels, "  local:\n");
            append_key_bool(channels, 4, "enabled", TRUE);
        }

        if (clawt_integration_find_binding(bindings, "cmacs") != NULL) {
            g_string_append(channels, "  cmacs:\n");
            append_key_bool(channels, 4, "enabled", TRUE);
        }
    }

        /*
         * ...and whatever the passthrough declares under `channels:`,
         * merged into the blocks above rather than refused wholesale.
         * Refusing the section is what made
         * `channels.webhook.endpoints` -- the routing without which the
         * webhook listener receives nothing useful -- impossible to
         * express at all.
         */
        {
            /*
             * Held, because get_raw_yaml() generates a fresh string on
             * every call -- it is transfer-full, and passing it straight
             * in leaks one per agent per render.
             */
            g_autofree gchar *raw =
                clawt_agent_config_get_raw_yaml(agent, "libreclaw");

            blocks = passthrough_channels(raw, &merged_keys, NULL);
        }

        if (!merge_passthrough_channels(channels, blocks, merged_keys,
                                        error))
            return NULL;

        g_string_append(channels, "\n");
        g_string_append(out, channels->str);
    }

    /*
     * Passthrough last, so it wins.
     *
     * clawtilla does not model every libreclaw option and never will.
     * Copying the subtree across verbatim means a libreclaw setting
     * clawtilla has not heard of is still reachable, without waiting for
     * clawtilla to grow a schema entry for it.
     */
    passthrough = clawt_agent_config_get_raw_yaml(agent, "libreclaw");

    /*
     * A passthrough block that redeclares a section clawtilla already
     * rendered would win outright: YAML keeps the last of two identical
     * top-level keys, so a stray `session:` here silently deleted the
     * per-agent persist_dir -- and two agents sharing a persist_dir means
     * one resuming the other's conversation, which reads as the model
     * hallucinating a history it never had.
     */
    if (passthrough != NULL && *passthrough != '\0') {
        /*
         * `channels:` is deliberately not here any more: it is merged
         * per channel above, key by key, because the collision hazard is
         * per key rather than per section -- and refusing the whole
         * section made libreclaw's webhook routing unexpressible.  The
         * five that remain are single blocks clawtilla owns outright.
         */
        static const gchar *const rendered_sections[] = {
            "agent:", "ai:", "session:", "database:", "skills:", NULL
        };
        g_auto(GStrv) lines = g_strsplit(passthrough, "\n", -1);
        gsize i;

        for (i = 0; lines[i] != NULL; i++) {
            gsize j;

            for (j = 0; rendered_sections[j] != NULL; j++) {
                if (!g_str_has_prefix(lines[i], rendered_sections[j]))
                    continue;

                /*
                 * The agent is not named here.  Everything that surfaces
                 * this error -- the daemon's warning, the reload's list
                 * of refusals, `config render` -- already knows which
                 * agent it asked about and says so, and saying it twice
                 * read as "chief: chief: the libreclaw: block ...".
                 */
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "the libreclaw: block redeclares '%s', which "
                            "clawtilla renders itself; the two would "
                            "collide and clawtilla's would be discarded",
                            rendered_sections[j]);
                return NULL;
            }
        }
    }

    if (passthrough != NULL && *passthrough != '\0') {
        /*
         * Without its `channels:`, which went into the block above.
         * Left here as well it would be a second top-level `channels:`
         * and YAML keeps the last -- which is the exact collision the
         * whole-section refusal existed to prevent, reintroduced by the
         * fix for it.
         */
        g_autofree gchar *rest = passthrough_without_channels(passthrough);

        if (rest != NULL && *rest != '\0') {
            g_string_append(out,
                "# ── Passthrough from the agent's `libreclaw:` block ──\n");
            append_block(out, 0, rest);
        }
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

gboolean
clawt_config_write_agent_files(ClawtConfig       *config,
                               ClawtAgentConfig  *agent,
                               const gchar       *link_socket,
                               gchar            **out_config_path,
                               GError           **error)
{
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *credentials_dir = NULL;
    g_autofree gchar *secrets_dir = NULL;
    g_autofree gchar *config_path = NULL;
    g_autofree gchar *rendered = NULL;
    g_autofree gchar *matrix_token = NULL;
    g_autofree gchar *imap_file = NULL;
    g_autofree gchar *smtp_file = NULL;
    const gchar *agent_id;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), FALSE);
    g_return_val_if_fail(agent != NULL, FALSE);
    g_return_val_if_fail(link_socket != NULL, FALSE);

    agent_id = clawt_agent_config_get_id(agent);
    state_dir = clawt_config_agent_state_dir(config, agent_id);

    /*
     * A bare file reference resolves against secrets.dir, which is what
     * the schema has always said it does.  Passing NULL meant it resolved
     * against whatever directory the daemon happened to be started in.
     */
    secrets_dir = clawt_config_get_path_value(config, "secrets.dir");

    if (!clawt_ensure_dir(state_dir, 0700, error))
        return FALSE;

    /*
     * The workspace and its identity files, created before anything
     * tries to read them.  Existing files are left alone, so this is
     * safe to run on every start -- which is when it runs, because an
     * agent that was created before a file joined the standard set
     * should get it too rather than staying a version behind.
     */
    if (!clawt_workspace_scaffold(agent, error))
        return FALSE;

    /*
     * The .mcp.json that puts clawtilla's tools into the agent's
     * session. Written on every start because it carries the daemon's
     * socket and the token path, and a stale one points at nothing.
     */
    {
        /*
         * The IPC socket comes from the config rather than being passed
         * in: that is where the daemon gets its own from, so the two can
         * never disagree about where an agent should dial.
         */
        g_autofree gchar *ipc_socket =
            clawt_config_get_path_value(config, "daemon.socket");

        if (!clawt_workspace_write_mcp_config(config, agent, ipc_socket,
                                               state_dir, error))
            return FALSE;

        /*
         * And the paragraph that tells the agent what those servers are,
         * which has to happen here rather than at scaffold time: the
         * scaffolder only writes files that are missing, so an agent
         * given a Matrix account today would never be told about it.
         */
        if (!clawt_workspace_update_tools_org(config, agent, error))
            return FALSE;
    }

    credentials_dir = g_build_filename(state_dir, "credentials", NULL);
    if (!clawt_ensure_dir(credentials_dir, 0700, error))
        return FALSE;

    /*
     * The link token, created once and then left alone.
     *
     * Regenerating it on every render would lock out an agent that is
     * already connected -- it read the file at start and has no reason to
     * look again.  The socket's own permissions are the first line here;
     * the token stops one agent on this machine claiming another's
     * identity and reading its mail.
     */
    {
        g_autofree gchar *token_path = g_build_filename(state_dir, "token",
                                                        NULL);

        if (!g_file_test(token_path, G_FILE_TEST_EXISTS)) {
            g_autofree gchar *token = clawt_generate_token(error);

            if (token == NULL)
                return FALSE;

            if (!clawt_write_file_atomic(token_path, token, -1, 0600, FALSE,
                                         error))
                return FALSE;
        }
    }

    /*
     * The agent's own `credentials:` block.
     *
     * These were declared in the schema, documented as "written to
     * credential files at 0600", and then never resolved by anything --
     * so an agent configured with {env: ANTHROPIC_API_KEY} started
     * cleanly and simply had no key.  Each one is now written to a file
     * AND passed to the child as an environment variable named after the
     * key in upper case, because a provider CLI wants the variable and
     * anything file-based wants the path.
     */
    {
        g_autoptr(GHashTable) credentials =
            clawt_agent_config_get_credentials(agent);
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_iter_init(&iter, credentials);

        while (g_hash_table_iter_next(&iter, &key, &value)) {
            g_autofree gchar *path = g_build_filename(credentials_dir, key,
                                                      NULL);
            g_autofree gchar *resolved = NULL;
            g_autoptr(GError) local = NULL;

            resolved = clawt_secret_ref_resolve(
                value, secrets_dir,
                (guint)clawt_config_get_int(config,
                                            "secrets.command_timeout_seconds"),
                &local);

            if (resolved == NULL) {
                g_autofree gchar *described =
                    clawt_secret_ref_describe(value);

                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                            "credentials.%s: could not resolve %s: %s",
                            (const gchar *)key, described,
                            local != NULL ? local->message
                                          : "unknown reason");
                return FALSE;
            }

            if (!clawt_write_file_atomic(path, resolved, -1, 0600, FALSE,
                                         error))
                return FALSE;
        }
    }

    {
        g_autoptr(GPtrArray) bindings =
            clawt_integration_resolve_for_agent(config, agent);
        ClawtIntegrationBinding *matrix =
            clawt_integration_find_binding(bindings, "matrix");
        ClawtIntegrationBinding *email =
            clawt_integration_find_binding(bindings, "email");

        if (!write_secret_file(config, matrix, "access_token",
                               secrets_dir, credentials_dir,
                               "matrix_credentials.json",
                               "access_token", &matrix_token, error))
            return FALSE;

        if (!write_login_file(config, email,
                              email != NULL
                                  ? clawt_integration_binding_get_string(
                                        email, "username")
                                  : NULL,
                              "password",
                              secrets_dir, credentials_dir,
                              "imap_credentials.json",
                              &imap_file, error))
            return FALSE;

        /*
         * The same login for both directions unless the config separates
         * them. libreclaw wants two files; making the user say the same
         * password twice would be a worse answer than writing it twice.
         */
        if (!write_login_file(config, email,
                              email != NULL
                                  ? clawt_integration_binding_get_string(
                                        email, "username")
                                  : NULL,
                              "password",
                              secrets_dir, credentials_dir,
                              "smtp_credentials.json",
                              &smtp_file, error))
            return FALSE;
    }

    rendered = clawt_config_render_agent(config, agent, link_socket,
                                         state_dir, error);
    if (rendered == NULL)
        return FALSE;

    config_path = g_build_filename(state_dir, "config.yaml", NULL);

    if (!clawt_write_file_atomic(config_path, rendered, -1, 0600, FALSE,
                                 error))
        return FALSE;

    if (out_config_path != NULL)
        *out_config_path = g_steal_pointer(&config_path);

    return TRUE;
}
