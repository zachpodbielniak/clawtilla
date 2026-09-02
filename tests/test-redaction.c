/*
 * test-redaction.c - What clawt_redact_secrets() actually catches
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Redaction is applied on the way *in* -- to the event log, to
 * transcripts, to the transcript index and to an agent's log ring --
 * because everything it guards is replayed into a model's context or
 * pasted into a bug report, and a key that reached one of those is there
 * for ever.  That makes its coverage a security property rather than a
 * nicety, and coverage is the half a caller cannot see: a pattern that
 * does not match reports nothing, so a credential it misses looks
 * exactly like a line that held no credential.
 *
 * Asserted on shapes this daemon actually brokers, not on a
 * representative sample.
 */

#include <clawtilla.h>

#include <string.h>

/* The secret is gone and something says so in its place. */
static void
assert_redacted(const gchar *line, const gchar *secret)
{
    g_autofree gchar *out = clawt_redact_secrets(line);

    g_assert_nonnull(out);

    if (strstr(out, secret) != NULL)
        g_error("'%s' survived redaction: %s", secret, out);

    g_assert_nonnull(strstr(out, "[REDACTED]"));
}

/*
 * An HTTP header is how every connector, the venture bridge and ntfy
 * carry a credential, and it was the one shape the assignment patterns
 * structurally could not see: after "auth" comes "orization", not a
 * separator, and after "bearer" comes a space rather than a ":" or "=".
 * So `Authorization: Bearer <opaque token>` -- which is what a libreclaw
 * child writes when it logs a request -- went through untouched.
 */
static void
test_an_authorization_header(void)
{
    assert_redacted("GET /v1/x\nAuthorization: Bearer "
                    "eyJhbGciOiJIUzI1NiJ9.payload.signature",
                    "eyJhbGciOiJIUzI1NiJ9.payload.signature");

    assert_redacted("authorization: bearer 0123456789abcdef0123456789abcdef",
                    "0123456789abcdef0123456789abcdef");

    assert_redacted("Proxy-Authorization: Basic "
                    "dXNlcjpodW50ZXIyaHVudGVyMg==",
                    "dXNlcjpodW50ZXIyaHVudGVyMg==");

    /* And the scheme word on its own, with no header name in front. */
    assert_redacted("curl -H 'Bearer sometokenvaluehere1234'",
                    "sometokenvaluehere1234");
}

/*
 * The header name and the scheme survive.
 *
 * A log line reading "[REDACTED]" says nothing about which request it
 * was; the value is the part that must not be kept, and the shape around
 * it is the part worth keeping.
 */
static void
test_the_header_stays_readable(void)
{
    g_autofree gchar *out =
        clawt_redact_secrets("Authorization: Bearer ghp_"
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    g_assert_nonnull(strstr(out, "Authorization"));
    g_assert_nonnull(strstr(out, "[REDACTED]"));
}

/*
 * The forge tokens this project's own tooling handles.  gitctl drives
 * GitHub, GitLab, Forgejo and Gitea, and only GitHub's shapes were on
 * the list.
 */
static void
test_forge_tokens(void)
{
    assert_redacted("pushed with glpat-AAAAAAAAAAAAAAAAAAAA",
                    "glpat-AAAAAAAAAAAAAAAAAAAA");
    assert_redacted("runner glrt-BBBBBBBBBBBBBBBBBBBB",
                    "glrt-BBBBBBBBBBBBBBBBBBBB");
    assert_redacted("ghp_cccccccccccccccccccccccccccccccccccc",
                    "ghp_cccccccccccccccccccccccccccccccccccc");
    assert_redacted("github_pat_11ABCDEFG0abcdefghijklmnop",
                    "github_pat_11ABCDEFG0abcdefghijklmnop");
}

/* The rest of the Slack family, and the two cloud shapes. */
static void
test_other_well_known_shapes(void)
{
    assert_redacted("xoxp-1111111111-2222222222-abcdefgh",
                    "xoxp-1111111111-2222222222-abcdefgh");
    assert_redacted("xapp-1-A00000000-1111111111-abcdef",
                    "xapp-1-A00000000-1111111111-abcdef");
    assert_redacted("AIzaSyA00000000000000000000000000000000",
                    "AIzaSyA00000000000000000000000000000000");
    assert_redacted("AKIAIOSFODNN7EXAMPLE", "AKIAIOSFODNN7EXAMPLE");
}

/* The shapes that were already covered, so widening did not narrow. */
static void
test_the_original_shapes_still_match(void)
{
    assert_redacted("ANTHROPIC_API_KEY=sk-ant-api03-"
                    "abcdefghijklmnopqrstuvwxyz",
                    "sk-ant-api03-abcdefghijklmnopqrstuvwxyz");
    assert_redacted("password: hunter2hunter2", "hunter2hunter2");
    assert_redacted("access_token = syt_YWdlbnQ_abcdefghij_012345",
                    "syt_YWdlbnQ_abcdefghij_012345");
    assert_redacted("-----BEGIN OPENSSH PRIVATE KEY-----",
                    "-----BEGIN OPENSSH PRIVATE KEY-----");
}

/*
 * Ordinary prose is left alone.
 *
 * A redaction that fires on anything long turns a transcript into
 * "[REDACTED]" and teaches everybody to stop reading it, which is how a
 * real one gets skimmed past.
 */
static void
test_ordinary_text_is_untouched(void)
{
    g_autofree gchar *out = clawt_redact_secrets(
        "The deploy finished at 14:02 and the container is healthy. "
        "See https://example.org/runs/1234 for the log.");

    g_assert_null(strstr(out, "REDACTED"));
}

/*
 * Best effort, and the docs say so.
 *
 * An opaque OAuth access token pasted on its own has no shape to
 * recognise -- no prefix, no key word, no header -- so it is not caught,
 * and pinning that here is the difference between a documented limit and
 * a surprise.  The remedy for those is that a secret never reaches these
 * sinks in the first place, which is what the rest of the tree enforces.
 */
static void
test_a_bare_opaque_token_is_a_known_gap(void)
{
    g_autofree gchar *out =
        clawt_redact_secrets("gAAAAABmZm9vYmFyYmF6cXV1eA");

    g_assert_null(strstr(out, "REDACTED"));
}

static void
test_null_survives(void)
{
    g_assert_null(clawt_redact_secrets(NULL));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/redaction/authorization-header",
                    test_an_authorization_header);
    g_test_add_func("/redaction/header-stays-readable",
                    test_the_header_stays_readable);
    g_test_add_func("/redaction/forge-tokens", test_forge_tokens);
    g_test_add_func("/redaction/well-known-shapes",
                    test_other_well_known_shapes);
    g_test_add_func("/redaction/original-shapes",
                    test_the_original_shapes_still_match);
    g_test_add_func("/redaction/prose-untouched",
                    test_ordinary_text_is_untouched);
    g_test_add_func("/redaction/opaque-token-gap",
                    test_a_bare_opaque_token_is_a_known_gap);
    g_test_add_func("/redaction/null", test_null_survives);

    return g_test_run();
}
