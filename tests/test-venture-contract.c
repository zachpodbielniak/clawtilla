/*
 * test-venture-contract.c - The VENTURE answers the task mirror reads
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * tests/fixtures/venture/ is a copy of VENTURE's own
 * tests/fixtures/api-contract/: the shape -- every key, its type -- of
 * each answer clawt-venture-mirror.c reads, recorded by VENTURE's
 * /auth/clawtilla-contract.  VENTURE fails when an answer stops matching
 * its copy; this checks every key the mirror reads is in ours, with the
 * type it reads it as.  `make check-venture-contract` diffs the two
 * copies, so a contract change on either side is a failing check rather
 * than a fleet whose tickets quietly stop updating.
 */

#include <clawtilla.h>

#include <string.h>

/* One fixture, parsed. */
static JsonNode *
contract(const gchar *name)
{
    g_autofree gchar *file = g_strconcat(name, ".json", NULL);
    g_autofree gchar *path = g_build_filename(CLAWT_TEST_FIXTURES, "venture",
                                              file, NULL);
    g_autofree gchar *text = NULL;
    g_autoptr(GError) error = NULL;
    JsonNode *node;

    g_assert_true(g_file_get_contents(path, &text, NULL, &error));
    g_assert_no_error(error);
    node = json_from_string(text, &error);
    g_assert_no_error(error);
    g_assert_nonnull(node);

    return node;
}

/*
 * The type recorded for a dotted @key of @node: "records.0.id" reads
 * into the list's first record.
 */
static const gchar *
type_of(JsonNode *node, const gchar *key)
{
    g_auto(GStrv) parts = g_strsplit(key, ".", -1);
    JsonNode *at = node;
    guint i;

    for (i = 0; parts[i] != NULL; i++) {
        if (at == NULL)
            return NULL;

        /* A list stands for its first element: look the key up there. */
        if (JSON_NODE_HOLDS_ARRAY(at)) {
            JsonArray *array = json_node_get_array(at);

            at = json_array_get_length(array) > 0
                ? json_array_get_element(array, 0) : NULL;
            if (at == NULL)
                return NULL;
        }

        if (!JSON_NODE_HOLDS_OBJECT(at) ||
            !json_object_has_member(json_node_get_object(at), parts[i]))
            return NULL;

        at = json_object_get_member(json_node_get_object(at), parts[i]);
    }

    return (at != NULL && JSON_NODE_HOLDS_VALUE(at))
        ? json_node_get_string(at) : NULL;
}

/* Asserts @name records @key as @type (or as null, where null is allowed). */
static void
expect(const gchar *name, const gchar *key, const gchar *type)
{
    g_autoptr(JsonNode) node = contract(name);
    const gchar *recorded = type_of(node, key);

    if (g_strcmp0(recorded, type) != 0)
        g_error("VENTURE contract %s: the mirror reads %s as %s; VENTURE "
                "records %s", name, key, type,
                recorded != NULL ? recorded : "no such key");
}

/* Every key the mirror reads, where it reads it, as what. */
static void
test_the_mirror_reads_what_venture_answers(void)
{
    /* whoami: the account's actor and username */
    expect("whoami", "actor", "string");
    expect("whoami", "user.username", "string");
    /* and the organizations it reaches: each is read on its own */
    expect("whoami", "memberships.organization_id", "int");

    /* resolve: the repository and its organization */
    expect("forge-repo-resolve", "id", "int");
    expect("forge-repo-resolve", "organization_id", "int");

    /* a filed ticket: its id, version, issue type and repository */
    expect("ticket-create", "id", "int");
    expect("ticket-create", "version", "int");
    expect("ticket-create", "issue_type", "string");
    expect("ticket-create", "repo_id", "int");
    expect("ticket-create", "parent_id", "int");

    /* the list the reverse poll and adoption read: under `records` */
    expect("ticket-list", "records.id", "int");
    expect("ticket-list", "records.version", "int");
    expect("ticket-list", "records.status", "string");
    expect("ticket-list", "records.external_ref", "string");
    expect("ticket-list", "records.updated_at", "string");
    expect("ticket-list", "records.title", "string");
    expect("ticket-list", "records.description", "string");
    expect("ticket-list", "records.organization_id", "int");

    /* an update: the version after it; and a re-read after a stale one */
    expect("ticket-update", "version", "int");
    expect("ticket", "version", "int");

    /* a project's pinned repository: its organization */
    expect("forge-repo", "id", "int");
    expect("forge-repo", "organization_id", "int");

    /* comments: made, and read back -- author is what tells whose */
    expect("ticket-comment-create", "id", "int");
    expect("ticket-comment-list", "records.id", "int");
    expect("ticket-comment-list", "records.ticket_id", "int");
    expect("ticket-comment-list", "records.author", "string");
    expect("ticket-comment-list", "records.body", "string");
    expect("ticket-comment-list", "records.external_ref", "string");

    /* the timeline intake reads: who set the assignee (0.47.0) */
    expect("ticket-activity", "action", "string");
    expect("ticket-activity", "actor", "string");
    expect("ticket-activity", "changes.assignee.to", "string");

    /* the branch link and the run */
    expect("ticket-link-create", "id", "int");
    expect("run-start", "id", "int");
    expect("run-finish", "state", "string");

    /* a milestone and a goal, as the context line reads them */
    expect("milestone", "name", "string");
    expect("milestone", "status", "string");
    expect("goal", "name", "string");
    expect("goal", "current_value", "double");
    expect("goal", "target_value", "double");
    expect("goal", "unit", "string");

    /* the pull request onto the branch's link (0.48.0) */
    expect("ticket-link-update", "pull_request_number", "int");
    expect("ticket-link-update", "pull_request_url", "string");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/venture-contract/the-mirror-reads-what-venture-answers",
                    test_the_mirror_reads_what_venture_answers);

    return g_test_run();
}
