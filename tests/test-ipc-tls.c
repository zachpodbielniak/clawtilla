/*
 * test-ipc-tls.c - The IPC listener's TLS, and pinning a certificate
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Before 0.66.0 a configured certificate was stored and never used: the
 * TCP listener spoke plain text whatever daemon.tls_cert said.  Checked
 * here against a real loopback listener, so behind CLAWT_TEST_INTEGRATION
 * (`make test` opens no network socket):
 *
 *  - a client pinning the right certificate connects and says hello;
 *  - a client pinning another certificate is refused;
 *  - a client validating normally refuses a self-signed one;
 *  - a client speaking plain text to a TLS listener gets nowhere.
 *
 * The pin itself is computed without a socket and checked always.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include <string.h>

#define TOKEN "test-token-0123456789abcdef0123456789abcdef"

static gboolean
integration(void)
{
    return g_getenv("CLAWT_TEST_INTEGRATION") != NULL;
}

static GTlsCertificate *
fixture_certificate(void)
{
    g_autofree gchar *cert = g_build_filename(CLAWT_TEST_FIXTURES, "tls", "test-cert.pem", NULL);
    g_autofree gchar *key = g_build_filename(CLAWT_TEST_FIXTURES, "tls", "test-key.pem", NULL);

    return g_tls_certificate_new_from_files(cert, key, NULL);
}

/* The pin is SHA-256 over the DER, in hex, and stable. */
static void
test_pin_is_stable(void)
{
    g_autoptr(GTlsCertificate) cert = fixture_certificate();
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;

    g_assert_nonnull(cert);
    a = clawt_tls_certificate_pin(cert);
    b = clawt_tls_certificate_pin(cert);
    g_assert_cmpuint(strlen(a), ==, 64);
    g_assert_cmpstr(a, ==, b);
}

typedef struct {
    gchar          *dir;
    ClawtIpcServer *server;
    guint16         port;
} Listener;

static gboolean
listener_up(
    Listener *l
){
    g_autofree gchar *sock = NULL;
    g_autofree gchar *cert = g_build_filename(CLAWT_TEST_FIXTURES, "tls", "test-cert.pem", NULL);
    g_autofree gchar *key = g_build_filename(CLAWT_TEST_FIXTURES, "tls", "test-key.pem", NULL);
    g_autoptr(GError) error = NULL;

    l->dir = g_dir_make_tmp("clawt-tls-XXXXXX", NULL);
    sock = g_build_filename(l->dir, "d.sock", NULL);
    l->port = (guint16)g_random_int_range(42000, 52000);
    l->server = clawt_ipc_server_new(sock);
    clawt_ipc_server_set_token(l->server, TOKEN);
    clawt_ipc_server_set_tls(l->server, cert, key);
    clawt_ipc_server_add_listener(l->server, "127.0.0.1", l->port, FALSE);
    if (!clawt_ipc_server_start(l->server, &error)) {
        g_test_skip(error->message);
        return FALSE;
    }
    return TRUE;
}

static void
listener_down(
    Listener *l
){
    clawt_ipc_server_stop(l->server);
    g_object_unref(l->server);
    while (g_main_context_iteration(NULL, FALSE))
        ;
    clawt_test_remove_tree(l->dir);
    g_free(l->dir);
}

typedef struct {
    gboolean done;
    gboolean ok;
} Outcome;

static void
on_connected(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    Outcome *o = user_data;

    o->ok = clawt_client_connect_finish(CLAWT_CLIENT(source), result, NULL);
    o->done = TRUE;
}

/*
 * Async, with the context turning: the listener is in this thread, so a
 * blocking connect would wait on a handshake nobody can answer.
 */
static gboolean
try_connect(
    guint16      port,
    const gchar *pin,
    gboolean     tls
){
    g_autoptr(ClawtClient) client = clawt_client_new_tcp("127.0.0.1", port, TOKEN);
    Outcome o = { FALSE, FALSE };
    gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;

    if (pin != NULL)
        clawt_client_set_tls_pin(client, pin);
    else if (tls)
        clawt_client_set_tls(client, TRUE, FALSE);
    clawt_client_connect_async(client, NULL, on_connected, &o);
    while (!o.done) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_main_context_iteration(NULL, TRUE);
    }
    if (o.ok)
        clawt_client_disconnect(client);
    return o.ok;
}

static void
test_listener_speaks_tls(void)
{
    Listener l;
    g_autoptr(GTlsCertificate) cert = NULL;
    g_autofree gchar *pin = NULL;

    if (!integration()) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION; opens a loopback socket");
        return;
    }
    memset(&l, 0, sizeof(l));
    if (!listener_up(&l))
        return;
    cert = fixture_certificate();
    pin = clawt_tls_certificate_pin(cert);

    g_assert_true(try_connect(l.port, pin, TRUE));
    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING, "*not the pinned one*");
    g_assert_false(try_connect(l.port, "00000000000000000000000000000000"
                                       "00000000000000000000000000000000", TRUE));
    g_test_assert_expected_messages();
    /* Validated normally, a self-signed certificate is refused. */
    g_assert_false(try_connect(l.port, NULL, TRUE));
    /* Plain text to a TLS listener says hello to nobody. */
    g_assert_false(try_connect(l.port, NULL, FALSE));
    listener_down(&l);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ipc-tls/pin-is-stable", test_pin_is_stable);
    g_test_add_func("/ipc-tls/listener-speaks-tls", test_listener_speaks_tls);

    return g_test_run();
}
