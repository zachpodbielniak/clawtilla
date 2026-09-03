/*
 * clawt-integration-field.c - What to ask somebody adding an integration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Both clients rendered configuration keys straight at people.  The web
 * editor labelled its inputs `imap_host`, `smtp_port`, `access_token`
 * and `user_id`, with the schema's whole documentation paragraph as the
 * placeholder; the GTK notify editor drew every backend's fields at
 * once, so choosing "Desktop notification" still asked for a Matrix
 * homeserver and a room.
 *
 * Neither is a mistake in any one place.  They are what a form looks
 * like when it is generated from a data model instead of written for the
 * person filling it in, and the fix is to write down the missing half:
 * what each thing is called, what it is for, what a real one looks like,
 * and when it applies at all.
 */

#include "clawt-integration-field.h"

#include <string.h>

/* ── Choices ─────────────────────────────────────────────────────── */

static const gchar *const notify_backends[] = {
    "desktop", "ntfy", "gotify", "matrix", "command", NULL
};

static const gchar *const notify_backend_labels[] = {
    "Desktop notification", "ntfy", "Gotify", "A Matrix room",
    "Run a command", NULL
};

static const gchar *const notify_priorities[] = {
    "low", "normal", "high", "urgent", NULL
};

static const gchar *const notify_priority_labels[] = {
    "Low", "Normal", "High", "Urgent", NULL
};

static const gchar *const notify_events[] = {
    "question", "error", "done", "routine", "update", NULL
};

static const gchar *const notify_event_labels[] = {
    "An agent is blocked on you",
    "An agent broke",
    "A task finished",
    "A routine failed to run",
    "A newer clawtilla exists",
    NULL
};

/* ── The fields ──────────────────────────────────────────────────── */

/*
 * Ordered as somebody fills them in, not as the schema declares them.
 *
 * The schema's order is the generated config file's order and is
 * load-bearing there for a different reason -- a section inserted
 * mid-way reopens the previous one.  Here the only thing that matters is
 * that the question you can answer comes before the one that depends on
 * it, which is why `backend` is first among notify's.
 */
static const ClawtIntegrationField fields[] = {

    /* ── matrix ──────────────────────────────────────────────────── */

    { "matrix", "homeserver", "Homeserver",
      "The server the bot's account lives on.",
      "https://matrix.example.org",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, TRUE },

    { "matrix", "user_id", "User ID",
      "The bot's own account, not yours -- two agents on one login "
      "answer as the same person.",
      "@bot:example.org",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, TRUE },

    { "matrix", "access_token", "Access token",
      "A reference -- env:NAME, file:PATH or command:... -- never the "
      "token itself. Sign in below and clawtilla mints one and writes "
      "the path here for you.",
      "file:~/.clawtilla/secrets/matrix.token",
      CLAWT_FIELD_SECRET, NULL, NULL, NULL, NULL, TRUE },

    { "matrix", "rooms", "Rooms",
      "Which rooms it listens in. Empty means every room it has been "
      "invited to.",
      "!abc:example.org, !def:example.org",
      CLAWT_FIELD_LIST, NULL, NULL, NULL, NULL, FALSE },

    { "matrix", "require_mention", "Only when mentioned",
      "Otherwise it answers everything said in those rooms.",
      NULL,
      CLAWT_FIELD_BOOLEAN, NULL, NULL, NULL, NULL, FALSE },

    /* ── email ───────────────────────────────────────────────────── */

    { "email", "username", "Mailbox",
      "The full address it signs in as, and replies from.",
      "agent@example.org",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, TRUE },

    { "email", "password", "Password",
      "A reference -- env:NAME, file:PATH or command:... -- never the "
      "password itself.",
      "env:AGENT_MAIL_PASSWORD",
      CLAWT_FIELD_SECRET, NULL, NULL, NULL, NULL, TRUE },

    { "email", "imap_host", "IMAP server",
      "Where it reads mail from.",
      "imap.example.org",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, TRUE },

    { "email", "imap_port", "IMAP port",
      "993 for TLS, which is almost always the answer.",
      "993",
      CLAWT_FIELD_INT, NULL, NULL, NULL, NULL, FALSE },

    { "email", "smtp_host", "SMTP server",
      "Where it sends replies through.",
      "smtp.example.org",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, TRUE },

    { "email", "smtp_port", "SMTP port",
      "587 for STARTTLS, 465 for TLS.",
      "587",
      CLAWT_FIELD_INT, NULL, NULL, NULL, NULL, FALSE },

    { "email", "folders", "Folders",
      "Which to watch. Empty means INBOX alone.",
      "INBOX, Projects",
      CLAWT_FIELD_LIST, NULL, NULL, NULL, NULL, FALSE },

    /* ── webhook ─────────────────────────────────────────────────── */

    { "webhook", "port", "Port",
      "Must differ per agent -- two cannot bind the same one, and the "
      "second to start simply fails.",
      "8790",
      CLAWT_FIELD_INT, NULL, NULL, NULL, NULL, TRUE },

    /*
     * No bind address here on purpose.  `bind_address` exists as
     * agents.integrations.webhook.bind_address -- an inline, per-agent
     * key -- and there is no `integrations.bind_address`, so a named
     * instance has nowhere to put one and the daemon would drop it:
     * clawt_daemon_apply_integration_fields() only applies members that
     * are `integrations.*` leaves.  Offering the control anyway would be
     * a field somebody fills in, a save that reports success, and a
     * value read from nowhere.
     */

    /* ── mcp ─────────────────────────────────────────────────────── */

    { "mcp", "command", "Command",
      "The server's binary. Use this or a URL, never both.",
      "npx",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, FALSE },

    { "mcp", "args", "Arguments",
      "One per comma. Only used with a command.",
      "-y, @modelcontextprotocol/server-filesystem, /srv/shared",
      CLAWT_FIELD_LIST, NULL, NULL, NULL, NULL, FALSE },

    { "mcp", "url", "URL",
      "For a server already running somewhere. Use this or a command, "
      "never both.",
      "https://mcp.example.org/sse",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, FALSE },

    { "mcp", "tools", "Tools",
      "Which of the server's tools agents may call. Empty means all of "
      "them.",
      "read_file, list_directory",
      CLAWT_FIELD_LIST, NULL, NULL, NULL, NULL, FALSE },

    /* ── connector ───────────────────────────────────────────────── */

    { "connector", "provider", "Provider",
      "Which catalogue entry this is an account with.",
      "github",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, TRUE },

    { "connector", "instance", "Instance",
      "For a self-hosted service, its own address. Every URL the "
      "connector uses is a path onto this.",
      "https://git.example.org",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, FALSE },

    { "connector", "account", "Account",
      "Which account, when you have more than one with this provider.",
      "work",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, FALSE },

    { "connector", "scopes", "Scopes",
      "What to ask the provider for. Leave empty for the catalogue's "
      "own list.",
      "repo, read:org",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, FALSE },

    { "connector", "tools", "Tools",
      "Which of the connector's tools agents may call. Empty means all "
      "of them.",
      "list_issues, create_issue",
      CLAWT_FIELD_LIST, NULL, NULL, NULL, NULL, FALSE },

    /* ── notify ──────────────────────────────────────────────────── */

    /*
     * First, because everything after it depends on the answer.  This is
     * the field whose absence made the old form ask a desktop
     * notification for a Matrix homeserver.
     */
    { "notify", "backend", "How it reaches you",
      NULL, NULL,
      CLAWT_FIELD_CHOICE, notify_backends, notify_backend_labels,
      NULL, NULL, TRUE },

    { "notify", "url", "Topic URL",
      "The ntfy topic to publish to. Anyone who knows it can read your "
      "notifications, so pick something nobody will guess.",
      "https://ntfy.sh/clawtilla-7f3a91",
      CLAWT_FIELD_TEXT, NULL, NULL, "backend", "ntfy", TRUE },

    { "notify", "url", "Server URL",
      "Your Gotify server.",
      "https://gotify.example.org",
      CLAWT_FIELD_TEXT, NULL, NULL, "backend", "gotify", TRUE },

    { "notify", "token", "Token",
      "A reference -- env:NAME, file:PATH or command:... -- never the "
      "token itself. Leave empty for an unprotected ntfy topic.",
      "env:NTFY_TOKEN",
      CLAWT_FIELD_SECRET, NULL, NULL, "backend", "ntfy,gotify", FALSE },

    { "notify", "homeserver", "Homeserver",
      "The account the notice is sent from.",
      "https://matrix.example.org",
      CLAWT_FIELD_TEXT, NULL, NULL, "backend", "matrix", TRUE },

    { "notify", "room", "Room",
      "A room with nobody else in it works well.",
      "!alerts:example.org",
      CLAWT_FIELD_TEXT, NULL, NULL, "backend", "matrix", TRUE },

    { "notify", "command", "Command",
      "Run when there is something to say.",
      "/usr/bin/notify-send",
      CLAWT_FIELD_TEXT, NULL, NULL, "backend", "command", TRUE },

    { "notify", "args", "Arguments",
      "One per comma. {{title}} and {{body}} are replaced; with neither, "
      "the two are appended as the last arguments.",
      "-u, critical, {{title}}, {{body}}",
      CLAWT_FIELD_LIST, NULL, NULL, "backend", "command", FALSE },

    { "notify", "events", "Tell me about",
      NULL, NULL,
      CLAWT_FIELD_FLAGS, notify_events, notify_event_labels,
      NULL, NULL, FALSE },

    { "notify", "priority", "Priority",
      NULL, NULL,
      CLAWT_FIELD_CHOICE, notify_priorities, notify_priority_labels,
      NULL, NULL, FALSE },

    { "notify", "title", "Say it is from",
      "Worth setting when more than one fleet notifies the same phone.",
      "workstation",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, FALSE },

    { "notify", "quiet_hours", "Quiet hours",
      "Silences this notifier completely, wrapping midnight if you ask "
      "it to. To still be woken for a broken agent, make a second "
      "notifier without this.",
      "23:00-07:00",
      CLAWT_FIELD_TEXT, NULL, NULL, NULL, NULL, FALSE }
};

/* ── Type names ──────────────────────────────────────────────────── */

/*
 * The ids are lowercase config values and both clients showed them as
 * headings and as the rows of a picker.  "mcp" and "notify" are not
 * words, and "local" and "cmacs" say nothing at all to somebody who has
 * not read the source.
 */
static const struct {
    const gchar *type;
    const gchar *label;
} type_labels[] = {
    { "matrix",    "Matrix" },
    { "email",     "Email" },
    { "webhook",   "Incoming webhook" },
    { "local",     "This terminal" },
    { "cmacs",     "Emacs" },
    { "mcp",       "MCP server" },
    { "connector", "Connected account" },
    { "notify",    "Notifications" }
};

const gchar *
clawt_integration_type_label(const gchar *type)
{
    gsize i;

    if (type == NULL)
        return "";

    for (i = 0; i < G_N_ELEMENTS(type_labels); i++) {
        if (g_strcmp0(type_labels[i].type, type) == 0)
            return type_labels[i].label;
    }

    /*
     * A type this build does not know is a plugin's, and its own id is a
     * better answer than a blank -- a picker with an empty row in it is
     * worse than one with an unfamiliar word.
     */
    return type;
}

/* ── Lookup ──────────────────────────────────────────────────────── */

const ClawtIntegrationField *
clawt_integration_fields(const gchar *type, gsize *n_fields)
{
    gsize first = 0;
    gsize count = 0;
    gsize i;

    g_return_val_if_fail(n_fields != NULL, NULL);

    *n_fields = 0;

    if (type == NULL)
        return fields;

    for (i = 0; i < G_N_ELEMENTS(fields); i++) {
        if (g_strcmp0(fields[i].type, type) != 0)
            continue;

        if (count == 0)
            first = i;

        count++;
    }

    *n_fields = count;

    /*
     * The table is grouped by type, so a run is contiguous -- checked by
     * the test rather than assumed, because a field added in the wrong
     * place would silently truncate its type's form here.
     */
    return fields + first;
}

gboolean
clawt_integration_field_applies(const ClawtIntegrationField *field,
                                const gchar                 *when_value)
{
    g_auto(GStrv) wanted = NULL;
    guint i;

    g_return_val_if_fail(field != NULL, FALSE);

    if (field->when_key == NULL)
        return TRUE;

    if (when_value == NULL)
        return FALSE;

    wanted = g_strsplit(field->when_value, ",", -1);

    for (i = 0; wanted[i] != NULL; i++) {
        if (g_strcmp0(g_strstrip(wanted[i]), when_value) == 0)
            return TRUE;
    }

    return FALSE;
}

const gchar *
clawt_integration_field_default(const ClawtIntegrationField *field)
{
    g_return_val_if_fail(field != NULL, NULL);

    if (field->kind != CLAWT_FIELD_CHOICE || field->choices == NULL)
        return NULL;

    return field->choices[0];
}

gchar *
clawt_integration_needs_summary(const gchar *type)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;
    g_autoptr(GPtrArray) names = NULL;
    GString *out;

    list = clawt_integration_fields(type, &n);
    names = g_ptr_array_new();

    for (i = 0; i < n; i++) {
        /*
         * Only the unconditional ones.  A field required *given* a
         * choice you have not made yet cannot be listed before you make
         * it -- saying a notifier needs a room would be wrong for four
         * of its five backends.
         */
        if (!list[i].required || list[i].when_key != NULL)
            continue;

        g_ptr_array_add(names, (gpointer)list[i].label);
    }

    if (names->len == 0)
        return NULL;

    out = g_string_new("Needs ");

    for (i = 0; i < names->len; i++) {
        if (i > 0)
            g_string_append(out, i + 1 == names->len ? " and " : ", ");

        g_string_append(out, g_ptr_array_index(names, i));
    }

    g_string_append_c(out, '.');

    return g_string_free(out, FALSE);
}
