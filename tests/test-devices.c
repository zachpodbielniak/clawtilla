/*
 * test-devices.c - Per-device client tokens, pairing and revoke
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three layers, each tested where it lives:
 *
 *   - ClawtDeviceStore on its own: a code's lifetime, single use and
 *     guess limit; the rate limit; hashing; idle expiry; what a listing
 *     can and cannot see; what the file on disk holds.
 *   - ClawtIpcServer's credential check: the owner token is untouched by
 *     anything the device store does.
 *   - The daemon end to end over its unix socket: a ClawtClient redeems
 *     a code, becomes that device, is refused the owner's verbs, and is
 *     disconnected when the owner revokes it.
 *
 * Every time is passed in, so nothing waits five minutes or ninety days;
 * nothing here opens a network socket.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

/* A fixed "now", well clear of zero so last_seen <= 0 never means "now". */
#define T0 ((gint64)1800000000)

typedef struct {
    gchar            *dir;
    gchar            *path;
    ClawtDeviceStore *store;
} StoreFixture;

static void
store_setup(StoreFixture *fixture)
{
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-devices-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->path = g_build_filename(fixture->dir, "state", "devices.yaml",
                                     NULL);
    fixture->store = clawt_device_store_new(fixture->path);
    g_assert_true(clawt_device_store_load(fixture->store, &error));
    g_assert_no_error(error);
}

static void
store_teardown(StoreFixture *fixture)
{
    g_clear_object(&fixture->store);
    clawt_test_remove_tree(fixture->dir);
    g_clear_pointer(&fixture->dir, g_free);
    g_clear_pointer(&fixture->path, g_free);
}

static guint
device_count(ClawtDeviceStore *store)
{
    g_autoptr(GPtrArray) devices = clawt_device_store_list(store);

    return devices->len;
}

/* Pairs one device and hands back its token and id. */
static gchar *
pair_one(ClawtDeviceStore *store, const gchar *name, gint64 now,
         gchar **id)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *code = NULL;
    gchar *token;

    code = clawt_device_store_begin_pairing(store, name, now, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(code);

    token = clawt_device_store_redeem(store, code, NULL, now, id, &error);
    g_assert_no_error(error);
    g_assert_nonnull(token);

    return token;
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static void
test_hash_is_sha256_hex(void)
{
    g_autofree gchar *hash = clawt_device_token_hash("abc");

    /* The FIPS 180-2 test vector, so this is SHA-256 and not "a hash". */
    g_assert_cmpstr(hash, ==,
                    "ba7816bf8f01cfea414140de5dae2223"
                    "b00361a396177a9cb410ff61f20015ad");
}

static void
test_secure_equals_contract(void)
{
    /*
     * The constant-time comparison every check here goes through.  Its
     * timing is not something a unit test can measure honestly; its
     * answers are, including the ones that must stay refusals.
     */
    g_assert_true(clawt_secure_equals("0123abcd", "0123abcd"));
    g_assert_false(clawt_secure_equals("0123abcd", "0123abce"));
    g_assert_false(clawt_secure_equals("0123abcd", "0123abc"));
    g_assert_false(clawt_secure_equals("", "x"));
    g_assert_false(clawt_secure_equals(NULL, NULL));
    g_assert_false(clawt_secure_equals("x", NULL));
}

static void
test_code_normalize_and_display(void)
{
    g_autofree gchar *spaced = clawt_device_code_normalize("123 456");
    g_autofree gchar *dashed = clawt_device_code_normalize(" 123-456 ");
    g_autofree gchar *display = clawt_device_code_display("123456");
    g_autofree gchar *countdown = clawt_device_countdown_text(299);
    g_autofree gchar *expired = clawt_device_countdown_text(0);

    g_assert_cmpstr(spaced, ==, "123456");
    g_assert_cmpstr(dashed, ==, "123456");
    g_assert_null(clawt_device_code_normalize("12345"));
    g_assert_null(clawt_device_code_normalize("1234567"));
    g_assert_null(clawt_device_code_normalize("12a456"));
    g_assert_null(clawt_device_code_normalize(NULL));
    g_assert_cmpstr(display, ==, "123 456");
    g_assert_cmpstr(countdown, ==, "4:59");
    g_assert_cmpstr(expired, ==, "expired");
}

static void
test_seen_text(void)
{
    g_autofree gchar *now = clawt_device_seen_text(T0, TRUE, T0 + 999999);
    g_autofree gchar *never = clawt_device_seen_text(0, FALSE, T0);
    g_autofree gchar *minutes = clawt_device_seen_text(T0, FALSE, T0 + 180);
    g_autofree gchar *days = clawt_device_seen_text(T0, FALSE,
                                                    T0 + 3 * 86400);
    g_autofree gchar *skew = clawt_device_seen_text(T0, FALSE, T0 - 100);

    g_assert_cmpstr(now, ==, "connected now");
    g_assert_cmpstr(never, ==, "never seen");
    g_assert_cmpstr(minutes, ==, "seen 3 minutes ago");
    g_assert_cmpstr(days, ==, "seen 3 days ago");
    g_assert_cmpstr(skew, ==, "seen just now");
}

/* ── Codes ───────────────────────────────────────────────────────── */

static void
test_code_is_six_digits(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    gint64 expires_at = 0;
    guint i;

    store_setup(&fixture);

    for (i = 0; i < 20; i++) {
        g_autofree gchar *code = clawt_device_store_begin_pairing(
            fixture.store, NULL, T0, &expires_at, &error);
        g_autofree gchar *again = NULL;

        g_assert_no_error(error);
        g_assert_cmpuint(strlen(code), ==, CLAWT_DEVICE_CODE_DIGITS);
        again = clawt_device_code_normalize(code);
        g_assert_cmpstr(again, ==, code);
    }

    g_assert_cmpint(expires_at, ==, T0 + CLAWT_DEVICE_CODE_LIFETIME_SECONDS);
    g_assert_cmpint(clawt_device_store_pairing_expires_at(fixture.store, T0),
                    ==, expires_at);

    store_teardown(&fixture);
}

static void
test_code_expires(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *code = NULL;
    g_autofree gchar *token = NULL;

    store_setup(&fixture);

    code = clawt_device_store_begin_pairing(fixture.store, "phone", T0, NULL,
                                            NULL);

    /* One second past its life, the right digits are no longer enough. */
    token = clawt_device_store_redeem(
        fixture.store, code, NULL,
        T0 + CLAWT_DEVICE_CODE_LIFETIME_SECONDS, NULL, &error);

    g_assert_null(token);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    g_assert_cmpint(clawt_device_store_pairing_expires_at(
                        fixture.store,
                        T0 + CLAWT_DEVICE_CODE_LIFETIME_SECONDS), ==, 0);

    store_teardown(&fixture);
}

static void
test_code_is_single_use(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *code = NULL;
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;

    store_setup(&fixture);

    code = clawt_device_store_begin_pairing(fixture.store, "phone", T0, NULL,
                                            NULL);
    first = clawt_device_store_redeem(fixture.store, code, NULL, T0 + 1, NULL,
                                      &error);
    g_assert_no_error(error);
    g_assert_nonnull(first);

    second = clawt_device_store_redeem(fixture.store, code, NULL, T0 + 2,
                                       NULL, &error);
    g_assert_null(second);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);

    g_assert_cmpuint(device_count(fixture.store), ==, 1);

    store_teardown(&fixture);
}

/* A code that is guaranteed not to be @code. */
static gchar *
wrong_code(const gchar *code)
{
    gchar *wrong = g_strdup(code);

    wrong[0] = (wrong[0] == '9') ? '0' : (gchar)(wrong[0] + 1);

    return wrong;
}

static void
test_guess_limit_invalidates_the_code(void)
{
    StoreFixture fixture = { 0 };
    g_autofree gchar *code = NULL;
    g_autofree gchar *wrong = NULL;
    g_autofree gchar *token = NULL;
    guint i;

    store_setup(&fixture);

    code = clawt_device_store_begin_pairing(fixture.store, "phone", T0, NULL,
                                            NULL);
    wrong = wrong_code(code);

    for (i = 0; i < CLAWT_DEVICE_CODE_MAX_GUESSES; i++) {
        g_autoptr(GError) error = NULL;

        g_assert_null(clawt_device_store_redeem(fixture.store, wrong, NULL,
                                                T0 + 1, NULL, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    }

    /* The right digits, after the last wrong guess: too late. */
    {
        g_autoptr(GError) error = NULL;

        token = clawt_device_store_redeem(fixture.store, code, NULL, T0 + 2,
                                          NULL, &error);
        g_assert_null(token);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    }

    g_assert_cmpint(clawt_device_store_pairing_expires_at(fixture.store,
                                                          T0 + 2), ==, 0);
    g_assert_cmpuint(device_count(fixture.store), ==, 0);

    store_teardown(&fixture);
}

static void
test_a_wrong_guess_below_the_limit_leaves_the_code(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *code = NULL;
    g_autofree gchar *wrong = NULL;
    g_autofree gchar *token = NULL;

    store_setup(&fixture);

    code = clawt_device_store_begin_pairing(fixture.store, "phone", T0, NULL,
                                            NULL);
    wrong = wrong_code(code);

    g_assert_null(clawt_device_store_redeem(fixture.store, wrong, NULL, T0,
                                            NULL, &error));
    g_clear_error(&error);

    /* Typed with the space the CLI prints: still the code. */
    {
        g_autofree gchar *spaced = clawt_device_code_display(code);

        token = clawt_device_store_redeem(fixture.store, spaced, NULL, T0,
                                          NULL, &error);
    }

    g_assert_no_error(error);
    g_assert_nonnull(token);

    store_teardown(&fixture);
}

static void
test_rate_limit_refuses_without_looking(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *code = NULL;
    g_autofree gchar *token = NULL;
    guint i;

    store_setup(&fixture);

    /* Failures with no code outstanding fill the window. */
    for (i = 0; i < CLAWT_DEVICE_REDEEM_FAILURES_PER_MINUTE; i++) {
        g_autoptr(GError) local = NULL;

        g_assert_null(clawt_device_store_redeem(fixture.store, "000000",
                                                NULL, T0, NULL, &local));
    }

    code = clawt_device_store_begin_pairing(fixture.store, NULL, T0, NULL,
                                            NULL);

    /* Inside the window even the right code is not looked at... */
    g_assert_null(clawt_device_store_redeem(fixture.store, code, NULL,
                                            T0 + 30, NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
    g_clear_error(&error);

    /* ...and the refusal did not spend it: a minute later it works. */
    token = clawt_device_store_redeem(fixture.store, code, "laptop",
                                      T0 + 61, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(token);

    store_teardown(&fixture);
}

/* ── Tokens ──────────────────────────────────────────────────────── */

static void
test_token_authenticates_and_is_stored_hashed(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *hash = NULL;
    g_autofree gchar *found = NULL;
    g_autofree gchar *text = NULL;
    GStatBuf st;

    store_setup(&fixture);

    token = pair_one(fixture.store, "phone", T0, &id);
    hash = clawt_device_token_hash(token);

    /* /dev/urandom's 32 bytes, as hex. */
    g_assert_cmpuint(strlen(token), ==, 64);

    found = clawt_device_store_authenticate(fixture.store, token, T0 + 5);
    g_assert_cmpstr(found, ==, id);

    g_assert_null(clawt_device_store_authenticate(fixture.store, "nope",
                                                  T0 + 5));
    g_assert_null(clawt_device_store_authenticate(fixture.store, NULL,
                                                  T0 + 5));
    g_assert_null(clawt_device_store_authenticate(fixture.store, hash,
                                                  T0 + 5));

    /* On disk: the hash, never the token, and only for this user. */
    g_assert_true(g_file_get_contents(fixture.path, &text, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(text, token));
    g_assert_nonnull(strstr(text, hash));

    g_assert_cmpint(g_stat(fixture.path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);

    store_teardown(&fixture);
}

static void
test_tokens_survive_a_reload(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtDeviceStore) again = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *found = NULL;
    g_autoptr(ClawtDevice) device = NULL;

    store_setup(&fixture);

    token = pair_one(fixture.store, "it's a phone", T0, &id);

    again = clawt_device_store_new(fixture.path);
    g_assert_true(clawt_device_store_load(again, &error));
    g_assert_no_error(error);

    found = clawt_device_store_authenticate(again, token, T0 + 1);
    g_assert_cmpstr(found, ==, id);

    /* A quote in the name survives the round trip. */
    device = clawt_device_store_lookup(again, id);
    g_assert_cmpstr(clawt_device_get_name(device), ==, "it's a phone");

    store_teardown(&fixture);
}

static void
test_list_never_carries_a_token(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GPtrArray) devices = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *token = NULL;
    ClawtDevice *device;

    store_setup(&fixture);

    token = pair_one(fixture.store, NULL, T0, &id);
    devices = clawt_device_store_list(fixture.store);

    g_assert_cmpuint(devices->len, ==, 1);
    device = g_ptr_array_index(devices, 0);

    g_assert_cmpstr(clawt_device_get_id(device), ==, id);
    g_assert_cmpint(clawt_device_get_created(device), ==, T0);
    g_assert_cmpint(clawt_device_get_last_seen(device), ==, T0);

    /* Named from the code first, then the device, then neither. */
    g_assert_cmpstr(clawt_device_get_name(device), ==, "unnamed device");

    store_teardown(&fixture);
}

static void
test_revoke(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *other_id = NULL;
    g_autofree gchar *other = NULL;
    g_autofree gchar *found = NULL;

    store_setup(&fixture);

    token = pair_one(fixture.store, "phone", T0, &id);
    other = pair_one(fixture.store, "laptop", T0, &other_id);

    g_assert_true(clawt_device_store_revoke(fixture.store, id, &error));
    g_assert_no_error(error);

    g_assert_null(clawt_device_store_authenticate(fixture.store, token,
                                                  T0 + 1));

    /* Revoking one leaves the other alone. */
    found = clawt_device_store_authenticate(fixture.store, other, T0 + 1);
    g_assert_cmpstr(found, ==, other_id);

    g_assert_false(clawt_device_store_revoke(fixture.store, id, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);

    store_teardown(&fixture);
}

static void
test_idle_expiry(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GPtrArray) expired = NULL;
    g_autoptr(GPtrArray) none = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *kept_id = NULL;
    g_autofree gchar *kept = NULL;
    const gint64 day = 86400;

    store_setup(&fixture);

    token = pair_one(fixture.store, "phone", T0, &id);
    kept = pair_one(fixture.store, "laptop", T0, &kept_id);

    /* 0 is never: a century later both still stand. */
    clawt_device_store_set_idle_seconds(fixture.store, 0);
    none = clawt_device_store_expire_idle(fixture.store, T0 + 36500 * day);
    g_assert_cmpuint(none->len, ==, 0);

    clawt_device_store_set_idle_seconds(fixture.store, 30 * day);

    /* The laptop stays connected: the sweep's touch keeps it fresh. */
    clawt_device_store_touch(fixture.store, kept_id, T0 + 29 * day);

    expired = clawt_device_store_expire_idle(fixture.store, T0 + 31 * day);
    g_assert_cmpuint(expired->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(expired, 0), ==, id);

    g_assert_null(clawt_device_store_authenticate(fixture.store, token,
                                                  T0 + 31 * day));
    g_assert_cmpuint(device_count(fixture.store), ==, 1);

    store_teardown(&fixture);
}

static void
test_idle_device_is_refused_at_hello(void)
{
    StoreFixture fixture = { 0 };
    g_autofree gchar *id = NULL;
    g_autofree gchar *token = NULL;

    store_setup(&fixture);

    token = pair_one(fixture.store, "phone", T0, &id);
    clawt_device_store_set_idle_seconds(fixture.store, 60);

    /* No sweep has run: the window is applied at the door too. */
    g_assert_null(clawt_device_store_authenticate(fixture.store, token,
                                                  T0 + 61));
    g_assert_null(clawt_device_store_lookup(fixture.store, id));

    store_teardown(&fixture);
}

/* ── The owner token ─────────────────────────────────────────────── */

static void
test_owner_token_is_unaffected(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(ClawtIpcServer) server = NULL;
    g_autoptr(GPtrArray) expired = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *who = NULL;
    const gchar *owner = "owner-token-0123456789";

    store_setup(&fixture);

    socket_path = g_build_filename(fixture.dir, "unused.sock", NULL);
    server = clawt_ipc_server_new(socket_path);
    clawt_ipc_server_set_token(server, owner);

    /* Before a store exists: only the owner. */
    g_assert_true(clawt_ipc_server_check_credential(server, owner, T0,
                                                    &who));
    g_assert_null(who);

    clawt_ipc_server_set_device_store(server, fixture.store);
    token = pair_one(fixture.store, "phone", T0, &id);

    /* A device is itself; the owner is still the owner (NULL). */
    g_assert_true(clawt_ipc_server_check_credential(server, token, T0, &who));
    g_assert_cmpstr(who, ==, id);
    g_clear_pointer(&who, g_free);

    g_assert_true(clawt_ipc_server_check_credential(server, owner, T0,
                                                    &who));
    g_assert_null(who);
    g_assert_false(clawt_ipc_server_check_credential(server, "wrong", T0,
                                                     NULL));

    /* Revoking and expiring every device leaves the owner token alone. */
    g_assert_true(clawt_device_store_revoke(fixture.store, id, NULL));
    clawt_device_store_set_idle_seconds(fixture.store, 1);
    expired = clawt_ipc_server_sweep_devices(server, T0 + 1000);

    g_assert_false(clawt_ipc_server_check_credential(server, token, T0,
                                                     NULL));
    g_assert_true(clawt_ipc_server_check_credential(server, owner,
                                                    T0 + 1000, &who));
    g_assert_null(who);

    g_clear_object(&server);
    store_teardown(&fixture);
}

/* ── connections.yaml ────────────────────────────────────────────── */

static void
test_connections_file_is_0600(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) list = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;
    ClawtConnection *connection;
    GStatBuf st;

    dir = g_dir_make_tmp("clawt-devices-conn-XXXXXX", &error);
    g_assert_no_error(error);
    path = g_build_filename(dir, "clawtilla", "connections.yaml", NULL);

    list = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_connection_free);
    connection = clawt_connection_new_remote("workstation", "100.72.0.41",
                                             CLAWT_DEFAULT_TCP_PORT, NULL);
    clawt_connection_set_token(connection, "device-token-from-pairing");
    g_ptr_array_add(list, connection);

    g_assert_true(clawt_connection_list_save(path, list, &error));
    g_assert_no_error(error);

    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);

    g_assert_true(g_file_get_contents(path, &text, NULL, &error));
    g_assert_nonnull(strstr(text, "device-token-from-pairing"));

    clawt_test_remove_tree(dir);
}

static void
test_pairing_refuses_a_malformed_code_before_dialling(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConnection) local = NULL;
    ClawtConnection *paired;

    /*
     * 192.0.2.1 is TEST-NET-1: were the shape check skipped, this would
     * try to dial it -- and under `unshare -rn` fail differently.
     */
    paired = clawt_connection_new_paired("x", "192.0.2.1", 0, FALSE, FALSE,
                                         "12345", NULL, &error);
    g_assert_null(paired);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    paired = clawt_connection_new_paired("x", "", 0, FALSE, FALSE, "123456",
                                         NULL, &error);
    g_assert_null(paired);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    local = clawt_connection_new_local("Local", NULL);
    g_assert_false(clawt_connection_pair(local, "123456", NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
}

/* ── The daemon, end to end over its socket ──────────────────────── */

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    gchar        *socket_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
} DaemonFixture;

static void
daemon_setup(DaemonFixture *fixture, const gchar *extra)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-devices-daemon-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);
    fixture->socket_path = g_build_filename(fixture->dir, "daemon.sock",
                                            NULL);

    /* The pins every daemon fixture here carries: nothing on the network. */
    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "%s"
        "defaults:\n  workspace_root: \"%s/agents\"\n",
        fixture->dir, fixture->socket_path, fixture->dir,
        extra != NULL ? extra : "", fixture->dir);

    g_assert_true(g_file_set_contents(fixture->config_path, yaml, -1,
                                      &error));
    g_assert_no_error(error);

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);
    clawt_daemon_start(fixture->daemon, &error);
    g_assert_no_error(error);
}

static void
daemon_teardown(DaemonFixture *fixture)
{
    clawt_daemon_stop(fixture->daemon);
    g_clear_object(&fixture->daemon);

    while (g_main_context_iteration(fixture->context, FALSE))
        ;

    g_clear_pointer(&fixture->context, g_main_context_unref);
    clawt_test_remove_tree(fixture->dir);
    g_clear_pointer(&fixture->dir, g_free);
    g_clear_pointer(&fixture->config_path, g_free);
    g_clear_pointer(&fixture->socket_path, g_free);
}

/* A request answered as the owner: no connection, so no device. */
static JsonNode *
owner_request(DaemonFixture *fixture, const gchar *kind,
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
payload_of(JsonNode *reply)
{
    return json_object_get_object_member(json_node_get_object(reply),
                                         "payload");
}

static gboolean
reply_ok(JsonNode *reply)
{
    return json_object_get_boolean_member(json_node_get_object(reply), "ok");
}

static void
test_daemon_pair_list_revoke(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(JsonNode) paired = NULL;
    g_autoptr(JsonNode) listed = NULL;
    g_autoptr(JsonNode) refused = NULL;
    g_autoptr(JsonNode) mine = NULL;
    g_autoptr(JsonNode) revoked = NULL;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *code = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *dump = NULL;
    g_autofree gchar *file = NULL;
    g_autofree gchar *devices_path = NULL;
    JsonObject *payload;
    JsonArray *devices;

    daemon_setup(&fixture, "  device_idle_days: 7\n");

    /* The owner issues a code; the reply carries it and its clock. */
    paired = owner_request(&fixture, "device.pair", "{\"name\": \"phone\"}");
    g_assert_true(reply_ok(paired));
    payload = payload_of(paired);
    code = g_strdup(json_object_get_string_member(payload, "code"));
    g_assert_cmpuint(strlen(code), ==, CLAWT_DEVICE_CODE_DIGITS);
    g_assert_cmpint(json_object_get_int_member(payload, "expires_in"), ==,
                    CLAWT_DEVICE_CODE_LIFETIME_SECONDS);

    /* No listener in a test, and the reply says so rather than inventing one. */
    g_assert_cmpuint(json_array_get_length(
                         json_object_get_array_member(payload, "addresses")),
                     ==, 0);

    /* A client redeems it over the socket and becomes that device. */
    g_main_context_push_thread_default(fixture.context);

    client = clawt_client_new(fixture.socket_path);
    clawt_client_set_auto_reconnect(client, FALSE);
    clawt_client_set_pairing_code(client, code, "ignored-name");
    g_assert_true(clawt_client_connect(client, &error));
    g_assert_no_error(error);

    token = g_strdup(clawt_client_get_token(client));
    id = g_strdup(clawt_client_get_device_id(client));
    g_assert_nonnull(token);
    g_assert_nonnull(id);

    /* That device may list -- and sees itself -- but may not pair. */
    mine = clawt_client_request(client, "device.list", NULL, &error);
    g_assert_no_error(error);
    devices = json_object_get_array_member(json_node_get_object(mine),
                                           "devices");
    g_assert_cmpuint(json_array_get_length(devices), ==, 1);
    g_assert_true(json_object_get_boolean_member(
        json_array_get_object_element(devices, 0), "current"));
    g_assert_true(json_object_get_boolean_member(
        json_array_get_object_element(devices, 0), "connected"));
    g_assert_false(json_object_get_boolean_member(
        json_node_get_object(mine), "owner"));

    refused = clawt_client_request(client, "device.pair", NULL, &error);
    g_assert_null(refused);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    /* The owner's listing: the name from the code, and no token anywhere. */
    listed = owner_request(&fixture, "device.list", NULL);
    g_assert_true(reply_ok(listed));
    payload = payload_of(listed);
    g_assert_true(json_object_get_boolean_member(payload, "owner"));
    g_assert_cmpint(json_object_get_int_member(payload, "idle_days"), ==, 7);
    devices = json_object_get_array_member(payload, "devices");
    g_assert_cmpstr(json_object_get_string_member(
                        json_array_get_object_element(devices, 0), "name"),
                    ==, "phone");

    dump = json_to_string(listed, FALSE);
    g_assert_null(strstr(dump, token));
    {
        g_autofree gchar *hash = clawt_device_token_hash(token);

        g_assert_null(strstr(dump, hash));
    }

    /* The token is in the state directory only as its hash, 0600. */
    devices_path = g_build_filename(fixture.dir, "state", "devices.yaml",
                                    NULL);
    g_assert_true(g_file_get_contents(devices_path, &file, NULL, NULL));
    g_assert_null(strstr(file, token));

    /* Revoking closes the live connection, not only the next hello. */
    revoked = owner_request(&fixture, "device.revoke",
                            "{\"id\": \"placeholder\"}");
    g_assert_false(reply_ok(revoked));
    g_clear_pointer(&revoked, json_node_unref);

    {
        g_autofree gchar *body = g_strdup_printf("{\"id\": \"%s\"}", id);

        revoked = owner_request(&fixture, "device.revoke", body);
    }

    g_assert_true(reply_ok(revoked));
    g_assert_cmpint(json_object_get_int_member(payload_of(revoked),
                                               "closed"), ==, 1);

    /*
     * The client learns its socket went away on the next request; a
     * bounded loop rather than a sleep, so this fails instead of hanging.
     */
    {
        g_autoptr(JsonNode) after = NULL;
        guint spins;

        for (spins = 0; spins < 1000 && clawt_client_is_connected(client);
             spins++)
            g_main_context_iteration(fixture.context, FALSE);

        after = clawt_client_request(client, "device.list", NULL, &error);
        g_assert_null(after);
        g_clear_error(&error);
    }

    clawt_client_disconnect(client);
    g_clear_object(&client);

    while (g_main_context_iteration(fixture.context, FALSE))
        ;

    g_main_context_pop_thread_default(fixture.context);

    daemon_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/devices/hash-is-sha256", test_hash_is_sha256_hex);
    g_test_add_func("/devices/secure-equals", test_secure_equals_contract);
    g_test_add_func("/devices/code-normalize-display",
                    test_code_normalize_and_display);
    g_test_add_func("/devices/seen-text", test_seen_text);
    g_test_add_func("/devices/code/six-digits", test_code_is_six_digits);
    g_test_add_func("/devices/code/expires", test_code_expires);
    g_test_add_func("/devices/code/single-use", test_code_is_single_use);
    g_test_add_func("/devices/code/guess-limit",
                    test_guess_limit_invalidates_the_code);
    g_test_add_func("/devices/code/one-wrong-guess",
                    test_a_wrong_guess_below_the_limit_leaves_the_code);
    g_test_add_func("/devices/code/rate-limit",
                    test_rate_limit_refuses_without_looking);
    g_test_add_func("/devices/token/hashed",
                    test_token_authenticates_and_is_stored_hashed);
    g_test_add_func("/devices/token/reload", test_tokens_survive_a_reload);
    g_test_add_func("/devices/list-has-no-token",
                    test_list_never_carries_a_token);
    g_test_add_func("/devices/revoke", test_revoke);
    g_test_add_func("/devices/idle/expiry", test_idle_expiry);
    g_test_add_func("/devices/idle/refused-at-hello",
                    test_idle_device_is_refused_at_hello);
    g_test_add_func("/devices/owner-token-unaffected",
                    test_owner_token_is_unaffected);
    g_test_add_func("/devices/connections-0600",
                    test_connections_file_is_0600);
    g_test_add_func("/devices/pair-refuses-malformed-code",
                    test_pairing_refuses_a_malformed_code_before_dialling);
    g_test_add_func("/devices/daemon/pair-list-revoke",
                    test_daemon_pair_list_revoke);

    return g_test_run();
}
