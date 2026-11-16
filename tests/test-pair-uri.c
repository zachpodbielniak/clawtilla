/*
 * test-pair-uri.c - The pairing URI and the QR code that carries it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The URI is the contract with the phone client, which is written in
 * another language against docs/ipc-protocol.org rather than against
 * this code -- so the exact bytes are asserted, not only that a build
 * parses back.  A round trip alone would pass with every value left
 * unescaped, and an IPv6 address would then split at its first colon on
 * the phone.
 *
 * The QR half depends on how the library was configured: built with
 * libqrencode the renderers are smoke-tested; built without it they must
 * refuse with NOT_SUPPORTED rather than return something empty that a
 * client would draw as a blank square.
 */

#include <clawtilla.h>

#include <string.h>

/* ── Building ────────────────────────────────────────────────────── */

static void
test_build_exact_bytes(void)
{
    const gchar *hosts[] = { "100.101.102.103", "fd7a:115c:a1e0::1", NULL };
    g_autofree gchar *uri = clawt_pair_uri_build("123456", hosts, 8792,
                                                 "phone");

    g_assert_cmpstr(uri, ==,
                    "clawtilla://pair?v=1&code=123456&port=8792"
                    "&host=100.101.102.103"
                    "&host=fd7a%3A115c%3Aa1e0%3A%3A1"
                    "&name=phone");
}

static void
test_build_normalises_code(void)
{
    const gchar *hosts[] = { "10.0.0.2", NULL };
    g_autofree gchar *spaced = clawt_pair_uri_build("123 456", hosts, 0,
                                                    NULL);
    g_autofree gchar *dashed = clawt_pair_uri_build("12-34-56", hosts, 0,
                                                    NULL);
    g_autofree gchar *empty = clawt_pair_uri_build(" - ", hosts, 0, NULL);

    g_assert_nonnull(strstr(spaced, "&code=123456&"));
    g_assert_cmpstr(spaced, ==, dashed);

    /* nothing left to redeem is no URI, not one with an empty code */
    g_assert_null(empty);
}

static void
test_build_null_name_and_default_port(void)
{
    const gchar *hosts[] = { "10.0.0.2", NULL };
    g_autofree gchar *none = clawt_pair_uri_build("123456", hosts, 0, NULL);
    g_autofree gchar *blank = clawt_pair_uri_build("123456", hosts, 0, "");
    g_autofree gchar *expected = g_strdup_printf(
        "clawtilla://pair?v=1&code=123456&port=%d&host=10.0.0.2",
        CLAWT_DEFAULT_TCP_PORT);

    g_assert_cmpstr(none, ==, expected);
    g_assert_cmpstr(blank, ==, expected);
}

static void
test_build_escapes_every_value(void)
{
    const gchar *hosts[] = { "10.0.0.2", NULL };
    g_autofree gchar *uri = clawt_pair_uri_build(
        "123456", hosts, 0, "Ann's phone & tablet=1/2?#");
    g_autofree gchar *name = NULL;

    /* no structural character from the name survives unescaped */
    g_assert_nonnull(strstr(uri,
        "&name=Ann%27s%20phone%20%26%20tablet%3D1%2F2%3F%23"));

    g_assert_true(clawt_pair_uri_parse(uri, NULL, NULL, NULL, &name, NULL));
    g_assert_cmpstr(name, ==, "Ann's phone & tablet=1/2?#");
}

/* ── From what the daemon bound ──────────────────────────────────── */

static void
test_from_addresses_splits_ports(void)
{
    /* the exact shapes clawt_ipc_server_list_bound() produces */
    const gchar *bound[] = {
        "100.101.102.103:9000",
        "[fd7a:115c:a1e0::1]:9000",
        NULL
    };
    g_autofree gchar *uri = clawt_pair_uri_build_from_addresses(
        "123456", bound, "phone");

    g_assert_cmpstr(uri, ==,
                    "clawtilla://pair?v=1&code=123456&port=9000"
                    "&host=100.101.102.103"
                    "&host=fd7a%3A115c%3Aa1e0%3A%3A1"
                    "&name=phone");
}

static void
test_from_addresses_drops_undialable(void)
{
    const gchar *bound[] = {
        "127.0.0.1:8792",
        "[::1]:8792",
        "0.0.0.0:8792",
        "[::]:8792",
        "192.168.1.20:8792",
        "192.168.1.21:8800",       /* another port: no room for it */
        "not an address",
        NULL
    };
    const gchar *only_loopback[] = { "127.0.0.1:8792", NULL };
    g_autofree gchar *uri = clawt_pair_uri_build_from_addresses(
        "123456", bound, NULL);
    g_autofree gchar *nothing = clawt_pair_uri_build_from_addresses(
        "123456", only_loopback, NULL);
    g_autofree gchar *empty = clawt_pair_uri_build_from_addresses(
        "123456", NULL, NULL);

    g_assert_cmpstr(uri, ==,
                    "clawtilla://pair?v=1&code=123456&port=8792"
                    "&host=192.168.1.20");

    /* a QR code nobody else can redeem is no QR code */
    g_assert_null(nothing);
    g_assert_null(empty);
}

static void
test_from_addresses_bare_default_port(void)
{
    const gchar *bound[] = { "fd7a:115c:a1e0::1", "10.1.2.3", NULL };
    g_autofree gchar *uri = clawt_pair_uri_build_from_addresses(
        "654321", bound, NULL);
    g_autofree gchar *expected = g_strdup_printf(
        "clawtilla://pair?v=1&code=654321&port=%d"
        "&host=fd7a%%3A115c%%3Aa1e0%%3A%%3A1&host=10.1.2.3",
        CLAWT_DEFAULT_TCP_PORT);

    g_assert_cmpstr(uri, ==, expected);
}

/* ── Parsing ─────────────────────────────────────────────────────── */

static void
test_round_trip(void)
{
    const gchar *hosts[] = { "100.101.102.103", "fd7a:115c:a1e0::1", NULL };
    g_autofree gchar *uri = clawt_pair_uri_build("123 456", hosts, 8800,
                                                 "phone");
    g_autofree gchar *code = NULL;
    g_autofree gchar *name = NULL;
    g_auto(GStrv) parsed = NULL;
    g_autoptr(GError) error = NULL;
    guint16 port = 0;

    g_assert_true(clawt_pair_uri_parse(uri, &code, &parsed, &port, &name,
                                       &error));
    g_assert_no_error(error);
    g_assert_cmpstr(code, ==, "123456");
    g_assert_cmpuint(port, ==, 8800);
    g_assert_cmpstr(name, ==, "phone");
    g_assert_cmpuint(g_strv_length(parsed), ==, 2);
    g_assert_cmpstr(parsed[0], ==, "100.101.102.103");
    g_assert_cmpstr(parsed[1], ==, "fd7a:115c:a1e0::1");
}

static void
test_parse_defaults_and_unknown_params(void)
{
    g_autofree gchar *code = NULL;
    g_autofree gchar *name = NULL;
    g_auto(GStrv) hosts = NULL;
    guint16 port = 0;

    /* no port, no name, a parameter from some later version */
    g_assert_true(clawt_pair_uri_parse(
        "CLAWTILLA://pair?v=1&code=123-456&host=10.0.0.2&future=yes",
        &code, &hosts, &port, &name, NULL));
    g_assert_cmpstr(code, ==, "123456");
    g_assert_cmpuint(port, ==, CLAWT_DEFAULT_TCP_PORT);
    g_assert_null(name);
    g_assert_cmpstr(hosts[0], ==, "10.0.0.2");
    g_assert_null(hosts[1]);
}

static void
test_parse_refuses(void)
{
    static const struct {
        const gchar *uri;
        gint         code;
    } cases[] = {
        { "https://pair?v=1&code=123456&host=10.0.0.2",
          CLAWT_ERROR_INVALID_ARGUMENT },
        { "clawtilla://connect?v=1&code=123456&host=10.0.0.2",
          CLAWT_ERROR_INVALID_ARGUMENT },
        { "clawtilla://pair?code=123456&host=10.0.0.2",
          CLAWT_ERROR_INVALID_ARGUMENT },
        { "clawtilla://pair?v=2&code=123456&host=10.0.0.2",
          CLAWT_ERROR_NOT_SUPPORTED },
        { "clawtilla://pair?v=1&host=10.0.0.2",
          CLAWT_ERROR_INVALID_ARGUMENT },
        { "clawtilla://pair?v=1&code=12a456&host=10.0.0.2",
          CLAWT_ERROR_INVALID_ARGUMENT },
        { "clawtilla://pair?v=1&code=123456&port=0&host=10.0.0.2",
          CLAWT_ERROR_INVALID_ARGUMENT },
        { "clawtilla://pair?v=1&code=123456&port=70000&host=10.0.0.2",
          CLAWT_ERROR_INVALID_ARGUMENT },
        { "clawtilla://pair?v=1&code=123456&port=87x&host=10.0.0.2",
          CLAWT_ERROR_INVALID_ARGUMENT },
        { "clawtilla://pair?v=1&code=123456",
          CLAWT_ERROR_INVALID_ARGUMENT },
        { "clawtilla://pair",
          CLAWT_ERROR_INVALID_ARGUMENT },
        { "not a uri at all",
          CLAWT_ERROR_INVALID_ARGUMENT },
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autoptr(GError) error = NULL;
        g_autofree gchar *code = NULL;

        g_test_message("%s", cases[i].uri);
        g_assert_false(clawt_pair_uri_parse(cases[i].uri, &code, NULL, NULL,
                                            NULL, &error));
        g_assert_error(error, CLAWT_ERROR, cases[i].code);
        g_assert_null(code);

        /* a refusal is logged text: it must not carry the live code */
        g_assert_null(strstr(error->message, "123456"));
    }
}

/* ── QR ──────────────────────────────────────────────────────────── */

#ifdef CLAWT_HAVE_QRENCODE

static void
test_qr_matrix(void)
{
    g_autoptr(GBytes) modules = NULL;
    g_autoptr(GError) error = NULL;
    const guint8 *data;
    gsize length = 0;
    guint size = 0;
    gsize i;

    g_assert_true(clawt_qr_is_available());

    modules = clawt_qr_encode(
        "clawtilla://pair?v=1&code=123456&port=8792&host=100.101.102.103",
        &size, &error);
    g_assert_no_error(error);
    g_assert_nonnull(modules);

    /* versions are 21 + 4k modules a side */
    g_assert_cmpuint(size, >=, 21);
    g_assert_cmpuint((size - 21) % 4, ==, 0);

    data = g_bytes_get_data(modules, &length);
    g_assert_cmpuint(length, ==, (gsize)size * size);

    for (i = 0; i < length; i++)
        g_assert_true(data[i] == 0 || data[i] == 1);

    /* the top-left finder pattern: a dark ring around a light one */
    g_assert_cmpuint(data[0], ==, 1);
    g_assert_cmpuint(data[6], ==, 1);
    g_assert_cmpuint(data[(gsize)size + 1], ==, 0);
    g_assert_cmpuint(data[(gsize)3 * size + 3], ==, 1);
}

static void
test_qr_terminal_and_svg(void)
{
    const gchar *text = "clawtilla://pair?v=1&code=123456&host=10.0.0.2";
    g_autoptr(GBytes) modules = NULL;
    g_autofree gchar *dark = NULL;
    g_autofree gchar *light = NULL;
    g_autofree gchar *svg = NULL;
    g_auto(GStrv) lines = NULL;
    guint size = 0;
    guint total;

    modules = clawt_qr_encode(text, &size, NULL);
    total = size + 2 * CLAWT_QR_QUIET_ZONE;

    dark = clawt_qr_render_terminal(text, FALSE, NULL);
    light = clawt_qr_render_terminal(text, TRUE, NULL);
    g_assert_true(g_utf8_validate(dark, -1, NULL));
    g_assert_cmpstr(dark, !=, light);

    /* two module rows a line, every line as wide as the code */
    lines = g_strsplit(dark, "\n", -1);
    g_assert_cmpuint(g_strv_length(lines), ==, (total + 1) / 2 + 1);
    g_assert_cmpuint(g_utf8_strlen(lines[0], -1), ==, total);

    /* on a dark terminal the quiet zone is a solid lit frame */
    g_assert_true(g_str_has_prefix(lines[0], "\342\226\210"));
    g_assert_null(strchr(lines[0], ' '));

    svg = clawt_qr_render_svg(text, NULL);
    g_assert_true(g_str_has_prefix(svg, "<svg "));
    g_assert_true(g_str_has_suffix(svg, "</svg>"));

    /* nothing from the text reaches markup that is placed unescaped */
    g_assert_null(strstr(svg, "clawtilla"));
    g_assert_null(strstr(svg, "123456"));
}

#else /* !CLAWT_HAVE_QRENCODE */

static void
test_qr_unavailable_refuses(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) modules = NULL;
    g_autofree gchar *terminal = NULL;
    g_autofree gchar *svg = NULL;
    guint size = 99;

    g_assert_false(clawt_qr_is_available());

    modules = clawt_qr_encode("clawtilla://pair", &size, &error);
    g_assert_null(modules);
    g_assert_cmpuint(size, ==, 0);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);

    /* the refusal names what to install, which is the whole remedy */
    g_assert_nonnull(strstr(error->message, "qrencode-devel"));
    g_clear_error(&error);

    terminal = clawt_qr_render_terminal("clawtilla://pair", FALSE, &error);
    g_assert_null(terminal);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
    g_clear_error(&error);

    svg = clawt_qr_render_svg("clawtilla://pair", &error);
    g_assert_null(svg);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
}

#endif /* CLAWT_HAVE_QRENCODE */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

    g_test_add_func("/pair-uri/build/exact-bytes", test_build_exact_bytes);
    g_test_add_func("/pair-uri/build/normalises-code",
                    test_build_normalises_code);
    g_test_add_func("/pair-uri/build/null-name-and-default-port",
                    test_build_null_name_and_default_port);
    g_test_add_func("/pair-uri/build/escapes-every-value",
                    test_build_escapes_every_value);
    g_test_add_func("/pair-uri/from-addresses/splits-ports",
                    test_from_addresses_splits_ports);
    g_test_add_func("/pair-uri/from-addresses/drops-undialable",
                    test_from_addresses_drops_undialable);
    g_test_add_func("/pair-uri/from-addresses/bare-default-port",
                    test_from_addresses_bare_default_port);
    g_test_add_func("/pair-uri/parse/round-trip", test_round_trip);
    g_test_add_func("/pair-uri/parse/defaults-and-unknown-params",
                    test_parse_defaults_and_unknown_params);
    g_test_add_func("/pair-uri/parse/refuses", test_parse_refuses);

#ifdef CLAWT_HAVE_QRENCODE
    g_test_add_func("/qr/matrix", test_qr_matrix);
    g_test_add_func("/qr/terminal-and-svg", test_qr_terminal_and_svg);
#else
    g_test_add_func("/qr/unavailable-refuses", test_qr_unavailable_refuses);
#endif

    return g_test_run();
}
