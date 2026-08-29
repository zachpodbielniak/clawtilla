/*
 * test-avatar.c - An agent's profile picture, found, resolved and served
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two layers: clawt_workspace_find_profile_picture() and the clawt_avatar_*
 * functions built on it need no daemon at all, and are tested directly
 * against a temporary directory.  The IPC verbs -- agent.avatar,
 * agent.avatar_set, agent.avatar_clear -- go through
 * clawt_daemon_handle_request() in-process, exactly as test-daemon.c does,
 * because the interesting failures there are in the handler, not in a
 * socket.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "clawt-test-util.h"

/*
 * Magic numbers only -- clawt_avatar_sniff_mime_type() reads nothing
 * past them, so a real image would only make these fixtures larger.
 */
static const guchar PNG_BYTES[] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 'x', 'x', 'x', 'x'
};
static const guchar JPEG_BYTES[] = { 0xff, 0xd8, 0xff, 'x', 'x', 'x', 'x' };
static const guchar WEBP_BYTES[] = {
    'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'
};
static const guchar TEXT_BYTES[] = "not a picture, just text";

/* ── Pure library: clawt_workspace_find_profile_picture() ──────────── */

typedef struct {
    gchar *dir;
} WorkspaceFixture;

static void
workspace_fixture_setup(WorkspaceFixture *fixture)
{
    fixture->dir = g_dir_make_tmp("clawt-avatar-XXXXXX", NULL);
}

static void
workspace_fixture_teardown(WorkspaceFixture *fixture)
{
    clawt_test_remove_tree(fixture->dir);
    g_clear_pointer(&fixture->dir, g_free);
}

static void
write_sibling(const gchar *dir, const gchar *name, const guchar *data,
             gsize length)
{
    g_autofree gchar *path = g_build_filename(dir, name, NULL);
    g_autoptr(GError) error = NULL;

    g_assert_true(g_file_set_contents(path, (const gchar *)data,
                                      (gssize)length, &error));
    g_assert_no_error(error);
}

static void
test_png_detected(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autofree gchar *expected = NULL;
    g_autofree gchar *found = NULL;

    workspace_fixture_setup(&fixture);
    expected = g_build_filename(fixture.dir, "profile-picture.png", NULL);
    write_sibling(fixture.dir, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));

    found = clawt_workspace_find_profile_picture(fixture.dir);
    g_assert_cmpstr(found, ==, expected);

    workspace_fixture_teardown(&fixture);
}

static void
test_jpg_detected(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autofree gchar *expected = NULL;
    g_autofree gchar *found = NULL;

    workspace_fixture_setup(&fixture);
    expected = g_build_filename(fixture.dir, "profile-picture.jpg", NULL);
    write_sibling(fixture.dir, "profile-picture.jpg", JPEG_BYTES,
                 sizeof(JPEG_BYTES));

    found = clawt_workspace_find_profile_picture(fixture.dir);
    g_assert_cmpstr(found, ==, expected);

    workspace_fixture_teardown(&fixture);
}

/*
 * The order is defined rather than left to the filesystem, so two files
 * present resolve the same way on every call. png before jpg before
 * jpeg before webp.
 */
static void
test_both_present_resolves_in_documented_order(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autofree gchar *png = NULL;
    g_autofree gchar *found = NULL;

    workspace_fixture_setup(&fixture);
    png = g_build_filename(fixture.dir, "profile-picture.png", NULL);
    write_sibling(fixture.dir, "profile-picture.webp", WEBP_BYTES,
                 sizeof(WEBP_BYTES));
    write_sibling(fixture.dir, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));
    write_sibling(fixture.dir, "profile-picture.jpg", JPEG_BYTES,
                 sizeof(JPEG_BYTES));

    found = clawt_workspace_find_profile_picture(fixture.dir);
    g_assert_cmpstr(found, ==, png);

    workspace_fixture_teardown(&fixture);
}

static void
test_neither_present_returns_null(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autofree gchar *found = NULL;

    workspace_fixture_setup(&fixture);

    found = clawt_workspace_find_profile_picture(fixture.dir);
    g_assert_null(found);

    workspace_fixture_teardown(&fixture);
}

/* A directory named profile-picture.png is not a picture. */
static void
test_a_directory_is_not_detected(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autofree gchar *as_dir = NULL;
    g_autofree gchar *found = NULL;

    workspace_fixture_setup(&fixture);
    as_dir = g_build_filename(fixture.dir, "profile-picture.png", NULL);
    g_assert_cmpint(g_mkdir(as_dir, 0700), ==, 0);

    found = clawt_workspace_find_profile_picture(fixture.dir);
    g_assert_null(found);

    workspace_fixture_teardown(&fixture);
}

/*
 * Unreadable is reported as absent, never as an error: this path has no
 * caller to hand an error to, since it is the fallback.
 */
static void
test_an_unreadable_file_reads_as_absent(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autofree gchar *path = NULL;
    g_autofree gchar *found = NULL;

    /*
     * Root ignores the permission bits, so a mode-0000 file is still
     * readable and this case cannot be made to happen.  Skipped rather
     * than quietly weakened, because `unshare -rn` -- the project's own
     * hermeticity check -- runs every test binary as root in a new user
     * namespace, and a test that fails there makes that check unusable
     * for the whole suite.
     */
    if (geteuid() == 0) {
        g_test_skip("root bypasses the permission bits");
        return;
    }

    workspace_fixture_setup(&fixture);
    path = g_build_filename(fixture.dir, "profile-picture.png", NULL);
    write_sibling(fixture.dir, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));
    g_assert_cmpint(g_chmod(path, 0000), ==, 0);

    found = clawt_workspace_find_profile_picture(fixture.dir);
    g_assert_null(found);

    g_chmod(path, 0600);
    workspace_fixture_teardown(&fixture);
}

/* ── clawt_avatar_resolve_path(): layering agents.avatar on top ─────── */

static void
test_explicit_avatar_overrides_the_detected_file(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autofree gchar *explicit_path = NULL;
    g_autofree gchar *found = NULL;

    workspace_fixture_setup(&fixture);
    write_sibling(fixture.dir, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));
    explicit_path = g_build_filename(fixture.dir, "chosen.jpg", NULL);
    write_sibling(fixture.dir, "chosen.jpg", JPEG_BYTES, sizeof(JPEG_BYTES));

    found = clawt_avatar_resolve_path(explicit_path, fixture.dir);
    g_assert_cmpstr(found, ==, explicit_path);

    workspace_fixture_teardown(&fixture);
}

static void
test_relative_explicit_path_resolves_against_workspace(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autofree gchar *expected = NULL;
    g_autofree gchar *found = NULL;

    workspace_fixture_setup(&fixture);
    expected = g_build_filename(fixture.dir, "somewhere-else.png", NULL);
    write_sibling(fixture.dir, "somewhere-else.png", PNG_BYTES,
                 sizeof(PNG_BYTES));

    found = clawt_avatar_resolve_path("somewhere-else.png", fixture.dir);
    g_assert_cmpstr(found, ==, expected);

    workspace_fixture_teardown(&fixture);
}

/*
 * A mistyped path and no path at all must not look the same: this falls
 * back to the auto-detected file, but only after warning.
 */
static void
test_nonexistent_explicit_path_falls_back_and_warns(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autofree gchar *detected = NULL;
    g_autofree gchar *found = NULL;

    workspace_fixture_setup(&fixture);
    detected = g_build_filename(fixture.dir, "profile-picture.png", NULL);
    write_sibling(fixture.dir, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*does not "
                          "exist*");
    found = clawt_avatar_resolve_path("no-such-file.png", fixture.dir);
    g_test_assert_expected_messages();

    g_assert_cmpstr(found, ==, detected);

    workspace_fixture_teardown(&fixture);
}

/*
 * With nothing auto-detected either, the fallback is all the way to
 * %NULL -- which is what tells a caller to show initials.  Still
 * warned, for the same reason: a typo and never having set one must not
 * read the same.
 */
static void
test_nonexistent_explicit_path_with_nothing_detected_warns_to_null(void)
{
    WorkspaceFixture fixture = { 0 };
    gchar *found = NULL;

    workspace_fixture_setup(&fixture);

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*does not "
                          "exist*");
    found = clawt_avatar_resolve_path("no-such-file.png", fixture.dir);
    g_test_assert_expected_messages();

    g_assert_null(found);

    workspace_fixture_teardown(&fixture);
}

/* ── Sniffing and the etag ───────────────────────────────────────── */

static void
test_sniffing_recognises_the_three_types(void)
{
    g_assert_cmpstr(
        clawt_avatar_sniff_mime_type(PNG_BYTES, sizeof(PNG_BYTES)), ==,
        "image/png");
    g_assert_cmpstr(
        clawt_avatar_sniff_mime_type(JPEG_BYTES, sizeof(JPEG_BYTES)), ==,
        "image/jpeg");
    g_assert_cmpstr(
        clawt_avatar_sniff_mime_type(WEBP_BYTES, sizeof(WEBP_BYTES)), ==,
        "image/webp");
    g_assert_null(
        clawt_avatar_sniff_mime_type(TEXT_BYTES, sizeof(TEXT_BYTES) - 1));

    g_assert_cmpstr(clawt_avatar_extension_for_mime_type("image/png"), ==,
                    "png");
    /*
     * .jpg, not .jpeg -- AVATAR_NAMES tries .jpg first, so a freshly
     * written picture is found on the first comparison rather than the
     * third.
     */
    g_assert_cmpstr(clawt_avatar_extension_for_mime_type("image/jpeg"), ==,
                    "jpg");
    g_assert_cmpstr(clawt_avatar_extension_for_mime_type("image/webp"), ==,
                    "webp");
}

static void
test_etag_changes_with_the_bytes(void)
{
    g_autofree gchar *first = clawt_avatar_compute_etag(PNG_BYTES,
                                                        sizeof(PNG_BYTES));
    g_autofree gchar *second = clawt_avatar_compute_etag(JPEG_BYTES,
                                                         sizeof(JPEG_BYTES));
    g_autofree gchar *first_again =
        clawt_avatar_compute_etag(PNG_BYTES, sizeof(PNG_BYTES));

    g_assert_cmpstr(first, !=, second);
    g_assert_cmpstr(first, ==, first_again);
}

/* ── clawt_avatar_read() ─────────────────────────────────────────── */

static void
test_read_returns_bytes_and_sniffed_mime(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    guchar *bytes = NULL;
    gsize length = 0;
    gchar *mime = NULL;
    gchar *etag = NULL;

    workspace_fixture_setup(&fixture);
    write_sibling(fixture.dir, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));

    g_assert_true(clawt_avatar_read(NULL, fixture.dir, 0, &bytes, &length,
                                    &mime, &etag, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(length, ==, sizeof(PNG_BYTES));
    g_assert_cmpint(memcmp(bytes, PNG_BYTES, length), ==, 0);
    g_assert_cmpstr(mime, ==, "image/png");
    g_assert_nonnull(etag);

    g_free(bytes);
    g_free(mime);
    g_free(etag);
    workspace_fixture_teardown(&fixture);
}

static void
test_read_over_the_cap_is_refused_naming_it(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    guchar *bytes = NULL;
    gsize length = 0;
    gchar *mime = NULL;
    gchar *etag = NULL;

    workspace_fixture_setup(&fixture);
    write_sibling(fixture.dir, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));

    g_assert_false(clawt_avatar_read(NULL, fixture.dir, 4, &bytes, &length,
                                     &mime, &etag, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_assert_nonnull(strstr(error->message, "avatar_max_bytes"));

    workspace_fixture_teardown(&fixture);
}

/* A text file named .png is refused by sniffing, not served as one. */
static void
test_read_refuses_a_text_file_named_png(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    guchar *bytes = NULL;
    gsize length = 0;
    gchar *mime = NULL;
    gchar *etag = NULL;

    workspace_fixture_setup(&fixture);
    write_sibling(fixture.dir, "profile-picture.png", TEXT_BYTES,
                 sizeof(TEXT_BYTES) - 1);

    g_assert_false(clawt_avatar_read(NULL, fixture.dir, 0, &bytes, &length,
                                     &mime, &etag, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);

    workspace_fixture_teardown(&fixture);
}

static void
test_read_with_no_picture_is_a_clean_not_found(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    guchar *bytes = NULL;
    gsize length = 0;
    gchar *mime = NULL;
    gchar *etag = NULL;

    workspace_fixture_setup(&fixture);

    g_assert_false(clawt_avatar_read(NULL, fixture.dir, 0, &bytes, &length,
                                     &mime, &etag, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);

    workspace_fixture_teardown(&fixture);
}

/* ── clawt_avatar_write() and clawt_avatar_clear() ──────────────────── */

static void
test_write_uses_the_sniffed_extension_not_a_claim(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    gchar *mime = NULL;
    g_autofree gchar *expected = NULL;

    workspace_fixture_setup(&fixture);

    /* JPEG bytes, even though nothing here ever claims a name at all. */
    g_assert_true(clawt_avatar_write(fixture.dir, JPEG_BYTES,
                                     sizeof(JPEG_BYTES), &mime, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(mime, ==, "image/jpeg");

    expected = g_build_filename(fixture.dir, "profile-picture.jpg", NULL);
    g_assert_true(g_file_test(expected, G_FILE_TEST_IS_REGULAR));

    g_free(mime);
    workspace_fixture_teardown(&fixture);
}

static void
test_a_second_write_replaces_the_first(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autofree gchar *png_path = NULL;
    g_autofree gchar *jpg_path = NULL;
    g_autofree gchar *found = NULL;

    workspace_fixture_setup(&fixture);

    g_assert_true(clawt_avatar_write(fixture.dir, PNG_BYTES,
                                     sizeof(PNG_BYTES), NULL, NULL));
    png_path = g_build_filename(fixture.dir, "profile-picture.png", NULL);
    g_assert_true(g_file_test(png_path, G_FILE_TEST_IS_REGULAR));

    /*
     * Replaced with a jpeg: the old .png must not survive, or it would
     * keep winning resolution over the "new" picture -- .png sorts
     * first in the detection order.
     */
    g_assert_true(clawt_avatar_write(fixture.dir, JPEG_BYTES,
                                     sizeof(JPEG_BYTES), NULL, NULL));
    jpg_path = g_build_filename(fixture.dir, "profile-picture.jpg", NULL);
    g_assert_true(g_file_test(jpg_path, G_FILE_TEST_IS_REGULAR));
    g_assert_false(g_file_test(png_path, G_FILE_TEST_EXISTS));

    found = clawt_workspace_find_profile_picture(fixture.dir);
    g_assert_cmpstr(found, ==, jpg_path);

    workspace_fixture_teardown(&fixture);
}

static void
test_clear_removes_it(void)
{
    WorkspaceFixture fixture = { 0 };

    workspace_fixture_setup(&fixture);

    g_assert_true(clawt_avatar_write(fixture.dir, PNG_BYTES,
                                     sizeof(PNG_BYTES), NULL, NULL));
    g_assert_nonnull(clawt_workspace_find_profile_picture(fixture.dir));

    g_assert_true(clawt_avatar_clear(fixture.dir));
    g_assert_null(clawt_workspace_find_profile_picture(fixture.dir));

    /* Idempotent: nothing left to remove, and that is not a failure. */
    g_assert_false(clawt_avatar_clear(fixture.dir));

    workspace_fixture_teardown(&fixture);
}

static void
test_write_refuses_a_payload_that_is_not_an_image(void)
{
    WorkspaceFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;

    workspace_fixture_setup(&fixture);

    g_assert_false(clawt_avatar_write(fixture.dir, TEXT_BYTES,
                                      sizeof(TEXT_BYTES) - 1, NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_assert_null(clawt_workspace_find_profile_picture(fixture.dir));

    workspace_fixture_teardown(&fixture);
}

/* ── The daemon's IPC verbs ──────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
} DaemonFixture;

static void
daemon_fixture_setup(DaemonFixture *fixture, const gchar *extra_yaml)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-avatar-daemon-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);

    /*
     * The five things every daemon fixture in this tree pins: the
     * socket, the state directory, the automation directory and the
     * workspace root all move into the temporary directory, and the
     * tailnet listener stays off -- `make test` opens no network socket
     * at all.
     */
    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "%s",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        extra_yaml != NULL ? extra_yaml : "");

    g_assert_true(g_file_set_contents(fixture->config_path, yaml, -1,
                                      &error));
    g_assert_no_error(error);

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);
    g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
daemon_fixture_teardown(DaemonFixture *fixture)
{
    if (fixture->daemon != NULL) {
        clawt_daemon_stop(fixture->daemon);
        g_clear_object(&fixture->daemon);
    }

    if (fixture->context != NULL) {
        while (g_main_context_iteration(fixture->context, FALSE))
            ;
    }

    g_clear_pointer(&fixture->context, g_main_context_unref);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
    g_clear_pointer(&fixture->config_path, g_free);
}

static JsonNode *
daemon_request(DaemonFixture *fixture, const gchar *kind,
              const gchar *payload_json)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");

    if (payload_json != NULL) {
        g_autoptr(JsonParser) parser = json_parser_new();

        g_assert_true(json_parser_load_from_data(parser, payload_json, -1,
                                                 NULL));
        clawt_ipc_frame_set_payload(
            frame, json_node_copy(json_parser_get_root(parser)));
    }

    return clawt_daemon_handle_request(fixture->daemon, frame);
}

static JsonObject *
daemon_payload_of(JsonNode *reply)
{
    return json_object_get_object_member(json_node_get_object(reply),
                                         "payload");
}

static gchar *
agent_workspace(DaemonFixture *fixture, const gchar *agent_id)
{
    ClawtAgentConfig *config =
        clawt_config_get_agent(clawt_daemon_get_config(fixture->daemon),
                               agent_id);

    g_assert_nonnull(config);

    return clawt_agent_config_get_workspace(config);
}

static void
test_ipc_avatar_returns_bytes_and_mime(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *payload;
    g_autofree guchar *decoded = NULL;
    gsize length = 0;

    daemon_fixture_setup(&fixture, "agents:\n  - id: chief\n");
    workspace = agent_workspace(&fixture, "chief");
    write_sibling(workspace, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));

    reply = daemon_request(&fixture, "agent.avatar",
                           "{\"agent\":\"chief\"}");
    g_assert_true(json_object_get_boolean_member(json_node_get_object(reply),
                                                 "ok"));

    payload = daemon_payload_of(reply);
    g_assert_cmpstr(json_object_get_string_member(payload, "mime"), ==,
                    "image/png");
    g_assert_nonnull(json_object_get_string_member(payload, "etag"));

    decoded = g_base64_decode(
        json_object_get_string_member(payload, "base64"), &length);
    g_assert_cmpuint(length, ==, sizeof(PNG_BYTES));
    g_assert_cmpint(memcmp(decoded, PNG_BYTES, length), ==, 0);

    daemon_fixture_teardown(&fixture);
}

static void
test_ipc_avatar_over_the_cap_refused(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *object;

    daemon_fixture_setup(
        &fixture,
        "agents:\n  - id: chief\ndefaults:\n  avatar_max_bytes: 4\n");
    workspace = agent_workspace(&fixture, "chief");
    write_sibling(workspace, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));

    reply = daemon_request(&fixture, "agent.avatar",
                           "{\"agent\":\"chief\"}");
    object = json_node_get_object(reply);

    g_assert_false(json_object_get_boolean_member(object, "ok"));
    g_assert_nonnull(
        strstr(json_object_get_string_member(object, "error"),
              "avatar_max_bytes"));

    daemon_fixture_teardown(&fixture);
}

static void
test_ipc_avatar_text_named_png_refused(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autoptr(JsonNode) reply = NULL;

    daemon_fixture_setup(&fixture, "agents:\n  - id: chief\n");
    workspace = agent_workspace(&fixture, "chief");
    write_sibling(workspace, "profile-picture.png", TEXT_BYTES,
                 sizeof(TEXT_BYTES) - 1);

    reply = daemon_request(&fixture, "agent.avatar",
                           "{\"agent\":\"chief\"}");
    g_assert_false(
        json_object_get_boolean_member(json_node_get_object(reply), "ok"));

    daemon_fixture_teardown(&fixture);
}

static void
test_ipc_avatar_no_picture_is_clean_not_found(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *object;

    daemon_fixture_setup(&fixture, "agents:\n  - id: chief\n");

    reply = daemon_request(&fixture, "agent.avatar",
                           "{\"agent\":\"chief\"}");
    object = json_node_get_object(reply);

    g_assert_false(json_object_get_boolean_member(object, "ok"));
    g_assert_cmpint((gint)json_object_get_int_member(object, "code"), ==,
                    CLAWT_ERROR_NOT_FOUND);

    daemon_fixture_teardown(&fixture);
}

static void
test_ipc_avatar_etag_changes_with_the_bytes(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *first_etag = NULL;
    g_autofree gchar *second_etag = NULL;

    daemon_fixture_setup(&fixture, "agents:\n  - id: chief\n");
    workspace = agent_workspace(&fixture, "chief");
    write_sibling(workspace, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));

    {
        g_autoptr(JsonNode) reply = daemon_request(
            &fixture, "agent.avatar", "{\"agent\":\"chief\"}");

        first_etag = g_strdup(json_object_get_string_member(
            daemon_payload_of(reply), "etag"));
    }

    {
        g_autofree gchar *path =
            g_build_filename(workspace, "profile-picture.png", NULL);
        g_autoptr(GError) error = NULL;
        static const guchar different[] = {
            0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 'y', 'y'
        };

        g_assert_true(g_file_set_contents(
            path, (const gchar *)different, sizeof(different), &error));
        g_assert_no_error(error);
    }

    {
        g_autoptr(JsonNode) reply = daemon_request(
            &fixture, "agent.avatar", "{\"agent\":\"chief\"}");

        second_etag = g_strdup(json_object_get_string_member(
            daemon_payload_of(reply), "etag"));
    }

    g_assert_cmpstr(first_etag, !=, second_etag);

    daemon_fixture_teardown(&fixture);
}

static gchar *
base64_of(const guchar *data, gsize length)
{
    return g_base64_encode(data, length);
}

static void
test_ipc_avatar_set_writes_sniffed_extension(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *encoded = base64_of(JPEG_BYTES, sizeof(JPEG_BYTES));
    g_autofree gchar *payload_json = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *expected = NULL;

    daemon_fixture_setup(&fixture, "agents:\n  - id: chief\n");
    workspace = agent_workspace(&fixture, "chief");

    payload_json = g_strdup_printf(
        "{\"agent\":\"chief\",\"data\":\"%s\"}", encoded);
    reply = daemon_request(&fixture, "agent.avatar_set", payload_json);

    g_assert_true(
        json_object_get_boolean_member(json_node_get_object(reply), "ok"));
    g_assert_cmpstr(
        json_object_get_string_member(daemon_payload_of(reply), "mime"),
        ==, "image/jpeg");

    /* The extension came from the bytes: nothing here ever named one. */
    expected = g_build_filename(workspace, "profile-picture.jpg", NULL);
    g_assert_true(g_file_test(expected, G_FILE_TEST_IS_REGULAR));

    daemon_fixture_teardown(&fixture);
}

static void
test_ipc_avatar_set_twice_replaces(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *png_encoded = base64_of(PNG_BYTES, sizeof(PNG_BYTES));
    g_autofree gchar *jpg_encoded =
        base64_of(JPEG_BYTES, sizeof(JPEG_BYTES));
    g_autofree gchar *first_json = NULL;
    g_autofree gchar *second_json = NULL;
    g_autofree gchar *png_path = NULL;
    g_autofree gchar *jpg_path = NULL;

    daemon_fixture_setup(&fixture, "agents:\n  - id: chief\n");
    workspace = agent_workspace(&fixture, "chief");

    first_json = g_strdup_printf("{\"agent\":\"chief\",\"data\":\"%s\"}",
                                 png_encoded);

    {
        g_autoptr(JsonNode) first_reply =
            daemon_request(&fixture, "agent.avatar_set", first_json);

        g_assert_true(json_object_get_boolean_member(
            json_node_get_object(first_reply), "ok"));
    }

    second_json = g_strdup_printf("{\"agent\":\"chief\",\"data\":\"%s\"}",
                                  jpg_encoded);

    {
        g_autoptr(JsonNode) second_reply =
            daemon_request(&fixture, "agent.avatar_set", second_json);

        g_assert_true(json_object_get_boolean_member(
            json_node_get_object(second_reply), "ok"));
    }

    png_path = g_build_filename(workspace, "profile-picture.png", NULL);
    jpg_path = g_build_filename(workspace, "profile-picture.jpg", NULL);

    g_assert_false(g_file_test(png_path, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(jpg_path, G_FILE_TEST_IS_REGULAR));

    daemon_fixture_teardown(&fixture);
}

static void
test_ipc_avatar_clear_removes_it(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autoptr(JsonNode) reply = NULL;

    daemon_fixture_setup(&fixture, "agents:\n  - id: chief\n");
    workspace = agent_workspace(&fixture, "chief");
    write_sibling(workspace, "profile-picture.png", PNG_BYTES,
                 sizeof(PNG_BYTES));

    reply = daemon_request(&fixture, "agent.avatar_clear",
                           "{\"agent\":\"chief\"}");

    g_assert_true(
        json_object_get_boolean_member(json_node_get_object(reply), "ok"));
    g_assert_true(json_object_get_boolean_member(daemon_payload_of(reply),
                                                 "removed"));
    g_assert_null(clawt_workspace_find_profile_picture(workspace));

    daemon_fixture_teardown(&fixture);
}

static void
test_ipc_avatar_set_refuses_a_non_image_payload(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *encoded =
        base64_of(TEXT_BYTES, sizeof(TEXT_BYTES) - 1);
    g_autofree gchar *payload_json = NULL;
    g_autoptr(JsonNode) reply = NULL;

    daemon_fixture_setup(&fixture, "agents:\n  - id: chief\n");
    workspace = agent_workspace(&fixture, "chief");

    payload_json = g_strdup_printf(
        "{\"agent\":\"chief\",\"data\":\"%s\"}", encoded);
    reply = daemon_request(&fixture, "agent.avatar_set", payload_json);

    g_assert_false(
        json_object_get_boolean_member(json_node_get_object(reply), "ok"));
    g_assert_null(clawt_workspace_find_profile_picture(workspace));

    daemon_fixture_teardown(&fixture);
}

/*
 * The negative test that matters most: agent.avatar_set has no path
 * parameter at all.  A "path" member in the payload is inert -- proven
 * behaviourally, by pointing it at a file with different bytes than
 * "data" and asserting the daemon wrote "data", never "path" -- rather
 * than merely absent from today's reading of the handler, so a future
 * edit cannot wire one up quietly.
 */
static void
test_avatar_set_has_no_path_parameter(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *decoy = NULL;
    g_autofree gchar *encoded = base64_of(PNG_BYTES, sizeof(PNG_BYTES));
    g_autofree gchar *payload_json = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *written_path = NULL;
    g_autofree gchar *contents = NULL;
    gsize length = 0;

    daemon_fixture_setup(&fixture, "agents:\n  - id: chief\n");
    workspace = agent_workspace(&fixture, "chief");

    /* A file agent.avatar_set must never read, named by a "path" member
     * riding along beside the real "data". */
    decoy = g_build_filename(fixture.dir, "decoy.jpg", NULL);
    write_sibling(fixture.dir, "decoy.jpg", JPEG_BYTES, sizeof(JPEG_BYTES));

    payload_json = g_strdup_printf(
        "{\"agent\":\"chief\",\"data\":\"%s\",\"path\":\"%s\"}", encoded,
        decoy);
    reply = daemon_request(&fixture, "agent.avatar_set", payload_json);

    g_assert_true(
        json_object_get_boolean_member(json_node_get_object(reply), "ok"));

    /* "data" won: a PNG was written, never the decoy JPEG "path" named. */
    g_assert_cmpstr(
        json_object_get_string_member(daemon_payload_of(reply), "mime"),
        ==, "image/png");

    written_path = g_build_filename(workspace, "profile-picture.png", NULL);
    g_assert_true(g_file_get_contents(written_path, &contents, &length,
                                      NULL));
    g_assert_cmpuint(length, ==, sizeof(PNG_BYTES));
    g_assert_cmpint(memcmp(contents, PNG_BYTES, length), ==, 0);

    daemon_fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/avatar/png-detected", test_png_detected);
    g_test_add_func("/avatar/jpg-detected", test_jpg_detected);
    g_test_add_func("/avatar/both-present-documented-order",
                    test_both_present_resolves_in_documented_order);
    g_test_add_func("/avatar/neither-present-null",
                    test_neither_present_returns_null);
    g_test_add_func("/avatar/directory-not-detected",
                    test_a_directory_is_not_detected);
    g_test_add_func("/avatar/unreadable-reads-as-absent",
                    test_an_unreadable_file_reads_as_absent);

    g_test_add_func("/avatar/explicit-overrides-detected",
                    test_explicit_avatar_overrides_the_detected_file);
    g_test_add_func("/avatar/relative-explicit-resolves-against-workspace",
                    test_relative_explicit_path_resolves_against_workspace);
    g_test_add_func("/avatar/nonexistent-explicit-falls-back-and-warns",
                    test_nonexistent_explicit_path_falls_back_and_warns);
    g_test_add_func(
        "/avatar/nonexistent-explicit-with-nothing-detected-warns-to-null",
        test_nonexistent_explicit_path_with_nothing_detected_warns_to_null);

    g_test_add_func("/avatar/sniffs-three-types",
                    test_sniffing_recognises_the_three_types);
    g_test_add_func("/avatar/etag-changes-with-bytes",
                    test_etag_changes_with_the_bytes);

    g_test_add_func("/avatar/read-bytes-and-mime",
                    test_read_returns_bytes_and_sniffed_mime);
    g_test_add_func("/avatar/read-over-cap-refused",
                    test_read_over_the_cap_is_refused_naming_it);
    g_test_add_func("/avatar/read-refuses-text-named-png",
                    test_read_refuses_a_text_file_named_png);
    g_test_add_func("/avatar/read-no-picture-not-found",
                    test_read_with_no_picture_is_a_clean_not_found);

    g_test_add_func("/avatar/write-uses-sniffed-extension",
                    test_write_uses_the_sniffed_extension_not_a_claim);
    g_test_add_func("/avatar/second-write-replaces-first",
                    test_a_second_write_replaces_the_first);
    g_test_add_func("/avatar/clear-removes-it", test_clear_removes_it);
    g_test_add_func("/avatar/write-refuses-non-image",
                    test_write_refuses_a_payload_that_is_not_an_image);

    g_test_add_func("/avatar/ipc-returns-bytes-and-mime",
                    test_ipc_avatar_returns_bytes_and_mime);
    g_test_add_func("/avatar/ipc-over-cap-refused",
                    test_ipc_avatar_over_the_cap_refused);
    g_test_add_func("/avatar/ipc-text-named-png-refused",
                    test_ipc_avatar_text_named_png_refused);
    g_test_add_func("/avatar/ipc-no-picture-not-found",
                    test_ipc_avatar_no_picture_is_clean_not_found);
    g_test_add_func("/avatar/ipc-etag-changes-with-bytes",
                    test_ipc_avatar_etag_changes_with_the_bytes);
    g_test_add_func("/avatar/ipc-set-writes-sniffed-extension",
                    test_ipc_avatar_set_writes_sniffed_extension);
    g_test_add_func("/avatar/ipc-set-twice-replaces",
                    test_ipc_avatar_set_twice_replaces);
    g_test_add_func("/avatar/ipc-clear-removes-it",
                    test_ipc_avatar_clear_removes_it);
    g_test_add_func("/avatar/ipc-set-refuses-non-image",
                    test_ipc_avatar_set_refuses_a_non_image_payload);
    g_test_add_func("/avatar/set-has-no-path-parameter",
                    test_avatar_set_has_no_path_parameter);

    return g_test_run();
}
