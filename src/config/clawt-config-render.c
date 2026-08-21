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
write_secret_file(ClawtConfig      *config,
                  ClawtAgentConfig *agent,
                  const gchar      *key,
                  const gchar      *dir,
                  const gchar      *filename,
                  const gchar      *json_member,
                  gchar           **out_path,
                  GError          **error)
{
    g_autoptr(ClawtSecretRef) ref = NULL;
    g_autofree gchar *value = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr(GError) local = NULL;
    guint timeout;

    *out_path = NULL;

    ref = clawt_agent_config_get_secret(agent, key);
    if (ref == NULL)
        return TRUE;

    timeout = (guint)clawt_config_get_int(config,
                                          "secrets.command_timeout_seconds");

    value = clawt_secret_ref_resolve(ref, NULL, timeout, &local);
    if (value == NULL) {
        g_autofree gchar *described = clawt_secret_ref_describe(ref);

        /*
         * Named by reference, never by value, and reported as a failure of
         * that one credential -- an agent whose Matrix token cannot be
         * fetched should say so rather than start silently deaf.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "%s: could not resolve %s: %s", key, described,
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
write_login_file(ClawtConfig      *config,
                 ClawtAgentConfig *agent,
                 const gchar      *username,
                 const gchar      *password_key,
                 const gchar      *dir,
                 const gchar      *filename,
                 gchar           **out_path,
                 GError          **error)
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

    ref = clawt_agent_config_get_secret(agent, password_key);
    if (ref == NULL || username == NULL)
        return TRUE;

    password = clawt_secret_ref_resolve(
        ref, NULL,
        (guint)clawt_config_get_int(config, "secrets.command_timeout_seconds"),
        &local);

    if (password == NULL) {
        g_autofree gchar *described = clawt_secret_ref_describe(ref);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "%s: could not resolve %s: %s", password_key, described,
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
}

static void
render_matrix_channel(GString          *out,
                      ClawtAgentConfig *agent,
                      const gchar      *token_path)
{
    g_auto(GStrv) rooms = NULL;

    if (!clawt_agent_config_get_boolean(agent, "integrations.matrix.enabled"))
        return;

    g_string_append(out, "  matrix:\n");
    append_key_bool(out, 4, "enabled", TRUE);
    append_key_value(out, 4, "homeserver",
                     clawt_agent_config_get_string(
                         agent, "integrations.matrix.homeserver"));
    append_key_value(out, 4, "user_id",
                     clawt_agent_config_get_string(
                         agent, "integrations.matrix.user_id"));
    append_key_value(out, 4, "access_token_file", token_path);

    rooms = clawt_agent_config_get_string_list(agent,
                                               "integrations.matrix.rooms");
    append_string_list(out, 4, "rooms", rooms);

    append_key_bool(out, 4, "require_mention",
                    clawt_agent_config_get_boolean(
                        agent, "integrations.matrix.require_mention"));
}

static void
render_email_channel(GString          *out,
                     ClawtAgentConfig *agent,
                     const gchar      *imap_path,
                     const gchar      *smtp_path)
{
    g_auto(GStrv) folders = NULL;

    if (!clawt_agent_config_get_boolean(agent, "integrations.email.enabled"))
        return;

    g_string_append(out, "  email:\n");
    append_key_bool(out, 4, "enabled", TRUE);
    append_key_value(out, 4, "imap_host",
                     clawt_agent_config_get_string(
                         agent, "integrations.email.imap_host"));
    append_key_int(out, 4, "imap_port",
                   clawt_agent_config_get_int(agent,
                                              "integrations.email.imap_port"));
    append_key_value(out, 4, "imap_credentials_file", imap_path);

    folders = clawt_agent_config_get_string_list(agent,
                                                 "integrations.email.folders");
    append_string_list(out, 4, "folders", folders);

    append_key_value(out, 4, "smtp_host",
                     clawt_agent_config_get_string(
                         agent, "integrations.email.smtp_host"));
    append_key_int(out, 4, "smtp_port",
                   clawt_agent_config_get_int(agent,
                                              "integrations.email.smtp_port"));
    append_key_value(out, 4, "smtp_credentials_file", smtp_path);
}

static void
render_webhook_channel(GString *out, ClawtAgentConfig *agent)
{
    if (!clawt_agent_config_get_boolean(agent, "integrations.webhook.enabled"))
        return;

    g_string_append(out, "  webhook:\n");
    append_key_bool(out, 4, "enabled", TRUE);
    append_key_int(out, 4, "listen_port",
                   clawt_agent_config_get_int(agent,
                                              "integrations.webhook.port"));
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

    identity_files = clawt_agent_config_get_string_list(
        agent, "persona.identity_files");
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
        g_autofree gchar *database = g_build_filename(state_dir,
                                                      "libreclaw.db", NULL);
        g_autofree gchar *skills = g_build_filename(state_dir, "skills", NULL);

        g_string_append(out, "session:\n");
        append_key_value(out, 2, "persist_dir", sessions);
        g_string_append(out, "\n");

        g_string_append(out, "database:\n");
        append_key_value(out, 2, "path", database);
        g_string_append(out, "\n");

        g_string_append(out, "skills:\n");
        append_key_value(out, 2, "dir", skills);
        g_string_append(out, "\n");
    }

    /* ── channels ── */
    g_string_append(out, "channels:\n");
    render_clawtilla_channel(out, agent, link_socket, state_dir);

    {
        g_autofree gchar *matrix_token =
            g_build_filename(state_dir, "credentials",
                             "matrix_credentials.json", NULL);
        g_autofree gchar *imap =
            g_build_filename(state_dir, "credentials",
                             "imap_credentials.json", NULL);
        g_autofree gchar *smtp =
            g_build_filename(state_dir, "credentials",
                             "smtp_credentials.json", NULL);

        render_matrix_channel(out, agent, matrix_token);
        render_email_channel(out, agent, imap, smtp);
    }

    render_webhook_channel(out, agent);

    /*
     * channels.local owns stdin and stdout.  Two agents in one process
     * would fight over the terminal, and an agent started by the daemon
     * has no terminal at all, so it is only ever rendered when asked for
     * explicitly.
     */
    if (clawt_agent_config_get_boolean(agent, "integrations.local")) {
        g_string_append(out, "  local:\n");
        append_key_bool(out, 4, "enabled", TRUE);
    }

    if (clawt_agent_config_get_boolean(agent, "integrations.cmacs")) {
        g_string_append(out, "  cmacs:\n");
        append_key_bool(out, 4, "enabled", TRUE);
    }

    g_string_append(out, "\n");

    /*
     * Passthrough last, so it wins.
     *
     * clawtilla does not model every libreclaw option and never will.
     * Copying the subtree across verbatim means a libreclaw setting
     * clawtilla has not heard of is still reachable, without waiting for
     * clawtilla to grow a schema entry for it.
     */
    passthrough = clawt_agent_config_get_raw_yaml(agent, "libreclaw");
    if (passthrough != NULL && *passthrough != '\0') {
        g_string_append(out,
            "# ── Passthrough from the agent's `libreclaw:` block ──\n");
        append_block(out, 0, passthrough);
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

    if (!clawt_ensure_dir(state_dir, 0700, error))
        return FALSE;

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

    if (!write_secret_file(config, agent, "integrations.matrix.access_token",
                           credentials_dir, "matrix_credentials.json",
                           "access_token", &matrix_token, error))
        return FALSE;

    if (!write_login_file(config, agent,
                          clawt_agent_config_get_string(
                              agent, "integrations.email.username"),
                          "integrations.email.password",
                          credentials_dir, "imap_credentials.json",
                          &imap_file, error))
        return FALSE;

    /*
     * The same login for both directions unless the config separates them.
     * libreclaw wants two files; making the user say the same password
     * twice would be a worse answer than writing it twice.
     */
    if (!write_login_file(config, agent,
                          clawt_agent_config_get_string(
                              agent, "integrations.email.username"),
                          "integrations.email.password",
                          credentials_dir, "smtp_credentials.json",
                          &smtp_file, error))
        return FALSE;

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
