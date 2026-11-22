/*
 * clawt-connector.c - The catalogue of services an agent can be given
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "integration/clawt-connector.h"

#include <yaml-glib.h>

/* ── The built-in entries ─────────────────────────────────────────── */

/*
 * Every entry here is somebody else's URL, and the scopes are somebody
 * else's vocabulary.  They are compiled in because the common case should
 * need no files and should be testable, and they are overridable because
 * being compiled in is not the same as being right for ever.
 *
 * Two things are deliberately absent.  There is no client id: clawtilla
 * has no OAuth application to lend and will not identify itself as
 * another project's.  And there is no `server_command` unless the
 * provider genuinely ships one under a name that is stable -- a guess at
 * an npx incantation ages faster than an endpoint does, and fails at the
 * moment an agent tries to work rather than at the moment somebody
 * connects.
 */

static const gchar *const github_server_args[] = { "stdio", NULL };

static const gchar *const venture_server_args[] = { "mcp", NULL };

/*
 * The nineteen tools `venturectl mcp` serves, so a `tools:` narrowing that
 * names one of them wrongly is caught when the plan is built rather
 * than by an agent finding its tool list empty.
 *
 * The names are verbs and there is deliberately no noun among them:
 * venture's surface is generated from its own record registry, so a
 * tool per record type would be a copy of somebody else's data model
 * and would go stale the first time a plugin registered one.
 */
static const gchar *const venture_tools[] = {
    "venture_schema", "venture_list", "venture_get", "venture_create",
    "venture_update", "venture_delete", "venture_reports", "venture_report",
    "venture_confirmations", "venture_modules", "venture_links",
    "venture_link", "venture_dashboards", "venture_dashboard",
    "venture_dashboard_build", "venture_factory", "venture_inbox",
    "venture_runs", "venture_desk", NULL
};

/*
 * One token is one actor in venture's audit trail, and the trail is the
 * point: it is what distinguishes a change an automation made from one
 * a person made from one the AI made.  Two agents sharing a token
 * collapse into a single name in every row they write, which is the
 * same failure two agents on one Matrix login have -- the fleet looks
 * like it is misbehaving and the file that caused it looks fine.
 *
 * Keyed on `token_file` rather than on the token, because the value
 * never appears in the configuration at all: `connector.begin` writes
 * it to a 0600 file and puts the *path* in `clawtilla.yaml`.  Two
 * agents naming one path are two agents holding one token, and the path
 * is the only spelling of that this check can see.
 */
static const gchar *const venture_identity[] = { "token_file", NULL };

static const ClawtConnectorInfo builtin[] = {
    { "github", "GitHub",
      "Repositories, issues and pull requests.", "Code forges",
      CLAWT_CONNECTOR_AUTH_DEVICE,
      "https://github.com/login/device/code",
      "https://github.com/login/oauth/access_token",
      NULL,
      "repo read:org read:user",
      "Settings -> Developer settings -> OAuth Apps, with device flow enabled",
      "https://docs.github.com/apps/oauth-apps",
      NULL,
      "github-mcp-server", github_server_args, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_ENV, "GITHUB_PERSONAL_ACCESS_TOKEN", NULL,
      NULL, NULL, NULL },

    { "gitlab", "GitLab",
      "Projects, issues and merge requests, on gitlab.com or your own.",
      "Code forges",
      CLAWT_CONNECTOR_AUTH_DEVICE,
      "/oauth/authorize_device",
      "/oauth/token",
      "/oauth/revoke",
      "api read_user",
      "User settings -> Applications, with the device flow ticked",
      "https://docs.gitlab.com/api/oauth2/",
      "https://gitlab.com",
      NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_ENV, "GITLAB_TOKEN", NULL,
      NULL, NULL, NULL },

    { "forgejo", "Forgejo",
      "Repositories on Codeberg, or on a Forgejo you run yourself.",
      "Code forges",
      CLAWT_CONNECTOR_AUTH_PKCE,
      "/login/oauth/authorize",
      "/login/oauth/access_token",
      NULL,
      "read:repository write:repository read:issue write:issue",
      "Settings -> Applications -> Manage OAuth2 Applications",
      "https://forgejo.org/docs/latest/user/oauth2-provider/",
      "https://codeberg.org",
      NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_ENV, "FORGEJO_TOKEN", NULL,
      NULL, NULL, NULL },

    { "google", "Google",
      "Gmail, Drive and Calendar.", "Productivity",
      CLAWT_CONNECTOR_AUTH_DEVICE,
      "https://oauth2.googleapis.com/device/code",
      "https://oauth2.googleapis.com/token",
      "https://oauth2.googleapis.com/revoke",
      "https://www.googleapis.com/auth/drive.readonly "
      "https://www.googleapis.com/auth/gmail.readonly",
      "Cloud console -> Credentials -> OAuth client ID, of type "
      "'TVs and Limited Input devices'",
      "https://developers.google.com/identity/protocols/oauth2/"
      "limited-input-device",
      NULL, NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_ENV, "GOOGLE_OAUTH_TOKEN", NULL,
      NULL, NULL, NULL },

    { "microsoft", "Microsoft 365",
      "Outlook mail, OneDrive and Teams.", "Productivity",
      CLAWT_CONNECTOR_AUTH_DEVICE,
      "https://login.microsoftonline.com/common/oauth2/v2.0/devicecode",
      "https://login.microsoftonline.com/common/oauth2/v2.0/token",
      NULL,
      "offline_access User.Read Mail.Read Files.Read",
      "Entra ID -> App registrations, with 'Allow public client flows' on",
      "https://learn.microsoft.com/entra/identity-platform/v2-oauth2-device-code",
      NULL, NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_ENV, "MICROSOFT_OAUTH_TOKEN", NULL,
      NULL, NULL, NULL },

    { "slack", "Slack",
      "Read and post in channels.", "Chat",
      CLAWT_CONNECTOR_AUTH_PKCE,
      "https://slack.com/oauth/v2/authorize",
      "https://slack.com/api/oauth.v2.access",
      NULL,
      "channels:read channels:history chat:write",
      "api.slack.com/apps -> your app -> OAuth & Permissions",
      "https://api.slack.com/authentication/oauth-v2",
      NULL, NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_ENV, "SLACK_BOT_TOKEN", NULL,
      NULL, NULL, NULL },

    { "notion", "Notion",
      "Pages and databases.", "Productivity",
      CLAWT_CONNECTOR_AUTH_PKCE,
      "https://api.notion.com/v1/oauth/authorize",
      "https://api.notion.com/v1/oauth/token",
      NULL,
      NULL,
      "notion.so/my-integrations, created as a public integration",
      "https://developers.notion.com/docs/authorization",
      NULL, NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_HEADER, "Authorization", "Bearer %s",
      NULL, NULL, NULL },

    { "linear", "Linear",
      "Issues and projects.", "Project tracking",
      CLAWT_CONNECTOR_AUTH_PKCE,
      "https://linear.app/oauth/authorize",
      "https://api.linear.app/oauth/token",
      "https://api.linear.app/oauth/revoke",
      "read write",
      "Linear -> Settings -> API -> OAuth applications",
      "https://developers.linear.app/docs/oauth/authentication",
      NULL, NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_HEADER, "Authorization", "Bearer %s",
      NULL, NULL, NULL },

    /*
     * The one that is ours.
     *
     * Compiled in rather than shipped as an overlay file because it is
     * the operator's own books: a fleet that runs venture should reach
     * it out of the box, and an entry somebody has to copy into
     * connectors.d before anything works is an entry most people never
     * find.
     *
     * `venturectl mcp` builds its tool surface from `GET
     * /api/v1/schema`, so the nine names below are the whole of it and
     * always will be -- a record type added by a plugin appears inside
     * `venture_list`'s type enumeration rather than as a tenth tool.
     * That is what makes @known_tools safe to write down here at all;
     * a catalogue naming venture's *record types* would be a copy of
     * somebody else's data model and would go stale on the first plugin.
     *
     * API_KEY rather than a flow: venture mints a bearer token from
     * `POST /api/v1/tokens` and returns the plaintext exactly once,
     * which is a key somebody already holds by the time they configure
     * this and not something clawtilla can obtain for them.
     */
    { "venture", "VENTURE",
      "Your own books: sales, expenses, invoices, tickets and contacts.",
      "Business",
      CLAWT_CONNECTOR_AUTH_API_KEY,
      NULL, NULL, NULL, NULL,
      "Sign in to the VENTURE web UI and create an API token; the "
      "plaintext is shown once",
      NULL,
      "http://localhost:8747",
      "venturectl", venture_server_args, NULL,
      "VENTURE_URL",
      CLAWT_CREDENTIAL_PLACEMENT_ENV, "VENTURE_TOKEN", NULL,
      venture_tools, venture_identity,
      "venture audits by actor, and two agents holding one token are one "
      "actor in that trail -- so which of them filed a change stops being "
      "answerable, and so does whether a person or a rule did" },

    /*
     * Image and video generation, for any agent the operator grants it.
     *
     * Compiled in because it is first-party in the sense that matters:
     * the office's asset steward is built to use it, and an entry
     * somebody has to discover in a registry is one the office's setup
     * cannot name.  It names the server and nothing else -- PKCE with no
     * endpoints is the discovered sign-in (RFC 9728, 8414, 7591; see
     * clawt-mcp-auth.h), so nothing is registered in a console and
     * Higgsfield can move its authorization server without a release
     * here.  The REST API (a key id and secret) is ai-glib's
     * AiHiggsfieldClient and belongs to the daemon, not to this entry.
     */
    { "higgsfield", "Higgsfield",
      "Image and video generation across Higgsfield's model catalogue; "
      "paid, every call spends credits.", "Media generation",
      CLAWT_CONNECTOR_AUTH_PKCE,
      NULL, NULL, NULL, NULL,
      "nothing to register: the server says how to sign in; a paid "
      "Higgsfield plan is required",
      "https://higgsfield.ai/mcp",
      NULL,
      NULL, NULL, CLAWT_HIGGSFIELD_MCP_URL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_HEADER, "Authorization", "Bearer %s",
      NULL, NULL, NULL },

    /*
     * The two that make the catalogue open rather than closed.
     *
     * Most MCP servers are not behind OAuth at all -- they want a key
     * from a dashboard in an environment variable, or a bearer token on
     * an HTTP endpoint.  Those are worth brokering for exactly the same
     * reason the OAuth ones are: the value stays out of the agent's
     * .mcp.json, out of its environment and out of the process table.
     * `credential_name` is overridden per instance, so one entry covers
     * every such service rather than one entry per service.
     */
    { "api-key", "Any API key",
      "A service with a key and an MCP server of its own.", "Generic",
      CLAWT_CONNECTOR_AUTH_API_KEY,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_ENV, "API_KEY", NULL,
      NULL, NULL, NULL },

    { "bearer", "Any bearer token",
      "An HTTP MCP server behind an Authorization header.", "Generic",
      CLAWT_CONNECTOR_AUTH_API_KEY,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_HEADER, "Authorization", "Bearer %s",
      NULL, NULL, NULL },

    /*
     * The integration brokers clawtilla can run (`broker:` in the config,
     * clawt-broker.h).  Each agent or team reaches its own gateway server
     * through a connector the config loader makes with one of these as
     * its provider, which is how the relay knows which gateway to name
     * when one refuses a call.  Usable by hand as well, for a gateway
     * somebody else runs: its URL and a key from its dashboard.
     */
    { "anythingmcp", "AnythingMCP",
      "An MCP gateway server: the app integrations assigned to it, behind "
      "one key.", "Integration broker",
      CLAWT_CONNECTOR_AUTH_API_KEY,
      NULL, NULL, NULL, NULL,
      "make an MCP API key on the server's page of the AnythingMCP "
      "dashboard; a key made there works for that server only",
      "https://github.com/HelpCode-ai/anythingmcp/blob/main/docs/"
      "api-reference.md",
      NULL,
      NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_HEADER, "Authorization", "Bearer %s",
      NULL, NULL, NULL },

    { "jentic", "Jentic One",
      "A credential broker: APIs called through it, checked against the "
      "agent's rules before a stored key is injected.", "Integration broker",
      CLAWT_CONNECTOR_AUTH_API_KEY,
      NULL, NULL, NULL, NULL,
      "the agent's API key, from its page in the Jentic One web app",
      "https://github.com/jentic/jentic-one/blob/main/docs/guides/"
      "mcp-http-endpoint.md",
      NULL,
      NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_HEADER, "Authorization", "Bearer %s",
      NULL, NULL, NULL },

    /*
     * Any remote MCP server, by its URL and nothing else.
     *
     * PKCE with no endpoints is what makes it work: the sign-in is asked
     * of the server (RFC 9728, RFC 8414, RFC 7591; see clawt-mcp-auth.h)
     * instead of copied into a catalogue file by hand.  A server that
     * turns out to want a key takes one pasted, like any connector.
     */
    { CLAWT_CONNECTOR_PROVIDER_REMOTE, "Remote MCP server",
      "Any MCP server reached by URL; it says how to sign in.", "Generic",
      CLAWT_CONNECTOR_AUTH_PKCE,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL,
      NULL,
      CLAWT_CREDENTIAL_PLACEMENT_HEADER, "Authorization", "Bearer %s",
      NULL, NULL, NULL }
};

const ClawtConnectorInfo *
clawt_connector_catalog_builtin(gsize *n_connectors)
{
    g_return_val_if_fail(n_connectors != NULL, NULL);

    *n_connectors = G_N_ELEMENTS(builtin);

    return builtin;
}

/* ── Entries on the heap ──────────────────────────────────────────── */

/*
 * A loaded catalogue mixes copies of the table above with entries read
 * from files, and the caller must not have to know which is which.  So
 * everything in it owns its strings and everything in it is freed the
 * same way.
 */

void
clawt_connector_info_free(ClawtConnectorInfo *info)
{
    if (info == NULL)
        return;

    g_free((gchar *)info->id);
    g_free((gchar *)info->name);
    g_free((gchar *)info->summary);
    g_free((gchar *)info->category);
    g_free((gchar *)info->auth_url);
    g_free((gchar *)info->token_url);
    g_free((gchar *)info->revoke_url);
    g_free((gchar *)info->scopes);
    g_free((gchar *)info->client_id_help);
    g_free((gchar *)info->docs_url);
    g_free((gchar *)info->default_instance);
    g_free((gchar *)info->server_command);
    g_strfreev((GStrv)info->server_args);
    g_free((gchar *)info->server_url);
    g_free((gchar *)info->instance_var);
    g_free((gchar *)info->credential_name);
    g_free((gchar *)info->credential_format);
    g_strfreev((GStrv)info->known_tools);
    g_strfreev((GStrv)info->identity_keys);
    g_free((gchar *)info->identity_note);

    g_free(info);
}

ClawtConnectorInfo *
clawt_connector_info_copy(const ClawtConnectorInfo *src)
{
    ClawtConnectorInfo *out;

    g_return_val_if_fail(src != NULL, NULL);

    out = g_new0(ClawtConnectorInfo, 1);

    out->id = g_strdup(src->id);
    out->name = g_strdup(src->name);
    out->summary = g_strdup(src->summary);
    out->category = g_strdup(src->category);
    out->auth = src->auth;
    out->auth_url = g_strdup(src->auth_url);
    out->token_url = g_strdup(src->token_url);
    out->revoke_url = g_strdup(src->revoke_url);
    out->scopes = g_strdup(src->scopes);
    out->client_id_help = g_strdup(src->client_id_help);
    out->docs_url = g_strdup(src->docs_url);
    out->default_instance = g_strdup(src->default_instance);
    out->server_command = g_strdup(src->server_command);
    out->server_args = (const gchar *const *)
        g_strdupv((GStrv)src->server_args);
    out->server_url = g_strdup(src->server_url);
    out->instance_var = g_strdup(src->instance_var);
    out->placement = src->placement;
    out->credential_name = g_strdup(src->credential_name);
    out->credential_format = g_strdup(src->credential_format);
    out->known_tools = (const gchar *const *)
        g_strdupv((GStrv)src->known_tools);
    out->identity_keys = (const gchar *const *)
        g_strdupv((GStrv)src->identity_keys);
    out->identity_note = g_strdup(src->identity_note);

    return out;
}

/* ── Reading an overlay file ──────────────────────────────────────── */

static const gchar *
member_string(YamlMapping *mapping, const gchar *key)
{
    YamlNode *node;

    if (mapping == NULL)
        return NULL;

    node = yaml_mapping_get_member(mapping, key);

    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
        return NULL;

    return yaml_node_get_string(node);
}

static GStrv
member_strv(YamlMapping *mapping, const gchar *key)
{
    YamlNode *node;
    YamlSequence *sequence;
    GPtrArray *out;
    guint i;
    guint length;

    if (mapping == NULL)
        return NULL;

    node = yaml_mapping_get_member(mapping, key);

    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE)
        return NULL;

    sequence = yaml_node_get_sequence(node);
    length = yaml_sequence_get_length(sequence);
    out = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);

        if (element == NULL ||
            yaml_node_get_node_type(element) != YAML_NODE_SCALAR)
            continue;

        g_ptr_array_add(out, g_strdup(yaml_node_get_string(element)));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}

/*
 * `credential_format` reaches g_strdup_printf(), and it arrives from a
 * file somebody edited.  A format string is a small programming language
 * and printf trusts it completely: `%d` against a pointer argument reads
 * whatever is in that register, and `%n` writes.  So the only formats
 * accepted are the ones that could have been meant -- exactly one %s,
 * literal percent signs doubled, nothing else.
 *
 * The failure this prevents is not a hostile catalogue file so much as a
 * typo in one, which would otherwise be a crash a long way from the file
 * that caused it.
 */
static gboolean
credential_format_is_safe(const gchar *format)
{
    const gchar *p;
    guint conversions = 0;

    if (format == NULL)
        return TRUE;

    for (p = format; *p != '\0'; p++) {
        if (*p != '%')
            continue;

        p++;

        if (*p == '%')
            continue;

        if (*p != 's')
            return FALSE;

        conversions++;
    }

    return conversions == 1;
}

static gboolean
read_enum(YamlMapping *mapping, const gchar *key, GType type, gint *out)
{
    const gchar *nick = member_string(mapping, key);

    if (nick == NULL)
        return FALSE;

    return clawt_enum_from_nick(type, nick, out);
}

/*
 * One entry from a file.  Returns NULL with @error set for the two
 * things that make an entry unusable rather than merely incomplete: no
 * id to key it by, and a format string that must not be handed to printf.
 */
static ClawtConnectorInfo *
connector_from_node(YamlNode *node, GError **error)
{
    YamlMapping *mapping;
    ClawtConnectorInfo *out;
    const gchar *id;
    const gchar *format;
    const gchar *auth_nick;
    gint value = 0;

    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "a connector must be a mapping");
        return NULL;
    }

    mapping = yaml_node_get_mapping(node);
    id = member_string(mapping, "id");

    if (id == NULL || *id == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "a connector needs an id");
        return NULL;
    }

    format = member_string(mapping, "credential_format");

    if (!credential_format_is_safe(format)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "connector '%s': credential_format must contain exactly "
                    "one %%s and no other conversion", id);
        return NULL;
    }

    /*
     * An absent `auth:` defaults to CLAWT_CONNECTOR_AUTH_NONE, which is a
     * legitimate value -- plenty of self-hosted servers want no
     * credential at all.  A *present* one that fails to parse is a
     * different thing: silently falling back to NONE would tell an
     * operator their typo'd "device" connector needs no authorization,
     * which is a worse failure than the typo itself.
     */
    auth_nick = member_string(mapping, "auth");

    if (auth_nick != NULL &&
        !clawt_enum_from_nick(CLAWT_TYPE_CONNECTOR_AUTH, auth_nick, &value)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "connector '%s': '%s' is not a known auth kind",
                    id, auth_nick);
        return NULL;
    }

    out = g_new0(ClawtConnectorInfo, 1);

    out->id = g_strdup(id);
    out->name = g_strdup(member_string(mapping, "name"));
    out->summary = g_strdup(member_string(mapping, "summary"));
    out->category = g_strdup(member_string(mapping, "category"));
    out->auth_url = g_strdup(member_string(mapping, "auth_url"));
    out->token_url = g_strdup(member_string(mapping, "token_url"));
    out->revoke_url = g_strdup(member_string(mapping, "revoke_url"));
    out->scopes = g_strdup(member_string(mapping, "scopes"));
    out->client_id_help = g_strdup(member_string(mapping, "client_id_help"));
    out->docs_url = g_strdup(member_string(mapping, "docs_url"));
    out->default_instance =
        g_strdup(member_string(mapping, "default_instance"));
    out->server_command = g_strdup(member_string(mapping, "server_command"));
    out->server_args = (const gchar *const *)member_strv(mapping,
                                                         "server_args");
    out->server_url = g_strdup(member_string(mapping, "server_url"));
    out->instance_var = g_strdup(member_string(mapping, "instance_var"));
    out->credential_name = g_strdup(member_string(mapping, "credential_name"));
    out->credential_format = g_strdup(format);
    out->known_tools = (const gchar *const *)member_strv(mapping,
                                                         "known_tools");
    out->identity_keys = (const gchar *const *)member_strv(mapping,
                                                           "identity_keys");
    out->identity_note = g_strdup(member_string(mapping, "identity_note"));

    if (out->name == NULL)
        out->name = g_strdup(id);

    if (out->category == NULL)
        out->category = g_strdup("Added here");

    /* Already validated above; @value carries whatever it parsed to, or
     * its zero-initialised CLAWT_CONNECTOR_AUTH_NONE when auth_nick was
     * absent. */
    out->auth = (ClawtConnectorAuth)value;

    value = 0;

    if (read_enum(mapping, "placement", CLAWT_TYPE_CREDENTIAL_PLACEMENT,
                  &value))
        out->placement = (ClawtCredentialPlacement)value;

    return out;
}

/*
 * Replaces an entry with the same id, rather than merging into it.
 *
 * Half of an override is worse than none: a new token endpoint against
 * an old authorization endpoint is a combination nobody wrote down, and
 * it fails pointing at neither file.
 */
static void
catalog_replace(GPtrArray *catalog, ClawtConnectorInfo *entry)
{
    guint i;

    for (i = 0; i < catalog->len; i++) {
        ClawtConnectorInfo *existing = g_ptr_array_index(catalog, i);

        if (g_strcmp0(existing->id, entry->id) == 0) {
            g_ptr_array_remove_index(catalog, i);
            g_ptr_array_insert(catalog, (gint)i, entry);
            return;
        }
    }

    g_ptr_array_add(catalog, entry);
}

static void
load_overlay_file(GPtrArray *catalog, const gchar *path)
{
    g_autoptr(YamlParser) parser = yaml_parser_new();
    g_autoptr(GError) error = NULL;
    YamlNode *root;
    YamlNode *list;
    YamlSequence *sequence;
    guint i;
    guint length;

    if (!yaml_parser_load_from_file(parser, path, &error)) {
        g_warning("connector file %s: %s", path, error->message);
        return;
    }

    root = yaml_parser_get_root(parser);

    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
        g_warning("connector file %s: expected a mapping at the top level",
                  path);
        return;
    }

    list = yaml_mapping_get_member(yaml_node_get_mapping(root), "connectors");

    if (list == NULL || yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE) {
        g_warning("connector file %s: expected a `connectors:` list", path);
        return;
    }

    sequence = yaml_node_get_sequence(list);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        g_autoptr(GError) entry_error = NULL;
        ClawtConnectorInfo *entry =
            connector_from_node(yaml_sequence_get_element(sequence, i),
                                &entry_error);

        if (entry == NULL) {
            g_warning("connector file %s: %s", path, entry_error->message);
            continue;
        }

        catalog_replace(catalog, entry);
    }
}

static gint
compare_entries(gconstpointer a, gconstpointer b)
{
    const ClawtConnectorInfo *left = *(const ClawtConnectorInfo **)a;
    const ClawtConnectorInfo *right = *(const ClawtConnectorInfo **)b;
    gint by_category;

    /*
     * The entry that needs nothing but a URL goes first, so a list that
     * opens on its first entry opens on the one most adds want -- in
     * every client, without any of them knowing its id.
     */
    if (g_strcmp0(left->id, CLAWT_CONNECTOR_PROVIDER_REMOTE) == 0 ||
        g_strcmp0(right->id, CLAWT_CONNECTOR_PROVIDER_REMOTE) == 0)
        return (g_strcmp0(right->id, CLAWT_CONNECTOR_PROVIDER_REMOTE) == 0) -
               (g_strcmp0(left->id, CLAWT_CONNECTOR_PROVIDER_REMOTE) == 0);

    by_category = g_strcmp0(left->category, right->category);

    if (by_category != 0)
        return by_category;

    return g_strcmp0(left->name, right->name);
}

GPtrArray *
clawt_connector_catalog_load(const gchar *overlay_dir, GError **error)
{
    GPtrArray *catalog =
        g_ptr_array_new_with_free_func((GDestroyNotify)
                                       clawt_connector_info_free);
    g_autoptr(GDir) dir = NULL;
    g_autoptr(GPtrArray) files = NULL;
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(builtin); i++)
        g_ptr_array_add(catalog, clawt_connector_info_copy(&builtin[i]));

    if (overlay_dir == NULL)
        goto done;

    dir = g_dir_open(overlay_dir, 0, NULL);

    if (dir == NULL)
        goto done;

    /*
     * Sorted, so that two files declaring the same id resolve the same
     * way on every machine.  Readdir order is a filesystem's business
     * and differs between one that was copied and one that grew.
     */
    files = g_ptr_array_new_with_free_func(g_free);

    for (;;) {
        const gchar *name = g_dir_read_name(dir);

        if (name == NULL)
            break;

        if (!g_str_has_suffix(name, ".yaml") && !g_str_has_suffix(name, ".yml"))
            continue;

        g_ptr_array_add(files, g_build_filename(overlay_dir, name, NULL));
    }

    g_ptr_array_sort_values(files, (GCompareFunc)g_strcmp0);

    for (i = 0; i < files->len; i++)
        load_overlay_file(catalog, g_ptr_array_index(files, i));

done:
    g_ptr_array_sort(catalog, compare_entries);

    return catalog;
}

const ClawtConnectorInfo *
clawt_connector_catalog_find(GPtrArray *catalog, const gchar *id)
{
    guint i;

    g_return_val_if_fail(catalog != NULL, NULL);

    if (id == NULL)
        return NULL;

    for (i = 0; i < catalog->len; i++) {
        const ClawtConnectorInfo *info = g_ptr_array_index(catalog, i);

        if (g_strcmp0(info->id, id) == 0)
            return info;
    }

    return NULL;
}

gchar *
clawt_connector_token_dir(const gchar *secrets_dir, const gchar *name)
{
    g_autofree gchar *folded = NULL;

    g_return_val_if_fail(secrets_dir != NULL, NULL);
    g_return_val_if_fail(name != NULL, NULL);

    /*
     * An instance name reaches this from a config file, so it may hold a
     * slash -- which would put the credential somewhere other than the
     * secrets directory, possibly on top of something else.  Anything
     * that is not plainly a filename is folded to an underscore.
     *
     * And, because this is a directory a confined agent's box binds,
     * a name that is empty or starts with a dot is folded too: `..`
     * would make the directory the whole of secrets.dir, `` the whole
     * of connectors/, and either bound into a box is every connector's
     * credential handed to one agent.
     */
    folded = (*name == '\0') ? g_strdup("_") : g_strdup(name);
    g_strdelimit(folded, "/\\ \t", '_');

    if (folded[0] == '.')
        folded[0] = '_';

    return g_build_filename(secrets_dir, "connectors", folded, NULL);
}

gchar *
clawt_connector_token_path(const gchar *secrets_dir, const gchar *name)
{
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(secrets_dir != NULL, NULL);
    g_return_val_if_fail(name != NULL, NULL);

    /*
     * One file in a directory of its own, never a file beside every
     * other connector's.  A confined agent's box binds the directory:
     * a bind of the file itself is a bind of the inode it was when the
     * box started, and a renewal writes a new file and renames it into
     * place -- so the box went on reading the token it started with,
     * and every relay started after the first renewal sent an expired
     * one.
     */
    dir = clawt_connector_token_dir(secrets_dir, name);

    return g_build_filename(dir, "token.json", NULL);
}

gchar *
clawt_connector_legacy_token_path(const gchar *secrets_dir, const gchar *name)
{
    g_autofree gchar *file = NULL;

    g_return_val_if_fail(secrets_dir != NULL, NULL);
    g_return_val_if_fail(name != NULL, NULL);

    file = g_strdup_printf("connector-%s.json", name);
    g_strdelimit(file, "/\\ \t", '_');

    return g_build_filename(secrets_dir, file, NULL);
}

gchar *
clawt_connector_find_token_file(const gchar *secrets_dir,
                                const gchar *name,
                                const gchar *configured)
{
    g_autofree gchar *current = NULL;
    g_autofree gchar *legacy = NULL;

    g_return_val_if_fail(secrets_dir != NULL, NULL);
    g_return_val_if_fail(name != NULL, NULL);

    current = clawt_connector_token_path(secrets_dir, name);
    legacy = clawt_connector_legacy_token_path(secrets_dir, name);

    /*
     * A path somebody chose -- a key file written by hand, a broker's
     * key -- is theirs, and is used as written.
     */
    if (configured != NULL && *configured != '\0' &&
        g_strcmp0(configured, current) != 0 &&
        g_strcmp0(configured, legacy) != 0)
        return g_strdup(configured);

    /*
     * Otherwise the daemon's own file, which may be newer than the
     * configuration that names it: a confined relay reads the
     * clawtilla.yaml its box was started with, and a connector moved
     * out of the flat layout -- or connected for the first time -- since
     * then is only to be found where the daemon now keeps it.
     */
    if (g_file_test(current, G_FILE_TEST_IS_REGULAR))
        return g_steal_pointer(&current);

    if (configured != NULL && *configured != '\0')
        return g_strdup(configured);

    return NULL;
}

gchar *
clawt_connector_default_overlay_dir(void)
{
    return g_build_filename(g_get_user_config_dir(), "clawtilla",
                            "connectors.d", NULL);
}

gchar *
clawt_connector_resolve_url(const ClawtConnectorInfo *info,
                            const gchar              *endpoint,
                            const gchar              *instance)
{
    const gchar *base;
    g_autofree gchar *trimmed = NULL;

    g_return_val_if_fail(info != NULL, NULL);

    if (endpoint == NULL)
        return NULL;

    /* Not self-hostable: the endpoint is already the whole answer. */
    if (info->default_instance == NULL)
        return g_strdup(endpoint);

    /*
     * A configured instance that already carries a scheme is taken as
     * written.  One that does not gets https, because the alternative is
     * dialling a plaintext host for an OAuth exchange, and a person who
     * typed `git.example.com` did not mean to.
     */
    base = (instance != NULL && *instance != '\0') ? instance
                                                   : info->default_instance;

    trimmed = g_strdup(base);
    g_strstrip(trimmed);

    while (g_str_has_suffix(trimmed, "/"))
        trimmed[strlen(trimmed) - 1] = '\0';

    if (!g_str_has_prefix(trimmed, "http://") &&
        !g_str_has_prefix(trimmed, "https://")) {
        g_autofree gchar *bare = g_steal_pointer(&trimmed);

        trimmed = g_strconcat("https://", bare, NULL);
    }

    if (g_str_has_prefix(endpoint, "http://") ||
        g_str_has_prefix(endpoint, "https://"))
        return g_strdup(endpoint);

    return g_strconcat(trimmed, endpoint[0] == '/' ? "" : "/", endpoint, NULL);
}

gchar *
clawt_connector_format_credential(const ClawtConnectorInfo *info,
                                  const gchar              *value)
{
    GString *out;
    const gchar *p;

    g_return_val_if_fail(info != NULL, NULL);
    g_return_val_if_fail(value != NULL, NULL);

    if (info->credential_format == NULL)
        return g_strdup(value);

    /*
     * Checked again here rather than trusted from load time, because an
     * entry can also come from the built-in table above.
     */
    if (!credential_format_is_safe(info->credential_format)) {
        g_warning("connector '%s': ignoring an unusable credential_format",
                  info->id);
        return g_strdup(value);
    }

    /*
     * Expanded by hand rather than by printf.
     *
     * Validating a format string and then handing it to printf anyway
     * leaves the whole of printf's behaviour standing behind a check
     * that has to be perfect for ever.  Substituting the one placeholder
     * here means a format string read from a file is never a format
     * string to anything -- it is text with a marker in it, and the
     * worst a wrong one can now do is produce a credential the service
     * rejects.
     */
    out = g_string_new(NULL);

    for (p = info->credential_format; *p != '\0'; p++) {
        if (*p != '%') {
            g_string_append_c(out, *p);
            continue;
        }

        p++;

        if (*p == '%')
            g_string_append_c(out, '%');
        else
            g_string_append(out, value);
    }

    return g_string_free(out, FALSE);
}

ClawtConnectorState
clawt_connector_state(gboolean connected, gint64 expires_at,
                      gboolean renewable, gint64 now)
{
    if (!connected)
        return CLAWT_CONNECTOR_UNAUTHORISED;

    if (now <= 0)
        now = g_get_real_time() / G_USEC_PER_SEC;

    /*
     * A token with no expiry does not expire.  Zero is the daemon's
     * spelling of "it did not say", and reading it as an instant in 1970
     * would report every such credential as long dead.
     */
    if (expires_at <= 0)
        return CLAWT_CONNECTOR_READY;

    /*
     * And one that can renew itself is working, whatever its expiry
     * says: the refresh happens on the next call.
     */
    if (renewable)
        return CLAWT_CONNECTOR_READY;

    return (expires_at <= now) ? CLAWT_CONNECTOR_EXPIRED
                               : CLAWT_CONNECTOR_READY;
}

const gchar *
clawt_connector_state_label(ClawtConnectorState state)
{
    switch (state) {
    case CLAWT_CONNECTOR_UNAUTHORISED:
        return "not authorised";

    case CLAWT_CONNECTOR_READY:
        return "authorised";

    case CLAWT_CONNECTOR_EXPIRED:
        return "expired -- authorise it again";
    }

    return "not authorised";
}

const gchar *
clawt_connector_state_tone(ClawtConnectorState state)
{
    switch (state) {
    case CLAWT_CONNECTOR_UNAUTHORISED:
        return "neutral";

    case CLAWT_CONNECTOR_READY:
        return "good";

    case CLAWT_CONNECTOR_EXPIRED:
        return "warn";
    }

    return "neutral";
}

gchar *
clawt_connector_name_for_url(const gchar *url)
{
    g_autoptr(GUri) uri = NULL;
    g_auto(GStrv) labels = NULL;
    g_autoptr(GInetAddress) literal = NULL;
    const gchar *host;
    const gchar *label;
    const gchar *p;
    GString *out;
    guint count;

    if (url == NULL)
        return NULL;

    uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);

    if (uri == NULL || (host = g_uri_get_host(uri)) == NULL || *host == '\0')
        return NULL;

    /* An address says nothing about the service behind it. */
    literal = g_inet_address_new_from_string(host);

    if (literal != NULL)
        return g_strdup(CLAWT_CONNECTOR_PROVIDER_REMOTE);

    /*
     * The label before the top-level domain is the one that names the
     * service -- `mcp.notion.com` is notion, `api.linear.app` is linear
     * -- and a single-label host such as `localhost` is itself.
     */
    labels = g_strsplit(host, ".", -1);
    count = g_strv_length(labels);
    label = (count >= 2) ? labels[count - 2] : labels[0];

    out = g_string_new(NULL);

    for (p = label; *p != '\0'; p++)
        g_string_append_c(out, g_ascii_isalnum(*p) ? g_ascii_tolower(*p)
                                                   : '-');

    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }

    return g_string_free(out, FALSE);
}

gchar *
clawt_connector_row_subtitle(const gchar *provider, const gchar *account)
{
    if (account != NULL && *account != '\0')
        return g_strdup_printf("%s -- %s", provider != NULL ? provider : "",
                               account);

    return g_strdup(provider != NULL ? provider : "");
}

gchar *
clawt_connector_sign_in_label(const gchar *auth_server)
{
    g_autoptr(GUri) uri = NULL;
    const gchar *host;

    if (auth_server == NULL || *auth_server == '\0')
        return g_strdup("Sign in");

    uri = g_uri_parse(auth_server, G_URI_FLAGS_NONE, NULL);
    host = (uri != NULL) ? g_uri_get_host(uri) : NULL;

    return g_strdup_printf("Sign in at %s",
                           (host != NULL && *host != '\0') ? host
                                                           : auth_server);
}
